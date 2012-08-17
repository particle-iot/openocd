/***************************************************************************
 *   Copyright (C) 2012 by George Harris                                   *
 *   george@luminairecoffee.com                                            *
 *																		   *
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

#include "imp.h"
#include "spi.h"
#include <jtag/jtag.h>
#include <helper/time_support.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

#define SSP_READ_REG(a) (_SSP_READ_REG(a))
#define _SSP_READ_REG(a)			\
{									\
	int __a;						\
	uint32_t __v;					\
									\
	__a = target_read_u32(target, ssp_base + (a), &__v); \
	if (__a != ERROR_OK)			\
		return __a;					\
	__v;							\
}

#define SSP_WRITE_REG(a, v)			\
{									\
	int __r;						\
									\
	__r = target_write_u32(target, ssp_base + (a), (v)); \
	if (__r != ERROR_OK)			\
		return __r;					\
}

#define IO_WRITE_REG(a, v)			\
{									\
	int __r;						\
									\
	__r = target_write_u32(target, io_base + (a), (v)); \
	if (__r != ERROR_OK)			\
		return __r;					\
}

#define IOCONFIG_WRITE_REG(a, v)			\
{									\
	int __r;						\
									\
	__r = target_write_u32(target, ioconfig_base + (a), (v)); \
	if (__r != ERROR_OK)			\
		return __r;					\
}

#define SSP_POLL_BUSY(timeout)		\
{									\
	int __r;						\
									\
	__r = poll_ssp_busy(target, ssp_base, timeout); \
	if (__r != ERROR_OK)			\
		return __r;					\
}

#define SSP_START_COMMAND() {  \
	lpcspifi_ssp_setcs(target, io_base, 0);     \
}

#define SSP_STOP_COMMAND() {   \
	lpcspifi_ssp_setcs(target, io_base, 1);     \
}

#define SSP_CR0		(0x00)  /* Control register 0 */
#define SSP_CR1		(0x04)  /* Control register 1 */
#define SSP_DATA	(0x08)  /* Data register (TX and RX) */
#define SSP_SR		(0x0C)  /* Status register */
#define SSP_CPSR	(0x10)  /* Clock prescale register */


/* Status register fields */
#define SSP_BSY		(0x00000010)

/* Timeout in ms */
#define SSP_CMD_TIMEOUT   (100)
#define SSP_PROBE_TIMEOUT (100)
#define SSP_MAX_TIMEOUT  (3000)

struct lpcspifi_flash_bank {
	struct working_area *write_algorithm;
	struct working_area *erase_algorithm;
	struct working_area *spifi_init_algorithm;
	int probed;
	uint32_t ssp_base;
	uint32_t io_base;
	uint32_t ioconfig_base;
	uint32_t bank_num;
	uint32_t max_spi_clock_mhz;
	struct flash_device *dev;
};

struct lpcspifi_target {
	char *name;
	uint32_t tap_idcode;
	uint32_t spifi_base;
	uint32_t ssp_base;
	uint32_t io_base;
	uint32_t ioconfig_base; /* base address for the port word pin registers */
};

static struct lpcspifi_target target_devices[] = {
	/* name,          tap_idcode, spifi_base, ssp_base,   io_base,    ioconfig_base */
	{ "LPC43xx/18xx", 0x4ba00477, 0x14000000, 0x40083000, 0x400F4000, 0x40086000 },
	{ NULL,           0,          0,          0,          0,          0 }
};

/* flash_bank lpcspifi <base> <size> <chip_width> <bus_width> <target>
 */
