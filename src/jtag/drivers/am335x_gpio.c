/***************************************************************************
 *   Copyright (C) 2018                                                    *
 *   by Kai Geissdoerfer, kai.geissdoerfer@tu-dresden.de                   *
 *                                                                         *
 *   Based on bcm2835gpio.c and imx_gpio                                   *
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

#include <sys/mman.h>

/* Clock manager for enabling clock for GPIO banks */
#define AM335X_CM_PER_START_ADDR 0x44E00000
#define AM335X_CM_PER_REGS_SIZE 250

struct am335x_cm_per_regs {
	char unused[172];
	/* One register to control clock of each GPIO1, GPIO2, GPIO3 */
	uint32_t gpio_clkctrl[3];
} __attribute__((packed, aligned(4)));

#define AM335X_GPIO_BANKS_COUNT 4
#define AM335X_GPIO_REGS_SIZE 2000

/* Each GPIO bank appears as its own peripheral in non-contiguous memory */
uint32_t gpio_bank_start_addr[] = {
	/* GPIO0 start address */
	0x44e07000,
	/* GPIO1 start address */
	0x4804C000,
	/* GPIO2 start address */
	0x481AC000,
	/* GPIO3 start address */
	0x481AE000
};

struct am335x_gpio_regs {
	char unused[308];
	/* output enable register*/
	uint32_t oe;
	uint32_t datain;
	uint32_t dataout;
} __attribute__((packed, aligned(4)));

static volatile struct am335x_gpio_regs *gpio_base[4];
static volatile struct am335x_cm_per_regs *cm_per_base;
static int fd_mem;

/* GPIO numbers for each signal. Negative values are invalid */
static int tck_gpio = -1;
static int tck_gpio_mode;
static int tms_gpio = -1;
static int tms_gpio_mode;
static int tdi_gpio = -1;
static int tdi_gpio_mode;
static int tdo_gpio = -1;
static int tdo_gpio_mode;
static int trst_gpio = -1;
static int trst_gpio_mode;
static int srst_gpio = -1;
static int srst_gpio_mode;
static int swclk_gpio = -1;
static int swclk_gpio_mode;
static int swdio_gpio = -1;
static int swdio_gpio_mode;

/* For storing GPIO clock enabled status */
static int gpio_cm_per_enabled[AM335X_GPIO_BANKS_COUNT];

enum PIN_MODE {OUTPUT = 0, INPUT = 1};
enum PIN_VALUE {LOW = 0, HIGH = 1};

/*
 * Note: GPIO number can be mapped to bank and pin with division/modulo
 * E.g. GPIO 41 is on GPIO0 (41/32) pin 9 (41%32)
*/
static inline void gpio_write(int gpio_num, int value)
{
	if (value)
		gpio_base[gpio_num / 32]->dataout |= (1U << (gpio_num % 32));
	else
		gpio_base[gpio_num / 32]->dataout &= ~(1U << (gpio_num % 32));
}

static inline void gpio_mode_set(int gpio_num, int mode)
{
	unsigned int bank = gpio_num / 32;
	unsigned int idx = gpio_num % 32;
	gpio_base[bank]->oe = (gpio_base[bank]->oe & ~(1U << idx)) | (mode << idx);
}

static inline int gpio_mode_get(int gpio_num)
{

	return (gpio_base[gpio_num / 32]->oe >> (gpio_num % 32)) & 1U;
}

static inline int gpio_read(int gpio_num)
{
	return (gpio_base[gpio_num / 32]->datain >> (gpio_num % 32)) & 1U;
}

