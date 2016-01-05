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

static uint32_t io_base_addr = 0;
static uint32_t rom_base_addr = 0;

static uint32_t bios_rom_addr = 0;
static uint32_t bios_rom_data = 0;

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
		.chip_writeb		= atapromise_chip_writeb,
		.chip_writew		= fallback_chip_writew,
		.chip_writel		= fallback_chip_writel,
		.chip_writen		= fallback_chip_writen,
};

static int atapromise_shutdown(void *data)
{
	printf("\n%s\n", __func__);

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
	if (!io_base_addr)
		return 1;

	rom_base_addr = pcidev_readbar(dev, PCI_BASE_ADDRESS_5);
	if (!rom_base_addr)
		return 1;

	if (register_shutdown(atapromise_shutdown, NULL))
		return 1;

	maplen = dev->rom_size;
	phys = physmap_ro("ROM", rom_base_addr, maplen);
	if (phys == ERROR_PTR) {
		return 1;
	}

	switch (dev->device_id) {
	case 0x4d30:
	case 0x4d38:
	case 0x0d30:
		bios_rom_addr = 0x14;
		bios_rom_data = 0;
		break;
	default:
		msg_perr("Unsupported device %04x\n", dev->device_id);
		return 1;
	}

	OUTB(1, io_base_addr + 0x10);

	register_par_master(&par_master_atapromise, BUS_PARALLEL);

	return 0;
}

static chipaddr last_write_addr = 0;

static void atapromise_chip_writeb(const struct flashctx *flash, uint8_t val,
				chipaddr addr)
{
	uint32_t data = 0;

	if (true || addr - last_write_addr != 1) {
		data = (addr << 8) | val;
	} else {
		/* ----------------------------
		 *             EAX
		 * ----------------------------
		 *              |      AX
		 * ----------------------------
		 *              |  AH   |  AL
		 * ----------------------------
		 *
		 * EAX := ?constant?
		 * AX := addr
		 * EAX <<= 8
		 * AL := data
		 */

		union {
			struct {
				uint8_t al, ah, _eax[2];
			} b;
			struct {
				uint16_t ax, _eax;
			} w;
			struct {
				uint32_t eax;
			} l;
		} xax;

		xax.l.eax = 0;

		xax.w.ax = 16384 * 1;
		xax.l.eax &= 0x0000ffff;
		xax.l.eax += addr;
		xax.l.eax <<= 8;
		xax.b.al = val;

		data = xax.l.eax;
	}


	//if (iaddr == 0x555 || iaddr == 0x2aa)
	//	printf("writeb: %04x := %02x (out=%08x)\n", (unsigned)addr, val, data);

	OUTL(data, io_base_addr + bios_rom_addr);
	last_write_addr = addr;
}

static uint8_t atapromise_chip_readb(const struct flashctx *flash,
				  const chipaddr addr)
{
	return ((uint8_t*)phys)[addr];
}

#else
#error PCI port I/O access is not supported on this architecture yet.
#endif
