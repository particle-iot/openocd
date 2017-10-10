/***************************************************************************
 *   Copyright (C) 2017 IndieSemi                                          *
 *   Artur Troian <troian.ap@gmail.com>                                    *
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

#include "imp.h"
#include <target/algorithm.h>
#include <target/armv7m.h>
#include <helper/types.h>
#include <target/target_type.h>

#define CODE_PAGE_SIZE (256 * 8)
#define MAIN_SECTORS_AMOUNT 64
#define NVR_SECTORS_AMOUNT 2
#define REDUNDANCY_SECTORS_AMOUNT 2

struct ind80xxx_info {
	uint32_t code_page_size;

	struct {
		bool       probed;
		int (*write) (
				struct flash_bank *bank
				, struct ind80xxx_info *chip
				, const uint8_t *buffer
				, uint32_t offset
				, uint32_t count);
	} bank[2];

	struct target *target;
};

struct nrf51_device_spec {
	uint16_t hwid;
	const char *part;
	const char *variant;
	const char *build_code;
	unsigned int flash_size_kb;
};

enum {
	FLASHCTRL_SFR_CTRL         = 0xA0000000,
	FLASHCTRL_SFR_ADDR         = 0xA0000004,
	FLASHCTRL_SFR_WRITEDATA    = 0xA0000008,
	FLASHCTRL_SFR_UNLOCKWRITE  = 0xA000000C,
	FLASHCTRL_SFR_STARTWRITE   = 0xA0000010,
	FLASHCTRL_SFR_UNLOCKERASE  = 0xA0000014,
	FLASHCTRL_SFR_STARTERASE   = 0xA0000018,
	FLASHCTRL_SFR_UNLOCKCTRLOP = 0xA000001C,
	FLASHCTRL_SFR_CTRLOP       = 0xA0000020,
};

#define E_FLASH_CTRL_CHIP                 ((uint8_t) 0x20)   /* Chip erase enable */
#define E_FLASH_CTRL_RECALL               ((uint8_t) 0010)   /* NVR SST region read enable */
#define E_FLASH_CTRL_NVR_SST_PRO          ((uint8_t) 0x08)   /* NVR_SST region protection */
#define E_FLASH_CTRL_NVR_SST              ((uint8_t) 0x04)   /* NVR_SST region selection */
#define E_FLASH_CTRL_NVR                  ((uint8_t) 0x02)   /* NVR region selection */
#define E_FLASH_CTRL_ADDRN                ((uint8_t) 0x01)   /* redundancy region selection */
#define E_FLASH_CTRL_MAIN                 ((uint8_t) 0x08)   /* default main region selection */

#define E_FLASH_CTRL_UNLOCK_PAT           ((uint32_t) 0xACDC1972)
#define E_FLASH_WRITE_UNLOCK_PAT          ((uint32_t) 0x55555555)
#define E_FLASH_WRITE_START_PAT           ((uint32_t) 0xAAAAAAAA)
#define E_FLASH_ERASE_UNLOCK_PAT          ((uint32_t) 0x66666666)
#define E_FLASH_ERASE_START_PAT           ((uint32_t) 0xEEEEEEEE)

enum {
	IND80XXX_MAIN_BASE = 0x00000000,
	IND80XXX_NVR_BASE = 0x00020000
};

static int ind80xxx_bank_is_probed(struct flash_bank *bank)
{
	struct ind80xxx_info *chip = bank->driver_priv;

	assert(chip != NULL);

	return chip->bank[bank->bank_number].probed;
}

static int ind80xxx_probe(struct flash_bank *bank);

static int ind80xxx_get_probed_chip_if_halted(struct flash_bank *bank, struct ind80xxx_info **chip)
{
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	*chip = bank->driver_priv;

	int probed = ind80xxx_bank_is_probed(bank);
	if (probed < 0)
		return probed;
	else if (!probed)
		return ind80xxx_probe(bank);
	else
		return ERROR_OK;
}