int setup_mmap_gpio(int gpio_num)
{

	unsigned int gpio_bank = gpio_num / 32;
	LOG_DEBUG("Initializing GPIO #%d", gpio_num);
	if (!gpio_base[gpio_bank]) {
		LOG_DEBUG("Mapping GPIO bank #%u", gpio_bank);
		gpio_base[gpio_bank] = mmap(0, AM335X_GPIO_REGS_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd_mem, gpio_bank_start_addr[gpio_bank]);
		if (gpio_base[gpio_bank] == MAP_FAILED)
			return -1;

		/* Clock for GPIO1-3 has to be enabled manually */
		if (gpio_bank != 0) {
			/* Backup state */
			LOG_DEBUG("Saving clock state for GPIO bank #%u", gpio_bank);
			/* Clock for GPIO1 is configured in gpio_clkctrl[0] */
			gpio_cm_per_enabled[gpio_bank] = cm_per_base->gpio_clkctrl[gpio_bank - 1] & (1 << 1);
			LOG_DEBUG("Enabling clock for GPIO bank #%u", gpio_bank);
			cm_per_base->gpio_clkctrl[gpio_bank - 1] |= (1 << 1);
		}
	}

	return ERROR_OK;

}

static void release_gpio_banks(void)
{
	unsigned int gpio_bank;
	for (gpio_bank = 0; gpio_bank < AM335X_GPIO_BANKS_COUNT; gpio_bank++) {
		if (gpio_base[gpio_bank]) {
			munmap((void *) gpio_base[gpio_bank], AM335X_GPIO_REGS_SIZE);

			/* Restore clock state for GPIO1-3 */
			if (gpio_bank != 0) {
				/* Clock for GPIO1 is configured in gpio_clkctrl[0] */
				uint32_t state = cm_per_base->gpio_clkctrl[gpio_bank - 1] & ~(1 << 1);
				state |= gpio_cm_per_enabled[gpio_bank];
				cm_per_base->gpio_clkctrl[gpio_bank - 1] = state;
			}
		}
	}
	munmap((void *) cm_per_base, AM335X_CM_PER_REGS_SIZE);

	close(fd_mem);
}

/*
 * Helper func to determine if gpio number valid
 *
 */
static int is_gpio_valid(int gpio)
{
	return gpio >= 0 && gpio < (32 * AM335X_GPIO_BANKS_COUNT);
}

static bb_value_t am335x_gpio_read(void);
static int am335x_gpio_write(int tck, int tms, int tdi);
static int am335x_gpio_reset(int trst, int srst);

static int am335x_gpio_swdio_read(void);
static void am335x_gpio_swdio_drive(bool is_output);

static int am335x_gpio_init(void);
static int am335x_gpio_quit(void);

static struct bitbang_interface am335x_gpio_bitbang = {
	.read = am335x_gpio_read,
	.write = am335x_gpio_write,
	.reset = am335x_gpio_reset,
	.swdio_read = am335x_gpio_swdio_read,
	.swdio_drive = am335x_gpio_swdio_drive,
	.blink = NULL
};

/* Transition delay coefficients. Tuned for AM335X 1GHz. Adjusted
 * experimentally for:10kHz, 100Khz, 500KHz. Measured mmap raw GPIO toggling
 * speed on AM335X@1GHz: 1.4MHz.
 */
static int speed_coeff = 230000;
static int speed_offset = 320;
static unsigned int jtag_delay;

static bb_value_t am335x_gpio_read(void)
{
	return gpio_read(tdo_gpio) ? BB_HIGH : BB_LOW;
}

static int am335x_gpio_write(int tck, int tms, int tdi)
{
	gpio_write(tms_gpio, tms);
	gpio_write(tdi_gpio, tdi);
	gpio_write(tck_gpio, tck);

	for (unsigned int i = 0; i < jtag_delay; i++)
		asm volatile ("");

	return ERROR_OK;
}

static int am335x_gpio_swd_write(int tck, int tms, int tdi)
{
	gpio_write(swdio_gpio, tdi);
	gpio_write(swclk_gpio, tck);

	for (unsigned int i = 0; i < jtag_delay; i++)
		asm volatile ("");

	return ERROR_OK;
}

/* (1) assert or (0) deassert reset lines */
static int am335x_gpio_reset(int trst, int srst)
{
	if (trst_gpio != -1)
		gpio_write(trst_gpio, trst);

	if (srst_gpio != -1)
		gpio_write(srst_gpio, srst);

	return ERROR_OK;
}

static void am335x_gpio_swdio_drive(bool is_output)
{
	gpio_mode_set(swdio_gpio, is_output ? OUTPUT : INPUT);
}

