/***************************************************************************
 *   Copyright (C) 2016 by Peter Andersson <pelleplutt1976@gmail.com>      *
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
#include <helper/binarybuffer.h>
#include <helper/time_support.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

/* Presume 20KB of RAM. Would there be more, we're fine anyway. */
#define DEFAULT_SP						(0x20000000 + 1024*20)

#define ROM_VERSION_TESTED				0xa232
#define ROM_API_TABLE					0x10000180

#define ROM_API_OFFSET_VERSION			(0)
#define ROM_API_OFFSET_FLASH_TABLE		(10*4)
#define ROM_API_FLASH_OFFSET_ERASE		(5*4)
#define ROM_API_FLASH_OFFSET_WRITE		(6*4)

#define FLASH_BASE													0x40030000
#define FLASH_O_STAT                                                0x0000001C
#define FLASH_O_FLASH_SIZE                                          0x0000002C
#define FLASH_O_FSM_WR_ENA                                          0x00002288
#define FLASH_O_FSM_BSLE0                                           0x000022E0
#define FLASH_O_FSM_BSLE1                                           0x000022E4
#define FLASH_O_FSM_BSLP0                                           0x000022F0
#define FLASH_O_FSM_BSLP1                                           0x000022F4
#define FLASH_O_FCFG_B0_SSIZE0                                      0x00002430

#define FLASH_FCFG_B0_SSIZE0_B0_SECT_SIZE_W                                  4
#define FLASH_FCFG_B0_SSIZE0_B0_SECT_SIZE_M                         0x0000000F
#define FLASH_FCFG_B0_SSIZE0_B0_SECT_SIZE_S                                  0

#define FLASH_FLASH_SIZE_SECTORS_W                                           8
#define FLASH_FLASH_SIZE_SECTORS_M                                  0x000000FF
#define FLASH_FLASH_SIZE_SECTORS_S                                           0

#define FLASH_STAT_BUSY                                             0x00000002

#define FSM_REG_WRT_ENABLE				5
#define FSM_REG_WRT_DISABLE				2

#define VIMS_BASE						0x40034000

#define VIMS_O_STAT						0x00000000
#define VIMS_STAT_MODE_CHANGING			0x00000008
#define VIMS_STAT_MODE_M				0x00000003

#define VIMS_O_CTL						0x00000004
#define VIMS_CTL_MODE_M					0x00000003

#define VIMS_MODE_CHANGING				0x4
#define VIMS_MODE_DISABLED				0x00000000 /*(VIMS_CTL_MODE_GPRAM)*/
#define VIMS_MODE_ENABLED				0x00000001 /*(VIMS_CTL_MODE_CACHE)*/
#define VIMS_MODE_OFF					0x00000003 /*(VIMS_CTL_MODE_OFF)*/

#define VIMS_MODE_SET_TIMEOUT_MS		3000
#define FLASH_TIMEOUT_MS				3000

struct cc_info {
	int probed;
	uint32_t flash_table_addr;
	uint8_t last_sector_access;
};

/* Call a function in the rom library.
 * r3 - the address of the rom library function
 * r0-r2 optional parameters to the rom function
 */
static const uint8_t trampoline_code[] = {
	0x98, 0x47,		/* blx     r3 */
	0x00, 0xbe,		/* bkpt    #0 */
};


/* flash bank cc13xx26xx <base> <size> 0 0 <target#> */
FLASH_BANK_COMMAND_HANDLER(cc13xx_26xx_flash_bank_command)
{
	struct cc_info *info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	bank->base = 0x00000000;
	info = malloc(sizeof(struct cc_info));
	memset(info, 0, sizeof(struct cc_info));

	bank->driver_priv = info;

	return ERROR_OK;
}

static int cc13xx_26xx_check_protection(struct flash_bank *bank, uint32_t sector_nbr)
{
	int prot_res = -1; /* protection status unknown by default */
	do {
		int res;
		uint32_t prot_status_le;
		uint32_t prot_status_lp;
		if (sector_nbr <= 31) {
			res = target_read_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLE0,
					&prot_status_le);
			if (res == ERROR_FAIL)
				break;

			res = target_read_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLP0,
					&prot_status_lp);
			if (res == ERROR_FAIL)
				break;

			prot_res = (prot_status_le & (1 << sector_nbr)) &&
					(prot_status_lp & (1 << sector_nbr));
		} else if (sector_nbr <= 63) {
			res = target_read_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLE1,
					&prot_status_le);
			if (res == ERROR_FAIL)
				break;

			res = target_read_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLP1,
					&prot_status_lp);
			if (res == ERROR_FAIL)
				break;

			prot_res = (prot_status_le & (1 << (sector_nbr & 0x1f))) &&
					(prot_status_lp & (1 << (sector_nbr & 0x1f)));
		}
	} while (0);

	return prot_res;
}

