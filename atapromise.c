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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdlib.h>
#include "flash.h"
#include "programmer.h"
#include "hwaccess.h"

#define PCI_VENDOR_ID_PROMISE	0x105a

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
//static uint32_t rom_addr = 0;

static uint32_t bios_rom_addr_data = 0;

static void *phys = NULL;
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

static int atapromise_shutdown(void *data)
{
	if (phys) {
		physunmap(phys, maplen);
		phys = NULL;
		maplen = 0;
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

	/* this is also borrowed from ptiflash */
	rpci_write_long(dev, PCI_ROM_ADDRESS, 0x0003c000);
	uint32_t romaddr = pci_read_long(dev, PCI_ROM_ADDRESS);
	if (romaddr & 0xc000) {
		maplen = 16;
	} else {
		maplen = romaddr & 0x10000 ? 128 : 64;
	}

	msg_pdbg("ROM size is %zu kB (reg=0x%08" PRIx32 ")\n", maplen, romaddr);

	maplen *= 1024;
	phys = physmap_ro("Promise BIOS", rom_base_addr, maplen);
	if (phys == ERROR_PTR) {
		return 1;
	}

	if (register_shutdown(atapromise_shutdown, NULL))
		return 1;

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

	register_par_master(&par_master_atapromise, BUS_PARALLEL);

	return 0;
}


static void atapromise_chip_writeb(const struct flashctx *flash, uint8_t val,
				chipaddr addr)
{
	uint32_t base = 0;
	uint32_t offset = 0;

#if 0
	static unsigned int program_cmd_idx = 0;

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
		offset = 0;
		base = rom_base_addr;
		/* fall through */
	default:
		program_cmd_idx = 0;
	}
#endif

	uint32_t data = ((offset & 0xffff) + base + addr) << 8 | val;

	OUTL(data, io_base_addr + bios_rom_addr_data);
}

static uint8_t atapromise_chip_readb(const struct flashctx *flash,
				  const chipaddr addr)
{
	return ((volatile uint8_t*)phys)[addr];
}

#else
#error PCI port I/O access is not supported on this architecture yet.
#endif