static int am335x_gpio_swdio_read(void)
{
	return gpio_read(swdio_gpio);
}

static int am335x_gpio_khz(int khz, int *jtag_speed)
{
	if (!khz) {
		LOG_DEBUG("RCLK not supported");
		return ERROR_FAIL;
	}
	*jtag_speed = speed_coeff/khz - speed_offset;
	if (*jtag_speed < 0)
		*jtag_speed = 0;
	return ERROR_OK;
}

static int am335x_gpio_speed_div(int speed, int *khz)
{
	*khz = speed_coeff/(speed + speed_offset);
	return ERROR_OK;
}

static int am335x_gpio_speed(int speed)
{
	jtag_delay = speed;
	return ERROR_OK;
}


COMMAND_HANDLER(am335x_gpio_handle_jtag_gpionums)
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
			"am335x_gpio GPIO config: tck = %d, tms = %d, tdi = %d, tdo = %d",
			tck_gpio, tms_gpio, tdi_gpio, tdo_gpio);

	return ERROR_OK;
}

COMMAND_HANDLER(am335x_gpio_handle_jtag_gpionum_tck)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], tck_gpio);

	command_print(CMD_CTX, "am335x_gpio GPIO config: tck = %d", tck_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(am335x_gpio_handle_jtag_gpionum_tms)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], tms_gpio);

	command_print(CMD_CTX, "am335x_gpio GPIO config: tms = %d", tms_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(am335x_gpio_handle_jtag_gpionum_tdo)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], tdo_gpio);

	command_print(CMD_CTX, "am335x_gpio GPIO config: tdo = %d", tdo_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(am335x_gpio_handle_jtag_gpionum_tdi)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], tdi_gpio);

	command_print(CMD_CTX, "am335x_gpio GPIO config: tdi = %d", tdi_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(am335x_gpio_handle_jtag_gpionum_srst)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], srst_gpio);

	command_print(CMD_CTX, "am335x_gpio GPIO config: srst = %d", srst_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(am335x_gpio_handle_jtag_gpionum_trst)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], trst_gpio);

	command_print(CMD_CTX, "am335x_gpio GPIO config: trst = %d", trst_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(am335x_gpio_handle_swd_gpionums)
{
	if (CMD_ARGC == 2) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], swclk_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], swdio_gpio);
	} else if (CMD_ARGC != 0) {
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	command_print(CMD_CTX,
			"am335x_gpio GPIO nums: swclk = %d, swdio = %d",
			swclk_gpio, swdio_gpio);

	return ERROR_OK;
}

COMMAND_HANDLER(am335x_gpio_handle_swd_gpionum_swclk)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], swclk_gpio);

	command_print(CMD_CTX, "am335x_gpio num: swclk = %d", swclk_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(am335x_gpio_handle_swd_gpionum_swdio)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], swdio_gpio);

	command_print(CMD_CTX, "am335x_gpio num: swdio = %d", swdio_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(am335x_gpio_handle_speed_coeffs)
{
	if (CMD_ARGC == 2) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], speed_coeff);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], speed_offset);
	}
	return ERROR_OK;
}

