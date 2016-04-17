/*
 * XMC1000 flash driver
 *
 * Copyright (c) 2016 Andreas Färber
 *
 * License: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

#define FLASH_BASE	0x10000000
#define PAU_BASE	0x40000000
#define SCU_BASE	0x40010000
#define NVM_BASE	0x40050000

#define FLASH_CS0	(FLASH_BASE + 0xf00)

#define PAU_FLSIZE	(PAU_BASE + 0x404)

#define SCU_IDCHIP	(SCU_BASE + 0x004)

#define NVMSTATUS	(NVM_BASE + 0x00)
#define NVMPROG		(NVM_BASE + 0x04)
#define NVMCONF		(NVM_BASE + 0x08)

#define NVMSTATUS_BUSY		(1 << 0)
#define NVMSTATUS_VERR_MASK	(0x3 << 2)

#define NVMPROG_ACTION_OPTYPE_IDLE_VERIFY	(0 << 0)
#define NVMPROG_ACTION_OPTYPE_WRITE		(1 << 0)
#define NVMPROG_ACTION_OPTYPE_PAGE_ERASE	(2 << 0)

#define NVMPROG_ACTION_ONE_SHOT_ONCE		(1 << 4)
#define NVMPROG_ACTION_ONE_SHOT_CONTINUOUS	(2 << 4)

#define NVMPROG_ACTION_VERIFY_EACH		(1 << 6)
#define NVMPROG_ACTION_VERIFY_NO		(2 << 6)
#define NVMPROG_ACTION_VERIFY_ARRAY		(3 << 6)

#define NVMPROG_ACTION_IDLE	0x00
#define NVMPROG_ACTION_MASK	0xff

#define NVM_PAGE_SIZE (16 * 4 * 4)

struct xmc1xxx_flash_bank {
	bool probed;
};

static int xmc1xxx_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	struct working_area *workarea;
	struct reg_param reg_params[3];
	struct armv7m_algorithm armv7m_algo;
	unsigned i;
	int retval, sector;
	const uint8_t erase_code[] = {
#include "../../../contrib/loaders/flash/xmc1xxx/erase.inc"
	};

	if (bank->target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_INFO("Infineon XMC1000 erase sectors %d to %d", first, last);

	retval = target_alloc_working_area(target, sizeof(erase_code),
			&workarea);
	if (retval != ERROR_OK) {
		LOG_ERROR("No working area available.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}
	retval = target_write_buffer(target, workarea->address,
			sizeof(erase_code), erase_code);
	if (retval != ERROR_OK)
		goto err_write_code;

	armv7m_algo.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_algo.core_mode = ARM_MODE_THREAD;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);

	buf_set_u32(reg_params[0].value, 0, 32, NVM_BASE);
	buf_set_u32(reg_params[1].value, 0, 32, bank->base + bank->sectors[first].offset);
	buf_set_u32(reg_params[2].value, 0, 32, bank->base + bank->sectors[last].offset + bank->sectors[last].size);

	retval = target_run_algorithm(target,
			0, NULL,
			ARRAY_SIZE(reg_params), reg_params,
			workarea->address, 0,
			1000, &armv7m_algo);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error executing flash sector erase "
			"programming algorithm");
		retval = target_write_u16(target, NVMPROG, NVMPROG_ACTION_IDLE);
		if (retval != ERROR_OK)
			LOG_WARNING("Couldn't restore NVMPROG ACTION");
		retval = ERROR_FLASH_OPERATION_FAILED;
		goto err_run;
	}

	for (sector = first; sector <= last; sector++)
		bank->sectors[sector].is_erased = 1;

err_run:
	for (i = 0; i < ARRAY_SIZE(reg_params); i++)
		destroy_reg_param(&reg_params[i]);

err_write_code:
	target_free_working_area(target, workarea);

err_alloc_code:
	return retval;
}

static int xmc1xxx_erase_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct working_area *workarea;
	struct reg_param reg_params[3];
	struct armv7m_algorithm armv7m_algo;
	uint16_t val;
	unsigned i;
	int retval, sector;
	const uint8_t erase_check_code[] = {
#include "../../../contrib/loaders/flash/xmc1xxx/erase_check.inc"
	};

	if (bank->target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = target_alloc_working_area(target, sizeof(erase_check_code),
			&workarea);
	if (retval != ERROR_OK) {
		LOG_ERROR("No working area available.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}
	retval = target_write_buffer(target, workarea->address,
			sizeof(erase_check_code), erase_check_code);
	if (retval != ERROR_OK)
		goto err_write_code;

	armv7m_algo.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_algo.core_mode = ARM_MODE_THREAD;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);

	buf_set_u32(reg_params[0].value, 0, 32, NVM_BASE);

	for (sector = 0; sector < bank->num_sectors; sector++) {
		uint32_t start = bank->base + bank->sectors[sector].offset;
		buf_set_u32(reg_params[1].value, 0, 32, start);
		buf_set_u32(reg_params[2].value, 0, 32, start + bank->sectors[sector].size);

		LOG_DEBUG("Erase-checking 0x%08" PRIx32, start);
		retval = target_run_algorithm(target,
				0, NULL,
				ARRAY_SIZE(reg_params), reg_params,
				workarea->address, 0,
				1000, &armv7m_algo);
		if (retval != ERROR_OK) {
			LOG_ERROR("Error executing flash sector erase check "
				"programming algorithm");
			retval = target_write_u16(target, NVMPROG, NVMPROG_ACTION_IDLE);
			if (retval != ERROR_OK)
				LOG_WARNING("Couldn't restore NVMPROG ACTION");
			retval = ERROR_FLASH_OPERATION_FAILED;
			goto err_run;
		}

		retval = target_read_u16(target, NVMSTATUS, &val);
		if (retval != ERROR_OK) {
			LOG_ERROR("Couldn't read NVMSTATUS");
		}
		bank->sectors[sector].is_erased = (val & NVMSTATUS_VERR_MASK) ? 0 : 1;
	}

err_run:
	for (i = 0; i < ARRAY_SIZE(reg_params); i++)
		destroy_reg_param(&reg_params[i]);

err_write_code:
	target_free_working_area(target, workarea);

err_alloc_code:
	return retval;
}

static int xmc1xxx_protect_check(struct flash_bank *bank)
{
	uint32_t nvmconf;
	int i, num_protected, retval;

	if (bank->target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = target_read_u32(bank->target, NVMCONF, &nvmconf);
	if (retval != ERROR_OK) {
		LOG_ERROR("Cannot read NVMCONF register.");
		return retval;
	}
	LOG_DEBUG("NVMCONF = %08" PRIx32, nvmconf);

	num_protected = (nvmconf >> 4) & 0xff;

	for (i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_protected = (i < num_protected) ? 1 : 0;

	return ERROR_OK;
}

static int xmc1xxx_get_info_command(struct flash_bank *bank, char *buf, int buf_size)
{
	uint32_t chipid[8];
	int i, retval;

	if (bank->target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Obtain the 8-word Chip Identification Number */
	for (i = 0; i < 7; i++) {
		retval = target_read_u32(bank->target, FLASH_CS0 + i * 4, &chipid[i]);
		if (retval != ERROR_OK) {
			LOG_ERROR("Cannot read CS0 register %i.", i);
			return retval;
		}
		LOG_DEBUG("ID[%d] = %08" PRIX32, i, chipid[i]);
	}
	retval = target_read_u32(bank->target, SCU_BASE + 0x000, &chipid[7]);
	if (retval != ERROR_OK) {
		LOG_ERROR("Cannot read DBGROMID register.");
		return retval;
	}
	LOG_DEBUG("ID[7] = %08" PRIX32, chipid[7]);

	snprintf(buf, buf_size, "XMC%" PRIx32 "00 %X flash %uKB ROM %uKB SRAM %uKB",
			(chipid[0] >> 12) & 0xff,
			0xAA + (chipid[7] >> 28) - 1,
			(((chipid[6] >> 12) & 0x3f) - 1) * 4,
			(((chipid[4] >> 8) & 0x3f) * 256) / 1024,
			(((chipid[5] >> 8) & 0x1f) * 256 * 4) / 1024);

	return ERROR_OK;
}

