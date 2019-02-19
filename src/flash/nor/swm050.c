/***************************************************************************
 *   Copyright (C) 2019 Icenowy Zheng <icenowy@aosc.io>                    *
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
#include <target/image.h>

#define SWM050_FLASH_PAGE_SIZE	0x200
#define SWM050_FLASH_PAGES	16

#define SWM050_CPU_ID		0xE000ED00
#define SWM050_CPU_ID_VAL	0x410CC200

#define SWM050_FLASH_REG1	0x1F000000
#define SWM050_FLASH_REG2	0x1F000038
#define SWM050_FLASH_KEY	0xAAAAAAAA

#define SWM050_SYSCTL_CFG_0	0x400F0000
#define SWM050_SYSCTL_DBLF	0x400F0008

static int swm050_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	uint32_t cfg_0_val, dblf_val;
	int retval, curr_page;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Save original clock configuration registers value */
	retval = target_read_u32(target, SWM050_SYSCTL_CFG_0, &cfg_0_val);
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u32(target, SWM050_SYSCTL_DBLF, &dblf_val);
	if (retval != ERROR_OK)
		return retval;

	/* Configure clock to 18MHz */
	retval = target_write_u32(target, SWM050_SYSCTL_CFG_0, 0x1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, SWM050_SYSCTL_DBLF, 0x0);
	if (retval != ERROR_OK)
		return retval;

	alive_sleep(1);

	/* Perform erase */
	retval = target_write_u32(target, SWM050_FLASH_REG1, 0x4);
	if (retval != ERROR_OK)
		return retval;

	for (curr_page = first; curr_page <= last; curr_page++) {
		retval = target_write_u32(target, SWM050_FLASH_PAGE_SIZE * curr_page, SWM050_FLASH_KEY);
		if (retval != ERROR_OK)
			return retval;

		alive_sleep(1);
	}

	/* Close flash interface */
	retval = target_write_u32(target, SWM050_FLASH_REG1, 0x0);
	if (retval != ERROR_OK)
		return retval;

	/* Restore clock configuration */
	retval = target_write_u32(target, SWM050_SYSCTL_CFG_0, cfg_0_val);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, SWM050_SYSCTL_DBLF, dblf_val);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int swm050_write(struct flash_bank *bank, const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	uint32_t cfg_0_val, dblf_val, words_remaining;
	int retval;

	/* Check offset alignment */
	if (offset % 4) {
		LOG_ERROR("offset 0x%" PRIx32 " breaks required 4-byte alignment", offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	/* Pad the buffer for word align */
	if (count % 4) {
		uint32_t new_count = (count / 4 + 1) * 4;
		uint8_t *new_buffer = malloc(new_count);

		if (new_buffer == NULL) {
			LOG_ERROR("data to write not word aligned and no memory for padding buffer");
			return ERROR_FAIL;
		}
		LOG_INFO("data to write not word aligned, padding with 0xff");

		buffer = memcpy(new_buffer, buffer, count);

		if (count % 4 == 1)
			new_buffer[count++] = 0xff;
		if (count % 4 == 2)
			new_buffer[count++] = 0xff;
		if (count % 4 == 3)
			new_buffer[count++] = 0xff;
	}

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Save original clock configuration registers value */
	retval = target_read_u32(target, SWM050_SYSCTL_CFG_0, &cfg_0_val);
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u32(target, SWM050_SYSCTL_DBLF, &dblf_val);
	if (retval != ERROR_OK)
		return retval;

	/* Configure clock to 18MHz */
	retval = target_write_u32(target, SWM050_SYSCTL_CFG_0, 0x1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, SWM050_SYSCTL_DBLF, 0x0);
	if (retval != ERROR_OK)
		return retval;

	alive_sleep(1);

	/* Perform write */
	retval = target_write_u32(target, SWM050_FLASH_REG1, 0x1);
	if (retval != ERROR_OK)
		return retval;

	words_remaining = count / 4;

	while (words_remaining) {
		uint32_t val;
		memcpy(&val, buffer, sizeof(uint32_t));

		retval = target_write_u32(target, offset, val);
		if (retval != ERROR_OK)
			return retval;

		words_remaining--;
		offset += 4;
		buffer += 4;
	}

	/* Close flash interface */
	retval = target_write_u32(target, SWM050_FLASH_REG1, 0x0);
	if (retval != ERROR_OK)
		return retval;

	/* Restore clock configuration */
	retval = target_write_u32(target, SWM050_SYSCTL_CFG_0, cfg_0_val);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, SWM050_SYSCTL_DBLF, dblf_val);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int swm050_info(struct flash_bank *bank, char *buf, int buf_size)
{
	snprintf(buf, buf_size, "swm050 flash driver");
	return ERROR_OK;
}

static int swm050_probe(struct flash_bank *bank)
{
	return ERROR_OK;
}

static int swm050_mass_erase(struct flash_bank *bank)
{
	struct target *target = bank->target;
	uint32_t cfg_0_val, dblf_val;
	int retval;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Save original clock configuration registers value */
	retval = target_read_u32(target, SWM050_SYSCTL_CFG_0, &cfg_0_val);
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u32(target, SWM050_SYSCTL_DBLF, &dblf_val);
	if (retval != ERROR_OK)
		return retval;

	/* Configure clock to 18MHz */
	retval = target_write_u32(target, SWM050_SYSCTL_CFG_0, 0x1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, SWM050_SYSCTL_DBLF, 0x0);
	if (retval != ERROR_OK)
		return retval;

	alive_sleep(1);

	/* Perform mass erase */
	retval = target_write_u32(target, SWM050_FLASH_REG1, 0x6);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, SWM050_FLASH_REG2, 0x1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, 0x0, SWM050_FLASH_KEY);
	if (retval != ERROR_OK)
		return retval;

	alive_sleep(1);

	/* Close flash interface */
	retval = target_write_u32(target, SWM050_FLASH_REG1, 0x0);
	if (retval != ERROR_OK)
		return retval;

	/* Restore clock configuration */
	retval = target_write_u32(target, SWM050_SYSCTL_CFG_0, cfg_0_val);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, SWM050_SYSCTL_DBLF, dblf_val);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

COMMAND_HANDLER(swm050_handle_mass_erase_command)
{
	int i;

	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	retval = swm050_mass_erase(bank);
	if (retval == ERROR_OK) {
		/* set all sectors as erased */
		for (i = 0; i < bank->num_sectors; i++)
			bank->sectors[i].is_erased = 1;

		command_print(CMD_CTX, "swm050 mass erase complete");
	} else
		command_print(CMD_CTX, "swm050 mass erase failed");

	return retval;
}

FLASH_BANK_COMMAND_HANDLER(swm050_flash_bank_command)
{
	uint32_t offset;
	int i;

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}
	bank->default_padded_value = bank->erased_value = 0xff;

	bank->base = 0x0;
	bank->size = SWM050_FLASH_PAGE_SIZE * SWM050_FLASH_PAGES;

	bank->num_sectors = SWM050_FLASH_PAGES;
	bank->sectors = alloc_block_array(0, SWM050_FLASH_PAGE_SIZE, SWM050_FLASH_PAGES);
	if (!bank->sectors)
		return ERROR_FAIL;

	offset = 0;
	for (i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].offset = offset;
		bank->sectors[i].size = SWM050_FLASH_PAGE_SIZE;
		offset += bank->sectors[i].size;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 0;
	}

	return ERROR_OK;
}

static const struct command_registration swm050_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.handler = swm050_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Erase entire flash device.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration swm050_command_handlers[] = {
	{
		.name = "swm050",
		.mode = COMMAND_ANY,
		.help = "swm050 flash command group",
		.usage = "",
		.chain = swm050_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver swm050_flash = {
	.name = "swm050",
	.commands = swm050_command_handlers,
	.flash_bank_command = swm050_flash_bank_command,
	.erase = swm050_erase,
	.write = swm050_write,
	.read = default_flash_read,
	.probe = swm050_probe,
	.auto_probe = swm050_probe,
	.erase_check = default_flash_blank_check,
	.info = swm050_info,
	.free_driver_priv = default_flash_free_driver_priv,
};