static int ind80xxx_generic_erase(struct ind80xxx_info *chip, uint32_t addr, uint8_t op)
{
	int res;
	res = target_write_u32(chip->target, FLASHCTRL_SFR_UNLOCKCTRLOP, E_FLASH_CTRL_UNLOCK_PAT);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to enable erase operation");
		return res;
	}

	res = target_write_u32(chip->target, FLASHCTRL_SFR_CTRLOP, 0x0000003F&op);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to set erase op");
		return res;
	}

	res = target_write_u32(chip->target, FLASHCTRL_SFR_ADDR, addr);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to set erase start addr");
		return res;
	}

	res = target_write_u32(chip->target, FLASHCTRL_SFR_UNLOCKERASE, E_FLASH_ERASE_UNLOCK_PAT);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to unlock erase operation");
		return res;
	}

	uint32_t val;
	res = target_read_u32(chip->target, FLASHCTRL_SFR_ADDR, &val);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to read erase addr");
		return res;
	}

	if (val != addr) {
		LOG_ERROR("Erase address does not match: expected=0x%"PRIx32" received=0x%"PRIx32, addr, val);
		return ERROR_FAIL;
	}

	res = target_write_u32(chip->target, FLASHCTRL_SFR_STARTERASE, E_FLASH_ERASE_START_PAT);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to start erase operation");
		return res;
	}

	return res;
}

static int ind80xxx_protect_check(struct flash_bank *bank)
{
	struct ind80xxx_info *chip = bank->driver_priv;

	assert(chip != NULL);

	for (int i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_protected = -1;

	return ERROR_OK;
}

static int ind80xxx_protect(struct flash_bank *bank, int set, int first, int last)
{
	ind80xxx_protect_check(bank);

	return ERROR_OK;
}

static int ind80xxx_probe(struct flash_bank *bank)
{
	struct ind80xxx_info *chip = bank->driver_priv;

	uint32_t bank_id = 0;

	if (bank->base == IND80XXX_MAIN_BASE) {
		chip->code_page_size = CODE_PAGE_SIZE;
		bank->num_sectors = MAIN_SECTORS_AMOUNT;
	} else {
		bank->num_sectors = NVR_SECTORS_AMOUNT;
		bank_id = 1;
	}

	bank->size = bank->num_sectors * chip->code_page_size;
	bank->sectors = calloc(bank->num_sectors, sizeof((bank->sectors)[0]));
	if (!bank->sectors)
		return ERROR_FLASH_BANK_NOT_PROBED;

	/* Fill out the sector information: all iND80XXX sectors are the same size and
	 * there is always a fixed number of them. */
	for (int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].size = chip->code_page_size;
		bank->sectors[i].offset = i * chip->code_page_size;

		/* mark as unknown */
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = -1;
	}

	ind80xxx_protect_check(bank);

	chip->bank[bank_id].probed = true;

	return ERROR_OK;
}

static int ind80xxx_auto_probe(struct flash_bank *bank)
{
	int probed = ind80xxx_bank_is_probed(bank);

	if (probed < 0)
		return probed;
	else if (probed)
		return ERROR_OK;
	else
		return ind80xxx_probe(bank);
}

static struct flash_sector *ind80xxx_find_sector_by_address(struct flash_bank *bank, uint32_t address)
{
	struct ind80xxx_info *chip = bank->driver_priv;

	for (int i = 0; i < bank->num_sectors; i++) {
		if (bank->sectors[i].offset <= address && address < (bank->sectors[i].offset + chip->code_page_size))
			return &bank->sectors[i];
	}

	return NULL;
}

static int ind80xxx_erase_all(struct ind80xxx_info *chip)
{
	LOG_INFO("Erasing all non-volatile memory");

	return ind80xxx_generic_erase(chip, 0x00000000, E_FLASH_CTRL_MAIN | E_FLASH_CTRL_CHIP);
}

#if 0
static int ind80xxx_erase_sector(struct flash_bank *bank, struct ind80xxx_info *chip, struct flash_sector *sector)
{
	LOG_INFO("Erasing page at 0x%"PRIx32, sector->offset);

	int res = ERROR_OK;
	res = ind80xxx_generic_erase(chip, sector->offset, E_FLASH_CTRL_MAIN);

	if (res == ERROR_OK)
		sector->is_erased = 1;
	else
		LOG_ERROR("Failed erase page at 0x%"PRIx32, sector->offset);

	return res;
}
#endif

static const uint8_t ind80xxx_flash_write_code[] = {
#include "../../../contrib/loaders/flash/ind/ind80xxx.inc"
};

