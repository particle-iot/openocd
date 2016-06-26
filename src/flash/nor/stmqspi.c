/***************************************************************************
 *   Copyright (C) 2010 by Antonio Borneo <borneo.antonio@gmail.com>,	   *
 *	   2016 by Andreas Bolsch <andreas.bolsch@mni.thm.de				   *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or	   *
 *   (at your option) any later version.								   *
 *																		   *
 *   This program is distributed in the hope that it will be useful,	   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of		   *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the		   *
 *   GNU General Public License for more details.						   *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

/* STM QuadSPI (QSPI) controller is a SPI bus controller
 * specifically designed for SPI memories.
 * Two working modes are available:
 * - indirect mode: the SPI is controlled by SW. Any custom commands can be sent
 *   on the bus.
 * - memory mapped mode: the SPI is under QSPI control. Memory content is directly
 *   accessible in CPU memory space. CPU can read and execute from memory
 *   (but not write to) */

/* ATTENTION:
 * To have flash mapped in CPU memory space, the QSPI controller
 * has to be in "memory mapped mode". This requires following constraints:
 * 1) The command "reset init" has to initialize QSPI controller and put
 *	it in memory mapped mode;
 * 2) every command in this file has to return to prompt in memory mapped mode. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include "spi.h"
#include <helper/time_support.h>
#include <target/algorithm.h>
#include <target/armv7m.h>
#include "stmqspi.h"

#define QSPI_BANK_SIZE	 0x01000000
#define QSPI_SEL_BANK0	 0x00000000 /* Select Bank0 */

#define QSPI_READ_REGB(a) (_QSPI_READ_REGB(a))
#define _QSPI_READ_REGB(a)			\
{									\
	int __a;						\
	uint8_t __v;					\
									\
	__a = target_read_u8(target, io_base + (a), &__v); \
	if (__a != ERROR_OK)			\
		return __a;					\
	__v;							\
}

#define QSPI_READ_REG(a) (_QSPI_READ_REG(a))
#define _QSPI_READ_REG(a)			\
{									\
	int __a;						\
	uint32_t __v;					\
									\
	__a = target_read_u32(target, io_base + (a), &__v); \
	if (__a != ERROR_OK)			\
		return __a;					\
	__v;							\
}

#define QSPI_WRITE_REG(a, v)		\
{									\
	int __r;						\
									\
	__r = target_write_u32(target, io_base + (a), (v)); \
	if (__r != ERROR_OK)			\
		return __r;					\
}

#define QSPI_POLL_BUSY(timeout)		\
{									\
	int __r;						\
									\
	__r = poll_busy(target, io_base, timeout); \
	if (__r != ERROR_OK)			\
		return __r;					\
}

#define QSPI_POLL_TCF(timeout)		\
{									\
	int __r;						\
									\
	__r = poll_tcf(target, io_base, timeout); \
	if (__r != ERROR_OK)			\
		return __r;					\
}

#define QSPI_SET_MM_MODE() QSPI_WRITE_REG(QSPI_CCR, \
	stmqspi_info->saved_ccr)

#define QSPI_CLEAR_TCF() QSPI_WRITE_REG(QSPI_FCR, (1<<QSPI_TCF))

/* saved ADSIZE<1:0> mask */
#define QSPI_ADDR_MASK (stmqspi_info->saved_ccr & QSPI_ADDR4)

/* QSPI_CCR values for various commands */
#define	QSPI_CCR_READ_STATUS ((QSPI_READ_REG(QSPI_CCR) & 0xF0000000) | \
	((QSPI_READ_MODE | QSPI_1LINE_MODE | QSPI_ADDR_MASK | SPIFLASH_READ_STATUS) & QSPI_NO_ADDR))

#define	QSPI_CCR_WRITE_STATUS ((QSPI_READ_REG(QSPI_CCR) & 0xF0000000) | \
	((QSPI_WRITE_MODE | QSPI_1LINE_MODE | QSPI_ADDR_MASK | SPIFLASH_WRITE_STATUS) & QSPI_NO_ADDR))

#define QSPI_CCR_WRITE_ENABLE ((QSPI_READ_REG(QSPI_CCR) & 0xF0000000) | \
	((QSPI_WRITE_MODE | QSPI_1LINE_MODE | QSPI_ADDR_MASK | SPIFLASH_WRITE_ENABLE) & \
	QSPI_NO_ADDR & QSPI_NO_DATA))

#define QSPI_CCR_SECTOR_ERASE ((QSPI_READ_REG(QSPI_CCR) & 0xF0000000) | \
	((QSPI_WRITE_MODE | QSPI_1LINE_MODE | QSPI_ADDR_MASK | \
	stmqspi_info->dev->erase_cmd) & QSPI_NO_DATA))

