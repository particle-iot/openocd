/***************************************************************************
 *   Copyright (C) 2018 by Konstantin Kraskovskiy <kraskovski@otsl.jp>     *
 *   Copyright (C) 2018 by OTSL Inc. <info@otsl.jp>                        *
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

/*
  * This is serial flash controller (aka SPIBSC) driver for Renesas EC-1.
  *
  * Its design is derived from mrvlqspi driver
  * Copyright (C) 2014 by Mahavir Jain <mjain@marvell.com>
  *
  */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include "spi.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/arm.h>
#include "../../../contrib/loaders/flash/ec-1/code.inc"

#define BLOCK_ERASE_TIMEOUT (50UL)
#define CHIP_ERASE_TIMEOUT (5000UL)

#define RCF (1UL << 9)

#define MODE_SPI (1UL)
#define MODE_MAP (0UL)
#define TEND (1UL)
#define SPIE (1UL)
#define SPIRE (4UL)
#define SPIWE (2UL)
#define SPIDE (0xFUL)
#define ADE_24 (0x7UL << 8)
#define OCDE (1UL << 12)
#define CDE (1UL << 14)
#define SSLN (1UL << 24)
#define MD (1UL << 31)

#define CMNCR 0x0UL /* SPIBSC_BASE + 0x0 */
#define SSLDR 0x4UL
#define SPBCR 0x8UL
#define DRCR 0xCUL

#define SMCR 0x20UL
#define SMCMR 0x24UL
#define SMADR 0x28UL
#define SMENR 0x30UL
#define SMRDR0 0x38UL
#define SMWDR0 0x40UL
#define CMNSR 0x48UL

struct platform_flash_bank {
	int probed;
	uint32_t reg_base;
	uint32_t bank_num;
	const struct flash_device *dev;
};

enum burst_size {
	BURST_NONE = 0,
	BURST_OCTET = 8,
	BURST_WORD = 15
};

static inline uint32_t spibsc_get_reg(struct flash_bank *bank, uint32_t reg)
{
	struct platform_flash_bank *platform_info = bank->driver_priv;
	return reg + platform_info->reg_base;
}

static inline int spibsc_set_addr(struct flash_bank *bank, uint32_t addr)
{
	int retval;
	uint32_t regval;
	struct target *target = bank->target;

	retval = target_write_u32(target,
			spibsc_get_reg(bank, SMADR), addr);
	if (retval != ERROR_OK)
		return retval;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, SMENR), &regval);
	if (retval != ERROR_OK)
		return retval;

	regval |= ADE_24;

	retval = target_write_u32(target,
			spibsc_get_reg(bank, SMENR), regval);

	return retval;
}

static inline int spibsc_set_cmd(struct flash_bank *bank, uint32_t cmd)
{
	int retval;
	uint32_t regval;
	struct target *target = bank->target;

	retval = target_write_u32(target,
			spibsc_get_reg(bank, SMCMR), (cmd << 16));
	if (retval != ERROR_OK)
		return retval;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, SMENR), &regval);
	if (retval != ERROR_OK)
		return retval;

	regval |= CDE;

	retval = target_write_u32(target,
			spibsc_get_reg(bank, SMENR), regval);

	return retval;
}

static inline int spibsc_mode_init(struct flash_bank *bank, bool mode)
{
	int retval;
	uint32_t regval;
	struct target *target = bank->target;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, CMNSR), &regval);
	if (retval != ERROR_OK)
		return retval;

	if (!(regval & TEND))
		return ERROR_FLASH_BUSY;


	if (mode == MODE_SPI) {
		regval = 0;

		retval = target_write_u32(target,
				spibsc_get_reg(bank, SMCR), regval);
		if (retval != ERROR_OK)
			return retval;

		retval = target_write_u32(target,
				spibsc_get_reg(bank, SMENR), regval);
		if (retval != ERROR_OK)
			return retval;

	}

	return ERROR_OK;
}

static int spibsc_start_transfer(struct flash_bank *bank)
{
	int retval;
	uint32_t regval;
	struct target *target = bank->target;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, CMNSR), &regval);
	if (retval != ERROR_OK)
		return retval;

	if (!(regval & TEND))
		return ERROR_FLASH_BUSY;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, SMCR), &regval);
	if (retval != ERROR_OK)
		return retval;

	regval |= SPIE;

	retval = target_write_u32(target,
			spibsc_get_reg(bank, SMCR), regval);
	if (retval != ERROR_OK)
		return retval;

	do {
		keep_alive();
		retval = target_read_u32(target,
				spibsc_get_reg(bank, CMNSR), &regval);
		if (retval != ERROR_OK)
			return retval;
	} while (!(regval & TEND));

	return ERROR_OK;
}

