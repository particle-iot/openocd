/***************************************************************************
 *   Copyright (C) 2017 Icenowy Zheng <icenowy@aosc.io>                    *
 *                                                                         *
 *   Based on bcm2835gpio.c (c) Creative Product Design and Paul Fertser   *
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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/interface.h>
#include "bitbang.h"

#include <unistd.h>
#include <sys/mman.h>

static int sunxigpio_read(void);
static void sunxigpio_write(int tck, int tms, int tdi);
static void sunxigpio_reset(int trst, int srst);

static int sunxi_swdio_read(void);
static void sunxi_swdio_drive(bool is_output);

static int sunxigpio_init(void);
static int sunxigpio_quit(void);

static struct bitbang_interface sunxigpio_bitbang = {
	.read = sunxigpio_read,
	.write = sunxigpio_write,
	.reset = sunxigpio_reset,
	.swdio_read = sunxi_swdio_read,
	.swdio_drive = sunxi_swdio_drive,
	.blink = NULL
};

static uint32_t pio_base = 0x01c20800;

struct sunxi_gpio {
	int num;
	uint32_t function_offset;
	uint32_t value_offset;
	uint8_t function_bit;
	uint8_t value_bit;
};

#define BIT(x)			(1 << (x))
#define GPIO_DIRECTION_INPUT	0
#define GPIO_DIRECTION_OUTPUT	1

/* GPIO structs for each signal. bit32 is valid (MSB is bit31) */
static struct sunxi_gpio tck_gpio = { -1, 0, 0, 32, 32 };
static struct sunxi_gpio tms_gpio = { -1, 0, 0, 32, 32 };
static struct sunxi_gpio tdi_gpio = { -1, 0, 0, 32, 32 };
static struct sunxi_gpio tdo_gpio = { -1, 0, 0, 32, 32 };
static struct sunxi_gpio trst_gpio = { -1, 0, 0, 32, 32 };
static struct sunxi_gpio srst_gpio = { -1, 0, 0, 32, 32 };
static struct sunxi_gpio swclk_gpio = { -1, 0, 0, 32, 32 };
static struct sunxi_gpio swdio_gpio = { -1, 0, 0, 32, 32 };

static uint32_t (*gpio_readl)(uint32_t addr);
static void (*gpio_writel)(uint32_t val, uint32_t addr);

static bool sunxi_is_gpio_num_valid(int gpio_num)
{
	return gpio_num >= 0 && gpio_num <= 287; /* from PA0 to PI31 */
}

static void sunxi_num_to_gpio(int gpio_num, struct sunxi_gpio *gpio)
{
	int bank = gpio_num / 32, pin = gpio_num % 32;

	gpio->num = gpio_num;
	gpio->value_offset = 0x24 * bank + 0x10;
	gpio->value_bit = pin;
	gpio->function_offset = 0x24 * bank;
	while (pin >= 8) {
		gpio->function_offset += 4;
		pin -= 8;
	}
	gpio->function_bit = pin * 4;
}

#define SUNXI_GPIO_PIN_NAME_LEN 5 /* PXxx\0 */
static void sunxi_gpio_num_get_name(int gpio_num, char *name)
{
	int bank = gpio_num / 32, pin = gpio_num % 32;
	name[0] = 'P';
	name[1] = 'A' + bank;
	name[2] = '0' + pin / 10;
	name[3] = '0' + pin % 10;
	name[4] = '\0';
}

static void sunxi_gpio_get_name(struct sunxi_gpio *gpio, char *name)
{
	sunxi_gpio_num_get_name(gpio->num, name);
}

static bool sunxi_gpio_is_valid(struct sunxi_gpio *gpio)
{
	return !(gpio->value_bit == 32 || gpio->function_bit == 32);
}

static int sunxi_read_gpio(struct sunxi_gpio *gpio)
{
	return !!(gpio_readl(gpio->value_offset) & BIT(gpio->value_bit));
}