FLASH_BANK_COMMAND_HANDLER(lpcspifi_flash_bank_command)
{
	struct lpcspifi_flash_bank *lpcspifi_info;

	LOG_DEBUG("%s", __func__);

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	lpcspifi_info = malloc(sizeof(struct lpcspifi_flash_bank));
	if (lpcspifi_info == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	bank->driver_priv = lpcspifi_info;
	lpcspifi_info->probed = 0;

	lpcspifi_info->spifi_init_algorithm = NULL;
	lpcspifi_info->write_algorithm = NULL;
	lpcspifi_info->erase_algorithm = NULL;

	return ERROR_OK;
}

/* Poll the SSP busy flag. When this comes back as 0, the transfer is complete
 * and the controller is idle. */
static int poll_ssp_busy(struct target *target, uint32_t ssp_base, int timeout)
{
	long long endtime;

	if (SSP_READ_REG(SSP_SR) & SSP_BSY)
		return ERROR_OK;

	endtime = timeval_ms() + timeout;
	do {
		alive_sleep(1);
		if ((SSP_READ_REG(SSP_SR) & SSP_BSY) == 0)
			return ERROR_OK;
	} while (timeval_ms() < endtime);

	LOG_ERROR("Timeout while polling BSY");
	return ERROR_FLASH_OPERATION_FAILED;
}

static int lpcspifi_ssp_setcs(struct target *target, uint32_t io_base, unsigned int value);
static int lpcspifi_set_hw_mode(struct flash_bank *bank);

/* Initialize the ssp module */
static int lpcspifi_set_sw_mode(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct lpcspifi_flash_bank *lpcspifi_info = bank->driver_priv;
	uint32_t ssp_base = lpcspifi_info->ssp_base;
	uint32_t io_base = lpcspifi_info->io_base;
	uint32_t ioconfig_base = lpcspifi_info->ioconfig_base;

	/* Re-initialize SPIFI. There are a couple of errata on this, so this makes
	sure that nothing's in an unhappy state. */
	lpcspifi_set_hw_mode(bank);

	/* Initialize the pins */
	IOCONFIG_WRITE_REG(0x194, 0x00000040);
	IOCONFIG_WRITE_REG(0x1A0, 0x00000044);
	IOCONFIG_WRITE_REG(0x190, 0x00000040);
	IOCONFIG_WRITE_REG(0x19C, 0x000000ED);
	IOCONFIG_WRITE_REG(0x198, 0x000000ED);
	IOCONFIG_WRITE_REG(0x18C, 0x000000EA);

	/* Set CS high & as an output */
	IO_WRITE_REG(0x12AC, 0xFFFFFFFF);
	IO_WRITE_REG(0x2014, 0x00000800);

	/* Initialize the module */
	SSP_WRITE_REG(SSP_CR0, 0x00000007);
	SSP_WRITE_REG(SSP_CR1, 0x00000000);
	SSP_WRITE_REG(SSP_CPSR, 0x00000008);
	SSP_WRITE_REG(SSP_CR1, 0x00000002);

	return ERROR_OK;
}

/* Un-initialize the ssp module */
static int lpcspifi_set_hw_mode(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct lpcspifi_flash_bank *lpcspifi_info = bank->driver_priv;
	uint32_t ssp_base = lpcspifi_info->ssp_base;
	struct armv7m_algorithm arm_algo;
	struct reg_param reg_params[1];
	int retval;

	LOG_DEBUG("Uninitializing LPC43xx SSP");
	/* Turn off the SSP module */
	SSP_WRITE_REG(SSP_CR1, 0x00000000);

	/* see contrib/loaders/flash/lpcspifi_init.s for src */
	static const uint32_t spifi_init_code[] = {
		0x0800ea4f, 0xaf00b0a1,
		0x43c0f44f, 0x0308f2c4,
		0x02f3f04f, 0x218cf8c3,
		0x43c0f44f, 0x0308f2c4,
		0x42c0f44f, 0x0208f2c4,
		0x41c0f44f, 0x0108f2c4,
		0x40c0f44f, 0x0008f2c4,
		0x04d3f04f, 0x419cf8c0,
		0xf8c14620, 0x46010198,
		0x1194f8c2, 0x1190f8c3,
		0x43c0f44f, 0x0308f2c4,
		0x0213f04f, 0x21a0f8c3,
		0x1318f240, 0x0340f2c1,
		0x681c681b, 0x30b4f240,
		0x0000f2c1, 0x0103f04f,
		0x02c0f04f, 0x0308ea4f,
		0xf00047a0, 0xbe00b800
	};

	arm_algo.common_magic = ARMV7M_COMMON_MAGIC;
	arm_algo.core_mode = ARMV7M_MODE_ANY;

	if (lpcspifi_info->spifi_init_algorithm == NULL) {
		LOG_DEBUG("Allocating working area for SPIFI init algorithm");
		/* Get memory for spifi initialization algorithm */
		retval = target_alloc_working_area(target, sizeof(spifi_init_code),
			&(lpcspifi_info->spifi_init_algorithm));
		if (retval != ERROR_OK)
			return retval;

		LOG_DEBUG("Writing algorithm to working area at 0x%08x",
			lpcspifi_info->spifi_init_algorithm->address);
		/* Write algorithm to working area */
		retval = target_write_buffer(target,
			lpcspifi_info->spifi_init_algorithm->address,
			sizeof(spifi_init_code),
			(uint8_t *)spifi_init_code
		);

		if (retval != ERROR_OK) {
			if (lpcspifi_info->spifi_init_algorithm != NULL) {
				target_free_working_area(target, lpcspifi_info->spifi_init_algorithm);
				lpcspifi_info->spifi_init_algorithm = NULL;
				destroy_reg_param(&reg_params[0]);
			}

			return retval;
		}
	}

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);		/* spifi clk speed */

	/* For now, this is the IRC speed on the LPC43xx, as the init
	 * algorithm doesn't currently set up clocking from other sources. */
	buf_set_u32(reg_params[0].value, 0, 32, 12);

	/* Run the algorithm */
	LOG_DEBUG("Running SPIFI init algorithm");
	retval = target_run_algorithm(target, 0 , NULL, 1, reg_params,
		lpcspifi_info->spifi_init_algorithm->address,
		lpcspifi_info->spifi_init_algorithm->address + sizeof(spifi_init_code) - 2,
		1000, &arm_algo);

	if (retval != ERROR_OK) {
		if (lpcspifi_info->spifi_init_algorithm != NULL) {
			target_free_working_area(target, lpcspifi_info->spifi_init_algorithm);
			lpcspifi_info->spifi_init_algorithm = NULL;
			destroy_reg_param(&reg_params[0]);
		}

		return retval;
	}

	LOG_DEBUG("SPIFI init algorithm successful, cleaning up");
	if (lpcspifi_info->spifi_init_algorithm != NULL) {
		target_free_working_area(target, lpcspifi_info->spifi_init_algorithm);
		lpcspifi_info->spifi_init_algorithm = NULL;
		destroy_reg_param(&reg_params[0]);
	}

	return ERROR_OK;
}