static int spibsc_cache_flush(struct flash_bank *bank)
{
	int retval;
	uint32_t val;
	struct target *target = bank->target;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, DRCR), &val);
	if (retval != ERROR_OK)
		return retval;

	val |= RCF;

	retval = target_write_u32(target,
			spibsc_get_reg(bank, DRCR), val);
	if (retval != ERROR_OK)
		return retval;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, DRCR), &val);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int spibsc_set_mode(struct flash_bank *bank, bool mode)
{
	int retval;
	uint32_t regval;
	struct target *target = bank->target;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, CMNCR), &regval);
	if (retval != ERROR_OK)
		return retval;

	if (!(regval & MD)) {

		retval = target_read_u32(target,
				spibsc_get_reg(bank, DRCR), &regval);
		if (retval != ERROR_OK)
			return retval;

		regval |= SSLN;

		retval = target_write_u32(target,
				spibsc_get_reg(bank, DRCR), regval);
		if (retval != ERROR_OK)
			return retval;

	}

	do {
		keep_alive();
		retval = target_read_u32(target,
				spibsc_get_reg(bank, CMNSR), &regval);
		if (retval != ERROR_OK)
			return retval;
	} while (regval != 1);

	/* Flush SPIBSC cache */
	retval = spibsc_cache_flush(bank);
	if (retval != ERROR_OK)
		return retval;

	/* set timing */
	retval = target_write_u32(target,
			spibsc_get_reg(bank, SSLDR), 0x030303);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target,
			spibsc_get_reg(bank, SPBCR), 0x3);
	if (retval != ERROR_OK)
		return retval;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, CMNCR), &regval);
	if (retval != ERROR_OK)
		return retval;

	regval |= mode << 31;

	retval = target_write_u32(target,
			spibsc_get_reg(bank, CMNCR), regval);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

/* Caller shall provide an appropriate pointer */
static int spibsc_cmd_read(struct flash_bank *bank, void *data, enum burst_size len)
{
	int retval;
	uint32_t regval;
	struct target *target = bank->target;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, SMCR), &regval);
	if (retval != ERROR_OK)
		return retval;

	if (len)
		regval |= SPIRE;
	else
		regval &= ~SPIRE;

	retval = target_write_u32(target,
			spibsc_get_reg(bank, SMCR), regval);
	if (retval != ERROR_OK)
		return retval;

	retval = target_read_u32(target,
			spibsc_get_reg(bank, SMENR), &regval);
	if (retval != ERROR_OK)
		return retval;

	regval &= ~SPIDE;
	regval |= (SPIDE & len);

	retval = target_write_u32(target,
			spibsc_get_reg(bank, SMENR), regval);
	if (retval != ERROR_OK)
		return retval;

	retval = spibsc_start_transfer(bank);
	if (retval != ERROR_OK)
		return retval;

	if (len == BURST_WORD) {
		retval = target_read_u32(target,
				spibsc_get_reg(bank, SMRDR0), data);
	} else if (len == BURST_OCTET) {
		retval = target_read_u8(target,
				spibsc_get_reg(bank, SMRDR0), data);
	} else
		retval = ERROR_FAIL;

	return retval;
}