static int cc13xx_26xx_protect_check(struct flash_bank *bank)
{
	/* probe sector protection status */
	int sect;

	for (sect = 0; sect < bank->num_sectors; sect++)
		bank->sectors[sect].is_protected = cc13xx_26xx_check_protection(bank, sect);

	return ERROR_OK;
}

static int cc13xx_26xx_get_vims_mode(struct flash_bank *bank, uint32_t *vims_mode)
{
	uint32_t cur_vims_mode;
	int res;

	res = target_read_u32(bank->target, VIMS_BASE + VIMS_O_STAT, &cur_vims_mode);
	if (ERROR_OK != res)
		return res;
	if (cur_vims_mode & VIMS_STAT_MODE_CHANGING)
		*vims_mode = VIMS_MODE_CHANGING;
	else
		*vims_mode = cur_vims_mode & VIMS_STAT_MODE_M;

	return ERROR_OK;
}

static int cc13xx_26xx_set_vims_mode(struct flash_bank *bank, uint32_t vims_mode)
{
	int64_t timeout;
	uint32_t cur_vims_mode;
	uint32_t new_vims_mode;
	int res;

	res = target_read_u32(bank->target, VIMS_BASE + VIMS_O_CTL, &cur_vims_mode);
	if (ERROR_OK != res)
		return res;
	cur_vims_mode &= ~VIMS_CTL_MODE_M;
	cur_vims_mode |= (vims_mode & VIMS_CTL_MODE_M);
	res = target_write_u32(bank->target, VIMS_BASE + VIMS_O_CTL, cur_vims_mode);
	if (ERROR_OK != res)
		return res;

	timeout = timeval_ms() + VIMS_MODE_SET_TIMEOUT_MS;
	do {
		if (timeval_ms() >= timeout) {
			LOG_ERROR("VIMS setting timeout");
			res = ERROR_TARGET_TIMEOUT;
			break;
		}
		res = cc13xx_26xx_get_vims_mode(bank, &new_vims_mode);
	} while (ERROR_OK == res && new_vims_mode != vims_mode);
	LOG_DEBUG("vims set %i", vims_mode);

	return res;
}

static int cc13xx_26xx_await_flash_ready(struct flash_bank *bank)
{
	int64_t timeout;
	uint32_t stat;
	int res;

	timeout = timeval_ms() + FLASH_TIMEOUT_MS;
	do {
		res = target_read_u32(bank->target, FLASH_BASE + FLASH_O_STAT, &stat);
		if (ERROR_OK != res)
			return res;
		if ((stat & FLASH_STAT_BUSY) == 0) {
			LOG_DEBUG("flash ready");
			break;
		}
		if (timeval_ms() >= timeout) {
			LOG_ERROR("flash busy timeout");
			res = ERROR_TARGET_TIMEOUT;
			break;
		}
	} while (res == ERROR_OK);

	return res;
}

