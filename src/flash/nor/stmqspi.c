/***************************************************************************
 *   Copyright (C) 2016 - 2018 by Andreas Bolsch                           *
 *   andreas.bolsch@mni.thm.de                                             *
 *                                                                         *
 *   Copyright (C) 2010 by Antonio Borneo                                  *
 *   borneo.antonio@gmail.com                                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or	   *
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

/* STM QuadSPI (QSPI) and OctoSPI (OCTOSPI) controller are SPI bus controllers
 * specifically designed for SPI memories.
 * Two working modes are available:
 * - indirect mode: the SPI is controlled by SW. Any custom commands can be sent
 *   on the bus.
 * - memory mapped mode: the SPI is under QSPI/OCTOSPI control. Memory content
 *   is directly accessible in CPU memory space. CPU can read and execute from
 *   memory (but not write to) */

/* ATTENTION:
 * To have flash mapped in CPU memory space, the QSPI/OCTOSPI controller
 * has to be in "memory mapped mode". This requires following constraints:
 * 1) The command "reset init" has to initialize QSPI/OCTOSPI controller and put
 *    it in memory mapped mode;
 * 2) every command in this file has to return to prompt in memory mapped mode. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/time_support.h>
#include <target/algorithm.h>
#include <target/armv7m.h>
#include "stmqspi.h"

#define READ_REG(a)											\
({															\
	uint32_t __v;											\
															\
	retval = target_read_u32(target, io_base + (a), &__v);	\
	(retval == ERROR_OK) ? __v : 0x0;						\
})

/* saved mode settings */
#define QSPI_MODE (stmqspi_info->saved_ccr & \
	(0xF0000000U | QSPI_DCYC_MASK | QSPI_4LINE_MODE | QSPI_ALTB_MODE | QSPI_ADDR4))

/* QSPI_CCR for various commands, these never use dummy cycles nor alternate bytes */
#define	QSPI_CCR_READ_STATUS \
	((QSPI_MODE & ~QSPI_DCYC_MASK & QSPI_NO_ADDR & QSPI_NO_ALTB) | \
	(QSPI_READ_MODE | SPIFLASH_READ_STATUS))

#define	QSPI_CCR_READ_ID \
	((QSPI_MODE & ~QSPI_DCYC_MASK & QSPI_NO_ADDR & QSPI_NO_ALTB) | \
	(QSPI_READ_MODE | SPIFLASH_READ_ID))

#define QSPI_CCR_WRITE_ENABLE \
	((QSPI_MODE & ~QSPI_DCYC_MASK & QSPI_NO_ADDR & QSPI_NO_ALTB & QSPI_NO_DATA) | \
	(QSPI_WRITE_MODE | SPIFLASH_WRITE_ENABLE))

#define QSPI_CCR_SECTOR_ERASE \
	((QSPI_MODE & ~QSPI_DCYC_MASK & QSPI_NO_ALTB & QSPI_NO_DATA) | \
	(QSPI_WRITE_MODE | stmqspi_info->dev.erase_cmd))

#define QSPI_CCR_MASS_ERASE \
	((QSPI_MODE & ~QSPI_DCYC_MASK & QSPI_NO_ADDR & QSPI_NO_ALTB & QSPI_NO_DATA) | \
	(QSPI_WRITE_MODE | stmqspi_info->dev.chip_erase_cmd))

#define QSPI_CCR_PAGE_PROG \
	((QSPI_MODE & ~QSPI_DCYC_MASK & QSPI_NO_ALTB) | \
	(QSPI_WRITE_MODE | stmqspi_info->dev.pprog_cmd))

/* saved mode settings */
#define OCTOSPI_MODE (stmqspi_info->saved_cr & 0xCFFFFFFF)

#define OPI_MODE ((stmqspi_info->saved_ccr & OCTOSPI_ISIZE_MASK) != 0)

#define OCTOSPI_MODE_CCR (stmqspi_info->saved_ccr & \
	(0xF0000000U | OCTOSPI_8LINE_MODE | OCTOSPI_ALTB_MODE | OCTOSPI_ADDR4))

/* OCTOSPI_CCR for various commands, these never use alternate bytes	*
 * for READ_STATUS and READ_ID, 4-byte address 0 and 4 dummy bytes must	*
 * be sent in OPI mode													*/
#define OPI_DUMMY 4U

#define	OCTOSPI_CCR_READ_STATUS \
	((OCTOSPI_MODE_CCR & OCTOSPI_NO_DDTR & \
	(OPI_MODE ? ~0U : OCTOSPI_NO_ADDR) & OCTOSPI_NO_ALTB))

#define	OCTOSPI_CCR_READ_ID \
	((OCTOSPI_MODE_CCR & OCTOSPI_NO_DDTR & \
	(OPI_MODE ? ~0U : OCTOSPI_NO_ADDR) & OCTOSPI_NO_ALTB))

#define OCTOSPI_CCR_WRITE_ENABLE \
	((OCTOSPI_MODE_CCR & OCTOSPI_NO_ADDR & OCTOSPI_NO_ALTB & OCTOSPI_NO_DATA))

#define OCTOSPI_CCR_SECTOR_ERASE \
	((OCTOSPI_MODE_CCR & OCTOSPI_NO_ALTB & OCTOSPI_NO_DATA))

#define OCTOSPI_CCR_MASS_ERASE \
	((OCTOSPI_MODE_CCR & OCTOSPI_NO_ADDR & OCTOSPI_NO_ALTB & OCTOSPI_NO_DATA))

#define OCTOSPI_CCR_PAGE_PROG \
	((OCTOSPI_MODE_CCR & QSPI_NO_ALTB))

#define SPI_ADSIZE (((stmqspi_info->saved_ccr >> SPI_ADSIZE_POS) & 0x3) + 1)

#define OPI_CMD(cmd) (OPI_MODE ? ((((uint16_t) cmd)<<8) | (~cmd & 0xFF)) : cmd)

#define OCTOSPI_CMD(mode, ccr, ir)										\
({																		\
	retval = target_write_u32(target, io_base + OCTOSPI_CR,				\
		OCTOSPI_MODE | mode);											\
	if (retval == ERROR_OK)												\
		retval = target_write_u32(target, io_base + OCTOSPI_TCR,		\
			(stmqspi_info->saved_tcr & ~OCTOSPI_DCYC_MASK) |			\
			((OPI_MODE && (mode == OCTOSPI_READ_MODE)) ?				\
			(OPI_DUMMY<<OCTOSPI_DCYC_POS) : 0));						\
	if (retval == ERROR_OK)												\
		retval = target_write_u32(target, io_base + OCTOSPI_CCR, ccr);	\
	if (retval == ERROR_OK)												\
		retval = target_write_u32(target, io_base + OCTOSPI_IR,			\
			OPI_CMD(ir));												\
	retval;																\
})

/* convert uint32_t into 4 uint8_t in little endian byte order */
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

/* convert uint32_t into 4 uint8_t in big endian byte order */
static inline uint32_t h_to_be_32(uint32_t val)
{
	union {
		uint32_t word;
		uint8_t byte[sizeof(uint32_t)];
	} res;

	res.byte[0] = (val>>24) & 0xFF;
	res.byte[1] = (val>>16) & 0xFF;
	res.byte[2] = (val>>8) & 0xFF;
	res.byte[3] = val & 0xFF;

	return res.word;
}

/* Timeout in ms */
#define SPI_CMD_TIMEOUT			(100)
#define SPI_PROBE_TIMEOUT		(100)
#define SPI_MAX_TIMEOUT			(2000)
#define SPI_MASS_ERASE_TIMEOUT	(400000)

