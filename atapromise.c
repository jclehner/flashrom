/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2015 Joseph C. Lehner <joseph.c.lehner@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#if defined(__i386__) || defined(__x86_64__)

#include <string.h>
#include <stdlib.h>
#include "flash.h"
#include "programmer.h"
#include "hwaccess.h"

#define MAX_ROM_DECODE (32 * 1024)
#define ADDR_MASK (MAX_ROM_DECODE - 1)

/*
 * In the absence of any public docs on the PDC2026x family, this programmer
 * was created through a mix of reverse-engineering and trial and error.
 *
 * The only device tested is an Ultra100 controller, but the logic for
 * programming the other 2026x controllers is the same, so it should,
 * in theory, work for those as well.
 *
 * This programmer is limited to the first 32 kB, which should be sufficient,
 * given that the ROM files for these controllers are 16 kB. Since flashrom
 * does not support flashing images smaller than the detected flash chip
 * (the tested Ultra100 uses a 128 kB MX29F001T chip), the chip size
 * is hackishly adjusted in atapromise_fixup_chip.
 *
 * To flash 32 kB files, use "allow32k=y".
 */

static uint32_t io_base_addr = 0;
static uint32_t rom_base_addr = 0;

static uint8_t *atapromise_bar = NULL;
static size_t rom_size = 0;

const struct dev_entry ata_promise[] = {
	{0x105a, 0x4d38, NT, "Promise", "PDC20262 (FastTrak66/Ultra66)"},
	{0x105a, 0x0d30, NT, "Promise", "PDC20265 (FastTrak100 Lite/Ultra100)"},
	{0x105a, 0x4d30, OK, "Promise", "PDC20267 (FastTrak100/Ultra100)"},
	{0},
};

static void atapromise_chip_writeb(const struct flashctx *flash, uint8_t val,
				chipaddr addr);
static uint8_t atapromise_chip_readb(const struct flashctx *flash,
		const chipaddr addr);

static const struct par_master par_master_atapromise = {
		.chip_readb		= atapromise_chip_readb,
		.chip_readw		= fallback_chip_readw,
		.chip_readl		= fallback_chip_readl,
		.chip_readn		= fallback_chip_readn,
		.chip_writeb	= atapromise_chip_writeb,
		.chip_writew	= fallback_chip_writew,
		.chip_writel	= fallback_chip_writel,
		.chip_writen	= fallback_chip_writen,
};

void *atapromise_map(const char *descr, uintptr_t phys_addr, size_t len)
{
	/* In case fallback_map ever returns something other than NULL. */
	return NULL;
}

static struct pci_dev *atapromise_find_bridge(struct pci_dev *dev)
{
	struct pci_dev *br;
	uint8_t bus_sec, bus_sub, htype;

	for (br = dev->access->devices; br; br = br->next) {
		/* Don't use br->hdrtype here! */
		htype = pci_read_byte(br, PCI_HEADER_TYPE) & 0x7f;
		if (htype != PCI_HEADER_TYPE_BRIDGE)
			continue;

		bus_sec = pci_read_byte(br, PCI_SECONDARY_BUS);
		bus_sub = pci_read_byte(br, PCI_SUBORDINATE_BUS);

		if (dev->bus >= bus_sec && dev->bus <= bus_sub) {
			msg_pdbg("Device is behind bridge %04x:%04x, BDF %02x:%02x.%x.\n",
					br->vendor_id, br->device_id, br->bus, br->dev, br->func);
			return br;
		}
	}

	return NULL;
}

static int atapromise_fixup_bridge(struct pci_dev *dev)
{
	struct pci_dev *br;
	uint16_t reg16;

	/* TODO: What about chained bridges? */
	br = atapromise_find_bridge(dev);
	if (br) {
		/* Make sure that the bridge memory windows are set correctly. */
		reg16 = pci_read_word(dev, PCI_BASE_ADDRESS_5 + 2) & 0xfff0;
		if (reg16 < pci_read_word(br, PCI_MEMORY_BASE)) {
			msg_pdbg("Adjusting memory base of bridge to %04x.\n", reg16);
			rpci_write_word(br, PCI_MEMORY_BASE, reg16);
		}

		reg16 += (MAX_ROM_DECODE / 1024);

		if (reg16 < pci_read_word(br, PCI_MEMORY_LIMIT)) {
			msg_pdbg("Adjusting memory limit of bridge to %04x.\n", reg16);
			rpci_write_word(br, PCI_MEMORY_LIMIT, reg16);
		}
	}

	return 0;
}