static int cc13xx_26xx_erase(struct flash_bank *bank, int first, int last)
{
	struct cc_info *info = (struct cc_info *)bank->driver_priv;

	int sect;
	struct reg_param reg_params[3];
	uint32_t flash_erase_rom_func;
	struct working_area *erase_algorithm;
	struct armv7m_algorithm arch_info;

	int res = ERROR_OK, res_vims;

	assert(info);

	/* check last sector access */
	if (last == bank->num_sectors-1 && !info->last_sector_access) {
		LOG_ERROR("No access to last sector. Get access by calling "
				"'cc13xx26xx access_last_sector <bank_id> on'");
		return ERROR_FAIL;
	}

	/* get rom flash erase function address */
	res = target_read_u32(bank->target,
			info->flash_table_addr + ROM_API_FLASH_OFFSET_ERASE,
			&flash_erase_rom_func);
	if (ERROR_OK != res)
		return res;
	LOG_DEBUG("flash erase func addr %08x", flash_erase_rom_func);

	/* disable flash cache */
	res = cc13xx_26xx_set_vims_mode(bank, VIMS_MODE_DISABLED);
	if (ERROR_OK != res)
		return res;

	/* place our trampoline in ram */
	res = target_alloc_working_area(bank->target, sizeof(trampoline_code),
			&erase_algorithm);
	if (ERROR_OK != res)
		return res;

	res = target_write_buffer(bank->target, erase_algorithm->address,
			sizeof(trampoline_code), trampoline_code);
	if (ERROR_OK != res)
		goto cleanup;

	/* setup call procedure */
	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);	/* sector erase address*/
	init_reg_param(&reg_params[1], "r3", 32, PARAM_OUT);	/* func */
	init_reg_param(&reg_params[2], "sp", 32, PARAM_OUT);	/* stack pointer */

	/* erase requested sectors */
	for (sect = first; sect <= last; sect++) {
		uint32_t sector_address = bank->base + bank->sectors[sect].offset;

		res = cc13xx_26xx_await_flash_ready(bank);
		if (ERROR_OK != res)
			break;

		LOG_DEBUG("erase sector %d", sect);

		/* Explicitly set SP to something valid, or else the board may
		 * be semi-bricked. Seems it can be unlocked again by using
		 * TI's SmartRF programmer 2 and running a CC2650 forced mass erase
		 */
		buf_set_u32(reg_params[0].value, 0, 32, sector_address);
		buf_set_u32(reg_params[1].value, 0, 32, flash_erase_rom_func);
		buf_set_u32(reg_params[2].value, 0, 32, DEFAULT_SP);
		LOG_DEBUG("call trampoline");

		arch_info.common_magic = ARMV7M_COMMON_MAGIC;
		arch_info.core_mode = ARM_MODE_THREAD;

		res = target_run_algorithm(bank->target,
				0, NULL,
				3, reg_params,
				erase_algorithm->address,
				0,
				10000,
				&arch_info);
		if (ERROR_OK != res)
			break;
	}

	LOG_DEBUG("cleanup reg_param");

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);

cleanup:

	LOG_DEBUG("cleanup algo area");
	target_free_working_area(bank->target, erase_algorithm);

	res_vims = cc13xx_26xx_set_vims_mode(bank, VIMS_MODE_ENABLED);

	return res == ERROR_OK ? res_vims : res;
}

static int cc13xx_26xx_protect(struct flash_bank *bank, int set, int first, int last)
{
	int res, res_clean, sect;

	if (!set) {
		/* not supported, must do a device reset */
		LOG_USER("CC13xx/CC26xx can only be unprotected by a device reset");
		return ERROR_JTAG_NOT_IMPLEMENTED;
	}

	res = target_write_u32(bank->target, FLASH_BASE + FLASH_O_FSM_WR_ENA,
			FSM_REG_WRT_ENABLE);
	if (ERROR_OK != res)
		return res;

	for (sect = first; sect <= last; sect++) {
		uint32_t x;
		if (sect <= 31) {
			res = target_read_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLE0,
							&x);
			if (ERROR_OK != res)
				break;
			res = target_write_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLE0,
							x | (1 << sect));
			if (ERROR_OK != res)
				break;
			res = target_read_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLP0,
							&x);
			if (ERROR_OK != res)
				break;
			res = target_write_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLP0,
							x | (1 << sect));
			if (ERROR_OK != res)
				break;
		} else if (sect <= 63) {
			res = target_read_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLE1,
							&x);
			if (ERROR_OK != res)
				break;
			res = target_write_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLE1,
							x | (1 << (sect & 0x1f)));
			if (ERROR_OK != res)
				break;
			res = target_read_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLP1,
							&x);
			if (ERROR_OK != res)
				break;
			res = target_write_u32(bank->target, FLASH_BASE + FLASH_O_FSM_BSLP1,
							x | (1 << (sect & 0x1f)));
			if (ERROR_OK != res)
				break;
		}
	}

	res_clean = target_write_u32(bank->target, FLASH_BASE + FLASH_O_FSM_WR_ENA,
			FSM_REG_WRT_DISABLE);

	return res == ERROR_OK ? res_clean : res;
}