struct sector_info {
	uint32_t address;
	uint32_t size;
	uint32_t result;
};

struct stmqspi_flash_bank {
	int probed;
	char devname[32];
	bool octo;
	struct flash_device dev;
	uint32_t io_base;
	uint32_t saved_cr;	/* FSEL and DFM bit mask in QUADSPI_CR / OCTOSPI_CR */
	uint32_t saved_ccr;
	uint32_t saved_tcr;	/* only for OCTOSPI */
	uint32_t saved_ir;	/* only for OCTOSPI */
};

FLASH_BANK_COMMAND_HANDLER(stmqspi_flash_bank_command)
{
	struct stmqspi_flash_bank *stmqspi_info;
	uint32_t io_base;

	LOG_DEBUG("%s", __func__);

	if (CMD_ARGC < 7)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], bank->base);
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[6], io_base);

	stmqspi_info = malloc(sizeof(struct stmqspi_flash_bank));
	if (stmqspi_info == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	bank->driver_priv = stmqspi_info;
	stmqspi_info->probed = 0;
	stmqspi_info->io_base = io_base;

	return ERROR_OK;
}

/* Poll busy flag */
/* timeout in ms */
static int poll_busy(struct flash_bank *bank, int timeout)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	int retval;
	long long endtime;

	if ((READ_REG(SPI_SR) & (1<<SPI_BUSY)) == 0)
		return ERROR_OK;

	endtime = timeval_ms() + timeout;
	do {
		alive_sleep(1);
		if ((READ_REG(SPI_SR) & (1<<SPI_BUSY)) == 0) {
			/* Clear transmit finished flag */
			retval = target_write_u32(target, io_base + SPI_FCR, (1<<SPI_TCF));
			return retval;
		} else
			LOG_DEBUG("busy: 0x%08X", READ_REG(SPI_SR));
	} while (timeval_ms() < endtime);

	LOG_ERROR("Timeout while polling BUSY");
	return ERROR_FLASH_OPERATION_FAILED;
}

/* Set to memory-mapped mode, e. g. after an error */
static int set_mm_mode(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	int retval;

	/* Reset Address register bits 0 and 1, see various errata sheets */
	retval = target_write_u32(target, io_base + SPI_AR, 0x0);
	if (retval != ERROR_OK)
		return retval;

	/* Abort any previous operation */
	retval = target_write_u32(target, io_base + SPI_CR,
		READ_REG(SPI_CR) | (1<<SPI_ABORT));
	if (retval != ERROR_OK)
		return retval;

	/* Wait for busy to be cleared */
	retval = poll_busy(bank, SPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	/* Finally switch to memory mapped mode */
	if (IS_OCTOSPI) {
		retval = target_write_u32(target, io_base + OCTOSPI_CR,
			OCTOSPI_MODE | OCTOSPI_MM_MODE);
		if (retval == ERROR_OK)
			retval = target_write_u32(target, io_base + OCTOSPI_CCR,
				stmqspi_info->saved_ccr);
		if (retval == ERROR_OK)
			retval = target_write_u32(target, io_base + OCTOSPI_TCR,
				stmqspi_info->saved_tcr);
		if (retval == ERROR_OK)
			retval = target_write_u32(target, io_base + OCTOSPI_IR,
				stmqspi_info->saved_ir);
	} else {
		retval = target_write_u32(target, io_base + QSPI_CCR, stmqspi_info->saved_ccr);
	}
	return retval;
}

/* Read the status register of the external SPI flash chip(s). */
static int read_status_reg(struct flash_bank *bank, uint32_t *status)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	uint8_t data;
	int retval;

	/* Wait for busy to be cleared */
	retval = poll_busy(bank, SPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		goto err;

	/* Read one byte per chip */
	retval = target_write_u32(target, io_base + SPI_DLR,
		(stmqspi_info->saved_cr & (1<<SPI_DUAL_FLASH)) ? 1 : 0);
	if (retval != ERROR_OK)
		goto err;

	/* Read status */
	if (IS_OCTOSPI)
		retval = OCTOSPI_CMD(OCTOSPI_READ_MODE, OCTOSPI_CCR_READ_STATUS, SPIFLASH_READ_STATUS);
	else
		retval = target_write_u32(target, io_base + QSPI_CCR, QSPI_CCR_READ_STATUS);

	/* Dummy address 0, only required for 8-line mode */
	retval = target_write_u32(target, io_base + SPI_AR, 0);
	if (retval != ERROR_OK)
		goto err;

	if (retval != ERROR_OK)
		goto err;

	*status = 0;

	if ((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) | (1<<SPI_FSEL_FLASH)))
		!= (1<<SPI_FSEL_FLASH)) {
		/* get status of flash 1 in dual or flash 1 only */
		retval = target_read_u8(target, io_base + SPI_DR, &data);
		if (retval != ERROR_OK)
			goto err;
		*status |= data;
	}

	if ((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) | (1<<SPI_FSEL_FLASH)))
		!= (0<<SPI_FSEL_FLASH)) {
		/* get status of flash 2 in dual or flash 2 only */
		retval = target_read_u8(target, io_base + SPI_DR, &data);
		if (retval != ERROR_OK)
			goto err;
		*status |= data << 8;
	}

	LOG_DEBUG("flash status regs: 0x%04" PRIx16, *status);

err:
	return retval;
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

		if ((status & ((SPIFLASH_BSY_BIT << 8) || SPIFLASH_BSY_BIT)) == 0)
			return retval;
		alive_sleep(25);
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

	/* Abort any previous operation */
	retval = target_write_u32(target, io_base + SPI_CR,
		READ_REG(SPI_CR) | (1<<SPI_ABORT));
	if (retval != ERROR_OK)
		return retval;

	/* Wait for busy to be cleared */
	retval = poll_busy(bank, SPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		goto err;

	/* Send write enable command */
	if (IS_OCTOSPI)
		retval = OCTOSPI_CMD(OCTOSPI_WRITE_MODE, OCTOSPI_CCR_WRITE_ENABLE, SPIFLASH_WRITE_ENABLE);
	else
		retval = target_write_u32(target, io_base + QSPI_CCR, QSPI_CCR_WRITE_ENABLE);
	if (retval != ERROR_OK)
		goto err;

	/* Dummy address 0, only required for 8-line mode */
	retval = target_write_u32(target, io_base + SPI_AR, 0);
	if (retval != ERROR_OK)
		goto err;

	/* Read flash status register */
	retval = read_status_reg(bank, &status);
	if (retval != ERROR_OK)
		goto err;

	/* Check write enabled for flash 1 */
	if (((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) | (1<<SPI_FSEL_FLASH)))
		!= (1<<SPI_FSEL_FLASH)) && ((status & SPIFLASH_WE_BIT) == 0)) {
		LOG_ERROR("Cannot write enable flash1. Status=0x%02" PRIx8, status & 0xFF);
		return ERROR_FAIL;
	}

	/* Check write enabled for flash 2 */
	if (((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) | (1<<SPI_FSEL_FLASH)))
		!= (0<<SPI_FSEL_FLASH)) && (((status >> 8) & SPIFLASH_WE_BIT) == 0)) {
		LOG_ERROR("Cannot write enable flash2. Status=0x%02" PRIx8, (status >> 8) & 0xFF);
		return ERROR_FAIL;
	}

err:
	return retval;
}

