/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2011 by Andreas Fritiofson                              *
 *   andreas.fritiofson@gmail.com                                          *
 *                                                                         *
 *   Copyright (C) 2013 by Paul Fertser                                    *
 *   fercerpav@gmail.com                                                   *
 *                                                                         *
 *   Copyright (C) 2015 by Dmitry Shpak                                    *
 *   disona@yandex.ru                                                      *
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
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

#define NIIET_FLASH_REG_BASE 0xA001C000
#define NIIET_FLASH_FMA (NIIET_FLASH_REG_BASE + 0x00)
#define NIIET_FLASH_FMD (NIIET_FLASH_REG_BASE + 0x04)
#define NIIET_FLASH_FMC (NIIET_FLASH_REG_BASE + 0x08)
#define NIIET_FLASH_FCIS (NIIET_FLASH_REG_BASE + 0x0C)
#define NIIET_FLASH_FCIC (NIIET_FLASH_REG_BASE + 0x14)
#define NIIET_FLASH_FMD2 (NIIET_FLASH_REG_BASE + 0x50)
#define NIIET_FLASH_FMD3 (NIIET_FLASH_REG_BASE + 0x54)
#define NIIET_FLASH_FMD4 (NIIET_FLASH_REG_BASE + 0x58)

/* Magic Key for Flashing */
#define NIIET_KEY 0xA4420000


#define NIIET_FLASH_WRITE (1 << 0)
#define NIIET_FLASH_PERASE (1 << 1)
#define NIIET_FLASH_FERASE (1 << 2)
#define NIIET_FLASH_IFBWRITE (1 << 4)
#define NIIET_FLASH_IFBPERASE (1 << 5)


struct niiet_flash_bank {
	int probed;
	unsigned int mem_type;
	unsigned int page_count;
	unsigned int sec_count;
};

/* flash bank <name> niiet <base> <size> 0 0 <target#> <type>
 * There is no sectors. Pages don't matter.
 */
FLASH_BANK_COMMAND_HANDLER(niiet_flash_bank_command)
{
	struct niiet_flash_bank *niiet_info;

	if (CMD_ARGC < 7)
		return ERROR_COMMAND_SYNTAX_ERROR;

	niiet_info = malloc(sizeof(struct niiet_flash_bank));

	bank->driver_priv = niiet_info;
	niiet_info->probed = 0;
	COMMAND_PARSE_NUMBER(uint, CMD_ARGV[6], niiet_info->mem_type);
	niiet_info->page_count = 1;
	niiet_info->sec_count = 1;
	return ERROR_OK;
}


static int niiet_protect_check(struct flash_bank *bank)
{
	return ERROR_OK;
}


static int niiet_mass_erase(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct niiet_flash_bank *niiet_info = bank->driver_priv;
	uint32_t flash_cmd, retval2;
	int retval;


	flash_cmd = 0x3;
	LOG_INFO("clearing status regiser...");
	/* Clear status register*/
	retval = target_write_u32(target, NIIET_FLASH_FCIC, flash_cmd);
	if (retval != ERROR_OK)
		return retval;

	/* Check if memory type is "info" or "main" */
	if (niiet_info->mem_type) {
		flash_cmd = (NIIET_KEY | NIIET_FLASH_IFBPERASE);
		LOG_INFO("erasing INFO memory...");
	} else {
		flash_cmd = (NIIET_KEY | NIIET_FLASH_FERASE);
		LOG_INFO("erasing MAIN memory...");
	}

	retval = target_write_u32(target, NIIET_FLASH_FMC, flash_cmd);
	if (retval != ERROR_OK)
		return retval;

	/* Now we should wait for "Operation finished" flag. If we get "Operation failed"
	* flag instead, then quit. In both cases respective flag is cleared */
	retval2 = 0;
	while (retval2 == 0) {
		retval = target_read_u32(target, NIIET_FLASH_FCIS, &retval2);
		if (retval != ERROR_OK) {
			LOG_WARNING("something went wrong...");
			return retval;
		}
	}

	retval2 &= 0x3;
	if (retval2 == 0x2) {
		retval = target_write_u32(target, NIIET_FLASH_FCIC, retval2);
		if (retval != ERROR_OK)
			return retval;

		LOG_WARNING("got EraseError flag in Status Register");
		retval = ERROR_FLASH_SECTOR_NOT_ERASED;
		return retval;
	} else {
		retval = target_write_u32(target, NIIET_FLASH_FCIC, retval2);
		if (retval != ERROR_OK)
			return retval;

		LOG_INFO("flash memory erased succesfully.");
		bank->sectors[0].is_erased = 1;
	}

	return retval;
}

static int niiet_erase(struct flash_bank *bank, int first, int last)
{
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	return niiet_mass_erase(bank);
}

static int niiet_protect(struct flash_bank *bank, int set, int first, int last)
{
	return ERROR_OK;
}