static int cc13xx_26xx_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct cc_info *info = (struct cc_info *)bank->driver_priv;

	uint32_t buffer_size = 16384;
	struct reg_param reg_params[5];
	uint32_t flash_write_rom_func;
	struct working_area *write_algorithm;
	struct working_area *source;
	struct armv7m_algorithm arch_info;

	int res = ERROR_OK, res_vims;

	assert(info);

	/* check last sector access */
	if (offset + count - 1 >= bank->sectors[bank->num_sectors-1].offset
			&& !info->last_sector_access) {
		LOG_ERROR("No access to last sector. Get access by calling "
				"'cc13xx26xx access_last_sector <bank_id> on'");
		return ERROR_FAIL;
	}

	/* get rom flash erase function address  */
	res = target_read_u32(bank->target,
			info->flash_table_addr + ROM_API_FLASH_OFFSET_WRITE,
			&flash_write_rom_func);
	if (ERROR_OK != res)
		return res;
	LOG_DEBUG("flash write func addr %08x", flash_write_rom_func);

	/* disable flash cache */
	res = cc13xx_26xx_set_vims_mode(bank, VIMS_MODE_DISABLED);
	if (ERROR_OK != res)
		return res;

	/* place our trampoline in ram */
	res = target_alloc_working_area(bank->target, sizeof(trampoline_code),
			&write_algorithm);
	if (ERROR_OK != res)
		return res;

	res = target_write_buffer(bank->target, write_algorithm->address,
			sizeof(trampoline_code), trampoline_code);
	if (ERROR_OK != res)
		goto cleanup;

	/* memory buffer */
	while (target_alloc_working_area_try(bank->target, buffer_size, &source)
			!= ERROR_OK) {
		buffer_size /= 2;
		if (buffer_size <= 256) {
			LOG_WARNING("no large enough working area available, "
					"can't do block memory writes");
			res = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
			goto cleanup;
		}
	}

	/* setup call procedure */
	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);	/* data address */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* write flash address */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);	/* length */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);	/* func */
	init_reg_param(&reg_params[4], "sp", 32, PARAM_OUT);	/* stack pointer */

	/* erase requested sectors */
	while (res == ERROR_OK && count > 0) {
		uint32_t length = source->size > count ? count : source->size;

		res = cc13xx_26xx_await_flash_ready(bank);
		if (ERROR_OK != res)
			break;

		LOG_DEBUG("copy data into source buffer");
		res = target_write_buffer(bank->target, source->address,
				length, buffer);
		if (ERROR_OK != res)
			break;

		LOG_DEBUG("program offset %08x, %i bytes", offset, length);

		/* Explicitly set SP to something valid, or else the board may
		 * be semi-bricked. Seems it can be unlocked again by using
		 * TI's SmartRF programmer 2 and running a CC2650 forced mass erase
		 */
		buf_set_u32(reg_params[0].value, 0, 32, source->address);
		buf_set_u32(reg_params[1].value, 0, 32, offset);
		buf_set_u32(reg_params[2].value, 0, 32, length);
		buf_set_u32(reg_params[3].value, 0, 32, flash_write_rom_func);
		buf_set_u32(reg_params[4].value, 0, 32, DEFAULT_SP);

		LOG_DEBUG("call trampoline");
		arch_info.common_magic = ARMV7M_COMMON_MAGIC;
		arch_info.core_mode = ARM_MODE_THREAD;

		res = target_run_algorithm(bank->target,
				0, NULL,
				5, reg_params,
				write_algorithm->address,
				0,
				10000,
				&arch_info);

		buffer += length;
		offset += length;
		count -= length;
	}

	LOG_DEBUG("cleanup reg_param");

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);

	LOG_DEBUG("cleanup working area");
	target_free_working_area(bank->target, source);

cleanup:

	LOG_DEBUG("cleanup algo area");
	target_free_working_area(bank->target, write_algorithm);
	res_vims = cc13xx_26xx_set_vims_mode(bank, VIMS_MODE_ENABLED);

	return res == ERROR_OK ? res_vims : res;
}