static void atapromise_fixup_chip(struct flashchip *chip)
{
	static bool once = false;
	unsigned int i, size;

	if (once)
		return;

	size = chip->total_size * 1024;
	if (size > rom_size)
	{
		/* Undefine all block_erasers that don't operate on the whole chip,
		 * and adjust the eraseblock size of the one that does.
		 */
		for (i = 0; i < NUM_ERASEFUNCTIONS; ++i) {
			if (chip->block_erasers[i].eraseblocks[0].size != size) {
				chip->block_erasers[i].eraseblocks[0].count = 0;
				chip->block_erasers[i].block_erase = NULL;
			} else {
				chip->block_erasers[i].eraseblocks[0].size = rom_size;
				break;
			}
		}

		if (i != NUM_ERASEFUNCTIONS) {
			chip->total_size = rom_size / 1024;
			if (chip->page_size > rom_size)
				chip->page_size = rom_size;
		} else {
			msg_pwarn("Failed to adjust size of chip \"%s\" (%d kB).\n",
					chip->name, chip->total_size);
		}
	}

	once = true;
}

static bool atapromise_allow32k()
{
	bool ret;
	char *p;

	p = extract_programmer_param("allow32k");
	if (!p) {
		return false;
	}

	ret = p[0] == '1' || p[0] == 'y' || p[0] == 'Y';
	free(p);
	return ret;
}

int atapromise_init(void)
{
	struct pci_dev *dev = NULL;
	char *param_32k = NULL;

	if (rget_io_perms())
		return 1;

	dev = pcidev_init(ata_promise, PCI_BASE_ADDRESS_4);
	if (!dev)
		return 1;

	if (atapromise_fixup_bridge(dev))
		return 1;

	io_base_addr = pcidev_readbar(dev, PCI_BASE_ADDRESS_4) & 0xfffe;
	if (!io_base_addr) {
		return 1;
	}

	/* Not exactly sure what this does, because flashing seems to work
	 * well without it. However, PTIFLASH does it, so we do it too.
	 */
	OUTB(1, io_base_addr + 0x10);

	rom_base_addr = pcidev_readbar(dev, PCI_BASE_ADDRESS_5);
	if (!rom_base_addr) {
		msg_pdbg("Failed to read BAR5\n");
		return 1;
	}

	if (atapromise_allow32k()) {
			if (dev->rom_size < (32 * 1024)) {
				msg_perr("ROM size is reported as %zu kB. Cannot flash 32 kB "
						"files.\n", dev->rom_size);
				return 1;
			}
			rom_size = 32 * 1024;
	} else {
		/* Default to 16 kB, so we can flash unpadded images */
		rom_size = 16 * 1024;
	}

	atapromise_bar = (uint8_t*)rphysmap("Promise", rom_base_addr, rom_size);
	if (atapromise_bar == ERROR_PTR) {
		return 1;
	}

	max_rom_decode.parallel = rom_size;
	register_par_master(&par_master_atapromise, BUS_PARALLEL);

	return 0;
}

static void atapromise_chip_writeb(const struct flashctx *flash, uint8_t val,
				chipaddr addr)
{
	uint32_t data;

	atapromise_fixup_chip(flash->chip);
	data = (rom_base_addr + (addr & ADDR_MASK)) << 8 | val;
	OUTL(data, io_base_addr + 0x14);
}

static uint8_t atapromise_chip_readb(const struct flashctx *flash,
				  const chipaddr addr)
{
	atapromise_fixup_chip(flash->chip);
	return pci_mmio_readb(atapromise_bar + (addr & ADDR_MASK));
}

#else
#error PCI port I/O access is not supported on this architecture yet.
#endif
