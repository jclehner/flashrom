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
#define PROMISE_ADDR_MASK 0x1ffff

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

static uint32_t bios_rom_addr_data = 0;

static uint8_t *atapromise_bar = NULL;
static size_t maplen = 0;

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
	msg_pdbg("\natapromise_map(\"%s\", %08zx, %zu", descr, phys_addr, len);
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

		if ((reg16 + 0x10) < pci_read_word(br, PCI_MEMORY_LIMIT)) {
			msg_pdbg2("Adjusting memory limit of bridge.\n");
			rpci_write_word(br, PCI_MEMORY_LIMIT, reg16 + 0x10);
		}
	}

	return 0;
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

	uint32_t saved = pci_read_long(dev, 0x30);

	rpci_write_word(dev, 0x30, 0xc000);
	rpci_write_word(dev, 0x32, 3);
	uint16_t reg16 = pci_read_word(dev, 0x30);
	if (reg16 & 0xc000) {
		maplen = 16;
	} else {
		reg16 = pci_read_word(dev, 0x32);
		maplen = reg16 & 1 ? 64 : 128;
	}

	msg_pdbg("ROM size is %zu kB (reg=0x%08" PRIx32 ")\n", maplen, 
			pci_read_long(dev, 0x30));
	pci_write_long(dev, 0x30, saved);

	io_base_addr = pcidev_readbar(dev, PCI_BASE_ADDRESS_4) & 0xfffe;
	if (!io_base_addr) {
		msg_pdbg("Failed to read BAR4");
		return 1;
	}

	/* not exactly sure what this does, but ptiflash does it too */
	OUTB(1, io_base_addr + 0x10);

	rom_base_addr = pcidev_readbar(dev, PCI_BASE_ADDRESS_5);
	if (!rom_base_addr) {
		msg_pdbg("Failed to read BAR5\n");
		return 1;
	}

	maplen = 128;
	maplen *= 1024;

	atapromise_bar = (uint8_t*)rphysmap("Promise BIOS", rom_base_addr, maplen);
	if (atapromise_bar == ERROR_PTR) {
		return 1;
	}

	switch (dev->device_id) {
	case 0x4d30:
	case 0x4d38:
	case 0x0d30:
		bios_rom_addr_data = 0x14;
		break;
	default:
		msg_perr("Unsupported device %04x\n", dev->device_id);
		return 1;
	}

	//max_rom_decode.parallel = maplen;
	register_par_master(&par_master_atapromise, BUS_PARALLEL);

	return 0;
}

static void atapromise_chip_fixup(const struct flashctx *flash)
{
	static bool fixed = false;
	if (fixed || flash->chip->total_size == maplen)
		return;
}

static void atapromise_chip_writeb(const struct flashctx *flash, uint8_t val,
				chipaddr addr)
{
	atapromise_chip_fixup(flash);

#if 0
	flash->chip->total_size = maplen / 1024;
#endif

#if 0
	uint32_t data = addr / 0x4000;
	data &= 0xffff;
	data += rom_base_addr;
	data += addr;
	
	data <<= 8;
	data |= val;
#endif

#if 0
	// this works for the first 0x1000 bytes only
	uint32_t data = addr << 8 | val;
	data |= (rom_base_addr + (uint32_t)addr / 0x1000) << 8;
	//data |= rom_base_addr /*+ (((uint32_t)addr / 0x1000) << 4)*/ << 8;
#else
	uint32_t data = (rom_base_addr + (addr & PROMISE_ADDR_MASK)) << 8 | val;
#endif

#if 1
	static unsigned int program_cmd_idx = 0;
	bool is_data = false;

	switch (program_cmd_idx) {
	case 0:
		program_cmd_idx += (addr == 0x555 && val == 0xaa) ? 1 : 0;
		break;
	case 1:
		program_cmd_idx += (addr == 0x2aa && val == 0x55) ? 1 : 0;
		break;
	case 2:
		program_cmd_idx += (addr == 0x555 && val == 0xa0) ? 1 : 0;
		break;
	case 3:
		is_data = true;
		/* fall through */
	default:
		program_cmd_idx = 0;
	}

#endif

	OUTL(data, io_base_addr + bios_rom_addr_data);

#if 1
	if (is_data /*&& addr && !(addr & 0xff)*/) {

		unsigned int i = 30;

		while (--i) {
			if (atapromise_chip_readb(flash, addr) == val)
				break;
		}


		msg_pdbg("data: %05x := %02x (%02x) (%08x) %u %s\n", (unsigned)addr & 0xfffff,
				val & 0xff, atapromise_chip_readb(flash, addr) & 0xff,
				(unsigned)data, i, i ? "" : "(error)");
	}
#if 0
	
	unsigned int i = 5000;
	while (--i) {
		OUTB(val, 0xEB);
	}
#endif
#endif
}

static uint8_t atapromise_chip_readb(const struct flashctx *flash,
				  const chipaddr addr)
{
	return pci_mmio_readb(atapromise_bar + (addr & PROMISE_ADDR_MASK));
}

#else
#error PCI port I/O access is not supported on this architecture yet.
#endif