COMMAND_HANDLER(stmqspi_handle_mass_erase_command)
{
	struct target *target = NULL;
	struct flash_bank *bank;
	struct stmqspi_flash_bank *stmqspi_info;
	struct duration bench;
	uint32_t io_base, status;
	int retval, sector;

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

	if (stmqspi_info->dev.chip_erase_cmd == 0x00) {
		LOG_ERROR("Mass erase not available for this device");
		return ERROR_FLASH_OPER_UNSUPPORTED;
	}

	for (sector = 0; sector <= bank->num_sectors; sector++) {
		if (bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	io_base = stmqspi_info->io_base;
	duration_start(&bench);

	retval = qspi_write_enable(bank);
	if (retval != ERROR_OK)
		goto err;

	/* Send Mass Erase command */
	if (IS_OCTOSPI)
		retval = OCTOSPI_CMD(OCTOSPI_WRITE_MODE, OCTOSPI_CCR_MASS_ERASE,
			stmqspi_info->dev.chip_erase_cmd);
	else
		retval = target_write_u32(target, io_base + QSPI_CCR, QSPI_CCR_MASS_ERASE);
	if (retval != ERROR_OK)
		goto err;

	/* Read flash status register(s) */
	retval = read_status_reg(bank, &status);
	if (retval != ERROR_OK)
		goto err;

	/* Check for command in progress for flash 1 */
	if (((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) | (1<<SPI_FSEL_FLASH)))
		!= (1<<SPI_FSEL_FLASH)) && ((status & SPIFLASH_BSY_BIT) == 0)) {
		LOG_ERROR("Mass erase command not accepted by flash1. Status=0x%04" PRIx16, status);
		retval = ERROR_FAIL;
		goto err;
	}

	/* Check for command in progress for flash 2 */
	if (((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) | (1<<SPI_FSEL_FLASH)))
		!= (0<<SPI_FSEL_FLASH)) && (((status >> 8) & SPIFLASH_BSY_BIT) == 0)) {
		LOG_ERROR("Mass erase command not accepted by flash2. Status=0x%04" PRIx16, status);
		retval = ERROR_FAIL;
		goto err;
	}

	/* Poll WIP for end of self timed Sector Erase cycle */
	retval = wait_till_ready(bank, SPI_MASS_ERASE_TIMEOUT);

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

err:
	/* Switch to memory mapped mode before return to prompt */
	set_mm_mode(bank);

	return retval;
}

static int log2u(uint32_t word)
{
	int result;

	for (result = 0; (unsigned int) result < sizeof(unsigned long) * 8; result++)
		if (word == (1UL<<result))
			return result;

	return -1;
}

COMMAND_HANDLER(stmqspi_handle_setparms)
{
	struct flash_bank *bank = NULL;
	struct target *target = NULL;
	struct stmqspi_flash_bank *stmqspi_info = NULL;
	struct flash_sector *sectors = NULL;
	uint32_t io_base, temp;
	int dual, fsize, retval;

	LOG_DEBUG("%s", __func__);

	if ((CMD_ARGC < 6) || (CMD_ARGC > 9))
		return ERROR_COMMAND_SYNTAX_ERROR;

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	target = bank->target;
	stmqspi_info = bank->driver_priv;

	/* invalidate all old info */
	if (stmqspi_info->probed)
		free(bank->sectors);
	bank->size = 0;
	bank->num_sectors = 0;
	bank->sectors = NULL;
	stmqspi_info->probed = 0;
	memset(&stmqspi_info->dev, 0, sizeof(stmqspi_info->dev));
	stmqspi_info->dev.name = "unknown";

	strncpy(stmqspi_info->devname, CMD_ARGV[1], sizeof(stmqspi_info->devname) - 1);
	stmqspi_info->devname[sizeof(stmqspi_info->devname) - 1] = '\0';

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], temp);
	stmqspi_info->dev.size_in_bytes = temp;
	if (log2u(stmqspi_info->dev.size_in_bytes) < 8) {
		command_print(CMD_CTX, "stmqspi: device size must be 2^n with n >= 8");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[3], stmqspi_info->dev.pagesize);
	if ((log2u(stmqspi_info->dev.pagesize) > log2u(stmqspi_info->dev.size_in_bytes)) ||
		(log2u(stmqspi_info->dev.pagesize) < 0)) {
		command_print(CMD_CTX, "stmqspi: page size must be 2^n and <= device size");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[4], stmqspi_info->dev.read_cmd);
	if ((stmqspi_info->dev.read_cmd != 0x03) &&
		(stmqspi_info->dev.read_cmd != 0x13)) {
		command_print(CMD_CTX, "stmqspi: only 0x03/0x13 READ cmd allowed");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[5], stmqspi_info->dev.pprog_cmd);
	if ((stmqspi_info->dev.pprog_cmd != 0x02) &&
		(stmqspi_info->dev.pprog_cmd != 0x12)) {
		command_print(CMD_CTX, "stmqspi: only 0x02/0x12 PPRG cmd allowed");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (CMD_ARGC > 6)
		COMMAND_PARSE_NUMBER(u8, CMD_ARGV[6], stmqspi_info->dev.chip_erase_cmd);
	else
		stmqspi_info->dev.chip_erase_cmd = 0x00;

	if (CMD_ARGC > 7) {
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[7], temp);
		stmqspi_info->dev.sectorsize = temp;
		if ((log2u(stmqspi_info->dev.sectorsize) > log2u(stmqspi_info->dev.size_in_bytes)) ||
			(log2u(stmqspi_info->dev.sectorsize) < 0)) {
			command_print(CMD_CTX, "stmqspi: sector size must be 2^n and <= device size");
			return ERROR_COMMAND_SYNTAX_ERROR;
		}

		if (CMD_ARGC > 8)
			COMMAND_PARSE_NUMBER(u8, CMD_ARGV[8], stmqspi_info->dev.erase_cmd);
		else
			return ERROR_COMMAND_SYNTAX_ERROR;
	} else {
		/* no sector size / sector erase cmd given, treat whole bank as a single sector */
		stmqspi_info->dev.erase_cmd = 0x00;
		stmqspi_info->dev.sectorsize = stmqspi_info->dev.size_in_bytes;
	}

	dual = (stmqspi_info->saved_cr & (1<<SPI_DUAL_FLASH)) ? 1 : 0;

	/* set correct size value */
	bank->size = stmqspi_info->dev.size_in_bytes << dual;

	io_base = stmqspi_info->io_base;
	fsize = (READ_REG(SPI_DCR)>>SPI_FSIZE_POS) & ((1U<<SPI_FSIZE_LEN) - 1);
	LOG_DEBUG("FSIZE = 0x%04x", fsize);
	if (bank->size != (1U<<(fsize + 1)))
		LOG_WARNING("FSIZE field in QSPI_DCR(1) doesn't match actual capacity.");

	/* create and fill sectors array */
	bank->num_sectors =
		stmqspi_info->dev.size_in_bytes / stmqspi_info->dev.sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (sectors == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset = sector * (stmqspi_info->dev.sectorsize << dual);
		sectors[sector].size = stmqspi_info->dev.sectorsize << dual;
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 0;
	}

	bank->sectors = sectors;
	stmqspi_info->dev.name = stmqspi_info->devname;
	stmqspi_info->probed = 1;

	return ERROR_OK;
}