static int cc13xx_26xx_probe(struct flash_bank *bank)
{
	struct cc_info *info = (struct cc_info *)bank->driver_priv;
	uint32_t rom_version;
	uint32_t rom_flash_table_address;
	uint32_t sector_size;
	uint32_t num_sectors;
	uint32_t i;
	int res;

	assert(info);

	/* get rom library version and flash library func table address */
	res = target_read_u32(bank->target,
			ROM_API_TABLE + ROM_API_OFFSET_VERSION,
			&rom_version);
	if (ERROR_OK != res)
		return res;

	if (rom_version != ROM_VERSION_TESTED) {
		LOG_WARNING("cc13xx_26xx flash driver only tested on "
				"rom version %08x, but found %08x",
				ROM_VERSION_TESTED, rom_version);
	}

	res = target_read_u32(bank->target,
			ROM_API_TABLE + ROM_API_OFFSET_FLASH_TABLE,
			&rom_flash_table_address);
	if (ERROR_OK != res)
		return res;
	info->flash_table_addr = rom_flash_table_address;

	/* get sector size */
	res = target_read_u32(bank->target,
			FLASH_BASE + FLASH_O_FCFG_B0_SSIZE0,
			&sector_size);
	if (ERROR_OK != res)
		return res;

	sector_size &= FLASH_FCFG_B0_SSIZE0_B0_SECT_SIZE_M;
	sector_size >>= FLASH_FCFG_B0_SSIZE0_B0_SECT_SIZE_S;
	sector_size *= 1024;

	/* get number of sectors */
	res = target_read_u32(bank->target, FLASH_BASE + FLASH_O_FLASH_SIZE,
			&num_sectors);
	if (ERROR_OK != res)
		return res;
	num_sectors &= FLASH_FLASH_SIZE_SECTORS_M;
	num_sectors >>= FLASH_FLASH_SIZE_SECTORS_S;
	bank->num_sectors = num_sectors;

	bank->size = num_sectors * sector_size;

	/* create flash sector model */
	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}
	bank->sectors = malloc(sizeof(struct flash_sector) * num_sectors);

	for (i = 0; i < num_sectors; i++) {
		bank->sectors[i].offset = i * sector_size;
		bank->sectors[i].size = sector_size;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = -1;
	}

	info->probed = true;

	return ERROR_OK;
}

static int cc13xx_26xx_auto_probe(struct flash_bank *bank)
{
	struct cc_info *info = (struct cc_info *)bank->driver_priv;
	assert(info);
	return cc13xx_26xx_probe(bank);
}

COMMAND_HANDLER(cc13xx_26xx_handle_access_last_sector)
{
	if (CMD_ARGC < 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	struct cc_info *info = (struct cc_info *)bank->driver_priv;
	assert(info);

	/* skip flash bank arg */
	CMD_ARGC--;
	CMD_ARGV++;

	while (CMD_ARGC) {
		if (0 == strcmp("on", CMD_ARGV[0]))
			info->last_sector_access = true;
		else if (0 == strcmp("off", CMD_ARGV[0]))
			info->last_sector_access = false;
		 else
			return ERROR_COMMAND_SYNTAX_ERROR;
		CMD_ARGC--;
		CMD_ARGV++;
	}

	command_print(CMD_CTX, "last sector access: %s",
			info->last_sector_access ? "enabled" : "disabled");

	return ERROR_OK;
}

static const struct command_registration cc13xx_26xx_exec_command_handlers[] = {
	{
		.name = "access_last_sector",
		.handler = cc13xx_26xx_handle_access_last_sector,
		.mode = COMMAND_EXEC,
		.usage = "bank_id 'off'|'on'",
		.help = "Grants or denies modification access to last sector.",
	},
	COMMAND_REGISTRATION_DONE
};


static const struct command_registration cc13xx26xx_command_handlers[] = {
	{
		.name = "cc13xx26xx",
		.mode = COMMAND_ANY,
		.help = "cc13xx/cc26xx flash command group",
		.usage = "",
		.chain = cc13xx_26xx_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver cc13xx_26xx_flash = {
	.name = "cc13xx26xx",
	.commands = cc13xx26xx_command_handlers,
	.flash_bank_command = cc13xx_26xx_flash_bank_command,
	.erase = cc13xx_26xx_erase,
	.protect = cc13xx_26xx_protect,
	.write = cc13xx_26xx_write,
	.read = default_flash_read,
	.probe = cc13xx_26xx_probe,
	.auto_probe = cc13xx_26xx_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = cc13xx_26xx_protect_check,
};
