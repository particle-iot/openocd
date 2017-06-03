/***************************************************************************
 *   Copyright (C) 2017 by Texas Instruments, Inc.                         *
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
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include "msp432p4.h"
#include <helper/binarybuffer.h>
#include <helper/time_support.h>
#include <target/algorithm.h>
#include <target/armv7m.h>
#include <target/image.h>

#define SECTOR_LENGTH       0x1000
#define FLASH_MAIN_SIZE_REG 0xE0043020
#define FLASH_INFO_SIZE_REG 0xE0043024
#define FLASH_2M_SIZE       0x00201008
#define DEVICE_ID_ADDR      0x0020100C
#define HARDWARE_REV_ADDR   0x00201010
#define FLASH_TIMEOUT       8000

struct msp432p4_bank {
	uint32_t device_id;
	uint32_t hardware_rev;
	bool probed[2];
	bool unlock_bsl;
	struct working_area *working_area;
	struct armv7m_algorithm armv7m_info;
};

static int msp432p4_auto_probe(struct flash_bank *bank);

static char* msp432p4_return_text(uint32_t return_code)
{
	switch (return_code) {
		case FLASH_BUSY:
			return "FLASH_BUSY";
		case FLASH_SUCCESS:
			return "FLASH_SUCCESS";
		case FLASH_ERROR:
			return "FLASH_ERROR";
		case FLASH_TIMEOUT_ERROR:
			return "FLASH_TIMEOUT_ERROR";
		case FLASH_VERIFY_ERROR:
			return "FLASH_VERIFY_WRONG";
		case FLASH_WRONG_COMMAND:
			return "FLASH_WRONG_COMMAND";
		case FLASH_POWER_ERROR:
			return "FLASH_POWER_ERROR";
		default:
			return "UNDEFINED_RETURN_CODE";
	}
}

static void msp432p4_init_params(struct msp432p4_algo_params *algo_params)
{
	algo_params->flash_command = FLASH_NO_COMMAND;
	algo_params->return_code = 0;
	algo_params->_reserved0 = 0;
	algo_params->address = 0;
	algo_params->length = 0;
	algo_params->buffer1_status = BUFFER_INACTIVE;
	algo_params->buffer2_status = BUFFER_INACTIVE;
	algo_params->erase_param = FLASH_ERASE_MAIN;
	algo_params->unlock_bsl = FLASH_LOCK_BSL;
}

static int msp432p4_exec_cmd(struct target *target, struct msp432p4_algo_params
			*algo_params, uint32_t command)
{
	int retval;

	// Make sure the given params do not include the command
	algo_params->flash_command = FLASH_NO_COMMAND;
	algo_params->return_code = 0;
	algo_params->buffer1_status = BUFFER_INACTIVE;
	algo_params->buffer2_status = BUFFER_INACTIVE;

	// Write out parameters to target memory
	retval = target_write_buffer(target, ALGO_PARAMS_BASE_ADDR,
				sizeof(struct msp432p4_algo_params), (uint8_t*)algo_params);
	if (ERROR_OK != retval) return retval;

	// Write out command to target memory
	retval = target_write_buffer(target, ALGO_FLASH_COMMAND_ADDR,
				sizeof(command), (uint8_t*)&command);

	return retval;
}

static int msp432p4_wait_return_code(struct target *target)
{
	uint32_t return_code = 0;
	long long start_ms;
	long long elapsed_ms;

	int retval = ERROR_OK;

	start_ms = timeval_ms();
	while (0 == return_code || FLASH_BUSY == return_code) {
		retval = target_read_buffer(target, ALGO_RETURN_CODE_ADDR,
					sizeof(return_code), (uint8_t*)&return_code);
		if (ERROR_OK != retval) return retval;

		elapsed_ms = timeval_ms() - start_ms;
		if (elapsed_ms > 500) keep_alive();
		if (elapsed_ms > FLASH_TIMEOUT) break;
	};

	if (FLASH_SUCCESS != return_code) {
		LOG_ERROR("Flash operation failed: %s",
			msp432p4_return_text(return_code));
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int msp432p4_wait_inactive(struct target *target, uint32_t buffer)
{
	uint32_t status_code = BUFFER_ACTIVE;
	uint32_t status_addr;
	long long start_ms;
	long long elapsed_ms;

	int retval;

	switch (buffer) {
		case 1: // Buffer 1
			status_addr = ALGO_BUFFER1_STATUS_ADDR;
			break;
		case 2: // Buffer 2
			status_addr = ALGO_BUFFER2_STATUS_ADDR;
			break;
		default:
			return ERROR_FAIL;
	}

	start_ms = timeval_ms();
	while (BUFFER_INACTIVE != status_code) {
		retval = target_read_buffer(target, status_addr, sizeof(status_code),
					(uint8_t*)&status_code);
		if (ERROR_OK != retval) return retval;

		elapsed_ms = timeval_ms() - start_ms;
		if (elapsed_ms > 500) keep_alive();
		if (elapsed_ms > FLASH_TIMEOUT) break;
	};

	if (BUFFER_INACTIVE != status_code) {
		LOG_ERROR("Flash operation failed: buffer not written to flash");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int msp432p4_init(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct msp432p4_bank *msp432p4_bank = bank->driver_priv;
	struct msp432p4_algo_params algo_params;
	struct reg_param reg_params[1];

	const uint8_t *loader_code;
	uint32_t loader_size;
	bool unlock_bsl;
	int retval;

	// Make sure we've probed the flash to get the device and size
	unlock_bsl = msp432p4_bank->unlock_bsl;
	retval = msp432p4_auto_probe(bank);
	if (ERROR_OK != retval) return retval;

	// Preserve current BSL setting across the probe call
	msp432p4_bank->unlock_bsl = unlock_bsl;

	// Choose appropriate flash helper algorithm
	loader_size = 0;
	switch (msp432p4_bank->device_id) {
		case 0xA000:
		case 0xA001:
		case 0xA002:
		case 0xA003:
		case 0xA004:
		case 0xA005:
			// Device is Falcon, check revision
			if (msp432p4_bank->hardware_rev >= 0x43 &&
				msp432p4_bank->hardware_rev <= 0x49) {
				loader_code = msp432p4_algo;
				loader_size = sizeof(msp432p4_algo);
			}
			break;
		case 0xA010:
		case 0xA012:
		case 0xA016:
		case 0xA019:
		case 0xA01F:
		case 0xA020:
		case 0xA022:
		case 0xA026:
		case 0xA029:
		case 0xA02F:
			// Device is Falcon 2M, check revision
			if (msp432p4_bank->hardware_rev >= 0x41 &&
				msp432p4_bank->hardware_rev <= 0x49) {
				loader_code = msp432p4_2m_algo;
				loader_size = sizeof(msp432p4_2m_algo);
			}
			break;
		default:
			// Fall through to check that loader_size wasn't set
			break;
	}
	if (0 == loader_size) {
		// Explicit device check failed. Report this and
		// then guess at which algo is best to use.
		LOG_INFO("msp432p4.flash: Unrecognized Device ID and Rev (%04x, %02x)",
			msp432p4_bank->device_id, msp432p4_bank->hardware_rev);
		if (msp432p4_bank->device_id < 0xA010) {
			// Assume this device is a Falcon
			loader_code = msp432p4_algo;
			loader_size = sizeof(msp432p4_algo);
		} else {
			// Assume this device is a Falcon 2M
			loader_code = msp432p4_2m_algo;
			loader_size = sizeof(msp432p4_2m_algo);
		}
	}

	// Check for working area to use for flash helper algorithm
	if (0 != msp432p4_bank->working_area) {
		target_free_working_area(target, msp432p4_bank->working_area);
	}
	retval = target_alloc_working_area(target, ALGO_WORKING_SIZE,
				&msp432p4_bank->working_area);
	if (ERROR_OK != retval) return retval;

	// Confirm the defined working address is the area we need to use
	if (ALGO_BASE_ADDR != msp432p4_bank->working_area->address) {
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	// Write flash helper algorithm into target memory
	retval = target_write_buffer(target, ALGO_BASE_ADDR, loader_size,
				loader_code);
	if (ERROR_OK != retval) return retval;

	// Initialize the ARMv7 specific info to run the algorithm
	msp432p4_bank->armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	msp432p4_bank->armv7m_info.core_mode = ARM_MODE_THREAD;

	// Initialize algorithm parameters to default values
	msp432p4_init_params(&algo_params);

	// Write out parameters to target memory
	retval = target_write_buffer(target, ALGO_PARAMS_BASE_ADDR,
				sizeof(algo_params), (uint8_t*)&algo_params);
	if (ERROR_OK != retval) return retval;

	// Initialize stack pointer for flash helper algorithm
	init_reg_param(&reg_params[0], "sp", 32, PARAM_OUT);
	buf_set_u32(reg_params[0].value, 0, 32, ALGO_STACK_POINTER_ADDR);

	// Begin executing the flash helper algorithm
	retval = target_start_algorithm(target, 0, 0, 1, reg_params,
				ALGO_ENTRY_ADDR, 0, &msp432p4_bank->armv7m_info);
	if (ERROR_OK != retval) {
		LOG_ERROR("ERROR: Failed to start flash helper algorithm");
		return retval;
	}

	// At this point, the algorithm is running on the target and
	// ready to receive commands and data to flash the target

	// Issue the init command to the flash helper algorithm
	retval = msp432p4_exec_cmd(target, &algo_params, FLASH_INIT);
	if (ERROR_OK != retval) return retval;

	retval = msp432p4_wait_return_code(target);

	// Mark erased status of sectors as "unknown"
	if (0 != bank->sectors) {
		for (int i = 0; i < bank->num_sectors; i++) {
			bank->sectors[i].is_erased = -1;
		}
	}

	return retval;
}

static int msp432p4_quit(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct msp432p4_bank *msp432p4_bank = bank->driver_priv;
	struct msp432p4_algo_params algo_params;

	int retval;

	// At the end of any flash operation, lock the BSL
	msp432p4_bank->unlock_bsl = false;

	// Initialize algorithm parameters to default values
	msp432p4_init_params(&algo_params);

	// Issue the exit command to the flash helper algorithm
	retval = msp432p4_exec_cmd(target, &algo_params, FLASH_EXIT);
	if (ERROR_OK != retval) return retval;

	retval = msp432p4_wait_return_code(target);

	// Regardless of the return code, attempt to halt the target
	(void)target_halt(target);

	// Now confirm target halted and clean up from flash helper algorithm
	retval = target_wait_algorithm(target, 0, 0, 0, 0, 0, FLASH_TIMEOUT,
				&msp432p4_bank->armv7m_info);

	target_free_working_area(target, msp432p4_bank->working_area);
	msp432p4_bank->working_area = 0;

	return retval;
}

static int msp432p4_mass_erase(struct flash_bank *bank, bool all)
{
	struct target *target = bank->target;
	struct msp432p4_bank *msp432p4_bank = bank->driver_priv;
	struct msp432p4_algo_params algo_params;

	int retval;

	if (TARGET_HALTED != target->state) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = msp432p4_init(bank);
	if (ERROR_OK != retval) return retval;

	// Initialize algorithm parameters to default values
	msp432p4_init_params(&algo_params);
	if (all) {
		algo_params.erase_param = FLASH_ERASE_MAIN | FLASH_ERASE_INFO;
		if (msp432p4_bank->unlock_bsl) {
			algo_params.unlock_bsl = FLASH_UNLOCK_BSL;
		}
	}

	// Issue the mass erase command to the flash helper algorithm
	retval = msp432p4_exec_cmd(target, &algo_params, FLASH_MASS_ERASE);
	if (ERROR_OK != retval) return retval;

	retval = msp432p4_wait_return_code(target);
	if (ERROR_OK != retval) return retval;

	retval = msp432p4_quit(bank);
	if (ERROR_OK != retval) return retval;

	// Mark all sectors erased
	if (0 != bank->sectors) {
		for (int i = 0; i < bank->num_sectors; i++) {
			bank->sectors[i].is_erased = 1;
		}
	}

	return retval;
}

COMMAND_HANDLER(msp432p4_mass_erase_command)
{
	struct flash_bank *bank;
	bool all;
	int retval;

	if (0 == CMD_ARGC) {
		all = false;
	} else if (1 == CMD_ARGC) {
		// Check argument for how much to erase
		if (0 == strcmp(CMD_ARGV[0], "main")) {
			all = false;
		} else if (0 == strcmp(CMD_ARGV[0], "all")) {
			all = true;
		} else {
			return ERROR_COMMAND_SYNTAX_ERROR;
		}
	} else {
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	retval = get_flash_bank_by_num(0, &bank);
	if (ERROR_OK != retval) return retval;

	retval = msp432p4_mass_erase(bank, all);
	if (ERROR_OK != retval) return retval;

	LOG_INFO("msp432p4: Mass erase of %s is complete.",
		all ? "main + info flash" : "main flash");

	return ERROR_OK;
}

COMMAND_HANDLER(msp432p4_unlock_bsl_command)
{
	struct flash_bank *bank;
	struct msp432p4_bank *msp432p4_bank;
	int retval;

	if (0 != CMD_ARGC) return ERROR_COMMAND_SYNTAX_ERROR;

	retval = get_flash_bank_by_num(0, &bank);
	if (ERROR_OK != retval) return retval;

	msp432p4_bank = bank->driver_priv;
	msp432p4_bank->unlock_bsl = true;

	LOG_INFO("msp432p4: BSL unlocked until end of next flash command.");

	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(msp432p4_flash_bank_command)
{
	struct msp432p4_bank *msp432p4_bank;
	struct flash_bank *info_bank;

	if (CMD_ARGC < 6) return ERROR_COMMAND_SYNTAX_ERROR;

	msp432p4_bank = malloc(sizeof(struct msp432p4_bank));
	if (0 == msp432p4_bank) return ERROR_FAIL;

	info_bank = malloc(sizeof(struct flash_bank));
	if (0 == info_bank) {
		free((void*)msp432p4_bank);
		return ERROR_FAIL;
	}

	// Initialize private flash information
	msp432p4_bank->device_id = 0;
	msp432p4_bank->hardware_rev = 0;
	msp432p4_bank->probed[0] = false;
	msp432p4_bank->probed[1] = false;
	msp432p4_bank->unlock_bsl = false;
	msp432p4_bank->working_area = 0;

	// Finish initialization of bank 0 (main flash)
	bank->driver_priv = msp432p4_bank;
	bank->next = info_bank;

	// Initialize bank 1 (info region)
	info_bank->name = bank->name;
	info_bank->target = bank->target;
	info_bank->driver = bank->driver;
	info_bank->driver_priv = bank->driver_priv;
	info_bank->bank_number = 1;
	info_bank->base = 0x00200000;
	info_bank->size = 0;
	info_bank->chip_width = 0;
	info_bank->bus_width = 0;
	info_bank->default_padded_value = 0xff;
	info_bank->num_sectors = 0;
	info_bank->sectors = 0;
	info_bank->next = 0;

	return ERROR_OK;
}

static int msp432p4_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	struct msp432p4_bank *msp432p4_bank = bank->driver_priv;
	struct msp432p4_algo_params algo_params;

	int retval;

	if (TARGET_HALTED != target->state) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	// Do a mass erase if user requested all sectors of main flash
	if ((0 == bank->bank_number) && (first == 0) &&
		(last == (bank->num_sectors - 1))) {
		// Request mass erase of main flash
		return msp432p4_mass_erase(bank, false);
	}

	retval = msp432p4_init(bank);
	if (ERROR_OK != retval) return retval;

	// Initialize algorithm parameters to default values
	msp432p4_init_params(&algo_params);

	// Adjust params if this is the info bank
	if (1 == bank->bank_number) {
		algo_params.erase_param = FLASH_ERASE_INFO;
		// And flag if BSL is unlocked
		if (msp432p4_bank->unlock_bsl) {
			algo_params.unlock_bsl = FLASH_UNLOCK_BSL;
		}
	}

	// Erase requested sectors one by one
	for (int i = first; i <= last; i++) {

		// Convert sector number to starting address of sector
		algo_params.address = i * SECTOR_LENGTH;

		// Issue the sector erase command to the flash helper algorithm
		retval = msp432p4_exec_cmd(target, &algo_params, FLASH_SECTOR_ERASE);
		if (ERROR_OK != retval) return retval;

		retval = msp432p4_wait_return_code(target);
		if (ERROR_OK != retval) return retval;

		// Mark the sector as erased
		if (0 != bank->sectors) bank->sectors[i].is_erased = 1;
	}

	retval = msp432p4_quit(bank);
	if (ERROR_OK != retval) return retval;

	return retval;
}

static int msp432p4_protect(struct flash_bank *bank, int set, int first,
	int last)
{
	return ERROR_OK;
}

static int msp432p4_write(struct flash_bank *bank, const uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct msp432p4_bank *msp432p4_bank = bank->driver_priv;
	struct msp432p4_algo_params algo_params;
	uint32_t size;
	uint32_t data_ready = BUFFER_DATA_READY;
	long long start_ms;
	long long elapsed_ms;
	uint32_t end_address = offset + count - 1;
	uint32_t sector;

	int retval;

	if (TARGET_HALTED != target->state) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = msp432p4_init(bank);
	if (ERROR_OK != retval) return retval;

	// Initialize algorithm parameters to default values
	msp432p4_init_params(&algo_params);

	// Set up parameters for requested flash write operation
	algo_params.address = offset;
	algo_params.length = count;

	// Check if this is the info bank
	if (1 == bank->bank_number) {
		// And flag if BSL is unlocked
		if (msp432p4_bank->unlock_bsl) {
			algo_params.unlock_bsl = FLASH_UNLOCK_BSL;
		}
	}

	// Set up flash helper algorithm to continuous flash mode
	retval = msp432p4_exec_cmd(target, &algo_params, FLASH_CONTINUOUS);
	if (ERROR_OK != retval) return retval;

	// Write requested data, one buffer at a time
	start_ms = timeval_ms();
	while (count > 0) {

		if (count > ALGO_BUFFER_SIZE) {
			size = ALGO_BUFFER_SIZE;
		} else {
			size = count;
		}

		// Put next block of data to flash into buffer
		retval = target_write_buffer(target, ALGO_BUFFER1_ADDR, size, buffer);
		if (ERROR_OK != retval) {
			LOG_ERROR("Unable to write data to target memory");
			return ERROR_FLASH_OPERATION_FAILED;
		}

		// Signal the flash helper algorithm that data is ready to flash
		retval = target_write_buffer(target, ALGO_BUFFER1_STATUS_ADDR,
					sizeof(data_ready), (uint8_t*)&data_ready);
		if (ERROR_OK != retval) return ERROR_FLASH_OPERATION_FAILED;

		retval = msp432p4_wait_inactive(target, 1);
		if (ERROR_OK != retval) return retval;

		count -= size;
		buffer += size;

		elapsed_ms = timeval_ms() - start_ms;
		if (elapsed_ms > 500) keep_alive();
	}

	// Confirm that the flash helper algorithm is finished
	retval = msp432p4_wait_return_code(target);
	if (ERROR_OK != retval) return retval;

	retval = msp432p4_quit(bank);
	if (ERROR_OK != retval) return retval;

	// Mark flashed sectors as "not erased"
	while (offset <= end_address) {
		sector = offset / SECTOR_LENGTH;
		bank->sectors[sector].is_erased = 0;
		offset += SECTOR_LENGTH;
	}

	return retval;
}

static int msp432p4_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct msp432p4_bank *msp432p4_bank = bank->driver_priv;

	uint32_t device_id;
	uint32_t hardware_rev;

	uint32_t base;
	uint32_t size;
	int num_sectors;
	int bank_id;

	int retval;

	bank_id = bank->bank_number;

	if (TARGET_HALTED != target->state) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = target_read_u32(target, DEVICE_ID_ADDR, &device_id);
	if (ERROR_OK != retval) return retval;

	msp432p4_bank->device_id = device_id & 0xFFFF;

	retval = target_read_u32(target, HARDWARE_REV_ADDR, &hardware_rev);
	if (ERROR_OK != retval) return retval;

	msp432p4_bank->hardware_rev = hardware_rev & 0xFF;

	if (0 == bank_id) {
		retval = target_read_u32(target, FLASH_MAIN_SIZE_REG, &size);
		if (ERROR_OK != retval) return retval;

		base = FLASH_MAIN_BASE;
		num_sectors = size / SECTOR_LENGTH;
	} else if (1 == bank_id) {
		retval = target_read_u32(target, FLASH_INFO_SIZE_REG, &size);
		if (ERROR_OK != retval) return retval;

		base = FLASH_INFO_BASE;
		num_sectors = size / SECTOR_LENGTH;
	} else {
		// Invalid bank number somehow
		return ERROR_FAIL;
	}

	if (0 != bank->sectors) {
		free(bank->sectors);
		bank->sectors = 0;
	}

	bank->sectors = malloc(sizeof(struct flash_sector) * num_sectors);
	if (0 == bank->sectors) return ERROR_FAIL;

	bank->base = base;
	bank->size = size;
	bank->num_sectors = num_sectors;

	for (int i = 0; i < num_sectors; i++) {
		bank->sectors[i].offset = i * SECTOR_LENGTH;
		bank->sectors[i].size = SECTOR_LENGTH;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 0;
	}

	// We've successfully determined the stats on this flash bank
	msp432p4_bank->probed[bank_id] = true;

	// If we fall through to here, then all went well

	return ERROR_OK;
}

static int msp432p4_auto_probe(struct flash_bank *bank)
{
	struct msp432p4_bank *msp432p4_bank = bank->driver_priv;

	int retval = ERROR_OK;

	if (bank->bank_number < 0 || bank->bank_number > 1) {
		// Invalid bank number somehow
		return ERROR_FAIL;
	}

	if (!msp432p4_bank->probed[bank->bank_number]) {
		retval = msp432p4_probe(bank);
	}

	return retval;
}

static int msp432p4_protect_check(struct flash_bank *bank)
{
	return ERROR_OK;
}

static int msp432p4_info(struct flash_bank *bank, char *buf, int buf_size)
{
	return ERROR_OK;
}

static const struct command_registration msp432p4_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.handler = msp432p4_mass_erase_command,
		.mode = COMMAND_EXEC,
		.help = "Erase entire flash memory on device.",
		.usage = "['main' | 'all']",
	},
	{
		.name = "unlock_bsl",
		.handler = msp432p4_unlock_bsl_command,
		.mode = COMMAND_EXEC,
		.help = "Allow BSL to be erased or written in next flash command.",
		.usage = "",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration msp432p4_command_handlers[] = {
	{
		.name = "msp432p4",
		.mode = COMMAND_EXEC,
		.help = "MSP432p4 flash command group",
		.usage = "",
		.chain = msp432p4_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver msp432p4_flash = {
	.name = "msp432p4",
	.commands = msp432p4_command_handlers,
	.flash_bank_command = msp432p4_flash_bank_command,
	.erase = msp432p4_erase,
	.protect = msp432p4_protect,
	.write = msp432p4_write,
	.read = default_flash_read,
	.probe = msp432p4_probe,
	.auto_probe = msp432p4_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = msp432p4_protect_check,
	.info = msp432p4_info,
};