#define QSPI_CCR_MASS_ERASE ((QSPI_READ_REG(QSPI_CCR) & 0xF0000000) | \
	((QSPI_WRITE_MODE | QSPI_1LINE_MODE | QSPI_ADDR_MASK | \
	stmqspi_info->dev->chip_erase_cmd) & QSPI_NO_ADDR & QSPI_NO_DATA))

#define QSPI_CCR_PAGE_WRITE ((QSPI_READ_REG(QSPI_CCR) & 0xF0000000) | \
	((QSPI_WRITE_MODE | QSPI_1LINE_MODE | QSPI_ADDR_MASK | SPIFLASH_PAGE_PROGRAM)))

/* convert uint32_t into 4 uint8_t in target (i. e. little endian) byte order, re-inventing the wheel ... */
static inline uint32_t h_to_le_32(uint32_t val)
{
	union {
		uint32_t word;
		uint8_t byte[sizeof(uint32_t)];
	} res;

	res.byte[0] = val & 0xFF;
	res.byte[1] = (val>>8) & 0xFF;
	res.byte[2] = (val>>16) & 0xFF;
	res.byte[3] = (val>>24) & 0xFF;

	return res.word;
}

/* Timeout in ms */
#define QSPI_CMD_TIMEOUT		(100)
#define QSPI_PROBE_TIMEOUT		(100)
#define QSPI_MAX_TIMEOUT		(2000)
#define QSPI_MASS_ERASE_TIMEOUT	(400000)

struct stmqspi_flash_bank {
	int probed;
	uint32_t io_base;
	uint32_t bank_num;
	uint32_t saved_ccr;
	uint32_t dual_mask; /* FSEL and DFM bit mask in QUADSPI_CR */
	const struct flash_device *dev;
};

struct stmqspi_target {
	char *name;
	uint32_t device_id;
	uint32_t qspi_base;
	uint32_t io_base;
};

static const struct stmqspi_target target_devices[] = {
	/* name,				device_id, qspi_base,   io_base */
	{ "stm32l4x6",			0x415,	0x90000000,	0xA0001000 },
	{ "stm32f446",			0x421,	0x90000000,	0xA0001000 },
	{ "stm32f469_479",		0x434,	0x90000000,	0xA0001000 },
	{ "stm32f74x_75x",		0x449,	0x90000000,	0xA0001000 },
	{ "stm32f76x_77x",		0x451,	0x90000000,	0xA0001000 },
	{ NULL,					0x000,	0x00000000,	0x00000000 },
};

FLASH_BANK_COMMAND_HANDLER(stmqspi_flash_bank_command)
{
	struct stmqspi_flash_bank *stmqspi_info;

	LOG_DEBUG("%s", __func__);

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	stmqspi_info = malloc(sizeof(struct stmqspi_flash_bank));
	if (stmqspi_info == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	bank->driver_priv = stmqspi_info;
	stmqspi_info->probed = 0;

	return ERROR_OK;
}

/* Poll busy flag */
/* timeout in ms */
static int poll_busy(struct target *target, uint32_t io_base, int timeout)
{
	long long endtime;

	if ((QSPI_READ_REG(QSPI_SR) & (1<<QSPI_BUSY)) == 0)
		return ERROR_OK;

	endtime = timeval_ms() + timeout;
	do {
		alive_sleep(1);
		if ((QSPI_READ_REG(QSPI_SR) & (1<<QSPI_BUSY)) == 0)
			return ERROR_OK;
	} while (timeval_ms() < endtime);

	LOG_ERROR("Timeout while polling BUSY");
	return ERROR_FLASH_OPERATION_FAILED;
}

/* Poll transmit finished flag */
/* timeout in ms */
static int poll_tcf(struct target *target, uint32_t io_base, int timeout)
{
	long long endtime;

	if (QSPI_READ_REG(QSPI_SR) & (1<<QSPI_TCF))
		return ERROR_OK;

	endtime = timeval_ms() + timeout;
	do {
		alive_sleep(1);
		if (QSPI_READ_REG(QSPI_SR) & (1<<QSPI_TCF))
			return ERROR_OK;
	} while (timeval_ms() < endtime);

	LOG_ERROR("Timeout while polling TCF");
	return ERROR_FLASH_OPERATION_FAILED;
}

/* Read the status register of the external SPI flash chip(s). */
static int read_status_reg(struct flash_bank *bank, uint32_t *status)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;

	/* Wait for busy to be cleared */
	QSPI_POLL_BUSY(QSPI_PROBE_TIMEOUT);

	/* Clear transmit finished flag */
	QSPI_CLEAR_TCF();

	/* Read one byte per chip */
	QSPI_WRITE_REG(QSPI_DLR, stmqspi_info->dual_mask & QSPI_DUAL_FLASH ? 1 : 0);

	/* Read status */
	QSPI_WRITE_REG(QSPI_CCR, QSPI_CCR_READ_STATUS);

	/* Poll transmit finished flag */
	QSPI_POLL_TCF(QSPI_CMD_TIMEOUT);

	/* clear transmit finished flag */
	QSPI_CLEAR_TCF();

	*status = QSPI_READ_REG(QSPI_DR) & 0x0000FFFF;
	if ((stmqspi_info->dual_mask & QSPI_DUAL_FLASH) == 0) {
		if (stmqspi_info->dual_mask & (1<<QSPI_FSEL_FLASH)) {
			/* Mask out flash 1 */
			*status &= 0x0000FF00;
		} else {
			/* Mask out flash 0 */
			*status &= 0x000000FF;
		}
	}

	return ERROR_OK;
}