static int ind80xxx_slow_flash_write(struct ind80xxx_info *chip, uint32_t offset, const uint8_t *buffer, uint32_t count)
{
	const uint32_t *buf = (void *)buffer;

	for (; count > 0; count -= 4) {
		int res;
		res = target_write_u32(chip->target, FLASHCTRL_SFR_ADDR, offset);
		if (res != ERROR_OK) {
			LOG_ERROR("Failed to set write address");
			return res;
		}
		res = target_write_u32(chip->target, FLASHCTRL_SFR_UNLOCKWRITE, E_FLASH_WRITE_UNLOCK_PAT);
		if (res != ERROR_OK) {
			LOG_ERROR("Failed to write unlock pattern");
			return res;
		}
		uint32_t val;
		res = target_read_u32(chip->target, FLASHCTRL_SFR_ADDR, &val);
		if (res != ERROR_OK) {
			LOG_ERROR("Failed to read write addr");
			return res;
		}
		if (val != offset) {
			LOG_ERROR("Write address does not match: expected=0x%"PRIx32" received=0x%"PRIx32, offset, val);
			return ERROR_FAIL;
		}
		res = target_write_u32(chip->target, FLASHCTRL_SFR_WRITEDATA, *buf);
		if (res != ERROR_OK) {
			LOG_ERROR("Failed to set write data");
			return res;
		}
		res = target_write_u32(chip->target, FLASHCTRL_SFR_STARTWRITE, E_FLASH_WRITE_START_PAT);
		if (res != ERROR_OK) {
			LOG_ERROR("Failed to set start write pattern");
			return res;
		}
		offset += 4;
		buf++;
	}

	return ERROR_OK;
}

/* Start a low level flash write for the specified region */
static int ind80xxx_ll_flash_write(struct ind80xxx_info *chip, uint32_t offset, const uint8_t *buffer, uint32_t count)
{
	struct target *target = chip->target;
	uint32_t buffer_size = 8192;
	struct working_area *write_algorithm;
	struct working_area *source;
	uint32_t address = 0 + offset;
	struct reg_param reg_params[4];
	struct armv7m_algorithm armv7m_info;
	int retval = ERROR_OK;


	LOG_INFO("Writing buffer to flash offset=0x%"PRIx32" count=0x%"PRIx32, offset, count);
	assert(count % 4 == 0);

	LOG_INFO("working area phys 0x%"PRIx32, (uint32_t)target->working_area_phys);
	/* allocate working area with flash programming code */
	if (target_alloc_working_area(target, sizeof(ind80xxx_flash_write_code), &write_algorithm) != ERROR_OK) {
		LOG_WARNING("no working area available, falling back to slow memory writes");

		return ind80xxx_slow_flash_write(chip, offset, buffer, count);
	}

	retval = target_write_buffer(target, write_algorithm->address,
								 sizeof(ind80xxx_flash_write_code),
								 ind80xxx_flash_write_code);
	if (retval != ERROR_OK) {
		LOG_ERROR("Cannot write flash algorithm, falling back to slow memory writes");
		return ind80xxx_slow_flash_write(chip, offset, buffer, count);
	}

	/* memory buffer */
	while (target_alloc_working_area(target, buffer_size, &source) != ERROR_OK) {
		buffer_size /= 2;
		buffer_size &= ~3UL; /* Make sure it's 4 byte aligned */
		if (buffer_size <= 256) {
			/* free working area, write algorithm already allocated */
			target_free_working_area(target, write_algorithm);

			LOG_WARNING("No large enough working area available, falling back to slow memory writes");
			return ind80xxx_slow_flash_write(chip, offset, buffer, count);
		}
	}

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);	/* workarea start, status (out) */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* workarea end */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);	/* target */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);	/* count */

	buf_set_u32(reg_params[0].value, 0, 32, source->address);
	buf_set_u32(reg_params[1].value, 0, 32, source->address + source->size);
	buf_set_u32(reg_params[2].value, 0, 32, address);
	buf_set_u32(reg_params[3].value, 0, 32, count);

	retval = target_run_flash_async_algorithm(target, buffer, count / 4, 4,
											  0, NULL,
											  4, reg_params,
											  source->address, source->size,
											  write_algorithm->address, 0,
											  &armv7m_info);

	target_free_working_area(target, source);
	target_free_working_area(target, write_algorithm);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);

	return retval;
}