COMMAND_HANDLER(stmqspi_handle_spicmd)
{
	struct target *target = NULL;
	struct flash_bank *bank;
	struct stmqspi_flash_bank *stmqspi_info = NULL;
	uint32_t io_base, addr;
	uint8_t num_write, num_read, cmd_byte, data;
	unsigned int count;
	const int max = 21;
	char temp[4], output[(2 + max + 256) * 3 + 8];
	int retval;

	LOG_DEBUG("%s", __func__);

	if ((CMD_ARGC < 3) || (CMD_ARGC > max + 3))
		return ERROR_COMMAND_SYNTAX_ERROR;

	num_write = CMD_ARGC - 2;
	if (num_write > max) {
		LOG_ERROR("at most %d bytes may be send", max);
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	target = bank->target;
	stmqspi_info = bank->driver_priv;
	io_base = stmqspi_info->io_base;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[1], num_read);
	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[2], cmd_byte);

	if (stmqspi_info->saved_cr & (1<<SPI_DUAL_FLASH)) {
		if ((num_write & 1) == 0) {
			LOG_ERROR("number of data bytes to write must be even in dual mode");
			return ERROR_COMMAND_SYNTAX_ERROR;
		}
		if ((num_read & 1) != 0) {
			LOG_ERROR("number of bytes to read must be even in dual mode");
			return ERROR_COMMAND_SYNTAX_ERROR;
		}
	}

	if (((num_write < 1) || (num_write > 5)) && (num_read > 0)) {
		LOG_ERROR("one cmd and up to four addr bytes must be send when reading");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	/* Abort any previous operation */
	retval = target_write_u32(target, io_base + SPI_CR,
		READ_REG(SPI_CR) | (1<<SPI_ABORT));
	if (retval != ERROR_OK)
		return retval;

	/* Wait for busy to be cleared */
	retval = poll_busy(bank, SPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	/* send command byte */
	snprintf(output, sizeof(output), "spicmd: %02x ", cmd_byte);
	if (num_read == 0) {
		/* write, send cmd byte */
		retval = target_write_u32(target, io_base + SPI_DLR, ((uint32_t) num_write) - 2);
		if (retval != ERROR_OK)
			goto err;

		if (IS_OCTOSPI)
			retval = OCTOSPI_CMD(OCTOSPI_WRITE_MODE,
				(OCTOSPI_MODE_CCR & OCTOSPI_NO_ALTB & OCTOSPI_NO_ADDR &
				((num_write == 1) ? OCTOSPI_NO_DATA : ~0U)), cmd_byte);
		else
			retval = target_write_u32(target, io_base + QSPI_CCR,
				(QSPI_MODE & ~QSPI_DCYC_MASK & QSPI_NO_ALTB & QSPI_NO_ADDR &
				((num_write == 1) ? QSPI_NO_DATA : ~0U)) |
				(QSPI_WRITE_MODE | cmd_byte));
		if (retval != ERROR_OK)
			goto err;

		/* send additional data bytes */
		for (count = 3; count < CMD_ARGC; count++) {
			COMMAND_PARSE_NUMBER(u8, CMD_ARGV[count], data);
			snprintf(temp, sizeof(temp), "%02x ", data);
			retval = target_write_u8(target, io_base + SPI_DR, data); \

			/* on stm32h743, target_write_u8 returns ERROR_FAIL, even if the
			 * write was successfull ... */
			if (retval == ERROR_FAIL)
				retval = ERROR_OK;

			if (retval != ERROR_OK)
				goto err;
			strncat(output, temp, sizeof(output) - strlen(output) - 1);
		}
		strncat(output, "-> ", sizeof(output) - strlen(output) - 1);
	} else {
		/* read, pack additional bytes into address */
		addr = 0;
		for (count = 3; count < CMD_ARGC; count++) {
			COMMAND_PARSE_NUMBER(u8, CMD_ARGV[count], data);
			snprintf(temp, sizeof(temp), "%02x ", data);
			addr = (addr << 8) | data;
			strncat(output, temp, sizeof(output) - strlen(output) - 1);
		}
		strncat(output, "-> ", sizeof(output) - strlen(output) - 1);

		/* send cmd byte, if ADMODE indicates no address, this already triggers command */
		retval = target_write_u32(target, io_base + SPI_DLR, ((uint32_t) num_read) - 1);
		if (retval != ERROR_OK)
			goto err;
		if (IS_OCTOSPI)
			retval = OCTOSPI_CMD(OCTOSPI_READ_MODE,
				(OCTOSPI_MODE_CCR & OCTOSPI_NO_DDTR & OCTOSPI_NO_ALTB &
				((num_write == 1) ? OCTOSPI_NO_ADDR : ~0U)) |
				(((num_write - 2) & 0x3U)<<SPI_ADSIZE_POS), cmd_byte);
		else
			retval = target_write_u32(target, io_base + QSPI_CCR,
				(QSPI_MODE & ~QSPI_DCYC_MASK & QSPI_NO_ALTB & ((num_write == 1) ? QSPI_NO_ADDR : ~0U)) |
				((QSPI_READ_MODE | (((num_write - 2) & 0x3U)<<SPI_ADSIZE_POS) | cmd_byte)));
		if (retval != ERROR_OK)
			goto err;

		if (num_write > 1) {
			/* if ADMODE indicates address required, only the write to AR triggers command */
			retval = target_write_u32(target, io_base + SPI_AR, addr);
			if (retval != ERROR_OK)
				goto err;
		}

		/* read response bytes */
		for ( ; num_read > 0; num_read--) {
			retval = target_read_u8(target, io_base + SPI_DR, &data);
			if (retval != ERROR_OK)
				goto err;
			snprintf(temp, sizeof(temp), "%02x ", data);
			strncat(output, temp, sizeof(output) - strlen(output) - 1);
		}
	}
	command_print(CMD_CTX, "%s", output);

err:
	/* Switch to memory mapped mode before return to prompt */
	set_mm_mode(bank);
	return retval;
}

static int qspi_erase_sector(struct flash_bank *bank, int sector)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	uint32_t status;
	int retval;

	/* Abort any previous operation */
	retval = target_write_u32(target, io_base + SPI_CR,
		READ_REG(SPI_CR) | (1<<SPI_ABORT));
	if (retval != ERROR_OK)
		return retval;

	/* Wait for busy to be cleared */
	retval = poll_busy(bank, SPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		goto err;

	retval = qspi_write_enable(bank);
	if (retval != ERROR_OK)
		goto err;

	/* Wait for busy to be cleared */
	retval = poll_busy(bank, SPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		goto err;

	/* Send Sector Erase command */
	if (IS_OCTOSPI)
		retval = OCTOSPI_CMD(OCTOSPI_WRITE_MODE, OCTOSPI_CCR_SECTOR_ERASE,
			stmqspi_info->dev.erase_cmd);
	else
		retval = target_write_u32(target, io_base + QSPI_CCR, QSPI_CCR_SECTOR_ERASE);
	if (retval != ERROR_OK)
		goto err;

	/* Address is sector offset, this write initiates command transmission */
	retval = target_write_u32(target, io_base + SPI_AR, bank->sectors[sector].offset);
	if (retval != ERROR_OK)
		goto err;

	/* Read flash status register(s) */
	retval = read_status_reg(bank, &status);
	if (retval != ERROR_OK)
		goto err;

	LOG_DEBUG("erase status regs: 0x%04" PRIx16, status);

	/* Check for command in progress for flash 1 */
	if (((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) | (1<<SPI_FSEL_FLASH)))
		!= (1<<SPI_FSEL_FLASH)) && ((status & SPIFLASH_BSY_BIT) == 0)) {
		LOG_ERROR("Sector erase command not accepted by flash1. Status=0x%04" PRIx16, status);
		retval = ERROR_FAIL;
		goto err;
	}

	/* Check for command in progress for flash 2 */
	if (((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) | (1<<SPI_FSEL_FLASH)))
		!= (0<<SPI_FSEL_FLASH)) && (((status >> 8) & SPIFLASH_BSY_BIT) == 0)) {
		LOG_ERROR("Sector erase command not accepted by flash2. Status=0x%04" PRIx16, status);
		retval = ERROR_FAIL;
		goto err;
	}

	/* Poll WIP for end of self timed Sector Erase cycle */
	retval = wait_till_ready(bank, SPI_MAX_TIMEOUT);

	/* Erase takes a long time, so some sort of progress message is a good idea */
	LOG_DEBUG("sector %4d erased", sector);