static int lpcspifi_ssp_setcs(struct target *target, uint32_t io_base, unsigned int value)
{
	if (value)
		IO_WRITE_REG(0x12AC, 0xFFFFFFFF);
	else
		IO_WRITE_REG(0x12AC, 0x00000000);

	return ERROR_OK;
}

/* Read the status register of the external SPI flash chip. */
static int read_status_reg(struct flash_bank *bank, uint32_t *status)
{
	struct target *target = bank->target;
	struct lpcspifi_flash_bank *lpcspifi_info = bank->driver_priv;
	uint32_t ssp_base = lpcspifi_info->ssp_base;
	uint32_t io_base = lpcspifi_info->io_base;

	SSP_START_COMMAND();
	SSP_WRITE_REG(SSP_DATA, SPIFLASH_READ_STATUS);
	SSP_POLL_BUSY(SSP_CMD_TIMEOUT);
	SSP_READ_REG(SSP_DATA);
	/* Dummy write to clock in the register */
	SSP_WRITE_REG(SSP_DATA, 0x00);
	SSP_POLL_BUSY(SSP_CMD_TIMEOUT);
	SSP_STOP_COMMAND();

	*status = SSP_READ_REG(SSP_DATA);

	return ERROR_OK;
}

/* check for BSY bit in flash status register */
/* timeout in ms */
static int wait_till_ready(struct flash_bank *bank, int timeout)
{
	uint32_t status;
	int retval;
	long long endtime;

	endtime = timeval_ms() + timeout;
	do {
		/* read flash status register */
		retval = read_status_reg(bank, &status);
		if (retval != ERROR_OK)
			return retval;

		if ((status & SPIFLASH_BSY_BIT) == 0)
			return ERROR_OK;
		alive_sleep(1);
	} while (timeval_ms() < endtime);

	LOG_ERROR("timeout");
	return ERROR_FAIL;
}