static void sunxi_write_gpio(struct sunxi_gpio *gpio, int val)
{
	uint32_t reg = gpio_readl(gpio->value_offset);

	if (val)
		reg |= BIT(gpio->value_bit);
	else
		reg &= ~BIT(gpio->value_bit);

	gpio_writel(reg, gpio->value_offset);
}

static void sunxi_gpio_direction(struct sunxi_gpio *gpio, int dir)
{
	if (dir != GPIO_DIRECTION_INPUT && dir != GPIO_DIRECTION_OUTPUT)
		return;

	uint32_t val = gpio_readl(gpio->function_offset);
	val &= ~(0xf << gpio->function_bit);
	val |= (dir << gpio->function_bit);
	gpio_writel(val, gpio->function_offset);
}

static int dev_mem_fd;
static volatile uint32_t *pio_mmap_base;

static uint32_t sunxi_gpio_readl(uint32_t addr)
{
	uint32_t val = *(pio_mmap_base + addr / sizeof(*pio_mmap_base));
	return val;
}

static void sunxi_gpio_writel(uint32_t val, uint32_t addr)
{
	*(pio_mmap_base + addr / sizeof(*pio_mmap_base)) = val;
}

static int sunxigpio_read(void)
{
	return sunxi_read_gpio(&tdo_gpio);
}

static void sunxigpio_write(int tck, int tms, int tdi)
{
	sunxi_write_gpio(&tck_gpio, tck);
	sunxi_write_gpio(&tms_gpio, tms);
	sunxi_write_gpio(&tdi_gpio, tdi);
}

static void sunxigpio_swd_write(int tck, int tms, int tdi)
{
	sunxi_write_gpio(&swdio_gpio, tdi);
	sunxi_write_gpio(&swclk_gpio, tck);
}

/* (1) assert or (0) deassert reset lines */
static void sunxigpio_reset(int trst, int srst)
{
	if (sunxi_gpio_is_valid(&trst_gpio))
		sunxi_write_gpio(&trst_gpio, trst);

	if (sunxi_gpio_is_valid(&srst_gpio))
		sunxi_write_gpio(&srst_gpio, srst);
}

static void sunxi_swdio_drive(bool is_output)
{
	if (is_output) {
		sunxi_gpio_direction(&swdio_gpio, GPIO_DIRECTION_OUTPUT);
		sunxi_write_gpio(&swdio_gpio, 1);
	} else {
		sunxi_gpio_direction(&swdio_gpio, GPIO_DIRECTION_INPUT);
	}
}

static int sunxi_swdio_read(void)
{
	return sunxi_read_gpio(&swdio_gpio);
}

COMMAND_HANDLER(sunxigpio_handle_jtag_gpionums)
{
	int num;
	char name[SUNXI_GPIO_PIN_NAME_LEN];
	if (CMD_ARGC == 4) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &tck_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &tms_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[2], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &tdi_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[3], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &tdo_gpio);
	} else if (CMD_ARGC != 0) {
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	command_print(CMD_CTX, "Allwinner GPIO config: ");
	sunxi_gpio_get_name(&tck_gpio, name);
	command_print(CMD_CTX, "tck = %s, ", name);
	sunxi_gpio_get_name(&tms_gpio, name);
	command_print(CMD_CTX, "tms = %s, ", name);
	sunxi_gpio_get_name(&tdi_gpio, name);
	command_print(CMD_CTX, "tdi = %s, ", name);
	sunxi_gpio_get_name(&tdo_gpio, name);
	command_print(CMD_CTX, "tdo = %s", name);

	return ERROR_OK;
}

COMMAND_HANDLER(sunxigpio_handle_jtag_gpionum_tck)
{
	int num;
	char name[SUNXI_GPIO_PIN_NAME_LEN];

	if (CMD_ARGC == 1) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &tck_gpio);
	}

	command_print(CMD_CTX, "Allwinner GPIO config: ");
	sunxi_gpio_get_name(&tck_gpio, name);
	command_print(CMD_CTX, "tck = %s", name);
	return ERROR_OK;
}