/* check for WIP (write in progress) bit(s) in status register(s) */
/* timeout in ms */
static int wait_till_ready(struct flash_bank *bank, int timeout)
{
	uint32_t status;
	int retval;
	long long endtime;

	endtime = timeval_ms() + timeout;
	do {
		/* Read flash status register(s) */
		retval = read_status_reg(bank, &status);
		if (retval != ERROR_OK)
			return retval;

		if ((status & (((1<<SPIFLASH_WIP)<<8) || (1<<SPIFLASH_WIP))) == 0)
			return ERROR_OK;
		alive_sleep(1);
	} while (timeval_ms() < endtime);

	LOG_ERROR("timeout");
	return ERROR_FAIL;
}

/* Send "write enable" command to SPI flash chip(s). */
static int qspi_write_enable(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	uint32_t status;
	int retval;

	/* Wait for busy to be cleared */
	QSPI_POLL_BUSY(QSPI_PROBE_TIMEOUT);

	/* clear transmit finished flag */
	QSPI_CLEAR_TCF();

	/* Send write enable command */
	QSPI_WRITE_REG(QSPI_CCR, QSPI_CCR_WRITE_ENABLE);

	/* Read flash status register */
	retval = read_status_reg(bank, &status);
	if (retval != ERROR_OK)
		return retval;

	/* Check write enabled for flash 1 */
	if (((stmqspi_info->dual_mask & ((1<<QSPI_DUAL_FLASH) | (1<<QSPI_FSEL_FLASH))) != (1<<QSPI_FSEL_FLASH))
		& ((status & (1<<SPIFLASH_WEL)) == 0)) {
		LOG_ERROR("Cannot enable write to flash1. Status=0x%08" PRIx32, status);
		return ERROR_FAIL;
	}

	/* Check write enabled for flash 2 */
	if (((stmqspi_info->dual_mask & ((1<<QSPI_DUAL_FLASH) | (1<<QSPI_FSEL_FLASH))) != 0)
		& (((status >> 8) & (1<<SPIFLASH_WEL)) == 0)) {
		LOG_ERROR("Cannot enable write to flash2. Status=0x%08" PRIx32, status);
		return ERROR_FAIL;
	}
	return ERROR_OK;
}

