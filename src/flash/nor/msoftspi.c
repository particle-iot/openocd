/***************************************************************************
 *   Copyright (C) 2010 by Antonio Borneo <borneo.antonio@gmail.com>,	   *
 *	   2017 by Andreas Bolsch <andreas.bolsch@mni.thm.de				   *
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include "spi.h"
#include <helper/time_support.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

#define CLR_PORT_BIT(data, pin)											\
{																		\
	(data) &= ~(msoftspi_info->pin.mask);								\
	retval = target_write_u32(target, msoftspi_info->pin.addr, data);	\
}

#define SET_PORT_BIT(data, pin)											\
{																		\
	(data) |= (msoftspi_info->pin.mask);								\
	retval = target_write_u32(target, msoftspi_info->pin.addr, data);	\
}

/* bit 1: address byte 4 with bits 24-31 required
 * bit 0: address byte 3 with bits 16-23 required */
#define ADDR_BYTES \
	(((msoftspi_info->dev.size_in_bytes > (1<<24)) ? 0x2 : 0x00) | \
	 ((msoftspi_info->dev.size_in_bytes > (1<<16)) ? 0x1 : 0x00))

/* convert uint32_t into 4 uint8_t in target (i. e. little endian)
 * byte order, re-inventing the wheel ... */
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

/* timeout in ms */
#define MSOFTSPI_CMD_TIMEOUT		(100)
#define MSOFTSPI_PROBE_TIMEOUT		(100)
#define MSOFTSPI_MAX_TIMEOUT		(2000)
#define MSOFTSPI_MASS_ERASE_TIMEOUT	(400000)

typedef struct {
	uint32_t addr;
	uint32_t mask;
} port_pin;

struct msoftspi_flash_bank {
	int probed;
	uint32_t bank_num;
	char devname[32];
	struct flash_device dev;
	port_pin ncs;
	port_pin sclk;
	port_pin miso;
	port_pin mosi;
	uint32_t bits_no;
};

struct sector_info {
	uint32_t offset;
	uint32_t size;
	uint32_t result;
};

FLASH_BANK_COMMAND_HANDLER(msoftspi_flash_bank_command)
{
	struct msoftspi_flash_bank *msoftspi_info;
	uint8_t bit_no;

	LOG_DEBUG("%s", __func__);

	if (CMD_ARGC < 14)
		return ERROR_COMMAND_SYNTAX_ERROR;

	msoftspi_info = malloc(sizeof(struct msoftspi_flash_bank));
	if (msoftspi_info == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	memset(&msoftspi_info->dev, 0, sizeof(msoftspi_info->dev));
	bank->driver_priv = msoftspi_info;
	msoftspi_info->probed = 0;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[6], msoftspi_info->ncs.addr);
	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[7], bit_no);
	if (bit_no < 32) {
		msoftspi_info->bits_no = ((msoftspi_info->bits_no & ~(0xFF<<24)) | (bit_no<<24));
		msoftspi_info->ncs.mask = 1<<bit_no;
	} else {
		command_print(CMD_CTX, "msoftspi: NCS bit number in 0 ... 31");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[8], msoftspi_info->sclk.addr);
	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[9], bit_no);
	if (bit_no < 32) {
		msoftspi_info->bits_no = ((msoftspi_info->bits_no & ~(0xFF<<16)) | (bit_no<<16));
		msoftspi_info->sclk.mask = 1<<bit_no;
	} else {
		command_print(CMD_CTX, "msoftspi: SCLK bit number in 0 ... 31");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[10], msoftspi_info->miso.addr);
	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[11], bit_no);
	if (bit_no < 32) {
		msoftspi_info->bits_no = ((msoftspi_info->bits_no & ~(0xFF<<8)) | (bit_no<<8));
		msoftspi_info->miso.mask = 1<<bit_no;
	} else {
		command_print(CMD_CTX, "msoftspi: MISO bit number in 0 ... 31");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[12], msoftspi_info->mosi.addr);
	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[13], bit_no);
	if (bit_no < 32) {
		msoftspi_info->bits_no = ((msoftspi_info->bits_no & ~(0xFF<<0)) | (bit_no<<0));
		msoftspi_info->mosi.mask = 1<<bit_no;
	} else {
		command_print(CMD_CTX, "msoftspi: MOSI bit number must be in 0 ... 31");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	LOG_DEBUG("NCS (out):  0x%" PRIx32 ", 0x%" PRIx32, msoftspi_info->ncs.addr, msoftspi_info->ncs.mask);
	LOG_DEBUG("SCLK (out): 0x%" PRIx32 ", 0x%" PRIx32, msoftspi_info->sclk.addr, msoftspi_info->sclk.mask);
	LOG_DEBUG("MISO (in):  0x%" PRIx32 ", 0x%" PRIx32, msoftspi_info->miso.addr, msoftspi_info->miso.mask);
	LOG_DEBUG("MOSI (out): 0x%" PRIx32 ", 0x%" PRIx32, msoftspi_info->mosi.addr, msoftspi_info->mosi.mask);

	if (msoftspi_info->sclk.addr == msoftspi_info->mosi.addr)
		LOG_INFO("SCLK and MOSI located on same port - should work anyway");
	return ERROR_OK;
}

/* Send and receive one byte via SPI */
/* bits 7 down to 0 are shifted out, MSB first */
static int msoftspi_shift_out(struct flash_bank *bank, uint32_t word)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	uint32_t port_sclk, port_mosi;
	int k, retval;

	LOG_DEBUG("0x%08" PRIx32, word);

	retval = target_read_u32(target, msoftspi_info->mosi.addr, &port_sclk);
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u32(target, msoftspi_info->mosi.addr, &port_mosi);
	if (retval != ERROR_OK)
		return retval;

	/* shift bit 7 into bit 31 */
	word <<= 24;

	for (k = 0; k < 8; k++)	{
		/* shift out data bit, msb first */
		if (word & (1<<31))
			SET_PORT_BIT(port_mosi, mosi)
		else
			CLR_PORT_BIT(port_mosi, mosi);
		if (retval != ERROR_OK)
			return retval;

		/* set SCLK */
		if (msoftspi_info->sclk.addr == msoftspi_info->mosi.addr)
			port_sclk = port_mosi;
		SET_PORT_BIT(port_sclk, sclk);
		if (retval != ERROR_OK)
			return retval;

		/* shift next bit into bit 31 */
		word <<= 1;

		/* clear SCLK */
		CLR_PORT_BIT(port_sclk, sclk);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int msoftspi_shift_in(struct flash_bank *bank, uint32_t *word)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	uint32_t port_sclk, port_miso;
	int k, retval;

	retval = target_read_u32(target, msoftspi_info->mosi.addr, &port_sclk);
	if (retval != ERROR_OK)
		return retval;

	for (k = 0; k < 8; k++)	{
		/* set SCLK */
		SET_PORT_BIT(port_sclk, sclk);
		if (retval != ERROR_OK)
			return retval;

		/* shift in data bit, msb first */
		retval = target_read_u32(target, msoftspi_info->miso.addr, &port_miso);
		if (retval != ERROR_OK)
			return retval;

		*word <<= 1;
		(port_miso & msoftspi_info->miso.mask) && (*word |= 0x1);

		/* clear SCLK */
		CLR_PORT_BIT(port_sclk, sclk);
		if (retval != ERROR_OK)
			return retval;
	}

	LOG_DEBUG("0x%08" PRIx32, *word);
	return ERROR_OK;
}