static int niiet_write_block(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct niiet_flash_bank *niiet_info = bank->driver_priv;
	uint32_t buffer_size = 16384 + 8;		/* 8 bytes for rp and wp */
	struct working_area *write_algorithm;
	struct working_area *source;
	uint32_t address = bank->base + offset;
	struct reg_param reg_params[6];
	struct armv7m_algorithm armv7m_info;
	uint32_t flash_cmd;
	int retval = ERROR_OK;


	/* Flasher code. Size = 0x64 bytes
	* Source code can be found in contrib\loaders\flash\niiet.S */

	static const uint8_t niiet32fx_flash_write_code[] = {
		0x17, 0x68, 0x00, 0x2F, 0x28, 0xD0, 0x56, 0x68, 0xBE, 0x42, 0x3F, 0xF4,
		0xF9, 0xAF, 0x37, 0x68, 0x47, 0x60, 0x77, 0x68, 0x07, 0x65, 0xB7, 0x68,
		0x47, 0x65, 0xF7, 0x68, 0x87, 0x65, 0x04, 0x60, 0x0E, 0x4F, 0x47, 0xEA,
		0x05, 0x07, 0x87, 0x60, 0x00, 0xF0, 0x0C, 0xF8, 0x10, 0x36, 0x10, 0x34,
		0x9E, 0x42, 0x01, 0xD3, 0x16, 0x46, 0x08, 0x36, 0x56, 0x60, 0x01, 0x39,
		0x00, 0x29, 0x0B, 0xDD, 0xFF, 0xF7, 0xDE, 0xBF, 0xC7, 0x68, 0x17, 0xF0,
		0x02, 0x0F, 0x05, 0xD1, 0x17, 0xF0, 0x01, 0x0F, 0xF8, 0xD0, 0x03, 0x4F,
		0x47, 0x61, 0x70, 0x47, 0x38, 0x46, 0x00, 0xBE, 0x00, 0x00, 0x42, 0xA4,
		0x01, 0x00, 0x00, 0x00};

	if (target_alloc_working_area(target, sizeof(niiet32fx_flash_write_code),
			&write_algorithm) != ERROR_OK) {
		LOG_WARNING("no working area available, can't do block memory writes");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	};


	retval = target_write_buffer(target, write_algorithm->address,
			sizeof(niiet32fx_flash_write_code), niiet32fx_flash_write_code);
	if (retval != ERROR_OK)
		return retval;

	while (target_alloc_working_area_try(target, buffer_size, &source) != ERROR_OK) {
		buffer_size /= 2;
		buffer_size &= ~15UL; /* Make sure it's 16 byte aligned */
		buffer_size += 8;	  /* And 8 bytes for WP and RP */
		if (buffer_size <= 256) {
			/* we already allocated the writing code, but failed to get a
			 * buffer, free the algorithm */
			target_free_working_area(target, write_algorithm);

			LOG_WARNING("no large enough working area available, can't do block memory writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	};

	/* Prepare flash command. If we need to write to INFO block it will be bit 4, if MAIN block - bit 0 */
	if (niiet_info->mem_type) {
		flash_cmd = NIIET_FLASH_IFBWRITE;
		LOG_INFO("block writes will be done to INFO memory block");
	} else {
		flash_cmd = NIIET_FLASH_WRITE;
		LOG_INFO("block writes will be done to MAIN memory block");
	}

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);	/* flash base (in), status (out) */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* count (128bit) */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);	/* buffer start */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);	/* buffer end */
	init_reg_param(&reg_params[4], "r4", 32, PARAM_IN_OUT);	/* target address */
	init_reg_param(&reg_params[5], "r5", 32, PARAM_IN_OUT);	/* flash command: info or main block */

	buf_set_u32(reg_params[0].value, 0, 32, NIIET_FLASH_REG_BASE);
	buf_set_u32(reg_params[1].value, 0, 32, count);
	buf_set_u32(reg_params[2].value, 0, 32, source->address);
	buf_set_u32(reg_params[3].value, 0, 32, source->address + source->size);
	buf_set_u32(reg_params[4].value, 0, 32, address);
	buf_set_u32(reg_params[5].value, 0, 32, flash_cmd);


	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;


	retval = target_run_flash_async_algorithm(target, buffer, count, 16,
			0, NULL,
			6, reg_params,
			source->address, source->size,
			write_algorithm->address, 0,
			&armv7m_info);

	if (retval == ERROR_FLASH_OPERATION_FAILED)
		LOG_ERROR("flash write failed at address 0x%"PRIx32,
				buf_get_u32(reg_params[4].value, 0, 32));


	target_free_working_area(target, source);
	target_free_working_area(target, write_algorithm);


	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);
	destroy_reg_param(&reg_params[5]);

	return retval;
}


