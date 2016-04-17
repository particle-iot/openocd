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

#define FLASH_BASE	0x10000000
#define PAU_BASE	0x40000000
#define SCU_BASE	0x40010000
#define NVM_BASE	0x40050000

#define FLASH_CS0	(FLASH_BASE + 0xf00)

#define PAU_FLSIZE	(PAU_BASE + 0x404)

#define SCU_IDCHIP	(SCU_BASE + 0x004)

#define NVMCONF		(NVM_BASE + 0x08)

struct xmc1xxx_flash_bank {
	bool probed;
};

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

extern struct flash_driver xmc4xxx_flash;

static int xmc1xxx_erase_check(struct flash_bank *bank)
{
	return xmc4xxx_flash.erase_check(bank);
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
	.erase_check = xmc1xxx_erase_check,
};