/* Check and erase flash sectors in specified range then start a low level page write.
   start/end must be sector aligned.
*/
static int ind80xxx_write_pages(struct flash_bank *bank, uint32_t start, uint32_t end, const uint8_t *buffer)
{
	int res = ERROR_FAIL;
	struct ind80xxx_info *chip = bank->driver_priv;
	struct flash_sector *sector;
	uint32_t offset;

	assert(start % chip->code_page_size == 0);
	assert(end % chip->code_page_size == 0);

	/* Erase all sectors */
	for (offset = start; offset < end; offset += chip->code_page_size) {
		sector = ind80xxx_find_sector_by_address(bank, offset);
		if (!sector) {
			LOG_ERROR("Invalid sector @ 0x%08"PRIx32, offset);
			return ERROR_FLASH_SECTOR_INVALID;
		}

		if (sector->is_protected) {
			LOG_ERROR("Can't erase protected sector @ 0x%08"PRIx32, offset);
			goto error;
		}

#if 0
		/* wait until erase by sector implemented */
		if (sector->is_erased != 1) {	/* 1 = erased, 0= not erased, -1 = unknown */
			res = ind80xxx_erase_sector(bank, chip, sector);
			if (res != ERROR_OK) {
				LOG_ERROR("Failed to erase sector @ 0x%08"PRIx32, sector->offset);
				goto error;
			}
		}
#endif
		sector->is_erased = 0;
	}

	res = ind80xxx_ll_flash_write(chip, start, buffer, (end - start));
	if (res != ERROR_OK)
		goto error;

	return ERROR_OK;
error:
	LOG_ERROR("Failed to write to ind80xxx flash");
	return res;
}

static int ind80xxx_erase(struct flash_bank *bank, int first, int last)
{
	int res;
	struct ind80xxx_info *chip;

	res = ind80xxx_get_probed_chip_if_halted(bank, &chip);
	if (res != ERROR_OK)
		return res;

#if 0
	/* For each sector to be erased */
	for (int s = first; s <= last && res == ERROR_OK; s++) {
		res = ind80xxx_erase_sector(bank, chip, &bank->sectors[s]);
		usleep(1000);
	}
#else
	if (last - first < bank->num_sectors)
		LOG_WARNING("Erasing entire chip only available. Waiting for errata fix for erase sector by sector");
#endif

	return ind80xxx_erase_all(chip);
}

static int ind80xxx_code_flash_write(struct flash_bank *bank,
								  struct ind80xxx_info *chip,
								  const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	int res;
	/* Need to perform reads to fill any gaps we need to preserve in the first page,
	   before the start of buffer, or in the last page, after the end of buffer */
	uint32_t first_page = offset/chip->code_page_size;
	uint32_t last_page = DIV_ROUND_UP(offset+count, chip->code_page_size);

	uint32_t first_page_offset = first_page * chip->code_page_size;
	uint32_t last_page_offset = last_page * chip->code_page_size;

	LOG_DEBUG("Padding write from 0x%08"PRIx32"-0x%08"PRIx32" as 0x%08"PRIx32"-0x%08"PRIx32,
			offset, offset+count, first_page_offset, last_page_offset);

	uint32_t page_cnt = last_page - first_page;
	uint8_t buffer_to_flash[page_cnt*chip->code_page_size];

	/* Fill in any space between start of first page and start of buffer */
	uint32_t pre = offset - first_page_offset;
	if (pre > 0) {
		res = target_read_memory(bank->target,
								 first_page_offset,
								 1,
								 pre,
								 buffer_to_flash);
		if (res != ERROR_OK)
			return res;
	}

	/* Fill in main contents of buffer */
	memcpy(buffer_to_flash+pre, buffer, count);

	/* Fill in any space between end of buffer and end of last page */
	uint32_t post = last_page_offset - (offset+count);
	if (post > 0) {
		/* Retrieve the full row contents from Flash */
		res = target_read_memory(bank->target,
								 offset + count,
								 1,
								 post,
								 buffer_to_flash+pre+count);
		if (res != ERROR_OK)
			return res;
	}

	return ind80xxx_write_pages(bank, first_page_offset, last_page_offset, buffer_to_flash);
}