/* Send "write enable" command to SPI flash chip. */
static int lpcspifi_write_enable(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct lpcspifi_flash_bank *lpcspifi_info = bank->driver_priv;
	uint32_t ssp_base = lpcspifi_info->ssp_base;
	uint32_t io_base = lpcspifi_info->io_base;
	uint32_t status;
	int retval;

	SSP_START_COMMAND();
	SSP_WRITE_REG(SSP_DATA, SPIFLASH_WRITE_ENABLE);
	SSP_POLL_BUSY(SSP_CMD_TIMEOUT);
	SSP_READ_REG(SSP_DATA);
	SSP_STOP_COMMAND();

	/* read flash status register */
	retval = read_status_reg(bank, &status);
	if (retval != ERROR_OK)
		return retval;

	/* Check write enabled */
	if ((status & SPIFLASH_WE_BIT) == 0) {
		LOG_ERROR("Cannot enable write to flash. Status=0x%08" PRIx32, status);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int lpcspifi_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	struct lpcspifi_flash_bank *lpcspifi_info = bank->driver_priv;
	uint32_t ssp_base = lpcspifi_info->ssp_base;
	uint32_t io_base = lpcspifi_info->io_base;
	struct reg_param reg_params[4];
	struct armv7m_algorithm arm_algo;
	int retval = ERROR_OK;
	int sector;

	LOG_DEBUG("%s: from sector %d to sector %d", __func__, first, last);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((first < 0) || (last < first) || (last >= bank->num_sectors)) {
		LOG_ERROR("Flash sector invalid");
		return ERROR_FLASH_SECTOR_INVALID;
	}

	if (!(lpcspifi_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	for (sector = first; sector <= last; sector++) {
		if (bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	lpcspifi_set_hw_mode(bank);

	/* see contrib/loaders/flash/lpcspifi_erase.s for src */
	static const uint32_t lpcspifi_flash_erase_code[] = {
		0x4ac0f44f, 0x0a08f2c4,
		0x08eaf04f, 0x818cf8ca,
		0x0840f04f, 0x8190f8ca,
		0x0840f04f, 0x8194f8ca,
		0x08edf04f, 0x8198f8ca,
		0x08edf04f, 0x819cf8ca,
		0x0844f04f, 0x81a0f8ca,
		0x4ac0f44f, 0x0a0ff2c4,
		0x6800f44f, 0x8014f8ca,
		0x4aa0f44f, 0x0a0ff2c4,
		0x08fff04f, 0x82acf8ca,
		0x0a00f04f, 0x0a05f2c4,
		0x0800f04f, 0x1800f2c0,
		0x8094f8ca, 0x5a00f44f,
		0x0a05f2c4, 0x0801f04f,
		0x8700f8ca, 0x5a40f44f,
		0x0a08f2c4, 0x0807f04f,
		0x8000f8ca, 0x0802f04f,
		0x8010f8ca, 0x8004f8ca,
		0xf852f000, 0x0906f04f,
		0xf83bf000, 0xf848f000,
		0xf84af000, 0x0905f04f,
		0xf833f000, 0x0900f04f,
		0xf82ff000, 0xf83cf000,
		0x0f02f019, 0x8045f000,
		0xf83af000, 0x0902ea4f,
		0xf823f000, 0x4910ea4f,
		0xf81ff000, 0x2910ea4f,
		0xf81bf000, 0x0900ea4f,
		0xf817f000, 0xf824f000,
		0xf826f000, 0x0905f04f,
		0xf80ff000, 0x0900f04f,
		0xf80bf000, 0xf818f000,
		0x0f01f019, 0xaff0f47f,
		0xb3013901, 0xf7ff4418,
		0xf44fbfbf, 0xf2c45a40,
		0xf8ca0a08, 0xf8da9008,
		0xf019900c, 0xf47f0f10,
		0xf8daaffa, 0x47709008,
		0x08fff04f, 0xb802f000,
		0x0800f04f, 0x4a80f44f,
		0x0a0ff2c4, 0x80abf8ca,
		0x20004770, 0x46306050,
		0xffffbe00
	};

	if (first == 0 && last == (bank->num_sectors - 1)
		&& lpcspifi_info->dev->chip_erase_cmd != lpcspifi_info->dev->erase_cmd) {
		/* If we're erasing the whole flash chip and it supports chip erase,
		use it. It will (theoretically) be faster. */
		LOG_DEBUG("Chip supports the bulk erase command."\
			" Will use bulk erase instead of sector-by-sector erase.");

		lpcspifi_set_sw_mode(bank);

		retval = lpcspifi_write_enable(bank);
		if (retval != ERROR_OK) {
			LOG_ERROR("Unable to enable write on SPI flash");
			lpcspifi_set_hw_mode(bank);
			return retval;
		}

		/* send SPI command "bulk erase" */
		SSP_START_COMMAND();
		SSP_WRITE_REG(SSP_DATA, lpcspifi_info->dev->chip_erase_cmd);
		SSP_POLL_BUSY(SSP_CMD_TIMEOUT);
		SSP_READ_REG(SSP_DATA);
		SSP_STOP_COMMAND();

		/* poll flash BSY for self-timed bulk erase */
		retval = wait_till_ready(bank, bank->num_sectors*SSP_MAX_TIMEOUT);

	} else {
		arm_algo.common_magic = ARMV7M_COMMON_MAGIC;
		arm_algo.core_mode = ARMV7M_MODE_ANY;

		if (lpcspifi_info->erase_algorithm == NULL) {
			/* Get memory for spifi initialization algorithm */
			retval = target_alloc_working_area(target, sizeof(lpcspifi_flash_erase_code),
				&(lpcspifi_info->erase_algorithm));
			if (retval != ERROR_OK) {
				LOG_WARNING("No working area available."\
					" You must configure a working area in order to erase SPI flash.");
				return retval;
			}

			/* Write algorithm to working area */
			retval = target_write_buffer(target, lpcspifi_info->erase_algorithm->address,
				sizeof(lpcspifi_flash_erase_code), (uint8_t *)lpcspifi_flash_erase_code);
			if (retval != ERROR_OK) {
				if (lpcspifi_info->erase_algorithm != NULL) {
					target_free_working_area(target, lpcspifi_info->erase_algorithm);
					lpcspifi_info->erase_algorithm = NULL;
				}
				return retval;
			}
		}

		init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);	/* Start address */
		init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* Sector count */
		init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);	/* Erase command */
		init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);	/* Sector size */

		buf_set_u32(reg_params[0].value, 0, 32, bank->sectors[first].offset);
		buf_set_u32(reg_params[1].value, 0, 32, last - first + 1);
		buf_set_u32(reg_params[2].value, 0, 32, lpcspifi_info->dev->erase_cmd);
		buf_set_u32(reg_params[3].value, 0, 32, bank->sectors[first].size);

		/* Run the algorithm */
		retval = target_run_algorithm(target, 0 , NULL, 4, reg_params,
			lpcspifi_info->erase_algorithm->address,
			lpcspifi_info->erase_algorithm->address + sizeof(lpcspifi_flash_erase_code) - 4,
			3000*(last - first + 1), &arm_algo);

		target_free_working_area(target, lpcspifi_info->erase_algorithm);
		lpcspifi_info->erase_algorithm = NULL;

		destroy_reg_param(&reg_params[0]);
		destroy_reg_param(&reg_params[1]);
		destroy_reg_param(&reg_params[2]);
		destroy_reg_param(&reg_params[3]);
	}

	lpcspifi_set_hw_mode(bank);

	return retval;
}

static int lpcspifi_protect(struct flash_bank *bank, int set,
	int first, int last)
{
	int sector;

	for (sector = first; sector <= last; sector++)
		bank->sectors[sector].is_protected = set;
	return ERROR_OK;
}

static int lpcspifi_write(struct flash_bank *bank, uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct lpcspifi_flash_bank *lpcspifi_info = bank->driver_priv;
	uint32_t page_size, buffer_size;
	struct working_area *fifo;
	struct reg_param reg_params[5];
	struct armv7m_algorithm armv7m_info;
	int sector;
	int retval = ERROR_OK;

	LOG_DEBUG("%s: offset=0x%08" PRIx32 " count=0x%08" PRIx32,
		__func__, offset, count);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset + count > lpcspifi_info->dev->size_in_bytes) {
		LOG_WARNING("Writes past end of flash. Extra data discarded.");
		count = lpcspifi_info->dev->size_in_bytes - offset;
	}

	/* Check sector protection */
	for (sector = 0; sector < bank->num_sectors; sector++) {
		/* Start offset in or before this sector? */
		/* End offset in or behind this sector? */
		if ((offset <
				(bank->sectors[sector].offset + bank->sectors[sector].size))
			&& ((offset + count - 1) >= bank->sectors[sector].offset)
			&& bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	page_size = lpcspifi_info->dev->pagesize;

	lpcspifi_set_hw_mode(bank);

	/* see contrib/loaders/flash/lpcspifi_write.s for src */
	static const uint32_t lpcspifi_flash_write_code[] = {
		0x4ac0f44f, 0x0a08f2c4,
		0x08eaf04f, 0x818cf8ca,
		0x0840f04f, 0x8190f8ca,
		0x0840f04f, 0x8194f8ca,
		0x08edf04f, 0x8198f8ca,
		0x08edf04f, 0x819cf8ca,
		0x0844f04f, 0x81a0f8ca,
		0x4ac0f44f, 0x0a0ff2c4,
		0x6800f44f, 0x8014f8ca,
		0x4aa0f44f, 0x0a0ff2c4,
		0x08fff04f, 0x82acf8ca,
		0x0a00f04f, 0x0a05f2c4,
		0x0800f04f, 0x1800f2c0,
		0x8094f8ca, 0x5a00f44f,
		0x0a05f2c4, 0x0801f04f,
		0x8700f8ca, 0x5a40f44f,
		0x0a08f2c4, 0x0807f04f,
		0x8000f8ca, 0x0802f04f,
		0x8010f8ca, 0x8004f8ca,
		0x0b00f04f, 0x459344a3,
		0xaffcf67f, 0xf86af000,
		0x0906f04f, 0xf853f000,
		0xf860f000, 0xf862f000,
		0x0905f04f, 0xf84bf000,
		0x0900f04f, 0xf847f000,
		0xf854f000, 0x0f02f019,
		0x805df000, 0xf852f000,
		0x0902f04f, 0xf83bf000,
		0x4912ea4f, 0xf837f000,
		0x2912ea4f, 0xf833f000,
		0x0902ea4f, 0xf82ff000,
		0x8000f8d0, 0x0f00f1b8,
		0x8047f000, 0x45476847,
		0xaff6f43f, 0x9b01f817,
		0xf821f000, 0xbf28428f,
		0x0708f100, 0x3b016047,
		0xf102b3bb, 0x45930201,
		0xafe6f47f, 0xf822f000,
		0xf00044a3, 0xf04ff823,
		0xf0000905, 0xf04ff80c,
		0xf0000900, 0xf000f808,
		0xf019f815, 0xf47f0f01,
		0xf7ffaff0, 0xf44fbfa7,
		0xf2c45a40, 0xf8ca0a08,
		0xf8da9008, 0xf019900c,
		0xf47f0f10, 0xf8daaffa,
		0x47709008, 0x08fff04f,
		0xb802f000, 0x0800f04f,
		0x4a80f44f, 0x0a0ff2c4,
		0x80abf8ca, 0x20004770,
		0x46306050, 0xffffbe00
	};

	if (target_alloc_working_area(target, sizeof(lpcspifi_flash_write_code),
			&lpcspifi_info->write_algorithm) != ERROR_OK) {
		LOG_WARNING("No working area available."\
			" You must configure a working area in order to write SPI flash.");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	};

	retval = target_write_buffer(target, lpcspifi_info->write_algorithm->address,
			sizeof(lpcspifi_flash_write_code),
			(uint8_t *)lpcspifi_flash_write_code);
	if (retval != ERROR_OK)
		return retval;

	/* FIFO allocation */
	buffer_size = 0x2000;
	while (target_alloc_working_area_try(target, buffer_size, &fifo) != ERROR_OK) {
		buffer_size /= 2;
		if (buffer_size < page_size) {
			/* if we already allocated the writing code, but failed to get a
			 * buffer, free the algorithm */
			if (lpcspifi_info->write_algorithm)
				target_free_working_area(target, lpcspifi_info->write_algorithm);

			LOG_WARNING("Working area too small; falling back to slow memory access");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	};

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARMV7M_MODE_ANY;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);		/* buffer start, status (out) */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);		/* buffer end */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);		/* target address */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);		/* count (halfword-16bit) */
	init_reg_param(&reg_params[4], "r4", 32, PARAM_OUT);		/* page size */

	buf_set_u32(reg_params[0].value, 0, 32, fifo->address);
	buf_set_u32(reg_params[1].value, 0, 32, fifo->address + fifo->size);
	buf_set_u32(reg_params[2].value, 0, 32, offset);
	buf_set_u32(reg_params[3].value, 0, 32, count);
	buf_set_u32(reg_params[4].value, 0, 32, page_size);

	retval = target_run_flash_async_algorithm(target, buffer, count, 1,
			0, NULL,
			5, reg_params,
			fifo->address, fifo->size,
			lpcspifi_info->write_algorithm->address, 0,
			&armv7m_info
	);

	if (retval == ERROR_FLASH_OPERATION_FAILED)
		LOG_ERROR("Error executing lpc43xx flash write algorithm");

	target_free_working_area(target, fifo);
	target_free_working_area(target, lpcspifi_info->write_algorithm);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);

	/* Switch to HW mode before return to prompt */
	lpcspifi_set_hw_mode(bank);
	return retval;
}