COMMAND_HANDLER(sunxigpio_handle_jtag_gpionum_tms)
{
	int num;
	char name[SUNXI_GPIO_PIN_NAME_LEN];

	if (CMD_ARGC == 1) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &tms_gpio);
	}

	command_print(CMD_CTX, "Allwinner GPIO config: ");
	sunxi_gpio_get_name(&tms_gpio, name);
	command_print(CMD_CTX, "tms = %s", name);
	return ERROR_OK;
}

COMMAND_HANDLER(sunxigpio_handle_jtag_gpionum_tdo)
{
	int num;
	char name[SUNXI_GPIO_PIN_NAME_LEN];

	if (CMD_ARGC == 1) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &tdo_gpio);
	}

	command_print(CMD_CTX, "Allwinner GPIO config: ");
	sunxi_gpio_get_name(&tdo_gpio, name);
	command_print(CMD_CTX, "tdo = %s", name);
	return ERROR_OK;
}

COMMAND_HANDLER(sunxigpio_handle_jtag_gpionum_tdi)
{
	int num;
	char name[SUNXI_GPIO_PIN_NAME_LEN];

	if (CMD_ARGC == 1) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &tdi_gpio);
	}

	command_print(CMD_CTX, "Allwinner GPIO config: ");
	sunxi_gpio_get_name(&tdi_gpio, name);
	command_print(CMD_CTX, "tdi = %s", name);
	return ERROR_OK;
}

COMMAND_HANDLER(sunxigpio_handle_jtag_gpionum_srst)
{
	int num;
	char name[SUNXI_GPIO_PIN_NAME_LEN];

	if (CMD_ARGC == 1) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &srst_gpio);
	}

	command_print(CMD_CTX, "Allwinner GPIO config: ");
	sunxi_gpio_get_name(&srst_gpio, name);
	command_print(CMD_CTX, "srst = %s", name);
	return ERROR_OK;
}

COMMAND_HANDLER(sunxigpio_handle_jtag_gpionum_trst)
{
	int num;
	char name[SUNXI_GPIO_PIN_NAME_LEN];

	if (CMD_ARGC == 1) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &trst_gpio);
	}

	command_print(CMD_CTX, "Allwinner GPIO config: ");
	sunxi_gpio_get_name(&trst_gpio, name);
	command_print(CMD_CTX, "trst = %s", name);
	return ERROR_OK;
}

COMMAND_HANDLER(sunxigpio_handle_swd_gpionums)
{
	int num;
	char name[SUNXI_GPIO_PIN_NAME_LEN];
	if (CMD_ARGC == 2) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &swclk_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &swdio_gpio);
	} else if (CMD_ARGC != 0) {
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	command_print(CMD_CTX, "Allwinner GPIO config: ");
	sunxi_gpio_get_name(&swclk_gpio, name);
	command_print(CMD_CTX, "swclk = %s, ", name);
	sunxi_gpio_get_name(&swdio_gpio, name);
	command_print(CMD_CTX, "swdio = %s", name);

	return ERROR_OK;
}

COMMAND_HANDLER(sunxigpio_handle_swd_gpionum_swclk)
{
	int num;
	char name[SUNXI_GPIO_PIN_NAME_LEN];

	if (CMD_ARGC == 1) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &swclk_gpio);
	}

	command_print(CMD_CTX, "Allwinner GPIO config: ");
	sunxi_gpio_get_name(&swclk_gpio, name);
	command_print(CMD_CTX, "swclk = %s", name);
	return ERROR_OK;
}