static int ind80xxx_write(struct flash_bank *bank, const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	int res;
	struct ind80xxx_info *chip;

	res = ind80xxx_get_probed_chip_if_halted(bank, &chip);
	if (res != ERROR_OK)
		return res;

	return chip->bank[bank->bank_number].write(bank, chip, buffer, offset, count);
}

FLASH_BANK_COMMAND_HANDLER(ind80xxx_flash_bank_command)
{
	static struct ind80xxx_info *chip;

	bank->bank_number = 0;

	if (!chip) {
		/* Create a new chip */
		chip = calloc(1, sizeof(*chip));
		if (!chip)
			return ERROR_FAIL;

		chip->target = bank->target;
	}

	chip->bank[bank->bank_number].write = ind80xxx_code_flash_write;
	chip->bank[bank->bank_number].probed = false;
	bank->driver_priv = chip;

	return ERROR_OK;
}

COMMAND_HANDLER(ind80xxx_handle_mass_erase_command)
{
	int res;
	struct flash_bank *bank = NULL;
	struct target *target = get_current_target(CMD_CTX);

	res = get_flash_bank_by_addr(target, 0x00000000, true, &bank);
	if (res != ERROR_OK)
		return res;

	assert(bank != NULL);

	struct ind80xxx_info *chip;

	res = ind80xxx_get_probed_chip_if_halted(bank, &chip);
	if (res != ERROR_OK)
		return res;

	res = ind80xxx_erase_all(chip);
	if (res == ERROR_OK) {
		/* set all sectors as erased */
		for (int i = 0; i < bank->num_sectors; i++)
			bank->sectors[i].is_erased = 1;

		command_print(CMD_CTX, "ind80xxx mass erase complete");
	} else {
		command_print(CMD_CTX, "ind80xxx mass erase failed");
	}
	return ERROR_OK;
}

static int ind80xxx_info(struct flash_bank *bank, char *buf, int buf_size)
{
	int res;

	struct ind80xxx_info *chip;

	res = ind80xxx_get_probed_chip_if_halted(bank, &chip);
	if (res != ERROR_OK)
		return res;

	snprintf(buf, buf_size,
			"\n[factory information control block]\n\n"
			"code page size: %"PRIu32"B\n"
			"code memory size: %"PRIu32"kB\n"
			"code region main size: %"PRIu32"kB\n"
			"code region NVR size: %"PRIu32"kB\n"
			"code region redundancy size: %"PRIu32"kB\n"
			"number of ram blocks: %"PRIu32"\n"
			"ram block 0 size: %"PRIu32"B\n",
			 CODE_PAGE_SIZE,
			 CODE_PAGE_SIZE * (MAIN_SECTORS_AMOUNT * NVR_SECTORS_AMOUNT * REDUNDANCY_SECTORS_AMOUNT),
			 CODE_PAGE_SIZE * MAIN_SECTORS_AMOUNT,
			 CODE_PAGE_SIZE * NVR_SECTORS_AMOUNT,
			 CODE_PAGE_SIZE * REDUNDANCY_SECTORS_AMOUNT,
			 1,
			 16);

	return ERROR_OK;
}

static const struct command_registration ind80xxx_exec_command_handlers[] = {
		{
				.name		= "mass_erase",
				.handler	= ind80xxx_handle_mass_erase_command,
				.mode		= COMMAND_EXEC,
				.help		= "Erase all flash contents of the chip.",
		},
		COMMAND_REGISTRATION_DONE
};

static const struct command_registration ind80xxx_command_handlers[] = {
		{
				.name	= "ind80xxx",
				.mode	= COMMAND_ANY,
				.help	= "ind80xxx flash command group",
				.usage	= "",
				.chain	= ind80xxx_exec_command_handlers,
		},
		COMMAND_REGISTRATION_DONE
};

struct flash_driver ind80xxx_flash = {
		.name			    = "ind80xxx",
		.commands		    = ind80xxx_command_handlers,
		.flash_bank_command	= ind80xxx_flash_bank_command,
		.info			    = ind80xxx_info,
		.erase			    = ind80xxx_erase,
		.protect		    = ind80xxx_protect,
		.write			    = ind80xxx_write,
		.read			    = default_flash_read,
		.probe			    = ind80xxx_probe,
		.auto_probe		    = ind80xxx_auto_probe,
		.erase_check        = default_flash_blank_check,
		.protect_check      = ind80xxx_protect_check,
};