COMMAND_HANDLER(stmqspi_handle_mass_erase_command)
{
	struct target *target = NULL;
	struct flash_bank *bank;
	struct stmqspi_flash_bank *stmqspi_info;
	struct duration bench;
	uint32_t io_base, status;
	int retval, sector, dual;

	LOG_DEBUG("%s", __func__);

	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	stmqspi_info = bank->driver_priv;
	target = bank->target;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!(stmqspi_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	for (sector = 0; sector <= bank->num_sectors; sector++) {
		if (bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	io_base = stmqspi_info->io_base;
	duration_start(&bench);

	/* Abort any previous operation */
	QSPI_WRITE_REG(QSPI_CR, QSPI_READ_REG(QSPI_CR) | (1<<QSPI_ABORT));

	/* Wait for busy to be cleared */
	QSPI_POLL_BUSY(QSPI_PROBE_TIMEOUT);

	retval = qspi_write_enable(bank);
	if (retval != ERROR_OK)
		return retval;

	dual = (stmqspi_info->dual_mask & (1<<QSPI_DUAL_FLASH)) ? 1 : 0;

	/* Clear block protect bits */
	QSPI_WRITE_REG(QSPI_CCR, QSPI_CCR_WRITE_STATUS);
	QSPI_WRITE_REG(QSPI_DLR, dual);
	QSPI_WRITE_REG(QSPI_DR, (1<<SPIFLASH_WEL));
	if (dual)
		QSPI_WRITE_REG(QSPI_DR, (1<<SPIFLASH_WEL));

	/* Wait for busy to be cleared */
	QSPI_POLL_BUSY(QSPI_PROBE_TIMEOUT);

	/* clear transmit finished flag */
	QSPI_CLEAR_TCF();

	retval = qspi_write_enable(bank);
	if (retval != ERROR_OK)
		return retval;

	/* Send Mass Erase command */
	QSPI_WRITE_REG(QSPI_CCR, QSPI_CCR_MASS_ERASE);

	/* Read flash status register(s) */
	retval = read_status_reg(bank, &status);
	if (retval != ERROR_OK)
		return retval;

	/* Check for command in progress for flash 1 */
	if (((stmqspi_info->dual_mask & ((1<<QSPI_DUAL_FLASH) | (1<<QSPI_FSEL_FLASH))) != (1<<QSPI_FSEL_FLASH))
		& ((status & (1<<SPIFLASH_WEL)) == 0)) {
		LOG_ERROR("Mass erase command not accepted by flash1. Status=0x%08" PRIx32, status);
		return ERROR_FAIL;
	}

	/* Check for command in progress for flash 2 */
	if (((stmqspi_info->dual_mask & ((1<<QSPI_DUAL_FLASH) | (1<<QSPI_FSEL_FLASH))) != 0)
		& (((status >> 8) & (1<<SPIFLASH_WEL)) == 0)) {
		LOG_ERROR("Mass erase command not accepted by flash2. Status=0x%08" PRIx32, status);
		return ERROR_FAIL;
	}

	/* poll WIP for end of self timed Sector Erase cycle */
	retval = wait_till_ready(bank, QSPI_MASS_ERASE_TIMEOUT);

	/* Switch to memory mapped mode before return to prompt */
	QSPI_SET_MM_MODE();

	duration_measure(&bench);
	if (retval == ERROR_OK) {
		/* set all sectors as erased */
		for (sector = 0; sector < bank->num_sectors; sector++)
			bank->sectors[sector].is_erased = 1;

		command_print(CMD_CTX, "stmqspi mass erase completed in"
			" %fs (%0.3f KiB/s)", duration_elapsed(&bench),
			duration_kbps(&bench, bank->size));
	} else {
		command_print(CMD_CTX, "stmqspi mass erase failed after %fs",
			duration_elapsed(&bench));
	}

	return retval;
}

static int qspi_erase_sector(struct flash_bank *bank, int sector)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	int dual;
	int retval;

	retval = qspi_write_enable(bank);
	if (retval != ERROR_OK)
		return retval;

	dual = (stmqspi_info->dual_mask & (1<<QSPI_DUAL_FLASH)) ? 1 : 0;

	/* Wait for busy to be cleared */
	QSPI_POLL_BUSY(QSPI_PROBE_TIMEOUT);

	/* clear transmit finished flag */
	QSPI_CLEAR_TCF();

	/* Send Sector Erase command */
	QSPI_WRITE_REG(QSPI_CCR, QSPI_CCR_SECTOR_ERASE);

	/* Address is sector offset, this write initiates command transmission */
	QSPI_WRITE_REG(QSPI_AR, bank->sectors[sector].offset >> dual);

	/* poll WIP for end of self timed Sector Erase cycle */
	retval = wait_till_ready(bank, QSPI_MAX_TIMEOUT);

	/* erasure takes a long time, so some sort of progress message is a good idea */
	LOG_USER("sector %4d erased", sector);

	/* Switch to memory mapped mode before return to prompt */
	QSPI_SET_MM_MODE();

	return retval;
}

static int stmqspi_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	int retval = ERROR_OK;
	int sector;

	LOG_DEBUG("%s: from sector %d to sector %d", __func__, first, last);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!(stmqspi_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	if ((first < 0) || (last < first) || (last >= bank->num_sectors)) {
		LOG_ERROR("Flash sector invalid");
		return ERROR_FLASH_SECTOR_INVALID;
	}

	/* Abort any previous operation */
	QSPI_WRITE_REG(QSPI_CR, QSPI_READ_REG(QSPI_CR) | (1<<QSPI_ABORT));

	for (sector = first; sector <= last; sector++) {
		if (bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	for (sector = first; sector <= last; sector++) {
		retval = qspi_erase_sector(bank, sector);
		if (retval != ERROR_OK)
			break;
		keep_alive();
	}

	return retval;
}

static int stmqspi_protect(struct flash_bank *bank, int set,
	int first, int last)
{
	int sector;

	for (sector = first; sector <= last; sector++)
		bank->sectors[sector].is_protected = set;
	return ERROR_OK;
}

static int qspi_write_buffer(struct flash_bank *bank, struct working_area *write_algorithm,
	const uint32_t code_len, const uint32_t flash_offset, const uint32_t page_size,
	uint32_t count, const uint8_t *buffer)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	struct reg_param reg_params[4];
	struct armv7m_algorithm armv7m_info;
	uint32_t exit_point, remaining;
	int retval = ERROR_OK;

	LOG_DEBUG("%s: offset=0x%08" PRIx32 " len=0x%08" PRIx32,
		__func__, flash_offset, count);

	/* after breakpoint instruction (halfword) one nop (halfword) and
	 * 3 words follow till end of code, that makes exactly 4 words */
	exit_point = write_algorithm->address + code_len - 4 * sizeof(uint32_t);
	target_write_buffer(target, write_algorithm->address + code_len,
		count, buffer);

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT); /* count (in), count (out) */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* page_size */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);	/* offset into flash address */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);	/* QSPI io_base */

	buf_set_u32(reg_params[0].value, 0, 32, count);
	buf_set_u32(reg_params[1].value, 0, 32, page_size);
	buf_set_u32(reg_params[2].value, 0, 32, flash_offset);
	buf_set_u32(reg_params[3].value, 0, 32, io_base);

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;

	retval = target_start_algorithm(target, 0, NULL,
		sizeof(reg_params) / sizeof(struct reg_param), reg_params,
		write_algorithm->address, exit_point, &armv7m_info);

	retval = target_wait_algorithm(target, 0, NULL,
		sizeof(reg_params) / sizeof(struct reg_param), reg_params,
		exit_point, QSPI_MAX_TIMEOUT, &armv7m_info);

	remaining = buf_get_u32(reg_params[0].value, 0, 32);
	if ((retval == ERROR_OK) && remaining)
		retval = ERROR_FLASH_OPERATION_FAILED;
	if (retval != ERROR_OK) {
		LOG_ERROR("flash write failed at address 0x%" PRIx32 ", remaining 0x%" PRIx32,
			flash_offset, remaining);
	} else {
		/* programming takes a long time, so some sort of progress message is a good idea */
		LOG_USER("pages %6d to %6d programmed", flash_offset / page_size,
			(flash_offset + count - 1) / page_size);
	}

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);

	/* Switch to memory mapped mode before return to prompt */
	QSPI_SET_MM_MODE();

	return retval;
}