static const struct command_registration am335x_gpio_command_handlers[] = {
	{
		.name = "am335x_gpio_jtag_nums",
		.handler = &am335x_gpio_handle_jtag_gpionums,
		.mode = COMMAND_CONFIG,
		.help = "gpio numbers for tck, tms, tdi, tdo. (in that order)",
		.usage = "(tck tms tdi tdo)* ",
	},
	{
		.name = "am335x_gpio_tck_num",
		.handler = &am335x_gpio_handle_jtag_gpionum_tck,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tck.",
	},
	{
		.name = "am335x_gpio_tms_num",
		.handler = &am335x_gpio_handle_jtag_gpionum_tms,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tms.",
	},
	{
		.name = "am335x_gpio_tdo_num",
		.handler = &am335x_gpio_handle_jtag_gpionum_tdo,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tdo.",
	},
	{
		.name = "am335x_gpio_tdi_num",
		.handler = &am335x_gpio_handle_jtag_gpionum_tdi,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tdi.",
	},
	{
		.name = "am335x_gpio_swd_nums",
		.handler = &am335x_gpio_handle_swd_gpionums,
		.mode = COMMAND_CONFIG,
		.help = "gpio numbers for swclk, swdio. (in that order)",
		.usage = "(swclk swdio)* ",
	},
	{
		.name = "am335x_gpio_swclk_num",
		.handler = &am335x_gpio_handle_swd_gpionum_swclk,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for swclk.",
	},
	{
		.name = "am335x_gpio_swdio_num",
		.handler = &am335x_gpio_handle_swd_gpionum_swdio,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for swdio.",
	},
	{
		.name = "am335x_gpio_srst_num",
		.handler = &am335x_gpio_handle_jtag_gpionum_srst,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for srst.",
	},
	{
		.name = "am335x_gpio_trst_num",
		.handler = &am335x_gpio_handle_jtag_gpionum_trst,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for trst.",
	},
	{
		.name = "am335x_gpio_speed_coeffs",
		.handler = &am335x_gpio_handle_speed_coeffs,
		.mode = COMMAND_CONFIG,
		.help = "SPEED_COEFF and SPEED_OFFSET for delay calculations.",
	},

	COMMAND_REGISTRATION_DONE
};

static const char * const am335x_gpio_transports[] = { "jtag", "swd", NULL };

struct jtag_interface am335x_gpio_interface = {
	.name = "am335x_gpio",
	.supported = DEBUG_CAP_TMS_SEQ,
	.execute_queue = bitbang_execute_queue,
	.transports = am335x_gpio_transports,
	.swd = &bitbang_swd,
	.speed = am335x_gpio_speed,
	.khz = am335x_gpio_khz,
	.speed_div = am335x_gpio_speed_div,
	.commands = am335x_gpio_command_handlers,
	.init = am335x_gpio_init,
	.quit = am335x_gpio_quit,
};

static bool am335x_gpio_jtag_mode_possible(void)
{
	if (!is_gpio_valid(tck_gpio))
		return 0;
	if (!is_gpio_valid(tms_gpio))
		return 0;
	if (!is_gpio_valid(tdi_gpio))
		return 0;
	if (!is_gpio_valid(tdo_gpio))
		return 0;
	return 1;
}

static bool am335x_gpio_swd_mode_possible(void)
{
	if (!is_gpio_valid(swclk_gpio))
		return 0;
	if (!is_gpio_valid(swdio_gpio))
		return 0;
	return 1;
}