/* Read the status register of the external SPI flash chip. */
static int read_status_reg(struct flash_bank *bank, uint32_t *status)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	uint32_t port;
	int retval, success;

	success = ERROR_FAIL;

	/* set NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	if (retval != ERROR_OK)
		goto err;
	SET_PORT_BIT(port, ncs);
	if (retval != ERROR_OK)
		goto err;

	/* clear NCS */
	CLR_PORT_BIT(port, ncs);
	if (retval != ERROR_OK)
		goto err;

	/* send command byte */
	retval = msoftspi_shift_out(bank, SPIFLASH_READ_STATUS);
	if (retval != ERROR_OK)
		goto err;

	/* set to input */

	/* get result byte */
	success = msoftspi_shift_in(bank, status);
	*status &= 0xFF;

err:
	/* set NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	SET_PORT_BIT(port, ncs);

	/* set to output */

	return success;
}

/* Check for WIP (write in progress) bit in status register */
/* timeout in ms */
static int wait_till_ready(struct flash_bank *bank, int timeout)
{
	uint32_t status;
	int retval;
	long long endtime;

	endtime = timeval_ms() + timeout;
	do {
		/* Read flash status register */
		retval = read_status_reg(bank, &status);
		if (retval != ERROR_OK)
			return retval;

		if ((status & SPIFLASH_BSY_BIT) == 0)
			return ERROR_OK;
		alive_sleep(25);
	} while (timeval_ms() < endtime);

	LOG_ERROR("timeout");
	return ERROR_FAIL;
}

