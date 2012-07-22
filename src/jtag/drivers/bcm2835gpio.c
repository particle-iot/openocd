/***************************************************************************
 *   Copyright (C) 2013 by Paul Fertser, fercerpav@gmail.com               *
 *                                                                         *
 *   Copyright (C) 2012 by Creative Product Design, marc @ cpdesign.com.au *
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

#define BCM2835_PERI_BASE	0x20000000
#define BCM2835_GPIO_BASE	(BCM2835_PERI_BASE + 0x200000) /* GPIO controller */

/* GPIO setup macros */
#define INP_GPIO(g) do { *(pio_base+((g)/10)) &= ~(7<<(((g)%10)*3)); } while (0)
#define OUT_GPIO(g) do { /* clear the mode bits first, then set as output */ \
			INP_GPIO(g); \
			*(pio_base+((g)/10)) |=  (1<<(((g)%10)*3)); } while (0)

#define GPIO_SET (*(pio_base+7))  /* sets   bits which are 1, ignores bits which are 0 */
#define GPIO_CLR (*(pio_base+10)) /* clears bits which are 1, ignores bits which are 0 */
#define GPIO_LEV (*(pio_base+13)) /* current level of the pin */

static int dev_mem_fd;
static volatile uint32_t *pio_base;

static int bcm2835gpio_read(void);
static void bcm2835gpio_write(int tck, int tms, int tdi);
static void bcm2835gpio_reset(int trst, int srst);

static int bcm2835gpio_init(void);
static int bcm2835gpio_quit(void);

static struct bitbang_interface bcm2835gpio_bitbang = {
	.read = bcm2835gpio_read,
	.write = bcm2835gpio_write,
	.reset = bcm2835gpio_reset,
	.blink = NULL
};

/* GPIO numbers for each signal. Negative values are invalid */
static int tck_gpio = -1;
static int tms_gpio = -1;
static int tdi_gpio = -1;
static int tdo_gpio = -1;
static int trst_gpio = -1;
static int srst_gpio = -1;

static int bcm2835gpio_read(void)
{
	return !!(GPIO_LEV & 1<<tdo_gpio);
}

static void bcm2835gpio_write(int tck, int tms, int tdi)
{
	uint32_t set = tck<<tck_gpio | tms<<tms_gpio | tdi<<tdi_gpio;
	uint32_t clear = !tck<<tck_gpio | !tms<<tms_gpio | !tdi<<tdi_gpio;

	GPIO_SET = set;
	GPIO_CLR = clear;
}

/* (1) assert or (0) deassert reset lines */
static void bcm2835gpio_reset(int trst, int srst)
{
	uint32_t set = 0;
	uint32_t clear = 0;

	if (trst_gpio > 0) {
		set |= !trst<<trst_gpio;
		clear |= trst<<trst_gpio;
	}

	if (srst_gpio > 0) {
		set |= !srst<<srst_gpio;
		clear |= srst<<srst_gpio;
	}

	GPIO_SET = set;
	GPIO_CLR = clear;
}

COMMAND_HANDLER(bcm2835gpio_handle_jtag_gpionums)
{
	if (CMD_ARGC == 4) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], tck_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], tms_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[2], tdi_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[3], tdo_gpio);
	} else if (CMD_ARGC != 0) {
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	command_print(CMD_CTX,
			"BCM2835 GPIO config: tck = %d, tms = %d, tdi = %d, tdi = %d",
			tck_gpio, tms_gpio, tdi_gpio, tdo_gpio);

	return ERROR_OK;
}