COMMAND_HANDLER(sunxigpio_handle_swd_gpionum_swdio)
{
	int num;
	char name[SUNXI_GPIO_PIN_NAME_LEN];

	if (CMD_ARGC == 1) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], num);
		if (sunxi_is_gpio_num_valid(num))
			sunxi_num_to_gpio(num, &swdio_gpio);
	}

	command_print(CMD_CTX, "Allwinner GPIO config: ");
	sunxi_gpio_get_name(&swdio_gpio, name);
	command_print(CMD_CTX, "swdio = %s", name);
	return ERROR_OK;
}

COMMAND_HANDLER(sunxigpio_handle_pio_base)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], pio_base);
	return ERROR_OK;
}

static const struct command_registration sunxigpio_command_handlers[] = {
	{
		.name = "sunxigpio_jtag_nums",
		.handler = &sunxigpio_handle_jtag_gpionums,
		.mode = COMMAND_CONFIG,
		.help = "gpio numbers for tck, tms, tdi, tdo. (in that order)",
		.usage = "(tck tms tdi tdo)* ",
	},
	{
		.name = "sunxigpio_tck_num",
		.handler = &sunxigpio_handle_jtag_gpionum_tck,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tck.",
	},
	{
		.name = "sunxigpio_tms_num",
		.handler = &sunxigpio_handle_jtag_gpionum_tms,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tms.",
	},
	{
		.name = "sunxigpio_tdo_num",
		.handler = &sunxigpio_handle_jtag_gpionum_tdo,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tdo.",
	},
	{
		.name = "sunxigpio_tdi_num",
		.handler = &sunxigpio_handle_jtag_gpionum_tdi,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tdi.",
	},
	{
		.name = "sunxigpio_swd_nums",
		.handler = &sunxigpio_handle_swd_gpionums,
		.mode = COMMAND_CONFIG,
		.help = "gpio numbers for swclk, swdio. (in that order)",
		.usage = "(swclk swdio)* ",
	},
	{
		.name = "sunxigpio_swclk_num",
		.handler = &sunxigpio_handle_swd_gpionum_swclk,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for swclk.",
	},
	{
		.name = "sunxigpio_swdio_num",
		.handler = &sunxigpio_handle_swd_gpionum_swdio,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for swdio.",
	},
	{
		.name = "sunxigpio_srst_num",
		.handler = &sunxigpio_handle_jtag_gpionum_srst,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for srst.",
	},
	{
		.name = "sunxigpio_trst_num",
		.handler = &sunxigpio_handle_jtag_gpionum_trst,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for trst.",
	},
	{
		.name = "sunxigpio_pio_base",
		.handler = &sunxigpio_handle_pio_base,
		.mode = COMMAND_CONFIG,
		.help = "Pin controller base (0x01c28000 except A80 and H6).",
	},

	COMMAND_REGISTRATION_DONE
};

static const char * const sunxi_transports[] = { "jtag", "swd", NULL };

struct jtag_interface sunxigpio_interface = {
	.name = "sunxigpio",
	.supported = DEBUG_CAP_TMS_SEQ,
	.execute_queue = bitbang_execute_queue,
	.transports = sunxi_transports,
	.swd = &bitbang_swd,
	.commands = sunxigpio_command_handlers,
	.init = sunxigpio_init,
	.quit = sunxigpio_quit,
};

static bool sunxigpio_jtag_mode_possible(void)
{
	if (!sunxi_gpio_is_valid(&tck_gpio))
		return 0;
	if (!sunxi_gpio_is_valid(&tms_gpio))
		return 0;
	if (!sunxi_gpio_is_valid(&tdi_gpio))
		return 0;
	if (!sunxi_gpio_is_valid(&tdo_gpio))
		return 0;
	return 1;
}

static bool sunxigpio_swd_mode_possible(void)
{
	if (!sunxi_gpio_is_valid(&swclk_gpio))
		return 0;
	if (!sunxi_gpio_is_valid(&swdio_gpio))
		return 0;
	return 1;
}

