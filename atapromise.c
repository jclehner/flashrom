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

static int memfd = -1;

const struct dev_entry ata_promise[] = {
	/* Promise PDC20267 (FastTrak100/Ultra100) */
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

	if (memfd >= 0)
		close(memfd);

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

	io_base_addr = pcidev_readbar(dev, PCI_BASE_ADDRESS_4);
	if (!io_base_addr)
		return 1;

	rom_base_addr = pcidev_readbar(dev, PCI_BASE_ADDRESS_5);
	if (!rom_base_addr)
		return 1;

	if (register_shutdown(atapromise_shutdown, NULL))
		return 1;

	memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd < 0) {
		perror("open");
		return 1;
	}

	io_base_addr &= 0xfffe;

	switch (dev->device_id) {
	case 0x4d30:
	case 0x4d38:
	case 0x0d30:
	default:
		bios_rom_addr = 0x14;
		bios_rom_data = 0;
		break;
	}

	OUTB(1, io_base_addr + 0x10);

	register_par_master(&par_master_atapromise, BUS_PARALLEL);

	return 0;
}

int silicon_id_cmd_byte = 0;

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
	//printf("readb: %04x => ", (unsigned)addr);

	uint8_t val;

	if (silicon_id_cmd_byte != 3 || true) {
#if 1
		off_t offset = rom_base_addr + addr;
		if (lseek(memfd, offset, SEEK_SET) == (off_t)-1) {
			perror("lseek");
			val = 0xff;
		} else {
			if (read(memfd, &val, 1) != 1) {
				perror("read");
				val = 0xff;
			}
		}
		//val = mmio_readb(mem + addr);
#else
		val = INB(rom_base_addr + (uint32_t)addr);
#endif
	} else {
		//printf("(fake) ");
		val = addr ? 0x18 : 0xc2;
	}

	//printf("%02x\n", val);

	return val;
}

#else
#error PCI port I/O access is not supported on this architecture yet.
#endif