/* Send "write enable" command to SPI flash chip */
static int msoftspi_write_enable(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	uint32_t port, status;
	int retval;

	/* clear NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	if (retval != ERROR_OK)
		goto err;
	CLR_PORT_BIT(port, ncs);
	if (retval != ERROR_OK)
		goto err;

	/* send write enable command */
	retval = msoftspi_shift_out(bank, SPIFLASH_WRITE_ENABLE);

err:
	/* set NCS */
	SET_PORT_BIT(port, ncs);
	if (retval != ERROR_OK)
		return retval;

	/* Read flash status register */
	retval = read_status_reg(bank, &status);
	if (retval != ERROR_OK)
		return retval;

	/* check write enabled */
	if ((status & SPIFLASH_WE_BIT) == 0) {
		LOG_ERROR("Cannot enable write to flash. Status=0x%08" PRIx32, status);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

/* Erase a single sector */
static int msoftspi_erase_sector(struct flash_bank *bank, int sector)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	uint32_t addr = bank->sectors[sector].offset;
	uint32_t port, data;
	int retval;

	retval = msoftspi_write_enable(bank);
	if (retval != ERROR_OK)
		return retval;

	if (bank->sectors[sector].is_protected) {
		LOG_ERROR("Flash sector %d protected", sector);
		return ERROR_FAIL;
	} else
		bank->sectors[sector].is_erased = -1;

	/* Send Sector Erase command */
	/* clear NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	if (retval != ERROR_OK)
		goto err;
	CLR_PORT_BIT(port, ncs);
	if (retval != ERROR_OK)
		goto err;

	/* send command byte */
	retval = msoftspi_shift_out(bank, msoftspi_info->dev.erase_cmd);
	if (retval != ERROR_OK)
		goto err;

	if (ADDR_BYTES & 0x2) {
		/* bits 24-31 */
		retval = msoftspi_shift_out(bank, addr >> 24);
		if (retval != ERROR_OK)
			goto err;
	}

	if (ADDR_BYTES & 0x1) {
		/* bits 16-23 */
		retval = msoftspi_shift_out(bank, addr >> 16);
		if (retval != ERROR_OK)
			goto err;
	}

	/* bits 8-15 */
	retval = msoftspi_shift_out(bank, addr >> 8);
	if (retval != ERROR_OK)
		goto err;

	/* bits 0-7 */
	retval = msoftspi_shift_out(bank, addr >> 0);
	if (retval != ERROR_OK)
		goto err;

err:
	/* set NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	SET_PORT_BIT(port, ncs);
	if (retval != ERROR_OK)
		return retval;

	/* read flash status register */
	retval = read_status_reg(bank, &data);
	if (retval != ERROR_OK)
		return retval;

	/* check for command in progress for flash */
	if ((data & SPIFLASH_WE_BIT) == 0) {
		LOG_DEBUG("Sector erase not accepted by flash or already completed. Status=0x%08" PRIx32, data);
		/* return ERROR_FAIL; */
	}

	/* poll WIP for end of self timed Sector Erase cycle */
	retval = wait_till_ready(bank, MSOFTSPI_MAX_TIMEOUT);

	/* erasure takes a long time, so some sort of progress message is a good idea */
	LOG_DEBUG("sector %4d erased", sector);

	return retval;
}

/* Erase range of sectors */
static int msoftspi_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	int retval = ERROR_OK;
	int sector;

	LOG_DEBUG("%s: from sector %d to sector %d", __func__, first, last);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!(msoftspi_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	if (msoftspi_info->dev.erase_cmd == 0x00) {
		LOG_ERROR("Sector erase not available");
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
		retval = msoftspi_erase_sector(bank, sector);
		if (retval != ERROR_OK)
			break;
		keep_alive();
	}

	if (retval != ERROR_OK)
		LOG_ERROR("Flash sector_erase failed on sector %d", sector);

	return retval;
}

/* Check whether flash is blank */
static int msoftspi_blank_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	struct duration bench;
	struct reg_param reg_params[3];
	struct armv7m_algorithm armv7m_info;
	struct working_area *erase_check_algorithm;
	struct sector_info erase_check_info;
	uint32_t buffer_size, exit_point, result;
	int num_sectors, sector, index, count, retval;
	const uint32_t erased = 0x00FF;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!(msoftspi_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	/* see contrib/loaders/erase_check/msoftspi_erase_check.S for src */
	static const uint8_t msoftspi_erase_check_code[] = {
		0x80, 0x46, 0x57, 0xa0, 0x81, 0x46, 0x01, 0x38, 0x83, 0x46, 0x94, 0x46,
		0x01, 0x39, 0x9b, 0x07, 0x19, 0x43, 0x8a, 0x46, 0x4b, 0x48, 0x4c, 0x49,
		0x00, 0xf0, 0x46, 0xf8, 0x4d, 0x46, 0xc0, 0xcd, 0xb4, 0x46, 0xbb, 0x46,
		0x03, 0x24, 0x00, 0xf0, 0x57, 0xf8, 0x57, 0x46, 0x7f, 0x00, 0x03, 0xd3,
		0x64, 0x46, 0x24, 0x0e, 0x00, 0xf0, 0x57, 0xf8, 0x57, 0x46, 0xbf, 0x00,
		0x03, 0xd3, 0x64, 0x46, 0x24, 0x0c, 0x00, 0xf0, 0x50, 0xf8, 0x64, 0x46,
		0x24, 0x0a, 0x00, 0xf0, 0x4c, 0xf8, 0x64, 0x46, 0x24, 0x00, 0x00, 0xf0,
		0x48, 0xf8, 0xc0, 0x46, 0x3c, 0x4a, 0x3d, 0x4b, 0x3f, 0x4d, 0x2d, 0x0a,
		0x00, 0xf0, 0x2a, 0xf8, 0xff, 0x27, 0x3f, 0x02, 0x3c, 0x43, 0x4f, 0x46,
		0xbe, 0x68, 0x26, 0x40, 0x24, 0x02, 0x26, 0x43, 0xbe, 0x60, 0x67, 0x46,
		0x01, 0x37, 0xbc, 0x46, 0x5e, 0x46, 0x01, 0x3e, 0xb3, 0x46, 0x05, 0xd0,
		0x56, 0x46, 0x3e, 0x42, 0xea, 0xe7, 0x00, 0xf0, 0x0d, 0xf8, 0xc9, 0xe7,
		0x00, 0xf0, 0x0a, 0xf8, 0x4f, 0x46, 0x5e, 0x46, 0x7e, 0x60, 0x0c, 0x37,
		0xb9, 0x46, 0x47, 0x46, 0x01, 0x3f, 0xb8, 0x46, 0xba, 0xd1, 0x46, 0xe0,
		0x24, 0x4e, 0x37, 0x68, 0x24, 0x4e, 0x37, 0x43, 0x22, 0x4e, 0x37, 0x60,
		0xc0, 0x46, 0x70, 0x47, 0x01, 0x24, 0x24, 0x06, 0x00, 0x27, 0x06, 0x68,
		0x3c, 0x43, 0x0e, 0x43, 0x06, 0x60, 0x17, 0x68, 0x1f, 0x40, 0xef, 0x41,
		0x8e, 0x43, 0x06, 0x60, 0x64, 0x00, 0xf5, 0xd3, 0x3c, 0x43, 0x70, 0x47,
		0x18, 0x4a, 0x19, 0x4b, 0x17, 0x68, 0x9f, 0x43, 0x17, 0x60, 0x1c, 0x4a,
		0x1c, 0x4b, 0x64, 0x00, 0x01, 0x34, 0xe4, 0x05, 0x06, 0x68, 0x8e, 0x43,
		0x90, 0x42, 0x10, 0xd0, 0x17, 0x68, 0x64, 0x00, 0x8e, 0x43, 0xad, 0x41,
		0x1d, 0x40, 0x06, 0x60, 0x1f, 0x43, 0xaf, 0x43, 0x17, 0x60, 0x0e, 0x43,
		0x64, 0x00, 0x06, 0x60, 0xf4, 0xd1, 0x06, 0x60, 0x8e, 0x43, 0x06, 0x60,
		0x70, 0x47, 0x64, 0x00, 0x8e, 0x43, 0xad, 0x41, 0x1d, 0x40, 0x06, 0x60,
		0x1e, 0x43, 0xae, 0x43, 0x16, 0x60, 0x0e, 0x43, 0x64, 0x00, 0x06, 0x60,
		0xf4, 0xd1, 0x06, 0x60, 0x8e, 0x43, 0x06, 0x60, 0x70, 0x47, 0xc0, 0x46,
		0x00, 0xbe, 0xc0, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};

	/* this will overlay the last 9 words of msoftspi_*_code in target */
	uint32_t port_buffer[] = {
		h_to_le_32(msoftspi_info->ncs.addr), h_to_le_32(msoftspi_info->ncs.mask),
		h_to_le_32(msoftspi_info->sclk.addr), h_to_le_32(msoftspi_info->sclk.mask),
		h_to_le_32(msoftspi_info->miso.addr), h_to_le_32(msoftspi_info->miso.mask),
		h_to_le_32(msoftspi_info->mosi.addr), h_to_le_32(msoftspi_info->mosi.mask),
		h_to_le_32(msoftspi_info->bits_no)
	};

	num_sectors = bank->num_sectors;
	while (buffer_size = sizeof(msoftspi_erase_check_code) + num_sectors * sizeof(erase_check_info),
		target_alloc_working_area_try(target, buffer_size, &erase_check_algorithm) != ERROR_OK) {
		num_sectors /= 2;
		if (num_sectors <= 2) {
			LOG_WARNING("not enough working area, can't do SPI blank check");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	/* prepare check code, excluding port_buffer */
	retval = target_write_buffer(target, erase_check_algorithm->address,
		sizeof(msoftspi_erase_check_code) - sizeof(port_buffer), msoftspi_erase_check_code);
	if (retval != ERROR_OK)
		goto err;

	/* prepare port_buffer values */
	retval = target_write_buffer(target, erase_check_algorithm->address
		+ sizeof(msoftspi_erase_check_code) - sizeof(port_buffer),
		sizeof(port_buffer), (uint8_t *) port_buffer);
	if (retval != ERROR_OK)
		goto err;

	duration_start(&bench);

	/* after breakpoint instruction (halfword) one nop (halfword) and
	 * port_buffer till end of code */
	exit_point = erase_check_algorithm->address + sizeof(msoftspi_erase_check_code)
		- sizeof(uint32_t) - sizeof(port_buffer);

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);	/* sector count */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* flash page_size */
	init_reg_param(&reg_params[2], "r3", 32, PARAM_OUT);	/* 2/3/4-byte address mode */

	sector = 0;
	while (sector < bank->num_sectors) {
		/* at most num_sectors sectors to handle in one run */
		count = bank->num_sectors - sector;
		if (count > num_sectors)
			count = num_sectors;

		for (index = 0; index < count; index++) {
			erase_check_info.offset = h_to_le_32(bank->sectors[sector + index].offset);
			erase_check_info.size = h_to_le_32(bank->sectors[sector + index].size);
			erase_check_info.result = h_to_le_32(erased);

			retval = target_write_buffer(target, erase_check_algorithm->address
				+ sizeof(msoftspi_erase_check_code) + index * sizeof(erase_check_info),
					sizeof(erase_check_info), (uint8_t *) &erase_check_info);
			if (retval != ERROR_OK)
				goto err;
		}

		buf_set_u32(reg_params[0].value, 0, 32, count);
		buf_set_u32(reg_params[1].value, 0, 32, msoftspi_info->dev.pagesize);
		buf_set_u32(reg_params[2].value, 0, 32, ADDR_BYTES);

		armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
		armv7m_info.core_mode = ARM_MODE_THREAD;

		LOG_DEBUG("checking sectors %d to %d", sector, sector + count - 1);
		/* check a block of sectors */
		retval = target_run_algorithm(target,
			0, NULL,
			3, reg_params,
			erase_check_algorithm->address, exit_point,
			count * MSOFTSPI_MAX_TIMEOUT,
			&armv7m_info);
		if (retval != ERROR_OK)
			break;

		for (index = 0; index < count; index++) {
			retval = target_read_buffer(target, erase_check_algorithm->address
				+ sizeof(msoftspi_erase_check_code) + index * sizeof(erase_check_info),
					sizeof(erase_check_info), (uint8_t *) &erase_check_info);
			if (retval != ERROR_OK)
				goto err;

			if ((erase_check_info.offset != h_to_le_32(bank->sectors[sector + index].offset)) ||
				(erase_check_info.size != 0)) {
				LOG_ERROR("corrupted blank check info");
				goto err;
			}

			result = h_to_le_32(erase_check_info.result);
			bank->sectors[sector + index].is_erased = ((result & 0xFF) == 0xFF);
			LOG_DEBUG("Flash sector %d checked: %04x", sector + index, result & 0xFFFF);
		}
		keep_alive();
		sector += count;
	}

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);

	duration_measure(&bench);
	LOG_INFO("msoftspi blank checked in"
			" %fs (%0.3f KiB/s)", duration_elapsed(&bench),
			duration_kbps(&bench, bank->size));

err:
	target_free_working_area(target, erase_check_algorithm);

	return retval;
}

static int msoftspi_protect(struct flash_bank *bank, int set,
	int first, int last)
{
	int sector;

	for (sector = first; sector <= last; sector++)
		bank->sectors[sector].is_protected = set;
	return ERROR_OK;
}

/* Read a block of data from flash or write a block of data to flash */
static int msoftspi_read_write_block(struct flash_bank *bank, uint8_t *buffer,
		uint32_t offset, uint32_t count, int write)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	struct reg_param reg_params[6];
	struct armv7m_algorithm armv7m_info;
	struct working_area *write_algorithm;
	static const uint8_t *code;
	uint32_t page_size, fifo_start, fifo_size, buffer_size;
	uint32_t exit_point, remaining;
	int code_size, retval = ERROR_OK;

	LOG_DEBUG("%s: offset=0x%08" PRIx32 " len=0x%08" PRIx32,
		__func__, offset, count);

	/* see contrib/loaders/flash/msoftspi_read.S for src */
	static const uint8_t msoftspi_read_code[] = {
		0x01, 0x38, 0x83, 0x46, 0x94, 0x46, 0x01, 0x39, 0x9b, 0x07, 0x19, 0x43,
		0x8a, 0x46, 0x48, 0x48, 0x48, 0x49, 0x00, 0xf0, 0x3a, 0xf8, 0x03, 0x24,
		0x00, 0xf0, 0x4f, 0xf8, 0x57, 0x46, 0x7f, 0x00, 0x03, 0xd3, 0x64, 0x46,
		0x24, 0x0e, 0x00, 0xf0, 0x4f, 0xf8, 0x57, 0x46, 0xbf, 0x00, 0x03, 0xd3,
		0x64, 0x46, 0x24, 0x0c, 0x00, 0xf0, 0x48, 0xf8, 0x64, 0x46, 0x24, 0x0a,
		0x00, 0xf0, 0x44, 0xf8, 0x64, 0x46, 0x24, 0x00, 0x00, 0xf0, 0x40, 0xf8,
		0xc0, 0x46, 0x3b, 0x4a, 0x3b, 0x4b, 0x3e, 0x4d, 0x2d, 0x0a, 0x00, 0xf0,
		0x22, 0xf8, 0x3d, 0x4e, 0x34, 0x70, 0x01, 0x36, 0x4e, 0x45, 0x00, 0xd3,
		0x46, 0x46, 0x3b, 0x4f, 0x00, 0x2f, 0x57, 0xd0, 0xbe, 0x42, 0xfa, 0xd0,
		0x37, 0xa7, 0x3e, 0x60, 0x67, 0x46, 0x01, 0x37, 0xbc, 0x46, 0x5e, 0x46,
		0x01, 0x3e, 0xb3, 0x46, 0x4c, 0xd4, 0x56, 0x46, 0x3e, 0x42, 0xe6, 0xe7,
		0x00, 0xf0, 0x01, 0xf8, 0xc5, 0xe7, 0x27, 0x4e, 0x37, 0x68, 0x27, 0x4e,
		0x37, 0x43, 0x25, 0x4e, 0x37, 0x60, 0xc0, 0x46, 0x70, 0x47, 0x01, 0x24,
		0x24, 0x06, 0x00, 0x27, 0x06, 0x68, 0x3c, 0x43, 0x0e, 0x43, 0x06, 0x60,
		0x17, 0x68, 0x1f, 0x40, 0xef, 0x41, 0x8e, 0x43, 0x06, 0x60, 0x64, 0x00,
		0xf5, 0xd3, 0x3c, 0x43, 0x70, 0x47, 0x1b, 0x4a, 0x1b, 0x4b, 0x17, 0x68,
		0x9f, 0x43, 0x17, 0x60, 0x1e, 0x4a, 0x1f, 0x4b, 0x64, 0x00, 0x01, 0x34,
		0xe4, 0x05, 0x06, 0x68, 0x8e, 0x43, 0x90, 0x42, 0x10, 0xd0, 0x17, 0x68,
		0x64, 0x00, 0x8e, 0x43, 0xad, 0x41, 0x1d, 0x40, 0x06, 0x60, 0x1f, 0x43,
		0xaf, 0x43, 0x17, 0x60, 0x0e, 0x43, 0x64, 0x00, 0x06, 0x60, 0xf4, 0xd1,
		0x06, 0x60, 0x8e, 0x43, 0x06, 0x60, 0x70, 0x47, 0x64, 0x00, 0x8e, 0x43,
		0xad, 0x41, 0x1d, 0x40, 0x06, 0x60, 0x1e, 0x43, 0xae, 0x43, 0x16, 0x60,
		0x0e, 0x43, 0x64, 0x00, 0x06, 0x60, 0xf4, 0xd1, 0x06, 0x60, 0x8e, 0x43,
		0x06, 0x60, 0x70, 0x47, 0xff, 0xf7, 0xb7, 0xff, 0x58, 0x46, 0x01, 0x30,
		0x62, 0x46, 0xc0, 0x46, 0x00, 0xbe, 0xc0, 0x46, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	/* see contrib/loaders/flash/msoftspi_write.S for src */
	static const uint8_t msoftspi_write_code[] = {
		0x01, 0x38, 0x83, 0x46, 0x94, 0x46, 0x01, 0x39, 0x9b, 0x07, 0x19, 0x43,
		0x8a, 0x46, 0x50, 0x48, 0x50, 0x49, 0x00, 0xf0, 0x4d, 0xf8, 0x05, 0x24,
		0x00, 0xf0, 0x62, 0xf8, 0xc0, 0x46, 0x4e, 0x4a, 0x4e, 0x4b, 0x51, 0x4d,
		0x2d, 0x0a, 0x00, 0xf0, 0x4b, 0xf8, 0x00, 0xf0, 0x41, 0xf8, 0x64, 0x08,
		0xf1, 0xd2, 0x5f, 0x46, 0x3f, 0x42, 0x00, 0xd5, 0x81, 0xe0, 0x06, 0x24,
		0x00, 0xf0, 0x50, 0xf8, 0x00, 0xf0, 0x36, 0xf8, 0x02, 0x24, 0x00, 0xf0,
		0x4b, 0xf8, 0x57, 0x46, 0x7f, 0x00, 0x03, 0xd3, 0x64, 0x46, 0x24, 0x0e,
		0x00, 0xf0, 0x4b, 0xf8, 0x57, 0x46, 0xbf, 0x00, 0x03, 0xd3, 0x64, 0x46,
		0x24, 0x0c, 0x00, 0xf0, 0x44, 0xf8, 0x64, 0x46, 0x24, 0x0a, 0x00, 0xf0,
		0x40, 0xf8, 0x64, 0x46, 0x24, 0x00, 0x00, 0xf0, 0x3c, 0xf8, 0x3d, 0x4f,
		0x00, 0x2f, 0x60, 0xd0, 0x3c, 0x4e, 0xbe, 0x42, 0xf9, 0xd0, 0x34, 0x78,
		0x00, 0xf0, 0x33, 0xf8, 0x39, 0x4e, 0x01, 0x36, 0x4e, 0x45, 0x00, 0xd3,
		0x46, 0x46, 0x37, 0xa7, 0x3e, 0x60, 0x67, 0x46, 0x01, 0x37, 0xbc, 0x46,
		0x5e, 0x46, 0x01, 0x3e, 0xb3, 0x46, 0x02, 0xd4, 0x56, 0x46, 0x3e, 0x42,
		0xe5, 0xd1, 0x00, 0xf0, 0x01, 0xf8, 0xb2, 0xe7, 0x25, 0x4e, 0x37, 0x68,
		0x25, 0x4e, 0x37, 0x43, 0x23, 0x4e, 0x37, 0x60, 0xc0, 0x46, 0x70, 0x47,
		0x01, 0x24, 0x24, 0x06, 0x00, 0x27, 0x06, 0x68, 0x3c, 0x43, 0x0e, 0x43,
		0x06, 0x60, 0x17, 0x68, 0x1f, 0x40, 0xef, 0x41, 0x8e, 0x43, 0x06, 0x60,
		0x64, 0x00, 0xf5, 0xd3, 0x3c, 0x43, 0x70, 0x47, 0x19, 0x4a, 0x1a, 0x4b,
		0x17, 0x68, 0x9f, 0x43, 0x17, 0x60, 0x1d, 0x4a, 0x1d, 0x4b, 0x64, 0x00,
		0x01, 0x34, 0xe4, 0x05, 0x06, 0x68, 0x8e, 0x43, 0x90, 0x42, 0x10, 0xd0,
		0x17, 0x68, 0x64, 0x00, 0x8e, 0x43, 0xad, 0x41, 0x1d, 0x40, 0x06, 0x60,
		0x1f, 0x43, 0xaf, 0x43, 0x17, 0x60, 0x0e, 0x43, 0x64, 0x00, 0x06, 0x60,
		0xf4, 0xd1, 0x06, 0x60, 0x8e, 0x43, 0x06, 0x60, 0x70, 0x47, 0x64, 0x00,
		0x8e, 0x43, 0xad, 0x41, 0x1d, 0x40, 0x06, 0x60, 0x1e, 0x43, 0xae, 0x43,
		0x16, 0x60, 0x0e, 0x43, 0x64, 0x00, 0x06, 0x60, 0xf4, 0xd1, 0x06, 0x60,
		0x8e, 0x43, 0x06, 0x60, 0x70, 0x47, 0x58, 0x46, 0x01, 0x30, 0x61, 0x46,
		0x00, 0xbe, 0xc0, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};

	code = write ? msoftspi_write_code : msoftspi_read_code;
	code_size = write ? sizeof(msoftspi_write_code) : sizeof(msoftspi_read_code);

	/* this will overlay the last 9 words of msoftspi_*_code in target */
	uint32_t port_buffer[] = {
		h_to_le_32(msoftspi_info->ncs.addr), h_to_le_32(msoftspi_info->ncs.mask),
		h_to_le_32(msoftspi_info->sclk.addr), h_to_le_32(msoftspi_info->sclk.mask),
		h_to_le_32(msoftspi_info->miso.addr), h_to_le_32(msoftspi_info->miso.mask),
		h_to_le_32(msoftspi_info->mosi.addr), h_to_le_32(msoftspi_info->mosi.mask),
		h_to_le_32(msoftspi_info->bits_no)
	};

	/* memory buffer, we assume sectorsize to be a power of 2 times page_size */
	page_size = msoftspi_info->dev.pagesize;
	fifo_size = msoftspi_info->dev.sectorsize;
	while (buffer_size = code_size + 2 * sizeof(uint32_t) + fifo_size,
			target_alloc_working_area_try(target, buffer_size, &write_algorithm) != ERROR_OK) {
		fifo_size /= 2;
		if (fifo_size < page_size) {
			/* we already allocated the reading/writing code, but failed to get a
			 * buffer, free the algorithm */
			target_free_working_area(target, write_algorithm);

			LOG_WARNING("not enough working area, can't do SPI %s",
				write ? "page writes" : "reads");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	};

	/* prepare read/write code, excluding port_buffer */
	retval = target_write_buffer(target, write_algorithm->address,
		code_size - sizeof(port_buffer), code);
	if (retval != ERROR_OK)
		goto err;

	/* prepare port_buffer values */
	retval = target_write_buffer(target, write_algorithm->address
		+ code_size - sizeof(port_buffer),
		sizeof(port_buffer), (uint8_t *) port_buffer);
	if (retval != ERROR_OK)
		goto err;

	/* target buffer starts right after flash_write_code, i. e.
	 * wp and rp are implicitly included in buffer!!! */
	fifo_start = write_algorithm->address + code_size + 2 * sizeof(uint32_t);

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT); /* count (in), status (out) */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* flash page_size */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_IN_OUT);	/* offset into flash address */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);	/* 2/3/4-byte address mode */
	init_reg_param(&reg_params[4], "r8", 32, PARAM_OUT);	/* fifo start */
	init_reg_param(&reg_params[5], "r9", 32, PARAM_OUT);	/* fifo end + 1 */

	buf_set_u32(reg_params[0].value, 0, 32, count);
	buf_set_u32(reg_params[1].value, 0, 32,
		write ? page_size : msoftspi_info->dev.sectorsize);
	buf_set_u32(reg_params[2].value, 0, 32, offset);
	buf_set_u32(reg_params[3].value, 0, 32, ADDR_BYTES);
	buf_set_u32(reg_params[4].value, 0, 32, fifo_start);
	buf_set_u32(reg_params[5].value, 0, 32, fifo_start + fifo_size);

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;

	/* after breakpoint instruction (halfword) one nop (halfword) and
	 * port_buffer till end of code */
	exit_point = write_algorithm->address + code_size
		- sizeof(uint32_t) - sizeof(port_buffer);

	if (write) {
		retval = target_run_flash_async_algorithm(target, buffer, count, 1,
				0, NULL,
				6, reg_params,
				write_algorithm->address + code_size,
				fifo_size + 2 * sizeof(uint32_t),
				write_algorithm->address, exit_point,
				&armv7m_info);
	} else {
		retval = target_run_read_async_algorithm(target, buffer, count, 1,
				0, NULL,
				6, reg_params,
				write_algorithm->address + code_size,
				fifo_size + 2 * sizeof(uint32_t),
				write_algorithm->address, exit_point,
				&armv7m_info);
	}

	remaining = buf_get_u32(reg_params[0].value, 0, 32);
	if ((retval == ERROR_OK) && remaining)
		retval = ERROR_FLASH_OPERATION_FAILED;
	if (retval != ERROR_OK) {
		offset = buf_get_u32(reg_params[2].value, 0, 32);
		LOG_ERROR("flash %s failed at address 0x%" PRIx32 ", remaining 0x%" PRIx32,
			write ? "write" : "read", offset, remaining);
	}

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);
	destroy_reg_param(&reg_params[5]);