static int stmqspi_write(struct flash_bank *bank, const uint8_t *buffer,
	uint32_t flash_offset, uint32_t count)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	struct working_area *write_algorithm;
	uint32_t io_base = stmqspi_info->io_base;
	uint32_t cur_count, page_size, buffer_size;
	int sector;
	int dual;
	int retval = ERROR_OK;

	/* see contrib/loaders/flash/stmqspi.S for src */
	static const uint8_t stmqspi_flash_write_code[] = {
		0x01, 0x38, 0x01, 0x39, 0x1c, 0x00, 0x20, 0x34, 0x30, 0xa5, 0x00, 0x26,
		0x1f, 0x68, 0xff, 0x09, 0x00, 0xd3, 0x01, 0x36, 0x9f, 0x68, 0xbf, 0x09,
		0xfc, 0xd2, 0x02, 0x27, 0xdf, 0x60, 0x1e, 0x61, 0x27, 0x4f, 0x5f, 0x61,
		0x9f, 0x68, 0xff, 0x08, 0xfc, 0xd3, 0x27, 0x78, 0x7f, 0x08, 0xf1, 0xd2,
		0x36, 0x42, 0x05, 0xd0, 0x9f, 0x68, 0xff, 0x08, 0xfc, 0xd3, 0x27, 0x78,
		0x7f, 0x08, 0xe9, 0xd2, 0x9f, 0x68, 0xbf, 0x09, 0xfc, 0xd2, 0x02, 0x27,
		0xdf, 0x60, 0x1e, 0x4f, 0x5f, 0x61, 0x9f, 0x68, 0xbf, 0x09, 0xfc, 0xd2,
		0x02, 0x27, 0xdf, 0x60, 0x1e, 0x61, 0x19, 0x4f, 0x5f, 0x61, 0x9f, 0x68,
		0xff, 0x08, 0xfc, 0xd3, 0x27, 0x78, 0xbf, 0x08, 0x26, 0xd3, 0x36, 0x42,
		0x05, 0xd0, 0x9f, 0x68, 0xff, 0x08, 0xfc, 0xd3, 0x27, 0x78, 0xbf, 0x08,
		0x1e, 0xd3, 0x9f, 0x68, 0xbf, 0x09, 0xfc, 0xd2, 0x02, 0x27, 0xdf, 0x60,
		0x88, 0x42, 0x01, 0xd8, 0x18, 0x61, 0x00, 0xe0, 0x19, 0x61, 0x0e, 0x4f,
		0x5f, 0x61, 0x9a, 0x61, 0x9f, 0x68, 0xff, 0x08, 0xfc, 0xd3, 0x2f, 0x78,
		0x27, 0x70, 0x01, 0x35, 0x01, 0x32, 0x01, 0x38, 0x01, 0xd4, 0x0a, 0x42,
		0xf4, 0xd1, 0x9f, 0x68, 0xbf, 0x08, 0xfc, 0xd3, 0x00, 0x00, 0xaf, 0xd5,
		0x01, 0x30, 0x01, 0xe0, 0x00, 0x20, 0x01, 0x38, 0x00, 0xbe, 0xc0, 0x46,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	/* This will overlay the last 3 words of stmqspi_flash_write_code in target */
	uint32_t ccr_buffer[] = {
		h_to_le_32(QSPI_CCR_READ_STATUS),
		h_to_le_32(QSPI_CCR_WRITE_ENABLE),
		h_to_le_32(QSPI_CCR_PAGE_WRITE),
	};

	LOG_DEBUG("%s: offset=0x%08" PRIx32 " count=0x%08" PRIx32,
		__func__, flash_offset, count);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (flash_offset + count > stmqspi_info->dev->size_in_bytes) {
		LOG_WARNING("Write past end of flash. Extra data discarded.");
		count = stmqspi_info->dev->size_in_bytes - flash_offset;
	}

	/* Check sector protection */
	for (sector = 0; sector < bank->num_sectors; sector++) {
		/* Start offset in or before this sector? */
		/* End offset in or behind this sector? */
		if ((flash_offset < (bank->sectors[sector].offset + bank->sectors[sector].size))
			&& ((flash_offset + count - 1) >= bank->sectors[sector].offset)
			&& bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	dual = (stmqspi_info->dual_mask & (1<<QSPI_DUAL_FLASH)) ? 1 : 0;
	if (dual & ((flash_offset & 1) != 0 || (count & 1) != 0)) {
		LOG_ERROR("For dual-QSPI writes must be two byte aligned: "
			"%s: address=0x%08" PRIx32 " len=0x%08" PRIx32, __func__,
			flash_offset, count);
		return ERROR_FAIL;
	}
	page_size = stmqspi_info->dev->pagesize << dual;

	/* memory buffer, we assume sectorsize to be a power of 2 times page_size */
	buffer_size = stmqspi_info->dev->sectorsize << dual;
	while (target_alloc_working_area_try(target,
			sizeof(stmqspi_flash_write_code) + buffer_size,
			&write_algorithm) != ERROR_OK) {
		buffer_size /= 2;
		if (buffer_size < page_size) {
			/* we already allocated the writing code, but failed to get a
			 * buffer, free the algorithm */
			target_free_working_area(target, write_algorithm);

			LOG_WARNING("not enough working area, can't do QSPI page writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	};

	/* prepare flash write code, excluding ccr_buffer */
	retval = target_write_buffer(target, write_algorithm->address,
		sizeof(stmqspi_flash_write_code) - sizeof(ccr_buffer),
		stmqspi_flash_write_code);
	if (retval != ERROR_OK)
		goto err;

	/* prepare QSPI_CCR register values */
	retval = target_write_buffer(target, write_algorithm->address
		+ sizeof(stmqspi_flash_write_code) - sizeof(ccr_buffer),
		sizeof(ccr_buffer), (uint8_t *) ccr_buffer);
	if (retval != ERROR_OK)
		goto err;

	/* Abort any previous operation */
	QSPI_WRITE_REG(QSPI_CR, QSPI_READ_REG(QSPI_CR) | (1<<QSPI_ABORT));

	/* buffer head not aligned to page size */
	if (count > 0 && (flash_offset & (page_size - 1)) != 0) {
		cur_count = page_size - (flash_offset & (page_size - 1));
		if (cur_count > count)
			cur_count = count;
		retval = qspi_write_buffer(bank, write_algorithm,
			sizeof(stmqspi_flash_write_code), flash_offset,
			page_size, cur_count, buffer);
		if (retval != ERROR_OK)
			goto err;
		flash_offset += cur_count;
		buffer += cur_count;
		count -= cur_count;
	}

	/* central part, aligned to page size */
	while (count > 0) {
		/* clip block at buffer_size */
		if (count > buffer_size)
			cur_count = buffer_size;
		else
			cur_count = count;

		retval = qspi_write_buffer(bank, write_algorithm,
			sizeof(stmqspi_flash_write_code), flash_offset,
			page_size, cur_count, buffer);
		if (retval != ERROR_OK)
			goto err;

		flash_offset += cur_count;
		buffer += cur_count;
		count -= cur_count;

		keep_alive();
	}

err:
	target_free_working_area(target, write_algorithm);

	return retval;
}

/* Return ID of flash device(s) */
/* On exit, indirect mode is kept */
static int read_flash_id(struct flash_bank *bank, uint32_t *id1, uint32_t *id2)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	int shift, retval;

	if ((target->state != TARGET_HALTED) && (target->state != TARGET_RESET)) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* poll WIP */
	retval = wait_till_ready(bank, QSPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	/* Clear transmit finished flag */
	QSPI_CLEAR_TCF();

	/* Read three bytes per chip */
	QSPI_WRITE_REG(QSPI_DLR, (stmqspi_info->dual_mask & (1<<QSPI_DUAL_FLASH) ? 6 : 3) - 1);

	/* Read id */
	QSPI_WRITE_REG(QSPI_CCR, (QSPI_READ_REG(QSPI_CCR) & 0xF0000000) |
	   ((QSPI_READ_MODE | QSPI_1LINE_MODE | QSPI_ADDR3 | SPIFLASH_READ_ID) & QSPI_NO_ADDR));

	/* Poll transmit finished flag */
	QSPI_POLL_TCF(QSPI_CMD_TIMEOUT);

	/* Clear transmit finished flag */
	QSPI_CLEAR_TCF();

	/* Read ID from Data Register */
	for (shift = 0; shift <= 16; shift += 8) {
		if ((stmqspi_info->dual_mask & ((1<<QSPI_DUAL_FLASH) |
			(1<<QSPI_FSEL_FLASH))) != (1<<QSPI_FSEL_FLASH)) {
			*id1 |= (QSPI_READ_REGB(QSPI_DR) << shift);
			if (!*id1) {
				LOG_ERROR("No response from QSPI flash1");
				return ERROR_FAIL;
			}

		}
		if ((stmqspi_info->dual_mask & ((1<<QSPI_DUAL_FLASH) |
			(1<<QSPI_FSEL_FLASH))) != 0) {
			*id2 |= (QSPI_READ_REGB(QSPI_DR) << shift);
			if (!*id2) {
				LOG_ERROR("No response from QSPI flash2");
				return ERROR_FAIL;
			}
		}
	}

	/* Switch to memory mapped mode before return to prompt */
	QSPI_SET_MM_MODE();

	return ERROR_OK;
}

static int stmqspi_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t device_id, io_base;
	struct flash_sector *sectors;
	uint32_t id1 = 0, id2 = 0;
	const struct stmqspi_target *target_device;
	const struct flash_device *p;
	int dual;
	int retval;

	if (stmqspi_info->probed)
		free(bank->sectors);
	stmqspi_info->probed = 0;

	/* read stm32 device id register, but use only bits 11 down to 0 */
	retval = target_read_u32(target, 0xE0042000, &device_id);
	if (retval != ERROR_OK)
		return retval;
	device_id &= 0xFFF;

	for (target_device = target_devices ; target_device->name ; ++target_device)
		if (target_device->device_id == device_id)
			break;

	if (!target_device->name) {
		LOG_ERROR("Device ID 0x%" PRIx32 " is not known as STM QSPI capable",
				target->tap->idcode);
		return ERROR_FAIL;
	}

	switch (bank->base - target_device->qspi_base) {
		case 0*QSPI_BANK_SIZE:
			stmqspi_info->bank_num = QSPI_SEL_BANK0;
			break;
		default:
			LOG_ERROR("Invalid QSPI base address 0x%" PRIx32, bank->base);
			return ERROR_FAIL;
	}
	io_base = target_device->io_base;
	stmqspi_info->io_base = io_base;

	/* save current FSEL and DFM bits in QSPI_CR */
	stmqspi_info->dual_mask = QSPI_READ_REG(QSPI_CR);
	/* save current QSPI_CCR value */;
	stmqspi_info->saved_ccr = QSPI_READ_REG(QSPI_CCR);
	LOG_DEBUG("Valid QSPI on device %s at 0x%" PRIx32 ", QSPI_CR 0x%"
		PRIx32 ", QSPI_CCR 0x%" PRIx32 ",%s", target_device->name,
		bank->base, stmqspi_info->dual_mask, stmqspi_info->saved_ccr,
		(QSPI_ADDR_MASK == QSPI_ADDR4) ? "4 byte addr" : " 3 byte addr");

	/* read and decode flash ID; returns in memory mapped mode */
	retval = read_flash_id(bank, &id1, &id2);
	QSPI_SET_MM_MODE();
	LOG_DEBUG("id1 0x%06X, id2 0x%06" PRIx32, id1, id2);

	if (retval != ERROR_OK)
		return retval;

	/* identify flash1 */
	stmqspi_info->dev = NULL;
	for (p = flash_devices; id1 && p->name ; p++) {
		if (p->device_id == id1) {
			stmqspi_info->dev = p;
			LOG_INFO("Found flash1 device \'%s\' (ID 0x%06" PRIx32 ")",
				p->name, id1);
			break;
		}
	}

	if (id1 && !p->name) {
		LOG_ERROR("Unknown flash1 device (ID 0x%06" PRIx32 ")", id1);
		return ERROR_FAIL;
	}

	/* identify flash2 */
	for (p = flash_devices; id2 && p->name ; p++) {
		if (p->device_id == id2) {
			LOG_INFO("Found flash2 device \'%s\' (ID 0x%06" PRIx32 ")",
				p->name, id2);

			if (!stmqspi_info->dev)
				stmqspi_info->dev = p;
			else {
				if ((stmqspi_info->dev->erase_cmd != p->erase_cmd) ||
					(stmqspi_info->dev->chip_erase_cmd != p->chip_erase_cmd) ||
					(stmqspi_info->dev->pagesize != p->pagesize) ||
					(stmqspi_info->dev->sectorsize != p->sectorsize) ||
					(stmqspi_info->dev->size_in_bytes != p->size_in_bytes)) {
					LOG_ERROR("Incompatible flash1/flash2 devices");
					return ERROR_FAIL;
				}
			}
			break;
		}
	}

	if (id2 && !p->name) {
		LOG_ERROR("Unknown flash2 device (ID 0x%06" PRIx32 ")", id2);
		return ERROR_FAIL;
	}

	dual = (stmqspi_info->dual_mask & (1<<QSPI_DUAL_FLASH)) ? 1 : 0;

	/* Set correct size value */
	bank->size = stmqspi_info->dev->size_in_bytes << dual;

	/* create and fill sectors array */
	bank->num_sectors =
		stmqspi_info->dev->size_in_bytes / stmqspi_info->dev->sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (sectors == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset = sector * (stmqspi_info->dev->sectorsize << dual);
		sectors[sector].size = (stmqspi_info->dev->sectorsize << dual);
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 1;
	}

	bank->sectors = sectors;
	stmqspi_info->probed = 1;

	return ERROR_OK;
}

static int stmqspi_auto_probe(struct flash_bank *bank)
{
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	if (stmqspi_info->probed)
		return ERROR_OK;
	return stmqspi_probe(bank);
}

static int stmqspi_protect_check(struct flash_bank *bank)
{
	/* Nothing to do. Protection is only handled in SW. */
	return ERROR_OK;
}

static int get_stmqspi_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;

	if (!(stmqspi_info->probed)) {
		snprintf(buf, buf_size,
			"\nQSPI flash bank not probed yet\n");
		return ERROR_OK;
	}

	snprintf(buf, buf_size, "\nQSPI flash information:"
		" Device \'%s\' (ID 0x%06" PRIx32 ")\n",
		stmqspi_info->dev->name, stmqspi_info->dev->device_id);

	return ERROR_OK;
}

static const struct command_registration stmqspi_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.handler = stmqspi_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Mass erase entire flash device.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration stmqspi_command_handlers[] = {
	{
		.name = "stmqspi",
		.mode = COMMAND_ANY,
		.help = "stmqspi flash command group",
		.usage = "",
		.chain = stmqspi_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver stmqspi_flash = {
	.name = "stmqspi",
	.commands = stmqspi_command_handlers,
	.flash_bank_command = stmqspi_flash_bank_command,
	.erase = stmqspi_erase,
	.protect = stmqspi_protect,
	.write = stmqspi_write,
	.read = default_flash_read,
	.probe = stmqspi_probe,
	.auto_probe = stmqspi_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = stmqspi_protect_check,
	.info = get_stmqspi_info,
	.usage = "",
};
