/***************************************************************************
 *   Copyright (C) 2012 by Paul Fertser                                    *
 *   fercerpav@gmail.com                                                   *
 *                                                                         *
 *   Based on at91rm9200.c (c) Anders Larsen                               *
 *   and RPi GPIO examples by Gert van Loo & Dom                           *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/interface.h>
#include "bitbang.h"

#include <sys/mman.h>

#define TDI_PIO		10	/* pin 19 */
#define TDO_PIO		9	/* pin 21 */
#define TCK_PIO		11	/* pin 23 */
#define TMS_PIO		25	/* pin 22 */
#define TRST_PIO	7	/* pin 26 */
#define SRST_PIO	24	/* pin 18 */

#define BCM2708_PERI_BASE	0x20000000
#define BCM2708_GPIO_BASE	(BCM2708_PERI_BASE + 0x200000) /* GPIO controller */

/* GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y) */
#define INP_GPIO(g) do { *(pio_base+((g)/10)) &= ~(7<<(((g)%10)*3)); } while (0)
#define OUT_GPIO(g) do { *(pio_base+((g)/10)) |=  (1<<(((g)%10)*3)); } while (0)

#define GPIO_SET (*(pio_base+7))  /* sets   bits which are 1, ignores bits which are 0 */
#define GPIO_CLR (*(pio_base+10)) /* clears bits which are 1, ignores bits which are 0 */
#define GPIO_LEV (*(pio_base+13)) /* current level of the pin */

static int dev_mem_fd;
static volatile uint32_t *pio_base;

static int raspberrypi_read(void);
static void raspberrypi_write(int tck, int tms, int tdi);
static void raspberrypi_reset(int trst, int srst);

static int raspberrypi_speed(int speed);
static int raspberrypi_init(void);
static int raspberrypi_quit(void);

static struct bitbang_interface raspberrypi_bitbang = {
	.read = raspberrypi_read,
	.write = raspberrypi_write,
	.reset = raspberrypi_reset,
	.blink = NULL
};

static int raspberrypi_read(void)
{
	return !!(GPIO_LEV & 1<<TDO_PIO);
}

static void raspberrypi_write(int tck, int tms, int tdi)
{
	if (tck)
		GPIO_SET = 1<<TCK_PIO;
	else
		GPIO_CLR = 1<<TCK_PIO;

	if (tms)
		GPIO_SET = 1<<TMS_PIO;
	else
		GPIO_CLR = 1<<TMS_PIO;

	if (tdi)
		GPIO_SET = 1<<TDI_PIO;
	else
		GPIO_CLR = 1<<TDI_PIO;
}

/* (1) assert or (0) deassert reset lines */
static void raspberrypi_reset(int trst, int srst)
{
	if (trst)
		GPIO_CLR = 1<<TRST_PIO;
	else
		GPIO_SET = 1<<TRST_PIO;

	if (srst)
		GPIO_CLR = 1<<SRST_PIO;
	else
		GPIO_SET = 1<<SRST_PIO;
}

static int raspberrypi_speed(int speed)
{
	return ERROR_OK;
}

struct jtag_interface raspberrypi_interface = {
	.name = "raspberrypi",
	.execute_queue = bitbang_execute_queue,
	.speed = raspberrypi_speed,
	.transports = jtag_only,
	.init = raspberrypi_init,
	.quit = raspberrypi_quit,
};

static int raspberrypi_init(void)
{
	bitbang_interface = &raspberrypi_bitbang;

	dev_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (dev_mem_fd < 0) {
		perror("open");
		return ERROR_JTAG_INIT_FAILED;
	}

	pio_base = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				MAP_SHARED, dev_mem_fd, BCM2708_GPIO_BASE);

	if (pio_base == MAP_FAILED) {
		perror("mmap");
		close(dev_mem_fd);
		return ERROR_JTAG_INIT_FAILED;
	}

	/*
	 * Configure TDO as an input, and TDI, TCK, TMS, TRST, SRST
	 * as outputs.  Drive TDI and TCK low, and TMS/TRST/SRST high.
	 */
	INP_GPIO(TDO_PIO);

	GPIO_CLR = 1<<TDI_PIO | 1<<TCK_PIO;
	GPIO_SET = 1<<TMS_PIO | 1<<TRST_PIO | 1<<SRST_PIO;
	INP_GPIO(TDI_PIO);
	OUT_GPIO(TDI_PIO);
	INP_GPIO(TCK_PIO);
	OUT_GPIO(TCK_PIO);
	INP_GPIO(TMS_PIO);
	OUT_GPIO(TMS_PIO);
	INP_GPIO(TRST_PIO);
	OUT_GPIO(TRST_PIO);
	INP_GPIO(SRST_PIO);
	OUT_GPIO(SRST_PIO);

	return ERROR_OK;
}

static int raspberrypi_quit(void)
{
	return ERROR_OK;
}
