/*
 * Spansion FM4 flash
 *
 * Copyright (c) 2015 Andreas Färber
 *
 * Based on S6E2CC_MN709-00007 for S6E2CC/C5/C4/C3/C2/C1 series
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

#define FASZR 0x40000000

#define WDG_BASE 0x40011000
#define WDG_CTL (WDG_BASE + 0x008)
#define WDG_LCK (WDG_BASE + 0xC00)

struct fm4_flash_bank {
	bool probed;
};

static int fm4_disable_hw_watchdog(struct target *target)
{
	int retval;

	retval = target_write_u32(target, WDG_LCK, 0x1ACCE551);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, WDG_LCK, 0xE5331AAE);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, WDG_CTL, 0);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int fm4_enter_flash_cpu_programming_mode(struct target *target)
{
	uint32_t u32_value;
	int retval;

	/* FASZR ASZ = CPU programming mode */
	retval = target_write_u32(target, FASZR, 0x00000001);
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u32(target, FASZR, &u32_value);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int fm4_enter_flash_cpu_rom_mode(struct target *target)
{
	uint32_t u32_value;
	int retval;

	/* FASZR ASZ = CPU ROM mode */
	retval = target_write_u32(target, FASZR, 0x00000002);
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u32(target, FASZR, &u32_value);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int s6e2cc_flash_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	struct working_area *workarea;
	struct reg_param reg_params[4];
	struct armv7m_algorithm armv7m_algo;
	int retval, sector;
	const uint8_t erase_sector_code[] = {
#include "../../../contrib/loaders/flash/fm4_erase.inc"
	};

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_INFO("Spansion S6E2CC erase sectors %d to %d", first, last);

	retval = fm4_disable_hw_watchdog(target);
	if (retval != ERROR_OK)
		return retval;

	retval = fm4_enter_flash_cpu_programming_mode(target);
	if (retval != ERROR_OK)
		return retval;

	retval = target_alloc_working_area(target, sizeof(erase_sector_code),
	                                   &workarea);
	if (retval != ERROR_OK) {
		LOG_ERROR("No working area available.");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}
	retval = target_write_buffer(target, workarea->address,
	                             sizeof(erase_sector_code), erase_sector_code);
	if (retval != ERROR_OK)
		return retval;

	armv7m_algo.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_algo.core_mode = ARM_MODE_THREAD;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);
	init_reg_param(&reg_params[3], "r3", 32, PARAM_IN);

	for (sector = first; sector <= last; sector++) {
		uint32_t offset = bank->sectors[sector].offset;
		uint32_t result;

		buf_set_u32(reg_params[0].value, 0, 32, (offset & ~0xffff) | 0xAA8);
		buf_set_u32(reg_params[1].value, 0, 32, (offset & ~0xffff) | 0x554);
		buf_set_u32(reg_params[2].value, 0, 32, offset);

		retval = target_run_algorithm(target,
				0, NULL,
				ARRAY_SIZE(reg_params), reg_params,
				workarea->address, 0,
				1000, &armv7m_algo);
		if (retval != ERROR_OK) {
			LOG_ERROR("Error executing flash sector erase "
				"programming algorithm");
			return ERROR_FLASH_OPERATION_FAILED;
		}

		result = buf_get_u32(reg_params[3].value, 0, 32);
		if (result == 2) {
			LOG_ERROR("Timeout error from flash sector erase programming algorithm");
			return ERROR_FLASH_OPERATION_FAILED;
		} else if (result != 0) {
			LOG_ERROR("Unexpected error %d from flash sector erase programming algorithm", result);
			return ERROR_FLASH_OPERATION_FAILED;
		}

		bank->sectors[sector].is_erased = 1;
	}

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);

	target_free_working_area(target, workarea);

	retval = fm4_enter_flash_cpu_rom_mode(target);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int s6e2cc_flash_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t byte_count)
{
	struct target *target = bank->target;
	struct working_area *code_workarea, *data_workarea;
	struct reg_param reg_params[6];
	struct armv7m_algorithm armv7m_algo;
	uint32_t halfword_count = DIV_ROUND_UP(byte_count, 2);
	uint32_t result;
	unsigned i;
	int retval;
	const uint8_t write_block_code[] = {
#include "../../../contrib/loaders/flash/fm4_write.inc"
	};

	LOG_INFO("Spansion S6E2CC write at 0x%08" PRIx32 " (%" PRId32 ")",
		offset, byte_count);

	if (offset & 0x1) {
		LOG_ERROR("offset 0x%" PRIx32 " breaks required 2-byte alignment",
			offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}
	if (byte_count & 0x1) {
		LOG_WARNING("length %" PRId32 " is not 2-byte aligned, rounding up",
			byte_count);
	}

	retval = fm4_disable_hw_watchdog(target);
	if (retval != ERROR_OK)
		return retval;

	retval = target_alloc_working_area(target, sizeof(write_block_code),
	                                   &code_workarea);
	if (retval != ERROR_OK) {
		LOG_ERROR("No working area available for write code.");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}
	retval = target_write_buffer(target, code_workarea->address,
	                             sizeof(write_block_code), write_block_code);
	if (retval != ERROR_OK)
		return retval;

	retval = target_alloc_working_area(target,
		MIN(halfword_count * 2, target_get_working_area_avail(target)),
		&data_workarea);
	if (retval != ERROR_OK) {
		LOG_ERROR("No working area available for write data.");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	armv7m_algo.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_algo.core_mode = ARM_MODE_THREAD;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);
	init_reg_param(&reg_params[4], "r4", 32, PARAM_OUT);
	init_reg_param(&reg_params[5], "r5", 32, PARAM_IN);

	retval = fm4_enter_flash_cpu_programming_mode(target);
	if (retval != ERROR_OK)
		return retval;

	while (byte_count > 0) {
		uint32_t halfwords = MIN(halfword_count, data_workarea->size / 2);
		if (offset < 0x00100000 && offset + halfwords * 2 > 0x00100000) {
			/* XXX avoid flash-macro-overlapping writes */
			halfwords = (0x00100000 - offset) / 2;
		}

		LOG_INFO("copying %" PRId32 " bytes to SRAM 0x%08" PRIx32,
			MIN(halfwords * 2, byte_count), data_workarea->address);

		retval = target_write_buffer(target, data_workarea->address,
			MIN(halfwords * 2, byte_count), buffer);
		if (retval != ERROR_OK) {
			LOG_ERROR("Error writing data buffer");
			return ERROR_FLASH_OPERATION_FAILED;
		}

		LOG_INFO("writing 0x%08" PRIx32 "-0x%08" PRIx32 " (%" PRId32 "x)",
			offset, offset + halfwords * 2 - 1, halfwords);

		buf_set_u32(reg_params[0].value, 0, 32, (offset & ~0xffff) | 0xAA8);
		buf_set_u32(reg_params[1].value, 0, 32, (offset & ~0xffff) | 0x554);
		buf_set_u32(reg_params[2].value, 0, 32, offset);
		buf_set_u32(reg_params[3].value, 0, 32, data_workarea->address);
		buf_set_u32(reg_params[4].value, 0, 32, halfwords);

		retval = target_run_algorithm(target,
				0, NULL,
				ARRAY_SIZE(reg_params), reg_params,
				code_workarea->address, 0,
				5 * 60 * 1000, &armv7m_algo);
		if (retval != ERROR_OK) {
			LOG_ERROR("Error executing flash sector erase "
				"programming algorithm");
			return ERROR_FLASH_OPERATION_FAILED;
		}

		result = buf_get_u32(reg_params[5].value, 0, 32);
		if (result == 2) {
			LOG_ERROR("Timeout error from flash write programming algorithm");
			return ERROR_FLASH_OPERATION_FAILED;
		} else if (result != 0) {
			LOG_ERROR("Unexpected error %d from flash write programming algorithm", result);
			return ERROR_FLASH_OPERATION_FAILED;
		}

		halfword_count -= halfwords;
		offset += halfwords * 2;
		buffer += halfwords * 2;
		byte_count -= MIN(halfwords * 2, byte_count);
	}

	retval = fm4_enter_flash_cpu_rom_mode(target);

	for (i = 0; i < ARRAY_SIZE(reg_params); i++)
		destroy_reg_param(&reg_params[i]);

	target_free_working_area(target, code_workarea);
	target_free_working_area(target, data_workarea);

	return retval;
}

static int fm4_probe(struct flash_bank *bank)
{
	struct fm4_flash_bank *fm4_bank = bank->driver_priv;

	if (fm4_bank->probed)
		return ERROR_OK;

	if (bank->target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* S6E2Cx8: 20 */
	/* S6E2Cx9: 20 + 12 */
	/* S6E2CxA: 20 + 20 */
	bank->num_sectors = 2 * 20;
	bank->num_sectors += 5;

	LOG_DEBUG("%d sectors", bank->num_sectors);
	bank->sectors = calloc(bank->num_sectors,
	                       sizeof(struct flash_sector));
	uint32_t flash_addr = 0x00000000;
	int i;
	for (i = 0; i < bank->num_sectors - 5; i++) {
		int j = (i % 20) + 4;
		if (j < 8) {
			bank->sectors[i].size = 8 * 1024;
		} else if (j == 8) {
			bank->sectors[i].size = 32 * 1024;
		} else {
			bank->sectors[i].size = 64 * 1024;
		}
		bank->sectors[i].offset = flash_addr;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = -1;

		bank->size += bank->sectors[i].size;
		flash_addr += bank->sectors[i].size;
	}

	flash_addr = 0x00406000;
	for (; i < bank->num_sectors; i++) {
		bank->sectors[i].size = 8 * 1024;
		bank->sectors[i].offset = flash_addr;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = -1;

		bank->size += bank->sectors[i].size;
		flash_addr += bank->sectors[i].size;
	}

	fm4_bank->probed = true;

	return ERROR_OK;
}

static int fm4_auto_probe(struct flash_bank *bank)
{
	struct fm4_flash_bank *fm4_bank = bank->driver_priv;

	if (fm4_bank->probed)
		return ERROR_OK;

	return fm4_probe(bank);
}

static int fm4_protect_check(struct flash_bank *bank)
{
	return ERROR_OK;
}

static int fm4_get_info_command(struct flash_bank *bank, char *buf, int buf_size)
{
	//struct fm4_flash_bank *fm4_bank = bank->driver_priv;

	if (bank->target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	snprintf(buf, buf_size, "S6E2CCA");

	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(fm4_flash_bank_command)
{
	struct fm4_flash_bank *fm4_bank;

	fm4_bank = malloc(sizeof(struct fm4_flash_bank));
	if (!fm4_bank)
		return ERROR_FLASH_OPERATION_FAILED;

	fm4_bank->probed = false;

	bank->driver_priv = fm4_bank;

	return ERROR_OK;
}

static const struct command_registration fm4_exec_command_handlers[] = {
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration fm4_command_handlers[] = {
	{
		.name = "fm4",
		.mode = COMMAND_ANY,
		.help = "fm4 flash command group",
		.usage = "",
		.chain = fm4_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver fm4_flash = {
	.name = "fm4",
	.commands = fm4_command_handlers,
	.flash_bank_command = fm4_flash_bank_command,
	.info = fm4_get_info_command,
	.probe = fm4_probe,
	.auto_probe = fm4_auto_probe,
	.protect_check = fm4_protect_check,
	.erase = s6e2cc_flash_erase,
	.erase_check = default_flash_blank_check,
	.write = s6e2cc_flash_write,
};