err:
	/* Switch to memory mapped mode before return to prompt */
	set_mm_mode(bank);

	return retval;
}

static int stmqspi_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
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

	if (stmqspi_info->dev.erase_cmd == 0x00) {
		LOG_ERROR("Sector erase not available for this device");
		return ERROR_FLASH_OPER_UNSUPPORTED;
	}

	if ((first < 0) || (last < first) || (last >= bank->num_sectors)) {
		LOG_ERROR("Flash sector invalid");
		return ERROR_FLASH_SECTOR_INVALID;
	}

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

	if (retval != ERROR_OK)
		LOG_ERROR("Flash sector_erase failed on sector %d", sector);

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

/* Check whether flash is blank */
static int stmqspi_blank_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	struct duration bench;
	struct reg_param reg_params[1];
	struct armv7m_algorithm armv7m_info;
	struct working_area *erase_check_algorithm = NULL;
	struct sector_info *sectors = NULL;
	uint32_t buffer_size, exit_point, result;
	int num_sectors, sector, index, count, retval;
	const uint32_t erased = 0xFF;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!(stmqspi_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	/* Abort any previous operation */
	retval = target_write_u32(target, io_base + SPI_CR,
		READ_REG(SPI_CR) | (1<<SPI_ABORT));
	if (retval != ERROR_OK)
		return retval;

	/* Wait for busy to be cleared */
	retval = poll_busy(bank, SPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	/* see contrib/loaders/erase_check/stmqspi_erase_check.S for src */
	static const uint8_t stmqspi_erase_check_code[] = {
		0x0a, 0xa1, 0x0a, 0x68, 0x4b, 0x68, 0x8c, 0x68, 0x15, 0x78, 0xff, 0x26,
		0x36, 0x02, 0x2e, 0x43, 0x34, 0x40, 0x2d, 0x02, 0x2c, 0x43, 0x01, 0x32,
		0x01, 0x3b, 0xf5, 0xd1, 0x4b, 0x60, 0x8c, 0x60, 0x0c, 0x31, 0x01, 0x38,
		0xed, 0xd1, 0xc0, 0x46, 0x00, 0xbe, 0xc0, 0x46
	};

	num_sectors = bank->num_sectors;
	while (buffer_size = sizeof(stmqspi_erase_check_code) + num_sectors * sizeof(struct sector_info),
		target_alloc_working_area_try(target, buffer_size, &erase_check_algorithm) != ERROR_OK) {
		num_sectors /= 2;
		if (num_sectors < 2) {
			LOG_WARNING("not enough working area, can't do SPI blank check");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	/* prepare check code */
	retval = target_write_buffer(target, erase_check_algorithm->address,
		sizeof(stmqspi_erase_check_code), stmqspi_erase_check_code);
	if (retval != ERROR_OK)
		goto err;

	duration_start(&bench);

	/* after breakpoint instruction (halfword) one nop (halfword) till end of code */
	exit_point = erase_check_algorithm->address + sizeof(stmqspi_erase_check_code)
		- sizeof(uint32_t);

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);	/* size */

	sectors = malloc(sizeof(struct sector_info) * num_sectors);
	if (sectors == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	sector = 0;
	while (sector < bank->num_sectors) {
		/* at most num_sectors sectors to handle in one run */
		count = bank->num_sectors - sector;
		if (count > num_sectors)
			count = num_sectors;

		for (index = 0; index < count; index++) {
			sectors[index].address = h_to_le_32(bank->base + bank->sectors[sector + index].offset);
			sectors[index].size = h_to_le_32(bank->sectors[sector + index].size);
			sectors[index].result = h_to_le_32(erased);
		}

		retval = target_write_buffer(target, erase_check_algorithm->address
			+ sizeof(stmqspi_erase_check_code), sizeof(struct sector_info) * count,
			(uint8_t *) sectors);
		if (retval != ERROR_OK)
			goto err;

		buf_set_u32(reg_params[0].value, 0, 32, count);

		armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
		armv7m_info.core_mode = ARM_MODE_THREAD;

		LOG_DEBUG("checking sectors %d to %d", sector, sector + count - 1);
		/* check a block of sectors */
		retval = target_run_algorithm(target,
			0, NULL,
			1, reg_params,
			erase_check_algorithm->address, exit_point,
			count * (sectors[0].size>>10),
			&armv7m_info);
		if (retval != ERROR_OK)
			break;

		retval = target_read_buffer(target, erase_check_algorithm->address
			+ sizeof(stmqspi_erase_check_code), sizeof(struct sector_info) * count,
			(uint8_t *) sectors);
		if (retval != ERROR_OK)
			goto err;

		for (index = 0; index < count; index++) {
			if ((sectors[index].address != h_to_le_32(bank->base + bank->sectors[sector + index].offset)) ||
				(sectors[index].size != 0)) {
				LOG_ERROR("corrupted blank check info for sector %d", sector + index);
				goto err;
			}

			result = h_to_le_32(sectors[index].result);
			bank->sectors[sector + index].is_erased = ((result & 0xFF) == 0xFF);
			LOG_DEBUG("Flash sector %d checked: %04x", sector + index, result & 0xFFFF);
		}
		keep_alive();
		sector += count;
	}

	destroy_reg_param(&reg_params[0]);

	duration_measure(&bench);
	LOG_INFO("stmqspi blank check completed in"
			" %fs (%0.3f KiB/s)", duration_elapsed(&bench),
			duration_kbps(&bench, bank->size));

err:
	target_free_working_area(target, erase_check_algorithm);
	if (sectors)
		free(sectors);

	return retval;
}

static int qspi_write_block(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	struct reg_param reg_params[6];
	struct armv7m_algorithm armv7m_info;
	struct working_area *write_algorithm;
	uint32_t page_size, fifo_start, fifo_size, buffer_size, exit_point;
	uint32_t write_code_size, remaining;
	const uint8_t *write_code = NULL;
	int dual, retval = ERROR_OK;

	LOG_DEBUG("%s: offset=0x%08" PRIx32 " len=0x%08" PRIx32,
		__func__, offset, count);

	/* see contrib/loaders/flash/stmqspi_write.S for src */
	static const uint8_t stmqspi_write_code[] = {
		0x01, 0x38, 0x01, 0x39, 0x3f, 0x4c, 0x1f, 0x68, 0x7f, 0x06, 0xff, 0x0f,
		0xba, 0x46, 0x02, 0x25, 0x1f, 0x68, 0x2f, 0x43, 0x1f, 0x60, 0x20, 0x25,
		0x1d, 0x44, 0x9f, 0x68, 0xbf, 0x09, 0xfc, 0xd2, 0x02, 0x27, 0xdf, 0x60,
		0x1e, 0x61, 0x2b, 0x4f, 0x5f, 0x61, 0x2f, 0x78, 0x7f, 0x08, 0xee, 0xd2,
		0x57, 0x46, 0x3f, 0x42, 0x02, 0xd0, 0x2f, 0x78, 0x7f, 0x08, 0xe8, 0xd2,
		0x00, 0x42, 0x3f, 0xd4, 0x9f, 0x68, 0xbf, 0x09, 0xfc, 0xd2, 0x02, 0x27,
		0xdf, 0x60, 0x26, 0x4f, 0x5f, 0x61, 0x9f, 0x68, 0xbf, 0x09, 0xfc, 0xd2,
		0x02, 0x27, 0xdf, 0x60, 0x57, 0x46, 0x1f, 0x61, 0x1d, 0x4f, 0x5f, 0x61,
		0x2f, 0x78, 0xbf, 0x08, 0x2a, 0xd3, 0x57, 0x46, 0x3f, 0x42, 0x02, 0xd0,
		0x2f, 0x78, 0xbf, 0x08, 0x24, 0xd3, 0x9f, 0x68, 0xbf, 0x09, 0xfc, 0xd2,
		0x02, 0x27, 0xdf, 0x60, 0x07, 0x46, 0x88, 0x42, 0x00, 0xd9, 0x0f, 0x46,
		0x1f, 0x61, 0x1b, 0x4f, 0x5f, 0x61, 0x9a, 0x61, 0x1c, 0x4f, 0x00, 0x2f,
		0x16, 0xd0, 0xbc, 0x42, 0xfa, 0xd0, 0x27, 0x78, 0x2f, 0x70, 0x01, 0x32,
		0x01, 0x34, 0x4c, 0x45, 0x00, 0xd3, 0x44, 0x46, 0x17, 0xa7, 0x3c, 0x60,
		0x01, 0x38, 0x01, 0xd4, 0x0a, 0x42, 0xed, 0xd1, 0x9f, 0x68, 0xbf, 0x09,
		0xfc, 0xd2, 0x02, 0x27, 0xdf, 0x60, 0xa8, 0xe7, 0x00, 0x20, 0x02, 0x38,
		0x01, 0x30, 0x02, 0x26, 0x1f, 0x68, 0x37, 0x43, 0x1f, 0x60, 0xc0, 0x46,
		0x00, 0xbe, 0xc0, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};

	/* see contrib/loaders/flash/stmoctospi_write.S for src */
	static const uint8_t stmoctospi_write_code[] = {
		0x01, 0x38, 0x01, 0x39, 0x50, 0x4c, 0x1f, 0x68, 0x7f, 0x06, 0xff, 0x0f,
		0xba, 0x46, 0x02, 0x25, 0x1f, 0x68, 0x2f, 0x43, 0x1f, 0x60, 0x50, 0x25,
		0x1d, 0x44, 0x38, 0x4e, 0x1e, 0x44, 0x1f, 0x6a, 0xbf, 0x09, 0xfc, 0xd2,
		0x02, 0x27, 0x5f, 0x62, 0x3a, 0x4f, 0x1f, 0x60, 0x57, 0x46, 0x1f, 0x64,
		0x39, 0x4f, 0x37, 0x60, 0x39, 0x4f, 0xb7, 0x60, 0x39, 0x4f, 0x37, 0x61,
		0x00, 0x27, 0x9f, 0x64, 0x2f, 0x78, 0x7f, 0x08, 0xe3, 0xd2, 0x57, 0x46,
		0x3f, 0x42, 0x02, 0xd0, 0x2f, 0x78, 0x7f, 0x08, 0xdd, 0xd2, 0x00, 0x42,
		0x56, 0xd4, 0x1f, 0x6a, 0xbf, 0x09, 0xfc, 0xd2, 0x02, 0x27, 0x5f, 0x62,
		0x30, 0x4f, 0x1f, 0x60, 0x30, 0x4f, 0x37, 0x60, 0x30, 0x4f, 0xb7, 0x60,
		0x30, 0x4f, 0x37, 0x61, 0x1f, 0x6a, 0xbf, 0x09, 0xfc, 0xd2, 0x02, 0x27,
		0x5f, 0x62, 0x26, 0x4f, 0x1f, 0x60, 0x57, 0x46, 0x1f, 0x64, 0x25, 0x4f,
		0x37, 0x60, 0x25, 0x4f, 0xb7, 0x60, 0x25, 0x4f, 0x37, 0x61, 0x00, 0x27,
		0x9f, 0x64, 0x2f, 0x78, 0xbf, 0x08, 0x33, 0xd3, 0x57, 0x46, 0x3f, 0x42,
		0x02, 0xd0, 0x2f, 0x78, 0xbf, 0x08, 0x2d, 0xd3, 0x1f, 0x6a, 0xbf, 0x09,
		0xfc, 0xd2, 0x02, 0x27, 0x5f, 0x62, 0x21, 0x4f, 0x1f, 0x60, 0x07, 0x46,
		0x88, 0x42, 0x00, 0xd9, 0x0f, 0x46, 0x1f, 0x64, 0x1e, 0x4f, 0x37, 0x60,
		0x1e, 0x4f, 0xb7, 0x60, 0x1e, 0x4f, 0x37, 0x61, 0x9a, 0x64, 0x1e, 0x4f,
		0x00, 0x2f, 0x19, 0xd0, 0xbc, 0x42, 0xfa, 0xd0, 0x27, 0x78, 0x2f, 0x70,
		0x01, 0x32, 0x01, 0x34, 0x4c, 0x45, 0x00, 0xd3, 0x44, 0x46, 0x19, 0xa7,
		0x3c, 0x60, 0x01, 0x38, 0x01, 0xd4, 0x0a, 0x42, 0xed, 0xd1, 0x1f, 0x6a,
		0xbf, 0x09, 0xfc, 0xd2, 0x02, 0x27, 0x5f, 0x62, 0x89, 0xe7, 0x00, 0x00,
		0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x02, 0x38, 0x01, 0x30, 0x02, 0x26,
		0x1f, 0x68, 0x37, 0x43, 0x1f, 0x60, 0xc0, 0x46, 0x00, 0xbe, 0xc0, 0x46,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	/* This will overlay the last 12 words of stmqspi/stmoctospi_write_code in target */
	uint32_t ccr_buffer[][4] = {
		{
			h_to_le_32(OCTOSPI_MODE | OCTOSPI_READ_MODE),
			h_to_le_32(IS_OCTOSPI ? OCTOSPI_CCR_READ_STATUS : QSPI_CCR_READ_STATUS),
			h_to_le_32((stmqspi_info->saved_tcr & ~OCTOSPI_DCYC_MASK) |
						(OPI_MODE ? (OPI_DUMMY<<OCTOSPI_DCYC_POS) : 0)),
			h_to_le_32(OPI_CMD(SPIFLASH_READ_STATUS)),
		},
		{
			h_to_le_32(OCTOSPI_MODE | OCTOSPI_WRITE_MODE),
			h_to_le_32(IS_OCTOSPI ? OCTOSPI_CCR_WRITE_ENABLE : QSPI_CCR_WRITE_ENABLE),
			h_to_le_32(stmqspi_info->saved_tcr & ~OCTOSPI_DCYC_MASK),
			h_to_le_32(OPI_CMD(SPIFLASH_WRITE_ENABLE)),
		},
		{
			h_to_le_32(OCTOSPI_MODE | OCTOSPI_WRITE_MODE),
			h_to_le_32(IS_OCTOSPI ? OCTOSPI_CCR_PAGE_PROG : QSPI_CCR_PAGE_PROG),
			h_to_le_32(stmqspi_info->saved_tcr & ~OCTOSPI_DCYC_MASK),
			h_to_le_32(OPI_CMD(stmqspi_info->dev.pprog_cmd)),
		},
	};

	if (IS_OCTOSPI) {
		write_code = stmoctospi_write_code;
		write_code_size = sizeof(stmoctospi_write_code);
	} else {
		write_code = stmqspi_write_code;
		write_code_size = sizeof(stmqspi_write_code);
	}

	/* memory buffer, we assume sectorsize to be a power of 2 times page_size */
	dual = (stmqspi_info->saved_cr & (1<<SPI_DUAL_FLASH)) ? 1 : 0;
	page_size = stmqspi_info->dev.pagesize << dual;
	fifo_size = stmqspi_info->dev.sectorsize << dual;
	while (buffer_size = write_code_size + 2 * sizeof(uint32_t) + fifo_size,
			target_alloc_working_area_try(target, buffer_size, &write_algorithm) != ERROR_OK) {
		fifo_size /= 2;
		if (fifo_size < page_size) {
			LOG_WARNING("not enough working area, can't do QSPI page writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	};

	/* prepare flash write code, excluding ccr_buffer */
	retval = target_write_buffer(target, write_algorithm->address,
		write_code_size - sizeof(ccr_buffer), write_code);
	if (retval != ERROR_OK)
		goto err;

	/* prepare QSPI/OCTOSPI_CCR register values */
	retval = target_write_buffer(target, write_algorithm->address
		+ write_code_size - sizeof(ccr_buffer),
		sizeof(ccr_buffer), (uint8_t *) ccr_buffer);
	if (retval != ERROR_OK)
		goto err;

	/* target buffer starts right after flash_write_code, i. e.
	 * wp and rp are implicitly included in buffer!!! */
	fifo_start = write_algorithm->address + write_code_size + 2 * sizeof(uint32_t);

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT); /* count (in), status (out) */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* page_size */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_IN_OUT);	/* offset into flash address */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);	/* QSPI/OCTOSPI io_base */
	init_reg_param(&reg_params[4], "r8", 32, PARAM_OUT);	/* fifo start */
	init_reg_param(&reg_params[5], "r9", 32, PARAM_OUT);	/* fifo end + 1 */

	buf_set_u32(reg_params[0].value, 0, 32, count);
	buf_set_u32(reg_params[1].value, 0, 32, page_size);
	buf_set_u32(reg_params[2].value, 0, 32, offset);
	buf_set_u32(reg_params[3].value, 0, 32, io_base);
	buf_set_u32(reg_params[4].value, 0, 32, fifo_start);
	buf_set_u32(reg_params[5].value, 0, 32, fifo_start + fifo_size);

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;

	/* after breakpoint instruction (halfword) one nop (halfword) and
	 * ccr_buffer follow till end of code */
	exit_point = write_algorithm->address + write_code_size
	 - (sizeof(ccr_buffer) + sizeof(uint32_t));

	retval = target_run_flash_async_algorithm(target, buffer, count, 1,
			0, NULL,
			6, reg_params,
			write_algorithm->address + write_code_size,
			fifo_size + 2 * sizeof(uint32_t),
			write_algorithm->address, exit_point,
			&armv7m_info);

	remaining = buf_get_u32(reg_params[0].value, 0, 32);
	if ((retval == ERROR_OK) && remaining)
		retval = ERROR_FLASH_OPERATION_FAILED;

	if (retval != ERROR_OK) {
		offset = buf_get_u32(reg_params[2].value, 0, 32);
		LOG_ERROR("flash write failed at address 0x%" PRIx32 ", remaining 0x%" PRIx32,
			offset, remaining);
	}

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);
	destroy_reg_param(&reg_params[5]);

err:
	target_free_working_area(target, write_algorithm);

	/* Switch to memory mapped mode before return to prompt */
	set_mm_mode(bank);

	return retval;
}

static int stmqspi_write(struct flash_bank *bank, const uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	int retval, sector, dual, octal_dtr;

	LOG_DEBUG("%s: offset=0x%08" PRIx32 " count=0x%08" PRIx32,
		__func__, offset, count);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset + count > bank->size) {
		LOG_WARNING("Write beyond end of flash. Extra data discarded.");
		count = bank->size - offset;
	}

	/* Check sector protection */
	for (sector = 0; sector < bank->num_sectors; sector++) {
		/* Start offset in or before this sector? */
		/* End offset in or behind this sector? */
		if ((offset < (bank->sectors[sector].offset + bank->sectors[sector].size))
			&& ((offset + count - 1) >= bank->sectors[sector].offset)
			&& bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	dual = (stmqspi_info->saved_cr & (1<<SPI_DUAL_FLASH)) ? 1 : 0;
	octal_dtr = IS_OCTOSPI && (stmqspi_info->saved_ccr & (1<<OCTOSPI_DDTR));
	if ((dual || octal_dtr) & ((offset & 1) != 0 || (count & 1) != 0)) {
		LOG_ERROR("In dual-QSPI and octal-DTR modes writes must be two byte aligned: "
			"%s: address=0x%08" PRIx32 " len=0x%08" PRIx32, __func__, offset, count);
		return ERROR_FAIL;
	}

	/* Abort any previous operation */
	retval = target_write_u32(target, io_base + SPI_CR,
		READ_REG(SPI_CR) | (1<<SPI_ABORT));
	if (retval != ERROR_OK)
		return retval;

	/* Wait for busy to be cleared */
	retval = poll_busy(bank, SPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	return qspi_write_block(bank, buffer, offset, count);
}

/* Return ID of flash device(s) */
/* On exit, indirect mode is kept */
static int read_flash_id(struct flash_bank *bank, uint32_t *id1, uint32_t *id2)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	uint32_t io_base = stmqspi_info->io_base;
	int shift, retval;
	uint8_t byte;

	if ((target->state != TARGET_HALTED) && (target->state != TARGET_RESET)) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Abort any previous operation */
	retval = target_write_u32(target, io_base + SPI_CR,
		READ_REG(SPI_CR) | (1<<SPI_ABORT));
	if (retval != ERROR_OK)
		goto err;

	/* Poll WIP */
	retval = wait_till_ready(bank, SPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		goto err;

	/* Wait for busy to be cleared */
	retval = poll_busy(bank, SPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		goto err;

	/* Read three bytes per chip */
	retval = target_write_u32(target, io_base + SPI_DLR,
		(stmqspi_info->saved_cr & (1<<SPI_DUAL_FLASH) ? 6 : 3) - 1);
	if (retval != ERROR_OK)
		goto err;

	/* Read id */
	if (IS_OCTOSPI)
		retval = OCTOSPI_CMD(OCTOSPI_READ_MODE, OCTOSPI_CCR_READ_ID, SPIFLASH_READ_ID);
	else
		retval = target_write_u32(target, io_base + QSPI_CCR, QSPI_CCR_READ_ID);
	if (retval != ERROR_OK)
		goto err;

	/* Dummy address 0, only required for 8-line mode */
	retval = target_write_u32(target, io_base + SPI_AR, 0);
	if (retval != ERROR_OK)
		goto err;

	/* Read ID from Data Register */
	for (shift = 0; shift <= 16; shift += 8) {
		if ((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) |
			(1<<SPI_FSEL_FLASH))) != (1<<SPI_FSEL_FLASH)) {
			retval = target_read_u8(target, io_base + SPI_DR, &byte);
			if (retval != ERROR_OK)
				goto err;
			*id1 |= ((uint32_t) byte) << shift;
		}
		if ((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) |
			(1<<SPI_FSEL_FLASH))) != 0) {
			retval = target_read_u8(target, io_base + SPI_DR, &byte);
			if (retval != ERROR_OK)
				goto err;
			*id2 |= ((uint32_t) byte) << shift;
		}
	}

	if ((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) |
		(1<<SPI_FSEL_FLASH))) != (1<<SPI_FSEL_FLASH)) {
		if (!*id1)
			LOG_WARNING("No response from SPI flash1");
	}
	if ((stmqspi_info->saved_cr & ((1<<SPI_DUAL_FLASH) |
		(1<<SPI_FSEL_FLASH))) != 0) {
		if (!*id2)
			LOG_WARNING("No response from SPI flash2");
	}

err:
	/* Switch to memory mapped mode before return to prompt */
	set_mm_mode(bank);

	return retval;
}