static int sunxigpio_init(void)
{
	bitbang_interface = &sunxigpio_bitbang;
	gpio_readl = sunxi_gpio_readl;
	gpio_writel = sunxi_gpio_writel;

	LOG_INFO("Allwinner GPIO JTAG/SWD bitbang driver via /dev/mem");

	if (sunxigpio_jtag_mode_possible()) {
		if (sunxigpio_swd_mode_possible())
			LOG_INFO("JTAG and SWD modes enabled");
		else
			LOG_INFO("JTAG only mode enabled (specify swclk and swdio gpio to add SWD mode)");
		if (!sunxi_gpio_is_valid(&trst_gpio) && !sunxi_gpio_is_valid(&srst_gpio)) {
			LOG_ERROR("Require at least one of trst or srst gpios to be specified");
			return ERROR_JTAG_INIT_FAILED;
		}
	} else if (sunxigpio_swd_mode_possible()) {
		LOG_INFO("SWD only mode enabled (specify tck, tms, tdi and tdo gpios to add JTAG mode)");
	} else {
		LOG_ERROR("Require tck, tms, tdi and tdo gpios for JTAG mode and/or swclk and swdio gpio for SWD mode");
		return ERROR_JTAG_INIT_FAILED;
	}

	dev_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (dev_mem_fd < 0) {
		perror("open");
		return ERROR_JTAG_INIT_FAILED;
	}

	size_t pagesize = getpagesize();
	uint32_t pio_page = pio_base & (~(pagesize-1));
	uint32_t pio_offset_inpage = pio_base - pio_page;

	uint32_t *pio_page_mmap_base;
	pio_page_mmap_base = mmap(NULL, sysconf(_SC_PAGE_SIZE),
				  PROT_READ | PROT_WRITE,
				  MAP_SHARED, dev_mem_fd, pio_page);

	if (pio_page_mmap_base == MAP_FAILED) {
		perror("mmap");
		close(dev_mem_fd);
		return ERROR_JTAG_INIT_FAILED;
	}

	pio_mmap_base = pio_page_mmap_base +
			pio_offset_inpage / sizeof(*pio_page_mmap_base);

	/*
	 * Configure TDO as an input, and TDI, TCK, TMS, TRST, SRST
	 * as outputs.  Drive TDI and TCK low, and TMS/TRST/SRST high.
	 */
	if (sunxigpio_jtag_mode_possible()) {
		sunxi_gpio_direction(&tdo_gpio, GPIO_DIRECTION_INPUT);
		sunxi_gpio_direction(&tdi_gpio, GPIO_DIRECTION_OUTPUT);
		sunxi_write_gpio(&tdi_gpio, 0);
		sunxi_gpio_direction(&tck_gpio, GPIO_DIRECTION_OUTPUT);
		sunxi_write_gpio(&tck_gpio, 0);
		sunxi_gpio_direction(&tms_gpio, GPIO_DIRECTION_OUTPUT);
		sunxi_write_gpio(&tms_gpio, 1);
	}

	if (sunxigpio_swd_mode_possible()) {
		sunxi_gpio_direction(&swclk_gpio, GPIO_DIRECTION_OUTPUT);
		sunxi_write_gpio(&swclk_gpio, 0);
		sunxi_gpio_direction(&swdio_gpio, GPIO_DIRECTION_OUTPUT);
		sunxi_write_gpio(&swdio_gpio, 0);
	}

	if (sunxi_gpio_is_valid(&trst_gpio)) {
		sunxi_gpio_direction(&trst_gpio, GPIO_DIRECTION_OUTPUT);
		sunxi_write_gpio(&trst_gpio, 1);
	}

	if (sunxi_gpio_is_valid(&srst_gpio)) {
		sunxi_gpio_direction(&srst_gpio, GPIO_DIRECTION_OUTPUT);
		sunxi_write_gpio(&srst_gpio, 1);
	}

	if (swd_mode) {
		sunxigpio_bitbang.write = sunxigpio_swd_write;
		bitbang_switch_to_swd();
	}

	return ERROR_OK;
}

static int sunxigpio_quit(void)
{
	return ERROR_OK;
}