err:
	target_free_working_area(target, write_algorithm);

	return retval;
}

static int msoftspi_write(struct flash_bank *bank, const uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	int sector;

	LOG_DEBUG("%s: offset=0x%08" PRIx32 " count=0x%08" PRIx32,
		__func__, offset, count);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset + count > msoftspi_info->dev.size_in_bytes) {
		LOG_WARNING("Write beyond end of flash. Extra data discarded.");
		count = msoftspi_info->dev.size_in_bytes - offset;
	}

	/* check sector protection */
	for (sector = 0; sector < bank->num_sectors; sector++) {
		/* Start offset in or before this sector? */
		/* End offset in or behind this sector? */
		if ((offset < (bank->sectors[sector].offset + bank->sectors[sector].size))
			&& ((offset + count - 1) >= bank->sectors[sector].offset)) {
			if (bank->sectors[sector].is_protected) {
				LOG_ERROR("Flash sector %d protected", sector);
				return ERROR_FAIL;
			} else
				bank->sectors[sector].is_erased = -1;
		}
	}

	return msoftspi_read_write_block(bank, (uint8_t *) buffer, offset, count, 1);
}

static int msoftspi_read(struct flash_bank *bank, uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;

	LOG_DEBUG("%s: offset=0x%08" PRIx32 " count=0x%08" PRIx32,
		__func__, offset, count);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!(msoftspi_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	if (offset + count > msoftspi_info->dev.size_in_bytes) {
		LOG_WARNING("Read beyond end of flash. Extra data to be ignored.");
		count = msoftspi_info->dev.size_in_bytes - offset;
	}

	return msoftspi_read_write_block(bank, buffer, offset, count, 0);
}

/* Return ID of flash device */
static int read_flash_id(struct flash_bank *bank, uint32_t *id)
{
	struct target *target = bank->target;
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	uint32_t port, data;
	int k, retval, success;

	success = ERROR_FAIL;

	if ((target->state != TARGET_HALTED) && (target->state != TARGET_RESET)) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* clear SCLK */
	retval = target_read_u32(target, msoftspi_info->sclk.addr, &port);
	if (retval != ERROR_OK)
		goto err;
	port &= ~msoftspi_info->sclk.mask;
	retval = target_write_u32(target, msoftspi_info->sclk.addr, port);
	if (retval != ERROR_OK)
		goto err;

	/* set NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	if (retval != ERROR_OK)
		goto err;
	SET_PORT_BIT(port, ncs);
	if (retval != ERROR_OK)
		goto err;

	/* poll WIP */
	retval = wait_till_ready(bank, MSOFTSPI_PROBE_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	/* clear NCS */
	CLR_PORT_BIT(port, ncs);
	retval = target_write_u32(target, msoftspi_info->ncs.addr, port);
	if (retval != ERROR_OK)
		goto err;

	success = msoftspi_shift_out(bank, SPIFLASH_READ_ID);
	if (success != ERROR_OK)
		goto err;

	/* set to input */

	for (k = 0; k < 3; k++) {
		success = msoftspi_shift_in(bank, &data);
		if (success != ERROR_OK)
			goto err;
	}

	/* three bytes received, placed in bits 0 to 23, byte reversed */
	*id = ((data & 0xFF) << 16) | (data & 0xFF00) | ((data & 0xFF0000) >> 16);

	if ((*id == 0x000000) || (*id == 0xFFFFFF)) {
		LOG_INFO("No response from flash");
		success = ERROR_TARGET_NOT_EXAMINED;
	}

err:
	/* set NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	SET_PORT_BIT(port, ncs);

	/* set to output */

	return success;
}

/* Read id from flash chip  */
static int msoftspi_probe(struct flash_bank *bank)
{
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;
	struct flash_sector *sectors;
	const struct flash_device *p;
	uint32_t id = 0;
	int retval;

	if (msoftspi_info->probed)
		free(bank->sectors);
	msoftspi_info->probed = 0;

	/* read and decode flash ID */
	retval = read_flash_id(bank, &id);
	LOG_DEBUG("id 0x%06" PRIx32, id);
	if (retval == ERROR_TARGET_NOT_EXAMINED) {
		/* no id retrieved, so id must be set manually */
		LOG_INFO("No id - set flash parameters manually");
		return ERROR_OK;
	}
	if (retval != ERROR_OK)
		return retval;

	/* identify flash */
	msoftspi_info->dev.name = NULL;
	for (p = flash_devices; id && p->name ; p++) {
		if (p->device_id == id) {
			memcpy(&msoftspi_info->dev, p, sizeof(msoftspi_info->dev));
			LOG_INFO("flash \'%s\' id = 0x%06" PRIx32
				 "\nflash size = %lukbytes",
				 p->name, id, p->size_in_bytes>>10);
			break;
		}
	}

	if (id && !p->name) {
		LOG_ERROR("Unknown flash device id = 0x%06" PRIx32, id);
		return ERROR_FAIL;
	}

	/* set correct size value */
	bank->size = msoftspi_info->dev.size_in_bytes;

	/* create and fill sectors array */
	bank->num_sectors =
		msoftspi_info->dev.size_in_bytes / msoftspi_info->dev.sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (sectors == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset = sector * (msoftspi_info->dev.sectorsize);
		sectors[sector].size = msoftspi_info->dev.sectorsize;
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 0;
	}

	bank->sectors = sectors;
	msoftspi_info->probed = 1;

	return ERROR_OK;
}

static int msoftspi_auto_probe(struct flash_bank *bank)
{
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;

	if (msoftspi_info->probed)
		return ERROR_OK;
	return msoftspi_probe(bank);
}

static int msoftspi_protect_check(struct flash_bank *bank)
{
	/* nothing to do. Protection is only handled in SW. */
	return ERROR_OK;
}

static int get_msoftspi_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct msoftspi_flash_bank *msoftspi_info = bank->driver_priv;

	if (!(msoftspi_info->probed)) {
		snprintf(buf, buf_size,	"\nmsoftspi flash bank not probed yet\n");
		return ERROR_OK;
	}

	snprintf(buf, buf_size, "flash \'%s\' device id = 0x%06" PRIx32
			"\nflash size = %dkBytes\npage size = %d"
			", mass_erase = 0x%02x, sector_erase = 0x%02x",
			msoftspi_info->dev.name, msoftspi_info->dev.device_id,
			bank->size>>10,	msoftspi_info->dev.pagesize,
			msoftspi_info->dev.chip_erase_cmd, msoftspi_info->dev.erase_cmd);

	return ERROR_OK;
}

COMMAND_HANDLER(msoftspi_handle_mass_erase_command)
{
	struct target *target = NULL;
	struct flash_bank *bank;
	struct msoftspi_flash_bank *msoftspi_info;
	struct duration bench;
	uint32_t port, data;
	int retval, sector;

	LOG_DEBUG("%s", __func__);

	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	msoftspi_info = bank->driver_priv;
	target = bank->target;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!(msoftspi_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	if (msoftspi_info->dev.chip_erase_cmd == 0x00) {
		LOG_ERROR("Mass erase not available");
		return ERROR_FLASH_OPER_UNSUPPORTED;
	}

	for (sector = 0; sector < bank->num_sectors; sector++) {
		if (bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	duration_start(&bench);

	retval = msoftspi_write_enable(bank);
	if (retval != ERROR_OK)
		return retval;

	/* Send Mass Erase command */
	/* clear NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	if (retval != ERROR_OK)
		goto err;
	CLR_PORT_BIT(port, ncs);
	if (retval != ERROR_OK)
		goto err;

	/* send command byte */
	retval = msoftspi_shift_out(bank, msoftspi_info->dev.chip_erase_cmd);
	if (retval != ERROR_OK)
		goto err;

err:
	/* set NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	SET_PORT_BIT(port, ncs);
	if (retval != ERROR_OK)
		return retval;

	/* read flash status register */
	retval = read_status_reg(bank, &data);
	if (retval != ERROR_OK)
		return retval;

	/* check for command in progress for flash */
	if ((data & SPIFLASH_WE_BIT) == 0) {
		LOG_ERROR("Mass erase command not accepted by flash. Status=0x%08" PRIx32, data);
		return ERROR_FAIL;
	}

	/* poll WIP for end of self timed Sector Erase cycle */
	retval = wait_till_ready(bank, MSOFTSPI_MASS_ERASE_TIMEOUT);

	duration_measure(&bench);
	if (retval == ERROR_OK) {
		/* set all sectors as erased */
		for (sector = 0; sector < bank->num_sectors; sector++)
			bank->sectors[sector].is_erased = 1;

		command_print(CMD_CTX, "msoftspi mass erase completed in"
			" %fs (%0.3f KiB/s)", duration_elapsed(&bench),
			duration_kbps(&bench, bank->size));
	} else {
		command_print(CMD_CTX, "msoftspi mass erase failed after %fs",
			duration_elapsed(&bench));
	}

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

COMMAND_HANDLER(msoftspi_handle_setparms)
{
	struct flash_bank *bank = NULL;
	struct msoftspi_flash_bank *msoftspi_info = NULL;
	struct flash_sector *sectors = NULL;
	uint32_t temp;
	int retval;

	LOG_DEBUG("%s", __func__);

	if (CMD_ARGC < 2 || CMD_ARGC > 7)
		return ERROR_COMMAND_SYNTAX_ERROR;

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;
	msoftspi_info = bank->driver_priv;

	/* invalidate all old info */
	if (msoftspi_info->probed)
		free(bank->sectors);
	msoftspi_info->probed = 0;
	msoftspi_info->dev.name = NULL;
	msoftspi_info->dev.device_id = 0;

	strncpy(msoftspi_info->devname, CMD_ARGV[1], sizeof(msoftspi_info->devname) - 1);
	msoftspi_info->devname[sizeof(msoftspi_info->devname) - 1] = '\0';

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], temp);
	msoftspi_info->dev.size_in_bytes = temp;
	if (log2u(msoftspi_info->dev.size_in_bytes) < 8) {
		command_print(CMD_CTX, "msoftspi: device size must be 2^n with n >= 8");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[3], msoftspi_info->dev.pagesize);
	if ((log2u(msoftspi_info->dev.pagesize) > log2u(msoftspi_info->dev.size_in_bytes)) ||
		(log2u(msoftspi_info->dev.pagesize) < 0)) {
		command_print(CMD_CTX, "msoftspi: page size must be 2^n and <= device size");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (CMD_ARGC > 4)
		COMMAND_PARSE_NUMBER(u8, CMD_ARGV[4], msoftspi_info->dev.chip_erase_cmd);
	else
		msoftspi_info->dev.chip_erase_cmd = 0x00;

	if (CMD_ARGC > 5) {
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[5], temp);
		msoftspi_info->dev.sectorsize = temp;
		if ((log2u(msoftspi_info->dev.sectorsize) > log2u(msoftspi_info->dev.size_in_bytes)) ||
			(log2u(msoftspi_info->dev.sectorsize) < 0)) {
			command_print(CMD_CTX, "msoftspi: sector size must be 2^n and <= device size");
			return ERROR_COMMAND_SYNTAX_ERROR;
		}

		if (CMD_ARGC > 6)
			COMMAND_PARSE_NUMBER(u8, CMD_ARGV[6], msoftspi_info->dev.erase_cmd);
		else
			return ERROR_COMMAND_SYNTAX_ERROR;
	} else {
		/* no sector size / sector erase cmd given, treat whole bank as a single sector */
		msoftspi_info->dev.erase_cmd = 0x00;
		msoftspi_info->dev.sectorsize = msoftspi_info->dev.size_in_bytes;
	}

	/* set correct size value */
	bank->size = msoftspi_info->dev.size_in_bytes;

	/* create and fill sectors array */
	bank->num_sectors =
		msoftspi_info->dev.size_in_bytes / msoftspi_info->dev.sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (sectors == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset = sector * (msoftspi_info->dev.sectorsize);
		sectors[sector].size = msoftspi_info->dev.sectorsize;
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 0;
	}

	bank->sectors = sectors;
	msoftspi_info->dev.name = msoftspi_info->devname;
	msoftspi_info->probed = 1;

	return ERROR_OK;
}

COMMAND_HANDLER(msoftspi_handle_spicmd)
{
	struct target *target = NULL;
	struct flash_bank *bank;
	struct msoftspi_flash_bank *msoftspi_info;
	uint32_t port, data;
	uint8_t num_bytes, cmd_byte;
	const int max = 21;
	unsigned int count;
	char temp[4], output[(2 + max + 256) * 3 + 8];
	int retval;

	LOG_DEBUG("%s", __func__);

	if ((CMD_ARGC < 3) || (CMD_ARGC > max + 3))
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (CMD_ARGC > max + 3)	{
		LOG_ERROR("at most %d bytes may be send", max);
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	msoftspi_info = bank->driver_priv;
	target = bank->target;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[1], num_bytes);
	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[2], cmd_byte);

	/* clear NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	if (retval != ERROR_OK)
		goto err;
	CLR_PORT_BIT(port, ncs);
	if (retval != ERROR_OK)
		goto err;

	/* send command byte */
	snprintf(output, sizeof(output), "spicmd: %02x ", cmd_byte & 0xFF);
	retval = msoftspi_shift_out(bank, cmd_byte);
	if (retval != ERROR_OK)
		goto err;

	/* send additional bytes */
	for (count = 3; count < CMD_ARGC; count++) {
		COMMAND_PARSE_NUMBER(u8, CMD_ARGV[count], cmd_byte);
		snprintf(temp, sizeof(temp), "%02x ", cmd_byte & 0xFF);
		retval = msoftspi_shift_out(bank, cmd_byte);
		if (retval != ERROR_OK)
			goto err;
		strncat(output, temp, sizeof(output) - strlen(output) - 1);
	}

	/* set to input */
	strncat(output, "-> ", sizeof(output) - strlen(output) - 1);

	for ( ; num_bytes > 0; num_bytes--)	{
		retval = msoftspi_shift_in(bank, &data);
		if (retval != ERROR_OK)
			goto err;
		snprintf(temp, sizeof(temp), "%02x ", data & 0xFF);
		strncat(output, temp, sizeof(output) - strlen(output) - 1);
	}
	command_print(CMD_CTX, "%s", output);

err:
	/* set NCS */
	retval = target_read_u32(target, msoftspi_info->ncs.addr, &port);
	SET_PORT_BIT(port, ncs);

	/* set to output */

	return retval;
}

static const struct command_registration msoftspi_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.handler = msoftspi_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Mass erase entire flash device.",
	},
	{
		.name = "setparms",
		.handler = msoftspi_handle_setparms,
		.mode = COMMAND_EXEC,
		.usage = "bank_id name total_size page_size [ mass_erase_cmd ] [ sector_size sector_erase_cmd ]",
		.help = "Set flash chip parameters",
	},
	{
		.name = "spicmd",
		.handler = msoftspi_handle_spicmd,
		.mode = COMMAND_EXEC,
		.usage = "bank_id num_resp cmd_byte ...",
		.help = "Send low-level command cmd_byte and following bytes, read num_bytes.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration msoftspi_command_handlers[] = {
	{
		.name = "msoftspi",
		.mode = COMMAND_ANY,
		.help = "msoftspi flash command group",
		.usage = "",
		.chain = msoftspi_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver msoftspi_flash = {
	.name = "msoftspi",
	.commands = msoftspi_command_handlers,
	.flash_bank_command = msoftspi_flash_bank_command,
	.erase = msoftspi_erase,
	.protect = msoftspi_protect,
	.write = msoftspi_write,
	.read = msoftspi_read,
	.probe = msoftspi_probe,
	.auto_probe = msoftspi_auto_probe,
	.erase_check = msoftspi_blank_check,
	.protect_check = msoftspi_protect_check,
	.info = get_msoftspi_info,
};