static int stmqspi_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct stmqspi_flash_bank *stmqspi_info = bank->driver_priv;
	struct flash_sector *sectors = NULL;
	uint32_t io_base = stmqspi_info->io_base;
	uint32_t id1 = 0, id2 = 0;
	const struct flash_device *p;
	int dual, fsize, retval;

	if (stmqspi_info->probed)
		free(bank->sectors);
	bank->size = 0;
	bank->num_sectors = 0;
	bank->sectors = NULL;
	stmqspi_info->probed = 0;
	memset(&stmqspi_info->dev, 0, sizeof(stmqspi_info->dev));
	stmqspi_info->dev.name = "unknown";

	stmqspi_info->octo = (READ_REG(OCTOSPI_MAGIC) == OCTO_MAGIC_ID);

	/* save current FSEL and DFM bits in QSPI/OCTOSPI_CR, current QSPI/OCTOSPI_CCR value */
	stmqspi_info->saved_cr = READ_REG(SPI_CR);
	if (retval == ERROR_OK)
		stmqspi_info->saved_ccr = READ_REG(SPI_CCR);

	if (IS_OCTOSPI) {
		uint32_t mtyp;

		mtyp = ((READ_REG(OCTOSPI_DCR1) & OCTOSPI_MTYP_MASK))>>OCTOSPI_MTYP_POS;
		if (retval == ERROR_OK)
			stmqspi_info->saved_tcr = READ_REG(OCTOSPI_TCR);
		if (retval == ERROR_OK)
			stmqspi_info->saved_ir = READ_REG(OCTOSPI_IR);
		if ((mtyp != 0x0) && (mtyp != 0x1)) {
			retval = ERROR_FAIL;
			LOG_ERROR("Only regular SPI protocol supported in OCTOSPI");
		}
		if (retval == ERROR_OK) {
			LOG_DEBUG("OCTOSPI at 0x%08" PRIx32 ", io_base at 0x%08" PRIx32 ", OCTOSPI_CR 0x%08"
				PRIx32 ", OCTOSPI_CCR 0x%08" PRIx32 ", %d-byte addr", bank->base, io_base,
				stmqspi_info->saved_cr, stmqspi_info->saved_ccr, SPI_ADSIZE);
		} else {
			LOG_ERROR("No OCTOSPI at io_base 0x%08" PRIx32, io_base);
			stmqspi_info->probed = 0;
			stmqspi_info->dev.name = "none";
			return ERROR_FAIL;
		}
	} else {
		/* check that QSPI is actually present at all */
		if (retval == ERROR_OK) {
			LOG_DEBUG("QSPI at 0x%08" PRIx32 ", io_base at 0x%08" PRIx32 ", QSPI_CR 0x%08"
				PRIx32 ", QSPI_CCR 0x%08" PRIx32 ", %d-byte addr", bank->base, io_base,
				stmqspi_info->saved_cr, stmqspi_info->saved_ccr, SPI_ADSIZE);
		} else {
			LOG_ERROR("No QSPI at io_base 0x%08" PRIx32, io_base);
			stmqspi_info->probed = 0;
			stmqspi_info->dev.name = "none";
			return ERROR_FAIL;
		}
	}

	/* read and decode flash ID; returns in memory mapped mode */
	retval = read_flash_id(bank, &id1, &id2);
	set_mm_mode(bank);
	LOG_DEBUG("id1 0x%06" PRIx32 ", id2 0x%06" PRIx32, id1, id2);

	if (retval != ERROR_OK)
		return retval;

	/* identify flash1 */
	for (p = flash_devices; id1 && p->name ; p++) {
		if (p->device_id == id1) {
			memcpy(&stmqspi_info->dev, p, sizeof(stmqspi_info->dev));
			LOG_INFO("flash1 \'%s\' id = 0x%06" PRIx32 " size = %lukbytes",
				 p->name, id1, p->size_in_bytes>>10);
			break;
		}
	}

	if (id1 && !p->name)
		LOG_WARNING("Unknown flash1 device id = 0x%06" PRIx32, id1);

	/* identify flash2 */
	for (p = flash_devices; id2 && p->name ; p++) {
		if (p->device_id == id2) {
			LOG_INFO("flash2 \'%s\' id = 0x%06" PRIx32 " size = %lukbytes",
				 p->name, id2, p->size_in_bytes>>10);

			if (!stmqspi_info->dev.name)
				memcpy(&stmqspi_info->dev, p, sizeof(stmqspi_info->dev));
			else
				if ((stmqspi_info->dev.erase_cmd != p->erase_cmd) ||
					(stmqspi_info->dev.chip_erase_cmd != p->chip_erase_cmd) ||
					(stmqspi_info->dev.pagesize != p->pagesize) ||
					(stmqspi_info->dev.sectorsize != p->sectorsize) ||
					(stmqspi_info->dev.size_in_bytes != p->size_in_bytes)) {
					LOG_WARNING("Incompatible flash1/flash2 devices");
				}
			break;
		}
	}

	if (id2 && !p->name)
		LOG_WARNING("Unknown flash2 device id = 0x%06" PRIx32, id2);

	/* Set correct size value */
	dual = (stmqspi_info->saved_cr & (1<<SPI_DUAL_FLASH)) ? 1 : 0;
	bank->size = stmqspi_info->dev.size_in_bytes << dual;

	fsize = ((READ_REG(SPI_DCR)>>SPI_FSIZE_POS) & ((1U<<SPI_FSIZE_LEN) - 1));
	LOG_DEBUG("FSIZE = 0x%04x", fsize);
	if (bank->size != (1U<<(fsize + 1)))
		LOG_WARNING("FSIZE field in QSPI_DCR(1) doesn't match actual capacity.");

	if (stmqspi_info->dev.sectorsize != 0) {
		/* create and fill sectors array */
		bank->num_sectors =
			stmqspi_info->dev.size_in_bytes / stmqspi_info->dev.sectorsize;
		sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
		if (sectors == NULL) {
			LOG_ERROR("not enough memory");
			return ERROR_FAIL;
		}
	}

	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset = sector * (stmqspi_info->dev.sectorsize << dual);
		sectors[sector].size = (stmqspi_info->dev.sectorsize << dual);
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 0;
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

	snprintf(buf, buf_size, "\'%s\' %dkbytes id = 0x%06" PRIx32,
		stmqspi_info->dev.name, bank->size>>10, stmqspi_info->dev.device_id);

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
	{
		.name = "setparms",
		.handler = stmqspi_handle_setparms,
		.mode = COMMAND_EXEC,
		.usage = "bank_id name chip_size page_size read_cmd pprg_cmd "
			"[ mass_erase_cmd ] [ sector_size sector_erase_cmd ]",
		.help = "Set params of single flash chip",
	},
	{
		.name = "spicmd",
		.handler = stmqspi_handle_spicmd,
		.mode = COMMAND_EXEC,
		.usage = "bank_id num_resp cmd_byte ...",
		.help = "Send low-level command cmd_byte and following bytes or read num_resp.",
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
	.erase_check = stmqspi_blank_check,
	.protect_check = stmqspi_protect_check,
	.info = get_stmqspi_info,
};
