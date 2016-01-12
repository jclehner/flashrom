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

#define PCI_VENDOR_ID_PROMISE	0x105a
#define MAX_ROM_DECODE (32 * 1024)
#define ADDR_MASK (MAX_ROM_DECODE - 1)

/**
 * This programmer was created by reverse-engineering Promise's DOS-only
 * PTIFLASH utility (v1.45.0.1), as no public documentation on any of 
 * these chips exists.
 *
 * The only device tested is a PDC20267 controller, but the logic for
 * programming the other 2026x controllers is the same, so it should,
 * in theory, work for those as well.
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
	return NULL;
}

static int atapromise_find_bridge(struct pci_dev *dev, struct pci_dev **br)
{
	struct pci_filter flt;
	int err;
	char *br_bdf, *msg;
	uint8_t bus_sec, bus_sub;

	err = 1;
	msg = NULL;
	*br = NULL;

	br_bdf = extract_programmer_param("bridge");
	if (br_bdf) {
		if (!strcmp(br_bdf, "none")) {
			err = 0;
			goto out;
		} else if (!strcmp(br_bdf, "auto")) {
			free(br_bdf);
			br_bdf = NULL;
		} else {
			pci_filter_init(dev->access, &flt);
			msg = pci_filter_parse_slot(&flt, br_bdf);
			if (msg) {
				msg_perr("Invalid bridge device specified: %s.\n", msg);
				goto out;
			}
		}
	}

	for (*br = dev->access->devices; *br; *br = (*br)->next) {
		/* Don't use (*br)->hdrtype here! */
		uint8_t htype = pci_read_byte(*br, PCI_HEADER_TYPE) & 0x7f;
		if (br_bdf) {
			if (!pci_filter_match(&flt, *br))
				continue;
		} else {
			if (htype != PCI_HEADER_TYPE_BRIDGE)
				continue;
		}

		if (htype != PCI_HEADER_TYPE_BRIDGE) {
			msg_perr("Error: Specified device is not a bridge.\n");
			goto out;
		}

		bus_sec = pci_read_byte(*br, PCI_SECONDARY_BUS);
		bus_sub = pci_read_byte(*br, PCI_SUBORDINATE_BUS);

		if (dev->bus >= bus_sec && dev->bus <= bus_sub) {
			msg_pdbg("Found bridge %04x:%04x, BDF %02x:%02x.%x.\n",
					(*br)->vendor_id, (*br)->device_id, 
					(*br)->bus, (*br)->dev, (*br)->func);
			err = 0;
			goto out;
		} else if (br_bdf) {
			msg_perr("Error: Device %02x:%02x.%x is not attached to "
					"specified bridge\n", dev->bus, dev->dev, dev->func);
			goto out;
		}
	}

	if (br_bdf) {
		msg_perr("Specified bridge device not found.\n");
		err = 1;
	} else {
		msg_pdbg2("Device does not appear to be behind a bridge.\n");
		err = 0;
		*br = NULL;
	}

out:
	if (err) {
		*br = NULL;
	}
	free(br_bdf);
	return err;
}

static int atapromise_bridge_fixup(struct pci_dev *dev)
{
	struct pci_dev *br;
	uint16_t reg16;

	if (atapromise_find_bridge(dev, &br))
		return 1;

	if (br) {
		reg16 = pci_read_word(dev, PCI_BASE_ADDRESS_5 + 2) & 0xfff0;
		if (reg16 < pci_read_word(br, PCI_MEMORY_BASE)) {
			msg_pdbg2("Adjusting memory base of bridge.\n");
			rpci_write_word(br, PCI_MEMORY_BASE, reg16);
		}

		reg16 += 0x20;

		if (reg16 < pci_read_word(br, PCI_MEMORY_LIMIT)) {
			msg_pdbg2("Adjusting memory limit of bridge.\n");
			rpci_write_word(br, PCI_MEMORY_LIMIT, reg16);
		}
	}

	return 0;
}

static void atapromise_chip_fixup(struct flashchip *chip)
{
	static bool called = false;
	unsigned int i, size;

	if (called)
		return;
	
	size = chip->total_size * 1024;
	if (size > rom_size)
	{
		/* Remove all block_erasers that operate on sectors, and adjust
		 * the eraseblock size of the block_eraser that erases the whole
		 * chip.
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

	called = true;
}

int atapromise_init(void)
{
	struct pci_dev *dev = NULL;

	if (rget_io_perms())
		return 1;

	dev = pcidev_init(ata_promise, PCI_BASE_ADDRESS_4);
	if (!dev)
		return 1;

	if (atapromise_bridge_fixup(dev))
		return 1;

	io_base_addr = pcidev_readbar(dev, PCI_BASE_ADDRESS_4) & 0xfffe;
	if (!io_base_addr) {
		return 1;
	}

	/* Not exactly sure what this does, because flashing seems to work without
	 * it. However, PTIFLASH does it, so we do it too.
	 */
	OUTB(1, io_base_addr + 0x10);

	rom_base_addr = pcidev_readbar(dev, PCI_BASE_ADDRESS_5);
	if (!rom_base_addr) {
		msg_pdbg("Failed to read BAR5\n");
		return 1;
	}

	rom_size = dev->rom_size;

	msg_pdbg("ROM size reported as %zu kB.\n", rom_size / 1024);
	if (rom_size > MAX_ROM_DECODE) {
		rom_size = MAX_ROM_DECODE;
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

	atapromise_chip_fixup(flash->chip);
	data = (rom_base_addr + (addr & ADDR_MASK)) << 8 | val;
	OUTL(data, io_base_addr + 0x14);
}

static uint8_t atapromise_chip_readb(const struct flashctx *flash,
				  const chipaddr addr)
{
	atapromise_chip_fixup(flash->chip);
	return pci_mmio_readb(atapromise_bar + (addr & ADDR_MASK));
}

#else
#error PCI port I/O access is not supported on this architecture yet.
#endif