static int platform_flash_status(struct flash_bank *bank, uint8_t *status, unsigned long polling_interval_ms)
{
	int retval = ERROR_OK;

	retval = spibsc_mode_init(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	retval = spibsc_set_cmd(bank, SPIFLASH_READ_STATUS);
	if (retval != ERROR_OK)
		return retval;

	if (polling_interval_ms) {

		do {

			alive_sleep(polling_interval_ms);

			retval = spibsc_cmd_read(bank, status, BURST_OCTET);
			if (retval != ERROR_OK)
				return retval;

		} while ((*status) & SPIFLASH_BSY_BIT);

	} else {

		retval = spibsc_cmd_read(bank, status, BURST_OCTET);
		if (retval != ERROR_OK)
			return retval;

	}

	return retval;
}

static int platform_write_enable(struct flash_bank *bank)
{
	int retval;
	uint8_t data;

	retval = spibsc_mode_init(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	retval = spibsc_set_cmd(bank, SPIFLASH_WRITE_ENABLE);
	if (retval != ERROR_OK)
		return retval;

	retval = spibsc_start_transfer(bank);
	if (retval != ERROR_OK)
		return retval;

	retval = platform_flash_status(bank, &data, 0);
	if (retval != ERROR_OK)
		return retval;

	if ((data & 0xFFU) != 2)
		return ERROR_FLASH_BUSY;

	return retval;
}

static int platform_read_id(struct flash_bank *bank, uint32_t *id)
{
	int retval;

	LOG_DEBUG("Getting ID");

	retval = spibsc_set_mode(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	/* Initialize SPIBSC */
	retval = spibsc_mode_init(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	/* Set SPI instruction */
	retval = spibsc_set_cmd(bank, SPIFLASH_READ_ID);
	if (retval != ERROR_OK)
		return retval;

	/* Read 32 bits from SPIBSC */
	retval = spibsc_cmd_read(bank, id, BURST_WORD);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int platform_block_erase(struct flash_bank *bank, uint32_t offset)
{
	int retval;
	uint8_t data;
	struct platform_flash_bank *platform_info = bank->driver_priv;

	retval = spibsc_mode_init(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	/* Set read offset address */
	retval = spibsc_set_addr(bank, offset);
	if (retval != ERROR_OK)
		return retval;

	/* Set instruction */
	retval = spibsc_set_cmd(bank, platform_info->dev->erase_cmd);
	if (retval != ERROR_OK)
		return retval;

	retval = spibsc_start_transfer(bank);
	if (retval != ERROR_OK)
		return retval;

	LOG_INFO("0x%08" PRIx32 "KiB block erasing. Wait...",
			(unsigned)platform_info->dev->sectorsize);

	retval = platform_flash_status(bank, &data, BLOCK_ERASE_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	if (data & (SPIFLASH_BSY_BIT | SPIFLASH_WE_BIT))
		return ERROR_FLASH_OPERATION_FAILED;

	return ERROR_OK;
}

static int platform_bulk_erase(struct flash_bank *bank)
{
	int retval;
	uint8_t data;
	struct platform_flash_bank *platform_info = bank->driver_priv;

	retval = spibsc_mode_init(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	/* Set instruction */
	retval = spibsc_set_cmd(bank, platform_info->dev->chip_erase_cmd);
	if (retval != ERROR_OK)
		return retval;

	retval = spibsc_start_transfer(bank);
	if (retval != ERROR_OK)
		return retval;

	LOG_INFO("Chip erasing. Wait...");

	retval = platform_flash_status(bank, &data, CHIP_ERASE_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	if (data & (SPIFLASH_BSY_BIT | SPIFLASH_WE_BIT))
		return ERROR_FLASH_OPERATION_FAILED;

	return ERROR_OK;
}

static int platform_flash_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	struct platform_flash_bank *platform_info = bank->driver_priv;
	int retval = ERROR_OK;
	int sector;

	LOG_DEBUG("erase from sector %d to sector %d", first, last);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((first < 0) || (last < first) || (last >= bank->num_sectors)) {
		LOG_ERROR("Flash sector invalid");
		return ERROR_FLASH_SECTOR_INVALID;
	}

	if (!(platform_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	for (sector = first; sector <= last; sector++) {
		if (bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	retval = spibsc_set_mode(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	/* Initialize SPIBSC */
	retval = spibsc_mode_init(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	retval = platform_write_enable(bank);
	if (retval != ERROR_OK)
		return retval;

	/* If we're erasing the entire chip and the flash supports
	 * it, use a bulk erase instead of going sector-by-sector. */
	if (first == 0 && last == (bank->num_sectors - 1)
		&& platform_info->dev->chip_erase_cmd !=
					platform_info->dev->erase_cmd) {
		LOG_DEBUG("Chip supports the bulk erase command."\
		" Will use bulk erase instead of sector-by-sector erase.");
		retval = platform_bulk_erase(bank);
		if (retval == ERROR_OK) {
			return retval;
		} else
			LOG_WARNING("Bulk flash erase failed."
				" Falling back to sector-by-sector erase.");
	}

	for (sector = first; sector <= last; sector++) {
		retval = platform_block_erase(bank,
				sector * platform_info->dev->sectorsize);
		if (retval != ERROR_OK)
			return retval;
	}

	return retval;
}

static int platform_flash_write(struct flash_bank *bank,
		const uint8_t *buffer,
		uint32_t offset,
		uint32_t count)
{
	struct target *target = bank->target;
	struct platform_flash_bank *platform_info = bank->driver_priv;
	int retval = ERROR_OK;
	uint32_t i;

	LOG_INFO("Begin at offset=0x%08" PRIx32 " count=0x%08" PRIx32,
		offset, count);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!(platform_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	if (offset + count > platform_info->dev->size_in_bytes) {
		LOG_WARNING("Writes past end of flash. Extra data discarded.");
		count = platform_info->dev->size_in_bytes - offset;
	}

	retval = spibsc_set_mode(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	/* Initialize SPIBSC */
	retval = spibsc_mode_init(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	/* Check sector protection */
	for (i = 0; i < (unsigned)bank->num_sectors; i++) {
		/* Start offset in or before this sector? */
		/* End offset in or behind this sector? */
		if ((offset <
			(bank->sectors[i].offset + bank->sectors[i].size))
			&& ((offset + count - 1) >= bank->sectors[i].offset)
			&& bank->sectors[i].is_protected) {
			LOG_ERROR("Flash sector %d protected", i);
			return ERROR_FAIL;
		}
	}

	struct working_area *p_page, *p_code;
	struct reg_param reg_params[4];
	struct arm_algorithm arm_info;

	/* Check the target's RAM availability */
	i = target_get_working_area_avail(target);

	if (i < (code_len + platform_info->dev->pagesize + 8)) {
		LOG_ERROR("Insufficient memory. Please allocate at least"\
			" %uB of working area.",
			(code_len + platform_info->dev->pagesize));

		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	i = code_len + 8 - (code_len & 3);

	/* Allocate target's RAM for the code */
	if (target_alloc_working_area(target, i,
			&p_code) != ERROR_OK) {
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	/* Populate the code at the target */
	retval = target_write_buffer(target, p_code->address,
			code_len, code);
	if (retval != ERROR_OK) {
		target_free_working_area(target, p_code);
		return retval;
	}

	/* allocate the page buffer */
	if (target_alloc_working_area(target, platform_info->dev->pagesize,
			&p_page) != ERROR_OK) {
		target_free_working_area(target, p_code);
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	arm_info.common_magic = ARM_COMMON_MAGIC;
	arm_info.core_mode = ARM_MODE_SVC;
	arm_info.core_state = ARM_STATE_ARM;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT); /* count and result */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT); /* dummy */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT); /* offset */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT); /* buffer start */

	buf_set_u32(reg_params[3].value, 0, 32, p_page->address);

	while (count) {

		unsigned rest = platform_info->dev->pagesize - (offset & (platform_info->dev->pagesize - 1));

		LOG_INFO("...at offset=0x%08" PRIx32 " 0x%08" PRIx32 " bytes to write", offset, count);

		if (count <= rest)
			rest = count;

		buf_set_u32(reg_params[0].value, 0, 32, rest);
		buf_set_u32(reg_params[2].value, 0, 32, offset);

		/* Populate the data at the target */
		retval = target_write_buffer(target, p_page->address,
				rest, buffer);
		if (retval != ERROR_OK)
			break;

		count -= rest;
		offset += rest;
		buffer += rest;

		/* program page */
		retval = target_run_algorithm(target,
				0, NULL,
				4, reg_params,
				p_code->address, 0,
				10000, &arm_info);
		if (retval != ERROR_OK)
			break;

	}

	if (retval != ERROR_OK)
		LOG_ERROR("Error executing flash write algorithm");

	target_free_working_area(target, p_page);
	target_free_working_area(target, p_code);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);

	return retval;
}

int platform_flash_read(struct flash_bank *bank, uint8_t *buffer,
				uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct platform_flash_bank *platform_info = bank->driver_priv;
	int retval;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!(platform_info->probed)) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	if (!count)
		return ERROR_FAIL;

	retval = spibsc_set_mode(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	/* Initialize SPIBSC */
	retval = spibsc_mode_init(bank, MODE_SPI);
	if (retval != ERROR_OK)
		return retval;

	/* Set SPI instruction */
	retval = spibsc_set_cmd(bank, SPIFLASH_READ);
	if (retval != ERROR_OK)
		return retval;

	/* Set read offset address */
	retval = spibsc_set_addr(bank, offset++);
	if (retval != ERROR_OK)
		return retval;

	/* Read an octet from SPIBSC */
	retval = spibsc_cmd_read(bank, buffer++, BURST_OCTET);
	if (retval != ERROR_OK)
		return retval;
	count--;

	/* can be faster, but do you really need this? */
	while (count--) {
		retval = spibsc_set_addr(bank, offset++);
		if (retval != ERROR_OK)
			return retval;

		retval = spibsc_start_transfer(bank);
		if (retval != ERROR_OK)
			return retval;

		retval = target_read_u8(target,
				spibsc_get_reg(bank, SMRDR0), buffer++);
		if (retval != ERROR_OK)
			return retval;

	}

	return ERROR_OK;
}

static int platform_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct platform_flash_bank *platform_info = bank->driver_priv;
	uint32_t id = 0;
	int retval;
	struct flash_sector *sectors;

	/* If we've already probed, we should be fine to skip this time. */
	if (platform_info->probed)
		return ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Probe: target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	platform_info->probed = 0;
	platform_info->bank_num = bank->bank_number;

	/* Read flash JEDEC ID */
	retval = platform_read_id(bank, &id);
	if (retval != ERROR_OK)
		return retval;

	platform_info->dev = NULL;
	for (const struct flash_device *p = flash_devices; p->name ; p++)
		if (p->device_id == id) {
			platform_info->dev = p;
			break;
		}

	if (!platform_info->dev) {
		LOG_ERROR("Unknown flash device ID 0x%08" PRIx32, id);
		return ERROR_FAIL;
	}

	LOG_INFO("Found flash device \'%s\' ID 0x%08" PRIx32,
		platform_info->dev->name, platform_info->dev->device_id);

	/* Set correct size value */
	bank->size = platform_info->dev->size_in_bytes;

	/* create and fill sectors array */
	bank->num_sectors = platform_info->dev->size_in_bytes /
					platform_info->dev->sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (sectors == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset =
				sector * platform_info->dev->sectorsize;
		sectors[sector].size = platform_info->dev->sectorsize;
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 0;
	}

	bank->sectors = sectors;
	platform_info->probed = 1;

	return ERROR_OK;
}

static int platform_auto_probe(struct flash_bank *bank)
{
	int retval;
	uint32_t regval;
	struct target *target = bank->target;
	struct platform_flash_bank *platform_info = bank->driver_priv;

	if (platform_info->probed)
		return ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Autoprobe: target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = target_read_u32(target,
			spibsc_get_reg(bank, CMNSR), &regval);
	if (retval != ERROR_OK)
		return retval;

	if (retval & ~TEND) {
		LOG_ERROR("Not a SPIBSC or ROM is busy");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	return ERROR_OK;
}

static int platform_flash_erase_check(struct flash_bank *bank)
{
	/* Not implemented yet */
	return ERROR_OK;
}

static int platform_protect_check(struct flash_bank *bank)
{
	/* Not implemented yet */
	return ERROR_OK;
}

int platform_get_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct platform_flash_bank *platform_info = bank->driver_priv;

	if (!(platform_info->probed)) {
		snprintf(buf, buf_size,
			"\nSPI flash bank not probed yet\n");
		return ERROR_OK;
	}

	snprintf(buf, buf_size, "\nSPI flash information:\n"
		"  Device \'%s\' ID 0x%08" PRIx32 "\n",
		platform_info->dev->name, platform_info->dev->device_id);

	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(platform_flash_bank_command)
{
	struct platform_flash_bank *platform_info;

	if (CMD_ARGC < 7)
		return ERROR_COMMAND_SYNTAX_ERROR;

	platform_info = malloc(sizeof(struct platform_flash_bank));
	if (platform_info == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	/* Get SPIBSC controller base address */
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[6], platform_info->reg_base);
	bank->driver_priv = platform_info;
	platform_info->probed = 0;

	return ERROR_OK;
}

struct flash_driver ec1_flash = {
	.name = "spibsc",
	.flash_bank_command = platform_flash_bank_command,
	.erase = platform_flash_erase,
	.protect = NULL,
	.write = platform_flash_write,
	.read = platform_flash_read,
	.probe = platform_probe,
	.auto_probe = platform_auto_probe,
	.erase_check = platform_flash_erase_check,
	.protect_check = platform_protect_check,
	.info = platform_get_info,
	.free_driver_priv = default_flash_free_driver_priv,
};