/* Return ID of flash device */
/* On exit, SW mode is kept */
static int lpcspifi_read_flash_id(struct flash_bank *bank, uint32_t *id)
{
	struct target *target = bank->target;
	struct lpcspifi_flash_bank *lpcspifi_info = bank->driver_priv;
	uint32_t ssp_base = lpcspifi_info->ssp_base;
	uint32_t io_base = lpcspifi_info->io_base;
	int retval;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_DEBUG("Getting ID");
	lpcspifi_set_sw_mode(bank);

	/* poll WIP */
	retval = wait_till_ready(bank, SSP_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	/* Send SPI command "read ID" */
	SSP_START_COMMAND();
	SSP_WRITE_REG(SSP_DATA, SPIFLASH_READ_ID);
	SSP_POLL_BUSY(SSP_CMD_TIMEOUT);
	SSP_READ_REG(SSP_DATA);

	/* Dummy write to clock in data */
	SSP_WRITE_REG(SSP_DATA, 0x00);
	SSP_POLL_BUSY(SSP_CMD_TIMEOUT);
	((uint8_t *)id)[0] = SSP_READ_REG(SSP_DATA);

	/* Dummy write to clock in data */
	SSP_WRITE_REG(SSP_DATA, 0x00);
	SSP_POLL_BUSY(SSP_CMD_TIMEOUT);
	((uint8_t *)id)[1] = SSP_READ_REG(SSP_DATA);

	/* Dummy write to clock in data */
	SSP_WRITE_REG(SSP_DATA, 0x00);
	SSP_POLL_BUSY(SSP_CMD_TIMEOUT);
	((uint8_t *)id)[2] = SSP_READ_REG(SSP_DATA);

	SSP_STOP_COMMAND();
	return ERROR_OK;
}

static int lpcspifi_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct lpcspifi_flash_bank *lpcspifi_info = bank->driver_priv;
	uint32_t ssp_base;
	uint32_t io_base;
	uint32_t ioconfig_base;
	struct flash_sector *sectors;
	uint32_t id = 0; /* silence uninitialized warning */
	struct lpcspifi_target *target_device;
	struct working_area *dummy_area;
	int retval;

	/* If we've already probed, we should be fine to skip this time. */
	if (lpcspifi_info->probed)
		return ERROR_OK;
	lpcspifi_info->probed = 0;

	for (target_device = target_devices ; target_device->name ; ++target_device)
		if (target_device->tap_idcode == target->tap->idcode)
			break;
	if (!target_device->name) {
		LOG_ERROR("Device ID 0x%" PRIx32 " is not known as SPIFI capable",
				target->tap->idcode);
		return ERROR_FAIL;
	}

	/* Attempt to allocate a small working area (enough to hold our
	 * write/erase algorithms plus some FIFO space) to make sure
	 * enough has been allocated for this target; 1k should do it. */
	retval = target_alloc_working_area_try(target, 0x400, &dummy_area);
	if (retval != ERROR_OK) {
		LOG_ERROR("Target must have at least 1kB of working area for this driver to function.");
		return ERROR_FAIL;
	}

	if (dummy_area)
		target_free_working_area(target, dummy_area);

	ssp_base = target_device->ssp_base;
	io_base = target_device->io_base;
	ioconfig_base = target_device->ioconfig_base;
	lpcspifi_info->ssp_base = ssp_base;
	lpcspifi_info->io_base = io_base;
	lpcspifi_info->ioconfig_base = ioconfig_base;
	lpcspifi_info->bank_num = bank->bank_number;

	LOG_DEBUG("Valid SPIFI on device %s at address 0x%" PRIx32,
		target_device->name, bank->base);

	/* read and decode flash ID; returns in SW mode */
	retval = lpcspifi_read_flash_id(bank, &id);
	lpcspifi_set_hw_mode(bank);
	if (retval != ERROR_OK)
		return retval;

	lpcspifi_info->dev = NULL;
	for (struct flash_device *p = flash_devices; p->name ; p++)
		if (p->device_id == id) {
			lpcspifi_info->dev = p;
			break;
		}

	if (!lpcspifi_info->dev) {
		LOG_ERROR("Unknown flash device (ID 0x%08" PRIx32 ")", id);
		return ERROR_FAIL;
	}

	LOG_INFO("Found flash device \'%s\' (ID 0x%08" PRIx32 ")",
		lpcspifi_info->dev->name, lpcspifi_info->dev->device_id);

	/* Set correct size value */
	bank->size = lpcspifi_info->dev->size_in_bytes;

	/* create and fill sectors array */
	bank->num_sectors =
		lpcspifi_info->dev->size_in_bytes / lpcspifi_info->dev->sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (sectors == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset = sector * lpcspifi_info->dev->sectorsize;
		sectors[sector].size = lpcspifi_info->dev->sectorsize;
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 1;
	}

	bank->sectors = sectors;


	lpcspifi_set_hw_mode(bank);

	lpcspifi_info->probed = 1;
	return ERROR_OK;
}