static int niiet_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct niiet_flash_bank *niiet_info = bank->driver_priv;
	uint8_t *new_buffer = NULL;
	uint32_t flash_cmd;
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & 0xF) {
		LOG_ERROR("offset 0x%" PRIx32 " breaks required 4-word alignment", offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	/* If there's an odd number of words, the data has to be padded. Duplicate
	 * the buffer and use the normal code path with a single block write since
	 * it's probably cheaper than to special case the last odd write using
	 * discrete accesses. */

	int rem = count % 16;
	if (rem) {
		new_buffer = malloc(count + 16 - rem);
		if (new_buffer == NULL) {
			LOG_ERROR("Odd number of words to write and no memory for padding buffer");
			return ERROR_FAIL;
		}
		LOG_INFO("Odd number of words to write, padding with 0xFFFFFFFF");
		buffer = memcpy(new_buffer, buffer, count);
		while (rem < 16) {
			new_buffer[count++] = 0xff;
			rem++;
		}
	}


	int retval;

	/* try using block write */
	retval = niiet_write_block(bank, buffer, offset, count/16);


	if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE) {
		/* if block write failed (no sufficient working area),
		 * we use normal (slow) single halfword accesses */
		LOG_WARNING("Can't use block writes, falling back to single memory accesses");
		/* Clear state register */
		retval = target_write_u32(target, NIIET_FLASH_FCIC, 0x3);
		if (retval != ERROR_OK)
			goto free_buffer;

		if (niiet_info->mem_type) {
			flash_cmd = NIIET_KEY | NIIET_FLASH_IFBWRITE;
			LOG_WARNING("Writing INFO memory");
		} else {
			flash_cmd = NIIET_KEY | NIIET_FLASH_WRITE;
			LOG_WARNING("Writing MAIN memory");
		}

		unsigned int i = 0;
		while (count > 0) {
			uint32_t value[4];
			uint32_t op_state;

			/* Load target address */
			retval = target_write_u32(target, NIIET_FLASH_FMA, offset + i*16);
			if (retval != ERROR_OK)
				goto free_buffer;

			/* Prepare data (4 words) */
			memcpy(&value, buffer + i*16, 4*sizeof(uint32_t));

			retval = target_write_u32(target, NIIET_FLASH_FMD, value[0]);
			if (retval != ERROR_OK)
				goto free_buffer;
			retval = target_write_u32(target, NIIET_FLASH_FMD2, value[1]);
			if (retval != ERROR_OK)
				goto free_buffer;
			retval = target_write_u32(target, NIIET_FLASH_FMD3, value[2]);
			if (retval != ERROR_OK)
				goto free_buffer;
			retval = target_write_u32(target, NIIET_FLASH_FMD4, value[3]);
			if (retval != ERROR_OK)
				goto free_buffer;

			/* Write */
			retval = target_write_u32(target, NIIET_FLASH_FMC, flash_cmd);
			if (retval != ERROR_OK)
				goto free_buffer;

			/* Now we should wait "Operation finished" flag. If we get "Operation failed"
			 * flag instead, then quit. In both cases respective flag is cleared */
			op_state = 0;
			while (op_state == 0) {
				retval = target_read_u32(target, NIIET_FLASH_FCIS, &op_state);
				if (retval != ERROR_OK)
					goto free_buffer;

				op_state &= 0x3;
				if (op_state == 0x2) {
					retval = target_write_u32(target, NIIET_FLASH_FCIC, op_state);
					if (retval != ERROR_OK)
						goto free_buffer;

					LOG_WARNING("got WriteError flag in Status Register");
					retval = ERROR_FLASH_OPERATION_FAILED;
					goto free_buffer;
				} else if (op_state == 0x1) {
					retval = target_write_u32(target, NIIET_FLASH_FCIC, op_state);
					if (retval != ERROR_OK)
						goto free_buffer;
				}
			}
			count -= 16;
			i++;

		}
	}

free_buffer:
	if (new_buffer)
		free(new_buffer);

	return retval;
}

static int niiet_probe(struct flash_bank *bank)
{
	struct niiet_flash_bank *niiet_info = bank->driver_priv;
	unsigned int page_count, page_size, i;

	page_count = niiet_info->page_count;
	page_size = bank->size / page_count;

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}

	bank->num_sectors = page_count;
	bank->sectors = malloc(sizeof(struct flash_sector) * page_count);

	for (i = 0; i < page_count; i++) {
		bank->sectors[i].offset = i * page_size;
		bank->sectors[i].size = page_size;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 0;
	}

	niiet_info->probed = 1;

	return ERROR_OK;
}

static int niiet_auto_probe(struct flash_bank *bank)
{
	struct niiet_flash_bank *niiet_info = bank->driver_priv;
	if (niiet_info->probed)
		return ERROR_OK;
	return niiet_probe(bank);
}

static int get_niiet_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct niiet_flash_bank *niiet_info = bank->driver_priv;
	snprintf(buf, buf_size, "niiet32Fx - %s",
		 niiet_info->mem_type ? "info memory" : "main memory");

	return ERROR_OK;
}

struct flash_driver niiet_flash = {
	.name = "niiet",
	.usage = "flash bank <name> niiet <base> <size> 0 0 <target#> <type>\n"
	"<type>: 0 for main memory, 1 for info memory",
	.flash_bank_command = niiet_flash_bank_command,
	.erase = niiet_erase,
	.protect = niiet_protect,
	.write = niiet_write,
	.read = default_flash_read,
	.probe = niiet_probe,
	.auto_probe = niiet_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = niiet_protect_check,
	.info = get_niiet_info,
};