COMMAND_HANDLER(bcm2835gpio_handle_jtag_gpionum_tck)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], tck_gpio);

	command_print(CMD_CTX, "BCM2835 GPIO config: tck = %d", tck_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(bcm2835gpio_handle_jtag_gpionum_tms)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], tms_gpio);

	command_print(CMD_CTX, "BCM2835 GPIO config: tms = %d", tms_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(bcm2835gpio_handle_jtag_gpionum_tdo)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], tdo_gpio);

	command_print(CMD_CTX, "BCM2835 GPIO config: tdo = %d", tdo_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(bcm2835gpio_handle_jtag_gpionum_tdi)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], tdi_gpio);

	command_print(CMD_CTX, "BCM2835 GPIO config: tdi = %d", tdi_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(bcm2835gpio_handle_jtag_gpionum_srst)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], srst_gpio);

	command_print(CMD_CTX, "BCM2835 GPIO config: srst = %d", srst_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(bcm2835gpio_handle_jtag_gpionum_trst)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], trst_gpio);

	command_print(CMD_CTX, "BCM2835 GPIO config: trst = %d", trst_gpio);
	return ERROR_OK;
}

static const struct command_registration bcm2835gpio_command_handlers[] = {
	{
		.name = "bcm2835gpio_jtag_nums",
		.handler = &bcm2835gpio_handle_jtag_gpionums,
		.mode = COMMAND_CONFIG,
		.help = "gpio numbers for tck, tms, tdi, tdo. (in that order)",
		.usage = "(tck tms tdi tdo)* ",
	},
	{
		.name = "bcm2835gpio_tck_num",
		.handler = &bcm2835gpio_handle_jtag_gpionum_tck,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tck.",
	},
	{
		.name = "bcm2835gpio_tms_num",
		.handler = &bcm2835gpio_handle_jtag_gpionum_tms,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tms.",
	},
	{
		.name = "bcm2835gpio_tdo_num",
		.handler = &bcm2835gpio_handle_jtag_gpionum_tdo,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tdo.",
	},
	{
		.name = "bcm2835gpio_tdi_num",
		.handler = &bcm2835gpio_handle_jtag_gpionum_tdi,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tdi.",
	},
	{
		.name = "bcm2835gpio_srst_num",
		.handler = &bcm2835gpio_handle_jtag_gpionum_srst,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for srst.",
	},
	{
		.name = "bcm2835gpio_trst_num",
		.handler = &bcm2835gpio_handle_jtag_gpionum_trst,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for trst.",
	},
	COMMAND_REGISTRATION_DONE
};

struct jtag_interface bcm2835gpio_interface = {
	.name = "bcm2835gpio",
	.supported = DEBUG_CAP_TMS_SEQ,
	.execute_queue = bitbang_execute_queue,
	.transports = jtag_only,
	.commands = bcm2835gpio_command_handlers,
	.init = bcm2835gpio_init,
	.quit = bcm2835gpio_quit,
};

static int bcm2835gpio_init(void)
{
	bitbang_interface = &bcm2835gpio_bitbang;

	dev_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (dev_mem_fd < 0) {
		perror("open");
		return ERROR_JTAG_INIT_FAILED;
	}

	pio_base = mmap(NULL, sysconf(_SC_PAGE_SIZE), PROT_READ | PROT_WRITE,
				MAP_SHARED, dev_mem_fd, BCM2835_GPIO_BASE);

	if (pio_base == MAP_FAILED) {
		perror("mmap");
		close(dev_mem_fd);
		return ERROR_JTAG_INIT_FAILED;
	}

	/*
	 * Configure TDO as an input, and TDI, TCK, TMS, TRST, SRST
	 * as outputs.  Drive TDI and TCK low, and TMS/TRST/SRST high.
	 */
	INP_GPIO(tdo_gpio);

	GPIO_CLR = 1<<tdi_gpio | 1<<tck_gpio;
	GPIO_SET = 1<<tms_gpio | 1<<trst_gpio | 1<<srst_gpio;

	OUT_GPIO(tdi_gpio);
	OUT_GPIO(tck_gpio);
	OUT_GPIO(tms_gpio);
	OUT_GPIO(trst_gpio);
	OUT_GPIO(srst_gpio);

	return ERROR_OK;
}

static int bcm2835gpio_quit(void)
{
	close(dev_mem_fd);
	return ERROR_OK;
}