static int lpcspifi_auto_probe(struct flash_bank *bank)
{
	struct lpcspifi_flash_bank *lpcspifi_info = bank->driver_priv;
	if (lpcspifi_info->probed)
		return ERROR_OK;
	return lpcspifi_probe(bank);
}

static int lpcspifi_protect_check(struct flash_bank *bank)
{
	/* Nothing to do. Protection is only handled in SW. */
	return ERROR_OK;
}

static int get_lpcspifi_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct lpcspifi_flash_bank *lpcspifi_info = bank->driver_priv;

	if (!(lpcspifi_info->probed)) {
		snprintf(buf, buf_size,
			"\nSPIFI flash bank not probed yet\n");
		return ERROR_OK;
	}

	snprintf(buf, buf_size, "\nSPIFI flash information:\n"
		"  Device \'%s\' (ID 0x%08x)\n",
		lpcspifi_info->dev->name, lpcspifi_info->dev->device_id);

	return ERROR_OK;
}

struct flash_driver lpcspifi_flash = {
	.name = "lpcspifi",
	.flash_bank_command = lpcspifi_flash_bank_command,
	.erase = lpcspifi_erase,
	.protect = lpcspifi_protect,
	.write = lpcspifi_write,
	.read = default_flash_read,
	.probe = lpcspifi_probe,
	.auto_probe = lpcspifi_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = lpcspifi_protect_check,
	.info = get_lpcspifi_info,
};