static int xmc1xxx_probe(struct flash_bank *bank)
{
	struct xmc1xxx_flash_bank *xmc_bank = bank->driver_priv;
	uint32_t flash_addr = bank->base;
	uint32_t idchip, flsize;
	int i, retval;

	if (xmc_bank->probed)
		return ERROR_OK;

	if (bank->target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = target_read_u32(bank->target, SCU_IDCHIP, &idchip);
	if (retval != ERROR_OK) {
		LOG_ERROR("Cannot read IDCHIP register.");
		return retval;
	}

	if ((idchip & 0xffff0000) != 0x10000) {
		LOG_ERROR("IDCHIP register does not match XMC1xxx.");
		return ERROR_FAIL;
	}

	LOG_DEBUG("IDCHIP = %08" PRIx32, idchip);

	retval = target_read_u32(bank->target, PAU_FLSIZE, &flsize);
	if (retval != ERROR_OK) {
		LOG_ERROR("Cannot read FLSIZE register.");
		return retval;
	}

	bank->num_sectors = 1 + ((flsize >> 12) & 0x3f) - 1;
	bank->size = bank->num_sectors * 4 * 1024;
	bank->sectors = calloc(bank->num_sectors,
			       sizeof(struct flash_sector));
	for (i = 0; i < bank->num_sectors; i++) {
		if (i == 0) {
			bank->sectors[i].size = 0x200;
			bank->sectors[i].offset = 0xE00;
			flash_addr += 0x1000;
		} else {
			bank->sectors[i].size = 4 * 1024;
			bank->sectors[i].offset = flash_addr - bank->base;
			flash_addr += bank->sectors[i].size;
		}
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = -1;
	}

	bank->default_padded_value = 0x00;

	xmc_bank->probed = true;

	return ERROR_OK;
}

static int xmc1xxx_auto_probe(struct flash_bank *bank)
{
	struct xmc1xxx_flash_bank *xmc_bank = bank->driver_priv;

	if (xmc_bank->probed)
		return ERROR_OK;

	return xmc1xxx_probe(bank);
}

FLASH_BANK_COMMAND_HANDLER(xmc1xxx_flash_bank_command)
{
	struct xmc1xxx_flash_bank *xmc_bank;

	xmc_bank = malloc(sizeof(struct xmc1xxx_flash_bank));
	if (!xmc_bank)
		return ERROR_FLASH_OPERATION_FAILED;

	xmc_bank->probed = false;

	bank->driver_priv = xmc_bank;

	return ERROR_OK;
}

static const struct command_registration xmc1xxx_exec_command_handlers[] = {
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration xmc1xxx_command_handlers[] = {
	{
		.name = "xmc1xxx",
		.mode = COMMAND_ANY,
		.help = "xmc1xxx flash command group",
		.usage = "",
		.chain = xmc1xxx_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver xmc1xxx_flash = {
	.name = "xmc1xxx",
	.commands = xmc1xxx_command_handlers,
	.flash_bank_command = xmc1xxx_flash_bank_command,
	.info = xmc1xxx_get_info_command,
	.probe = xmc1xxx_probe,
	.auto_probe = xmc1xxx_auto_probe,
	.protect_check = xmc1xxx_protect_check,
	.read = default_flash_read,
	.erase = xmc1xxx_erase,
	.erase_check = xmc1xxx_erase_check,
};
