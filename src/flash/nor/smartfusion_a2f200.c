/***************************************************************************
 *   Copyright (C) 2012 by Thalita Drumond                                 *
 *   thalitafdrumond@gmail.com                                             *
 *                                                                         *
 *   Copyright (C) 2012 by Helen Fornazier                                 *
 *   helen.fornazier@gmail.com                                             *
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

#include "imp.h"
#include "helper/binarybuffer.h"
#include <target/algorithm.h>
#include <target/armv7m.h>

static /* unsigned const char smartfusion_a2f200_flash_write_code[] = ... */
#include "smartfusion_a2f200_write_code.h"

struct smartfusion_a2f200_flash_bank {
	uint32_t nvm_start;
	struct working_area *write_algorithm;
};

FLASH_BANK_COMMAND_HANDLER(smartfusion_a2f200_flash_bank_command)
{
	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	LOG_INFO("add flash_bank smartfusion_a2f200 %s", bank->name);

	bank->driver_priv =
	  calloc(sizeof(struct smartfusion_a2f200_flash_bank), 1);

	return ERROR_OK;
}

static int smartfusion_a2f200_protect(struct flash_bank *bank, int set,
	int first, int last)
{
	LOG_WARNING("smartfusion_a2f200_protect not supported yet");

	return ERROR_OK;
}

static int smartfusion_a2f200_protect_check(struct flash_bank *bank)
{
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_WARNING("smartfusion_a2f200_protect_check not supported yet");

	return ERROR_OK;
}

static int smartfusion_a2f200_erase(struct flash_bank *bank, int first, int last)
{
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_WARNING("smartfusion_a2f200_erase not supported yet");

	return ERROR_OK;
}

static int smartfusion_a2f200_write(struct flash_bank *bank, uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	struct smartfusion_a2f200_flash_bank *smartfusion_a2f200_info =
							bank->driver_priv;
	struct target *target = bank->target;
	uint32_t buffer_size = 16384; //FIXME Why this value? change?
	struct working_area *source;
	uint32_t address = bank->base + offset;
	struct reg_param reg_params[5];
	struct armv7m_algorithm armv7m_info;
	int retval = ERROR_OK;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (target_alloc_working_area(target,
			sizeof(smartfusion_a2f200_flash_write_code),
			&smartfusion_a2f200_info->write_algorithm) != ERROR_OK) {
		LOG_WARNING("no working area available, can't do block memory writes");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	};

	retval = target_write_buffer(target,
			smartfusion_a2f200_info->write_algorithm->address,
			sizeof(smartfusion_a2f200_flash_write_code),
			(uint8_t *)smartfusion_a2f200_flash_write_code);
	if (retval != ERROR_OK)
		return retval;

	/* memory buffer */
	while (target_alloc_working_area_try(target, buffer_size, &source)
								!= ERROR_OK) {
		buffer_size /= 2;

		if (buffer_size <= 256) {

			/* if we already allocated the writing code,
			 * but failed to get a buffer, free the algorithm */
			if (smartfusion_a2f200_info->write_algorithm)
				target_free_working_area(target,
				smartfusion_a2f200_info->write_algorithm);

			LOG_WARNING("no large enough working area available, can't do memory writes");

			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	};

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);	/* flash adress (in), status (out) */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* buffer start */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);	/* buffer size */

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARMV7M_MODE_ANY;

	while (count > 0) {

		/* write a part of the image in RAM
		 * (count has total size of the image) */
		const uint32_t thisrun_count = (count > buffer_size) ? buffer_size : count;

		/* writes thisrun_count bytes from buffer
		 * at source->address(in RAM) */
		retval = target_write_buffer(target, source->address,
							thisrun_count, buffer);
		if (retval != ERROR_OK)
			goto cleanup;

		/* set reg params */
		buf_set_u32(reg_params[0].value, 0, 32, address);
		buf_set_u32(reg_params[1].value, 0, 32, source->address);
		buf_set_u32(reg_params[2].value, 0, 32, source->size);

		LOG_DEBUG("Write 0x%04" PRIx32 " bytes to flash at 0x%08" PRIx32,
			thisrun_count, address);

		/* Execute algorithm, assume breakpoint for last instruction */
		retval = target_run_algorithm(target, 0, NULL,
				3, reg_params,
				smartfusion_a2f200_info->write_algorithm->address,
				0,
				10000,	/* FIXME 10s should be enough ? */
				&armv7m_info);

		/* Failed to run algorithm */
		if (retval != ERROR_OK) {
			LOG_ERROR("Execution of flash algorythm failed. Can't fall back. Please report.");
			retval = ERROR_FLASH_OPERATION_FAILED;
			goto cleanup;
		}

		/* Check return value from algo code */
		const uint32_t retval_target = buf_get_u32(reg_params[0].value, 0, 32);
		switch (retval_target) {
		case 0:
			retval = ERROR_OK;
			break;
		case 1:
			LOG_ERROR("eNVM Protection error.");
			retval = ERROR_FLASH_OPERATION_FAILED;
			goto cleanup;
		case 2:
			LOG_ERROR("eNVM write error.");
			retval = ERROR_FLASH_OPERATION_FAILED;
			goto cleanup;
		case 3:
			LOG_ERROR("eNVM invalid address error.");
			retval = ERROR_FLASH_OPERATION_FAILED;
			goto cleanup;
		default:
			LOG_ERROR("eNVM unknown error.");
			retval = ERROR_FLASH_OPERATION_FAILED;
			goto cleanup;
		}

		buffer += thisrun_count;
		address += thisrun_count;
		count -= thisrun_count;

		keep_alive();
	}

	/* free up resources */
cleanup:
	if (source)
		target_free_working_area(target, source);

	if (smartfusion_a2f200_info->write_algorithm) {
		target_free_working_area(target,
				smartfusion_a2f200_info->write_algorithm);
		smartfusion_a2f200_info->write_algorithm = NULL;
	}

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);

	return retval;
}

static int smartfusion_a2f200_probe(struct flash_bank *bank)
{
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* FIXME Complete this function */
	LOG_WARNING("smartfusion_a2f200_probe not supported yet");

	bank->base = 0x60000000;
	bank->size = 256000;
	bank->num_sectors = 1;
	bank->sectors = malloc(sizeof(struct flash_sector) * 1);

	bank->sectors[0].offset = 0;
	bank->sectors[0].size = bank->size;
	bank->sectors[0].is_erased = -1;
	bank->sectors[0].is_protected = 1;

	return ERROR_OK;
}

static int smartfusion_a2f200_auto_probe(struct flash_bank *bank)
{
	return smartfusion_a2f200_probe(bank);
}

static int smartfusion_a2f200_info(struct flash_bank *bank, char *buf, int buf_size)
{
	LOG_WARNING("smartfusion_a2f200_info not supported yet");

	return ERROR_OK;
}

const struct flash_driver smartfusion_a2f200_flash = {
	.name = "smartfusion_a2f200",
	.flash_bank_command = smartfusion_a2f200_flash_bank_command,
	.erase = smartfusion_a2f200_erase,
	.protect = smartfusion_a2f200_protect,
	.write = smartfusion_a2f200_write,
	.read = default_flash_read,
	.probe = smartfusion_a2f200_probe,
	.auto_probe = smartfusion_a2f200_auto_probe,
	.erase_check = default_flash_mem_blank_check,
	.protect_check = smartfusion_a2f200_protect_check,
	.info = smartfusion_a2f200_info,
};