static int am335x_gpio_init(void)
{
	bitbang_interface = &am335x_gpio_bitbang;

	LOG_INFO("am335x_gpio GPIO JTAG/SWD bitbang driver");

	if (am335x_gpio_jtag_mode_possible()) {
		if (am335x_gpio_swd_mode_possible())
			LOG_INFO("JTAG and SWD modes enabled");
		else
			LOG_INFO("JTAG only mode enabled (specify swclk and swdio gpio to add SWD mode)");
	} else if (am335x_gpio_swd_mode_possible()) {
		LOG_INFO("SWD only mode enabled (specify tck, tms, tdi and tdo gpios to add JTAG mode)");
	} else {
		LOG_ERROR("Require tck, tms, tdi and tdo gpios for JTAG mode and/or swclk and swdio gpio for SWD mode");
		return ERROR_JTAG_INIT_FAILED;
	}

	fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd_mem < 0) {
		perror("open");
		return ERROR_JTAG_INIT_FAILED;
	}

	/* Map Registers for Clock control of GPIO banks */
	cm_per_base = mmap(0, AM335X_CM_PER_REGS_SIZE, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd_mem, AM335X_CM_PER_START_ADDR);
	if (cm_per_base == MAP_FAILED) {
		LOG_ERROR("Failed to mmap Clock Manager registers");
		close(fd_mem);
		return ERROR_JTAG_INIT_FAILED;
	}
	int ret;
	/*
	 * Configure TDO as an input, and TDI, TCK, TMS, TRST, SRST
	 * as outputs.  Drive TDI and TCK low, and TMS/TRST/SRST high.
	 */
	if (am335x_gpio_jtag_mode_possible()) {

		ret = setup_mmap_gpio(tdo_gpio);
		if (ret != ERROR_OK)
			goto out_error;
		tdo_gpio_mode = gpio_mode_get(tdo_gpio);

		ret = setup_mmap_gpio(tdi_gpio);
		if (ret != ERROR_OK)
			goto out_error;
		tdi_gpio_mode = gpio_mode_get(tdi_gpio);

		ret = setup_mmap_gpio(tck_gpio);
		if (ret != ERROR_OK)
			goto out_error;
		tck_gpio_mode = gpio_mode_get(tck_gpio);

		ret = setup_mmap_gpio(tms_gpio);
		if (ret != ERROR_OK)
			goto out_error;
		tms_gpio_mode = gpio_mode_get(tms_gpio);

		gpio_mode_set(tdo_gpio, INPUT);
		gpio_mode_set(tdi_gpio, OUTPUT);
		gpio_mode_set(tck_gpio, OUTPUT);
		gpio_mode_set(tms_gpio, OUTPUT);

		gpio_write(tdi_gpio, LOW);
		gpio_write(tck_gpio, LOW);
		gpio_write(tms_gpio, HIGH);

	}
	if (am335x_gpio_swd_mode_possible()) {
		ret = setup_mmap_gpio(swclk_gpio);
		if (ret != ERROR_OK)
			goto out_error;
		swclk_gpio_mode = gpio_mode_get(swclk_gpio);

		ret = setup_mmap_gpio(swdio_gpio);
		if (ret != ERROR_OK)
			goto out_error;
		swdio_gpio_mode = gpio_mode_get(swdio_gpio);

		gpio_mode_set(swclk_gpio, OUTPUT);
		gpio_mode_set(swdio_gpio, OUTPUT);

		gpio_write(swdio_gpio, LOW);
		gpio_write(swclk_gpio, LOW);
	}
	if (trst_gpio != -1) {
		ret = setup_mmap_gpio(trst_gpio);
		if (ret != ERROR_OK)
			goto out_error;
		trst_gpio_mode = gpio_mode_get(trst_gpio);
		gpio_mode_set(trst_gpio, OUTPUT);
		gpio_write(trst_gpio, HIGH);
	}
	if (srst_gpio != -1) {
		ret = setup_mmap_gpio(srst_gpio);
		if (ret != ERROR_OK)
			goto out_error;
		srst_gpio_mode = gpio_mode_get(srst_gpio);
		gpio_mode_set(srst_gpio, OUTPUT);
		gpio_write(srst_gpio, HIGH);
	}

	LOG_DEBUG("saved pinmux settings: tck %d tms %d tdi %d "
		  "tdo %d trst %d srst %d", tck_gpio_mode, tms_gpio_mode,
		  tdi_gpio_mode, tdo_gpio_mode, trst_gpio_mode, srst_gpio_mode);

	if (swd_mode) {
		am335x_gpio_bitbang.write = am335x_gpio_swd_write;
		bitbang_switch_to_swd();
	}

	return ERROR_OK;
out_error:
	LOG_ERROR("Failed to setup a GPIO pin");
	release_gpio_banks();
	return ERROR_JTAG_INIT_FAILED;
}

static int am335x_gpio_quit(void)
{
	if (am335x_gpio_jtag_mode_possible()) {
		gpio_mode_set(tdo_gpio, tdo_gpio_mode);
		gpio_mode_set(tdi_gpio, tdi_gpio_mode);
		gpio_mode_set(tck_gpio, tck_gpio_mode);
		gpio_mode_set(tms_gpio, tms_gpio_mode);
	}
	if (am335x_gpio_swd_mode_possible()) {
		gpio_mode_set(swclk_gpio, swclk_gpio_mode);
		gpio_mode_set(swdio_gpio, swdio_gpio_mode);
	}
	if (trst_gpio != -1)
		gpio_mode_set(trst_gpio, trst_gpio_mode);
	if (srst_gpio != -1)
		gpio_mode_set(srst_gpio, srst_gpio_mode);

	release_gpio_banks();
	return ERROR_OK;
}
