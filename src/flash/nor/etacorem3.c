/*
 * Copyright (C) 2017-2018 by Rick Foos
 * rfoos@solengtech.com
 *
 * Copyright (C) 2017-2018 by Eta Compute, Inc.
 * www.etacompute.com
 *
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <stdbool.h>

#include "imp.h"
#include "helper/binarybuffer.h"
#include "target/algorithm.h"
#include "target/armv7m.h"
#include "target/cortex_m.h"
#include "target/arm_opcodes.h"

/**
 * @file
 * Flash programming support for ETA ECM3xx devices.
 */

/* Driver Debug. */
#if 0
#undef LOG_DEBUG
#define LOG_DEBUG LOG_INFO
#endif

/* SRAM Address for magic numbers. */

#define MAGIC_ADDR_M3ETA    (0x0001FFF0)
#define MAGIC_ADDR_ECM3501  (0x1001FFF0)

/* M3ETA */

#define ETA_COMMON_SRAM_MAX_M3ETA  (0x00020000)
#define ETA_COMMON_SRAM_BASE_M3ETA (0x00010000)
#define ETA_COMMON_SRAM_SIZE_M3ETA \
	(ETA_COMMON_SRAM_MAX_M3ETA  - ETA_COMMON_SRAM_BASE_M3ETA)

#define ETA_COMMON_FLASH_MAX_M3ETA  (0)
#define ETA_COMMON_FLASH_BASE_M3ETA (0)
#define ETA_COMMON_FLASH_SIZE_M3ETA (0)

/* ECM3501 */

#define ETA_COMMON_SRAM_MAX_ECM3501  (0x10020000)
#define ETA_COMMON_SRAM_BASE_ECM3501 (0x10000000)
#define ETA_COMMON_SRAM_SIZE_ECM3501 \
	(ETA_COMMON_SRAM_MAX_ECM3501 - ETA_COMMON_SRAM_BASE_ECM3501)

#define ETA_COMMON_FLASH_MAX_ECM3501  (0x01080000)
#define ETA_COMMON_FLASH_BASE_ECM3501 (0x01000000)
#define ETA_COMMON_FLASH_SIZE_ECM3501 \
	(ETA_COMMON_FLASH_MAX_ECM3501 - ETA_COMMON_FLASH_BASE_ECM3501)

#define ETA_COMMON_FLASH_PAGE_SIZE_ECM3501 (4096)
#define ETA_COMMON_FLASH_NUM_PAGES_ECM3501 \
	(ETA_COMMON_FLASH_SIZE_ECM3501 / ETA_COMMON_FLASH_PAGE_SIZE_ECM3501)
#define ETA_COMMON_FLASH_PAGE_ADDR_BITS (12)
#define ETA_COMMON_FLASH_PAGE_ADDR_MASK (0xFFFFF000)

/*
 * Common, used for parameter checks prior to bootrom call.
 */
#define ETA_COMMON_FLASH_MAX  (0x01080000)
#define ETA_COMMON_FLASH_BASE (0x01000000)
#define ETA_COMMON_FLASH_SIZE (ETA_COMMON_FLASH_MAX  - ETA_COMMON_FLASH_BASE)

/**
 * ETA flash bank info from probe.
 */
struct etacorem3_flash_bank {

	/* flash geometry */

	uint32_t num_pages;	/**< Number of flash pages.  */
	uint32_t pagesize;	/**< Flash Page Size  */

	/* part specific info needed by driver. */

	const char *target_name;
	target_addr_t magic_address;	/**< Location of keys in sram. */
	uint32_t sram_base;	/**< SRAM Start Address. */
	uint32_t sram_size;	/**< SRAM size calculated during probe. */
	uint32_t sram_max;
	uint32_t flash_base;	/**< Flash Start Address. */
	uint32_t flash_size;	/**< Flash size calculated during probe. */
	uint32_t flash_max;
	uint32_t bootrom_erase_entry;	/**< BootROM_erase_program */
	uint32_t bootrom_write_entry;	/**< BootROM_flash_program */
	int32_t bootrom_version;	/**< 0-chip, 1-fpga, 2-m3eta. */

	/* Timeouts */
	uint32_t time_per_page_erase;
	uint32_t timeout_erase;
	uint32_t timeout_program;
	bool probed;	/**< Flash bank has been probed. */
};

/**
 * @note SRAM allocations depend on -work-area-phys in target file.
 */

/** Old Stack/Param location. */
#define SRAM_PARAM_START        (0x10001000)
/** Target buffer size. */
#define SRAM_BUFFER_SIZE        (0x00002000)
/** Target algorithm stack size. */
#define SRAM_STACK_SIZE         (0x00000100)

/** if return code not ok, print warning message. */
#define CHECK_STATUS(rc, msg) { \
		if (rc != ERROR_OK) \
			LOG_WARNING("status(%d):%s\n", rc, msg); }

/*
 * Global storage for driver.
 */


/** Magic numbers loaded into SRAM before BootROM call. */
static const uint32_t magic_numbers[] = {
	0xc001c0de,
	0xc001c0de,
	0xdeadbeef,
	0xc369a517,
};

/*
 * Utilities
 */

static int target_write_u32_array(struct target *target, target_addr_t address,
	uint32_t count, const uint32_t *srcbuf)
{
	int retval = ERROR_OK;
	for (uint32_t i = 0; i < count; i++) {
		retval = target_write_u32(target, address + (i * 4), srcbuf[i]);
		if (retval != ERROR_OK)
			break;
	}
	return retval;
}

/**
 * Set magic numbers in target sram.
 * @param bank information.
 * @returns status of write buffer.
 */
static int set_magic_numbers(struct flash_bank *bank)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	/* Use endian neutral call. */
	int retval = target_write_u32_array(bank->target, etacorem3_bank->magic_address,
			(sizeof(magic_numbers)/sizeof(uint32_t)), magic_numbers);
	return retval;
}

/*
 * Timeouts.
 */

/* 1.13289410199725 Seconds to 1133 Microseconds */
#define TIME_PER_PAGE_ERASE_ECM3501 (1133)
#define TIMEOUT_ERASE_ECM3501       (6000)
#define TIMEOUT_PROGRAM_ECM3501     (2000)
#define TIMEOUT_ERASE_FPGA          (4000)
#define TIMEOUT_PROGRAM_FPGA        (1500)

/*
 * Jump table for ecm35xx BootROM's.
 */
#define BOOTROM_LOADER_FLASH_M3ETA      (0x000004F8)
#define BOOTROM_LOADER_FPGA_M3ETA       (0x00000564)
#define BOOTROM_FLASH_ERASE_ECM3501     (0x00000385)
#define BOOTROM_FLASH_PROGRAM_ECM3501   (0x000004C9)
#define BOOTROM_FLASH_ERASE_FPGA        (0x00000249)
#define BOOTROM_FLASH_PROGRAM_FPGA      (0x000002CD)

/*
 * Check for BootROM version with values at jump table locations.
 */
#define CHECK_FLASH_M3ETA               (0xb08cb580)
#define CHECK_FPGA_M3ETA                (0xb08cb580)
#define CHECK_FLASH_ERASE_FPGA          (0x00b089b4)
#define CHECK_FLASH_PROGRAM_FPGA        (0x00b089b4)
#define CHECK_FLASH_ERASE_ECM3501       (0x00b086b5)
#define CHECK_FLASH_PROGRAM_ECM3501     (0x00b088b5)

/**
 * Get BootROM variant from BootRom address contents.
 * Set default, do not return errors.
 */
static int32_t get_variant(struct flash_bank *bank)
{
	/* Detect chip or FPGA bootROM. */
	uint32_t check_erase_fpga, check_program_fpga;
	uint32_t check_erase_board, check_program_board;
	uint32_t check_flash_m3eta, check_fpga_m3eta;
	int32_t retval;

	/* ECM3501 FPGA */
	retval = target_read_u32(bank->target, BOOTROM_FLASH_PROGRAM_FPGA, &check_program_fpga);
	if (retval == ERROR_OK)
		retval = target_read_u32(bank->target, BOOTROM_FLASH_ERASE_FPGA, &check_erase_fpga);











	/* ECM3501 CHIP */
	if (retval == ERROR_OK)
		retval = target_read_u32(bank->target,
				BOOTROM_FLASH_PROGRAM_ECM3501,
				&check_program_board);
	if (retval == ERROR_OK)
		retval = target_read_u32(bank->target,
				BOOTROM_FLASH_ERASE_ECM3501,
				&check_erase_board);

	/* M3ETA CHIP */
	if (retval == ERROR_OK)
		retval = target_read_u32(bank->target,
				BOOTROM_LOADER_FLASH_M3ETA,
				&check_flash_m3eta);
	if (retval == ERROR_OK)
		retval =
			target_read_u32(bank->target, BOOTROM_LOADER_FPGA_M3ETA, &check_fpga_m3eta);











	if (retval == ERROR_OK) {
		if ((check_program_fpga == CHECK_FLASH_PROGRAM_FPGA) && \
			(check_erase_fpga == CHECK_FLASH_ERASE_FPGA))
			retval = 1;	/* fpga bootrom */
		else if ((check_program_board == CHECK_FLASH_PROGRAM_ECM3501) && \
			(check_erase_board == CHECK_FLASH_ERASE_ECM3501))
			retval = 0;	/* chip bootrom */
		else if ((check_flash_m3eta == CHECK_FLASH_M3ETA) && \
			(check_fpga_m3eta == CHECK_FPGA_M3ETA))
			retval = 2;	/* m3eta bootrom */
		else {
			LOG_WARNING("Unknown BootROM version. Default to ECM3501.");
			retval = 0;	/* chip bootrom */
		}
	} else {
		LOG_WARNING("BootROM entry points could not be read (%d).", retval);
		retval = 0;	/* Default is ECM3501 chip. */
	}
	LOG_DEBUG("Bootrom version: %d", retval);
	return retval;
}

/** write the caller's_is_erased flag to given sectors. */
static int write_is_erased(struct flash_bank *bank, int first, int last, int flag)
{
	if ((first > bank->num_sectors) || (last > bank->num_sectors))
		return ERROR_FAIL;

	for (int i = first; i < last; i++)
		bank->sectors[i].is_erased = flag;

	LOG_DEBUG("%d pages erased!", 1+(last-first));

	return ERROR_OK;
}

/**
 * Find memory size.
 * @param bank
 * @param startaddress
 * @param maxsize
 * @param increment
 * @returns on success size in bytes.
 * @returns on failure 0.
 *
 */
static uint32_t get_memory_size(struct flash_bank *bank,
	uint32_t startaddress,
	uint32_t maxsize,
	uint32_t increment)
{
	uint32_t i;

	/* Chip has no Memory. */
	if (maxsize == 0)
		return 0;

	/* The memory scan causes a bus fault. Ignore expected error messages. */
	int save_debug_level = debug_level;
	debug_level = LOG_LVL_OUTPUT;

	/* Read flash size we are testing. 0 - Max flash size, 16k increments. */
	for (i = 0; i < maxsize; i += increment) {
		uint32_t data;
		int retval = target_read_u32(bank->target, startaddress+i, &data);
		if (retval != ERROR_OK)
			break;
	}
	/* Restore debug output level. */
	debug_level = save_debug_level;

	LOG_DEBUG("Memory starting at 0x%08X size: %d KB.",
		startaddress, i/1024);
	return i;
}

/*
 * OpenOCD exec commands.
 */

/** Breakpoint loaded to sram location of return codes. */
#define BREAKPOINT                  (0xfffffffe)

/** Target sram wrapper code for erase. */
static const uint8_t erase_sector_code[] = {
#include "../../../contrib/loaders/flash/etacorem3/erase.inc"
};

/** SRAM parameters for erase. */
typedef struct {
	uint32_t flash_address;
	uint32_t flash_length;
	uint32_t options;
	uint32_t bootrom_entry_point;
	int32_t bootrom_version;	/**< 0-chip, 1-fpga, 2-m3eta. */
	uint32_t retval;
} eta_erase_interface;

/** range check of erase parameters. */
static int check_erase_parameters(eta_erase_interface *sramargs)
{
	int retval = ERROR_OK;

	/* Flash address min/max */
	if (sramargs->flash_address <  ETA_COMMON_FLASH_BASE) {
		retval = 1;
		goto parameter_error;
	}
	if (sramargs->flash_address >= ETA_COMMON_FLASH_MAX) {
		retval = 2;
		goto parameter_error;
	}

	/* Flash length < max. */
	if ((sramargs->flash_address + sramargs->flash_length) > ETA_COMMON_FLASH_MAX) {
		retval = 3;
		goto parameter_error;
	}

	/* Options 0 or 1 */
	if ((sramargs->options != 0) && (sramargs->options != 1)) {
		retval = 4;
		goto parameter_error;
	}

	/* Match one possible entry point. */
	retval = 5;
	const uint32_t bootrom_addresses[] = {
		BOOTROM_FLASH_ERASE_ECM3501,
		BOOTROM_FLASH_PROGRAM_ECM3501,
		BOOTROM_FLASH_ERASE_FPGA,
		BOOTROM_FLASH_PROGRAM_FPGA
	};
	for (unsigned i = 0; i < ARRAY_SIZE(bootrom_addresses); i++) {
		if (sramargs->bootrom_entry_point == bootrom_addresses[i]) {
			retval = ERROR_OK;
			break;
		}
	}
	if (retval != ERROR_OK)
		goto parameter_error;

	/* Bootrom version 0-chip, 1-fpga, 2-m3eta. */
	if ((sramargs->bootrom_version < 0) || (sramargs->bootrom_version > 2)) {
		retval = 6;
		goto parameter_error;
	}

parameter_error:
	if (retval != ERROR_OK) {
		LOG_ERROR("erase parameter error %d.", retval);
		retval = ERROR_COMMAND_ARGUMENT_INVALID;
	}
	return retval;
}

/** Target sram wrapper code for write. */
static const uint8_t write_sector_code[] = {
#include "../../../contrib/loaders/flash/etacorem3/write.inc"
};

/** SRAM parameters for write. */
typedef struct {
	uint32_t flash_address;
	uint32_t flash_length;
	uint32_t sram_buffer;
	uint32_t options;	/**< 1 - Write 512 bytes at a time. */
	uint32_t bootrom_entry_point;
	int32_t bootrom_version;	/**< 0-chip, 1-fpga, 2-m3eta. */
	uint32_t retval;
} eta_write_interface;

/** range check of write parameters. */
static int check_write_parameters(eta_write_interface *sramargs)
{
	int retval = ERROR_OK;

	/* Flash address min/max */
	if (sramargs->flash_address <  ETA_COMMON_FLASH_BASE) {
		retval = 1;
		goto parameter_error;
	}
	if (sramargs->flash_address >= ETA_COMMON_FLASH_MAX) {
		retval = 2;
		goto parameter_error;
	}

	/* Flash length < max. */
	if ((sramargs->flash_address + sramargs->flash_length) > ETA_COMMON_FLASH_MAX) {
		retval = 3;
		goto parameter_error;
	}

	/* Options 0 or 1 */
	if ((sramargs->options != 0) && (sramargs->options != 1)) {
		retval = 4;
		goto parameter_error;
	}

	/* Match one possible entry point. */
	retval = 5;
	const uint32_t bootrom_addresses[] = {
		BOOTROM_FLASH_ERASE_ECM3501,
		BOOTROM_FLASH_PROGRAM_ECM3501,
		BOOTROM_FLASH_ERASE_FPGA,
		BOOTROM_FLASH_PROGRAM_FPGA
	};
	for (unsigned i = 0; i < ARRAY_SIZE(bootrom_addresses); i++) {
		if (sramargs->bootrom_entry_point == bootrom_addresses[i]) {
			retval = ERROR_OK;
			break;
		}
	}
	if (retval != ERROR_OK)
		goto parameter_error;

	/* Bootrom version 0-chip, 1-fpga, 2-m3eta. */
	if ((sramargs->bootrom_version < 0) || (sramargs->bootrom_version > 2)) {
		retval = 6;
		goto parameter_error;
	}

parameter_error:
	if (retval != ERROR_OK) {
		LOG_ERROR("erase parameter error %d.", retval);
		retval = ERROR_COMMAND_ARGUMENT_INVALID;
	}
	return retval;
}

/** Common code for erase commands. */
static int common_erase_run(struct flash_bank *bank,
	int param_size,
	eta_erase_interface sramargs)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	int retval;
	struct working_area *workarea = NULL, *paramarea = NULL;
	struct working_area *stackarea = NULL;

	retval = check_erase_parameters(&sramargs);
	if (retval != ERROR_OK)
		return retval;

	/*
	 * Load Magic numbers required for bootrom help function execution.
	 */
	retval = set_magic_numbers(bank);
	if (retval != ERROR_OK) {
		CHECK_STATUS(retval, "error writing magic numbers to target.");
		goto err_write_code;
	}

	struct reg_param reg_params[2];
	struct armv7m_algorithm armv7m_algo;

	/*
	 * Load erase code.
	 */
	retval = target_alloc_working_area(bank->target,
			sizeof(erase_sector_code), &workarea);
	LOG_DEBUG("workarea address: " TARGET_ADDR_FMT ".", workarea->address);
	if (retval != ERROR_OK) {
		LOG_ERROR("No working area available.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}
	LOG_DEBUG("erase size %lu", sizeof(erase_sector_code));
	retval = target_write_buffer(bank->target, workarea->address,
			sizeof(erase_sector_code), erase_sector_code);
	if (retval != ERROR_OK)
		goto err_write_code;

	/*
	 * Load SRAM parameters.
	 */
	retval = target_alloc_working_area(bank->target,
			param_size, &paramarea);
	LOG_DEBUG("parameter address: " TARGET_ADDR_FMT ".", paramarea->address);
	if (retval != ERROR_OK) {
		LOG_ERROR("No paramameter area available.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}
	retval = target_write_u32_array(bank->target, paramarea->address,
			(param_size/sizeof(uint32_t)), (uint32_t *)&sramargs);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to load sram parameters.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}

	/*
	 * Allocate stack area.
	 */
	retval = target_alloc_working_area(bank->target,
			SRAM_STACK_SIZE, &stackarea);
	LOG_DEBUG("stackarea address: " TARGET_ADDR_FMT ".", stackarea->address);
	if (retval != ERROR_OK) {
		LOG_ERROR("No stack area available.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}

	armv7m_algo.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_algo.core_mode = ARM_MODE_THREAD;

	/* allocate registers sp, and r0. */
	init_reg_param(&reg_params[0], "sp", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r0", 32, PARAM_OUT);

	/* Set the sram stack in sp. */
	buf_set_u32(reg_params[0].value, 0, 32, (stackarea->address + SRAM_STACK_SIZE));
	/* Set the sram parameter address in r0. */
	buf_set_u32(reg_params[1].value, 0, 32, paramarea->address);

	/* Ignore expected error messages from breakpoint. */
	int save_debug_level = debug_level;
	debug_level = LOG_LVL_OUTPUT;

	/* Run the code. */
	retval = target_run_algorithm(bank->target,
			0, NULL,
			ARRAY_SIZE(reg_params), reg_params,
			workarea->address, 0,
			etacorem3_bank->timeout_erase, &armv7m_algo);

	/* Restore debug output level. */
	debug_level = save_debug_level;

	/* Read return code from sram parameter area. */
	uint32_t retvalT;
	int retval1 = target_read_u32(bank->target,
			(paramarea->address + offsetof(eta_erase_interface, retval)),
			&retvalT);
	if ((retval != ERROR_OK) || (retval1 != ERROR_OK) || (retvalT != 0)) {
		LOG_ERROR(
			"Error executing flash erase %d, RC1 %d, TRC %d.",
			retval,
			retval1,
			retvalT);
		retval = ERROR_FLASH_OPERATION_FAILED;
		goto err_run;
	}

err_run:
	for (unsigned i = 0; i < ARRAY_SIZE(reg_params); i++)
		destroy_reg_param(&reg_params[i]);

err_write_code:
	if (workarea != NULL)
		target_free_working_area(bank->target, workarea);
	if (paramarea != NULL)
		target_free_working_area(bank->target, paramarea);
	if (stackarea != NULL)
		target_free_working_area(bank->target, stackarea);

err_alloc_code:

	return retval;
}
/**
 * Mass erase flash bank.
 * @param bank Pointer to the flash bank descriptor.
 * @return retval
 *
 */
static int etacorem3_mass_erase(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	int retval = ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}
	if (!etacorem3_bank->probed) {
		LOG_ERROR("Target not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	/*
	 * Load SRAM Parameters.
	 */
	eta_erase_interface sramargs = {
		etacorem3_bank->flash_base,	/**< Start of flash. */
		0x00000000,	/**< Length 0 for all. */
		0x00000001,	/**< Option 1, mass erase. */
		etacorem3_bank->bootrom_erase_entry,	/**< bootrom entry point. */
		etacorem3_bank->bootrom_version,/**< chip, fpga, m3eta */
		BREAKPOINT	/**< Return code from bootrom. */
	};

	/* Common erase execution code. */
	retval = common_erase_run(bank,
			sizeof(eta_erase_interface), sramargs);

	/* if successful, mark sectors as erased */
	if (retval == ERROR_OK)
		write_is_erased(bank,
			etacorem3_bank->flash_base,
			etacorem3_bank->flash_base + etacorem3_bank->flash_size,
			1);

	LOG_DEBUG("Mass erase on bank %d.", bank->bank_number);

/*err_alloc_code:*/
	return retval;
}

/**
 * Erase pages in flash.
 *
 * @param bank Pointer to the flash bank descriptor.
 * @param first
 * @param last
 * @return retval
 *
 */
static int etacorem3_erase(struct flash_bank *bank, int first, int last)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	struct target *target = bank->target;
	int retval;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}
	if (!etacorem3_bank->probed) {
		LOG_ERROR("Target not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	/*
	 * Check for valid page range.
	 */
	if ((first < 0) || (last < first) || (last >= (int)etacorem3_bank->num_pages))
		return ERROR_FLASH_SECTOR_INVALID;

	/*
	 * Mass Erase if all pages are given.
	 */
	if ((first == 0) && (last == ((int)etacorem3_bank->num_pages-1)))
		return etacorem3_mass_erase(bank);

	/*
	 * Load SRAM Parameters.
	 */
	eta_erase_interface sramargs = {
		etacorem3_bank->flash_base + (first * etacorem3_bank->pagesize),
		(last - first + 1) * etacorem3_bank->pagesize,	/**< Length in bytes. */
		0x00000000,	/**< Request page erase. */
		etacorem3_bank->bootrom_erase_entry,	/**< bootrom entry point. */
		etacorem3_bank->bootrom_version,/**< chip or fpga */
		BREAKPOINT	/**< Return code from bootrom. */
	};

	/* ECM3501 chip needs longer sector erase timeout, and user warning. */
	if (etacorem3_bank->time_per_page_erase != 0) {
		uint32_t erasetime = \
			(last - first + 1) * etacorem3_bank->time_per_page_erase;
		LOG_DEBUG("erasetime: %u.", erasetime);
		if (erasetime > 20000)
			LOG_INFO("Estimated erase time %u seconds.", erasetime/1000);
		etacorem3_bank->timeout_erase = erasetime;
	}

	/* Common erase execution code. */
	retval = common_erase_run(bank,
			sizeof(eta_erase_interface), sramargs);

	/* if successful, mark sectors as erased */
	if (retval == ERROR_OK)
		write_is_erased(bank, first, last, 1);

	return retval;
}

/**
 * Write pages to flash from buffer.
 * @param bank
 * @param buffer
 * @param offset
 * @param count
 * @returns success ERROR_OK
 */
static int etacorem3_write(struct flash_bank *bank,
	const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	struct target *target = bank->target;
	uint32_t address = bank->base + offset;
	uint32_t sram_buffer;
	uint32_t maxbuffer;
	uint32_t thisrun_count;
	struct working_area *workarea = NULL, *paramarea = NULL;
	struct working_area *bufferarea = NULL, *stackarea = NULL;
	int retval = ERROR_OK;

	/* Bootrom uses 32 bit boundaries, 64 bit count. */
	if (((count%4) != 0) || ((offset%4) != 0)) {
		LOG_ERROR("write block must be multiple of 4 bytes in offset & length");
		return ERROR_FAIL;
	}

	/*
	 * Load Magic numbers required for bootrom help function execution.
	 */
	retval = set_magic_numbers(bank);
	if (retval != ERROR_OK) {
		CHECK_STATUS(retval, "error writing magic numbers to target.");
		goto err_write_code;
	}

	/*
	 * Max buffer size for this device...
	 * Chip Bootrom can only write 512 bytes at a time.
	 * Target side code will block the write into 512 bytes.
	 */
	maxbuffer = SRAM_BUFFER_SIZE;

	/*
	 * Allocate space on target for write buffer.
	 */
	retval = target_alloc_working_area(bank->target,
			maxbuffer, &bufferarea);
	LOG_DEBUG("bufferarea address: " TARGET_ADDR_FMT ", retval %d.", bufferarea->address,
		retval);
	if (retval != ERROR_OK) {
		LOG_ERROR("No buffer area available.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}
	sram_buffer = bufferarea->address;

	/* R0 and SP for algorithm. */
	struct reg_param reg_params[2];

	/*
	 * Allocate space on target for write code.
	 */
	retval = target_alloc_working_area(bank->target,
			sizeof(write_sector_code), &workarea);
	LOG_DEBUG("workarea address: " TARGET_ADDR_FMT ", retval %d.", workarea->address, retval);
	if (retval != ERROR_OK) {
		LOG_ERROR("No working area available.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}
	/*
	 * Load code on target.
	 */
	retval = target_write_buffer(bank->target, workarea->address,
			sizeof(write_sector_code), write_sector_code);
	if (retval != ERROR_OK)
		goto err_write_code;

	/*
	 * Allocate sram parameter area.
	 */
	retval = target_alloc_working_area(bank->target,
			sizeof(eta_write_interface), &paramarea);
	LOG_DEBUG("parameter address: " TARGET_ADDR_FMT ".", paramarea->address);
	if (retval != ERROR_OK) {
		LOG_ERROR("No param area available.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}

	/*
	 * Allocate stack area.
	 */
	retval = target_alloc_working_area(bank->target,
			SRAM_STACK_SIZE, &stackarea);
	LOG_DEBUG("stackarea address: " TARGET_ADDR_FMT ".", stackarea->address);
	if (retval != ERROR_OK) {
		LOG_ERROR("No stack area available.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}

	while (count > 0) {
		if (count > maxbuffer)
			thisrun_count = maxbuffer;
		else
			thisrun_count = count;

		/*
		 * Load target Write Buffer.
		 */
		retval = target_write_buffer(target, sram_buffer, thisrun_count, buffer);
		if (retval != ERROR_OK) {
			LOG_ERROR("status(%d):%s\n", retval, "error writing buffer to target.");
			break;
		}

		/*
		 * Load SRAM Parameters.
		 */
		eta_write_interface sramargs = {
			address,	/**< Start address in flash. */
			thisrun_count,	/**< Length in bytes. */
			sram_buffer,
			0x00000001,	/**< Option 1, write flash in 512 byte blocks. */
			etacorem3_bank->bootrom_write_entry,	/**< bootrom entry point. */
			etacorem3_bank->bootrom_version,/**< chip or fpga */
			BREAKPOINT	/**< Return code from bootrom. */
		};
		retval = check_write_parameters(&sramargs);
		if (retval != ERROR_OK)
			break;
		retval = target_write_u32_array(bank->target,
				paramarea->address,
				(sizeof(eta_write_interface)/sizeof(uint32_t)),
				(uint32_t *)&sramargs);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to load sram parameters.");
			retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
			break;
		}

		struct armv7m_algorithm armv7m_algo;

		armv7m_algo.common_magic = ARMV7M_COMMON_MAGIC;
		armv7m_algo.core_mode = ARM_MODE_THREAD;

		/* allocate registers sp, and r0. */
		init_reg_param(&reg_params[0], "sp", 32, PARAM_OUT);
		init_reg_param(&reg_params[1], "r0", 32, PARAM_OUT);

		/* Set the sram stack in sp. */
		buf_set_u32(reg_params[0].value, 0, 32,
			(stackarea->address + SRAM_STACK_SIZE));
		/* Set the sram parameter address in r0. */
		buf_set_u32(reg_params[1].value, 0, 32, paramarea->address);

		/* Ignore expected error messages from breakpoint. */
		int save_debug_level = debug_level;
		debug_level = LOG_LVL_OUTPUT;

		/* Run the code. */
		retval = target_run_algorithm(bank->target,
				0, NULL,
				ARRAY_SIZE(reg_params), reg_params,
				workarea->address, 0,
				etacorem3_bank->timeout_program, &armv7m_algo);

		/* Restore debug output level. */
		debug_level = save_debug_level;

		/* Read return code from sram parameter area. */
		uint32_t retvalT;
		int retval1 = target_read_u32(bank->target,
				(paramarea->address + offsetof(eta_write_interface, retval)),
				&retvalT);
		if ((retval != ERROR_OK) || (retval1 != ERROR_OK) || (retvalT != 0)) {
			LOG_ERROR(
				"Error executing flash erase %d, RC1 %d, TRC %d.",
				retval,
				retval1,
				retvalT);
			LOG_DEBUG("address: 0x%08X, count: 0x%08X", address, thisrun_count);
			retval = ERROR_FLASH_OPERATION_FAILED;
			goto err_run;
		}
		buffer += thisrun_count;
		address += thisrun_count;
		count -= thisrun_count;
	}


err_run:
	for (unsigned i = 0; i < ARRAY_SIZE(reg_params); i++)
		destroy_reg_param(&reg_params[i]);

err_write_code:
	if (workarea != NULL)
		target_free_working_area(bank->target, workarea);
	if (paramarea != NULL)
		target_free_working_area(bank->target, paramarea);
	if (stackarea != NULL)
		target_free_working_area(bank->target, stackarea);
	if (bufferarea != NULL)
		target_free_working_area(bank->target, bufferarea);

err_alloc_code:

	return retval;
}

/**
 * Can't protect/unprotect pages on etacorem3.
 * @param bank
 * @param set
 * @param first
 * @param last
 * @returns
 */
static int etacorem3_protect(struct flash_bank *bank, int set, int first, int last)
{
	/*
	 * Can't protect/unprotect on the etacorem3.
	 * Initialized to unprotected.
	 */
	LOG_WARNING("Cannot protect/unprotect.");
	return ERROR_OK;
}

/**
 * Sectors are always unprotected.
 * @param bank
 * @returns
 *
 */
static int etacorem3_protect_check(struct flash_bank *bank)
{
	/*
	* sectors are always unprotected.
	* set at initialization.
	*/
	return ERROR_OK;
}
/**
 * Probe flash part, and build sector list.
 * name: etacorem3_probe
 * @param bank Pointer to the flash bank descriptor.
 * @return on success: ERROR_OK
 *
 */
static int etacorem3_probe(struct flash_bank *bank)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;

	if (etacorem3_bank->probed) {
		LOG_DEBUG("Part already probed.");
		return ERROR_OK;
	} else
		LOG_DEBUG("Probing part.");

	/* Check bootrom version, get_variant sets default, no errors. */
	etacorem3_bank->pagesize = 4096;
	etacorem3_bank->magic_address = MAGIC_ADDR_ECM3501;

	etacorem3_bank->bootrom_version = get_variant(bank);
	if (etacorem3_bank->bootrom_version == 0) {
		etacorem3_bank->target_name = "ECM3501";
		etacorem3_bank->bootrom_erase_entry = BOOTROM_FLASH_ERASE_ECM3501;
		etacorem3_bank->bootrom_write_entry = BOOTROM_FLASH_PROGRAM_ECM3501;
		etacorem3_bank->timeout_erase = TIMEOUT_ERASE_ECM3501;
		etacorem3_bank->timeout_program = TIMEOUT_PROGRAM_ECM3501;
		etacorem3_bank->time_per_page_erase = TIME_PER_PAGE_ERASE_ECM3501;
		/* Load for ECM3501. */
		etacorem3_bank->sram_base = ETA_COMMON_SRAM_BASE_ECM3501;
		etacorem3_bank->sram_max = ETA_COMMON_SRAM_MAX_ECM3501;
		etacorem3_bank->flash_base = ETA_COMMON_FLASH_BASE_ECM3501;
		etacorem3_bank->flash_max = ETA_COMMON_FLASH_MAX_ECM3501;

	} else if (etacorem3_bank->bootrom_version == 1) {
		etacorem3_bank->target_name = "ECM3501 FPGA";
		etacorem3_bank->bootrom_erase_entry = BOOTROM_FLASH_ERASE_FPGA;
		etacorem3_bank->bootrom_write_entry = BOOTROM_FLASH_PROGRAM_FPGA;
		etacorem3_bank->timeout_erase = TIMEOUT_ERASE_FPGA;
		etacorem3_bank->timeout_program = TIMEOUT_PROGRAM_FPGA;
		etacorem3_bank->time_per_page_erase = 0.0;
		/* Load for ECM3501. */
		etacorem3_bank->sram_base = ETA_COMMON_SRAM_BASE_ECM3501;
		etacorem3_bank->sram_max = ETA_COMMON_SRAM_MAX_ECM3501;
		etacorem3_bank->flash_base = ETA_COMMON_FLASH_BASE_ECM3501;
		etacorem3_bank->flash_max = ETA_COMMON_FLASH_MAX_ECM3501;

	} else if (etacorem3_bank->bootrom_version == 2) {
		etacorem3_bank->target_name = "M3ETA";
		etacorem3_bank->bootrom_erase_entry = 0;
		etacorem3_bank->bootrom_write_entry = 0;
		etacorem3_bank->pagesize = 0;
		etacorem3_bank->magic_address = MAGIC_ADDR_M3ETA;
		etacorem3_bank->time_per_page_erase = 0.0;
		/* Load for M3ETA. */
		etacorem3_bank->sram_base = ETA_COMMON_SRAM_BASE_M3ETA;
		etacorem3_bank->sram_max = ETA_COMMON_SRAM_MAX_M3ETA;
		etacorem3_bank->flash_base = ETA_COMMON_FLASH_BASE_M3ETA;
		etacorem3_bank->flash_max = ETA_COMMON_FLASH_MAX_M3ETA;

	}
	/* Do a size test. */
	etacorem3_bank->sram_size  = get_memory_size(bank,
			etacorem3_bank->sram_base,
			etacorem3_bank->sram_max,
			(16*1024));
	etacorem3_bank->flash_size = get_memory_size(bank,
			etacorem3_bank->flash_base,
			etacorem3_bank->flash_max,
			(32*1024));

	/* provide this for the benefit of the NOR flash framework */
	if (etacorem3_bank->flash_size && etacorem3_bank->pagesize)
		etacorem3_bank->num_pages = etacorem3_bank->flash_size / etacorem3_bank->pagesize;
	bank->base = (bank->bank_number * etacorem3_bank->flash_size) + \
		etacorem3_bank->flash_base;
	bank->size = etacorem3_bank->pagesize * etacorem3_bank->num_pages;
	bank->num_sectors = etacorem3_bank->num_pages;

	LOG_DEBUG("bank number: %d, base: 0x%08X, size: %d KB, num sectors: %d.",
		bank->bank_number,
		bank->base,
		bank->size/1024,
		bank->num_sectors);

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}

	if (bank->num_sectors != 0) {
		bank->sectors = calloc(bank->num_sectors, sizeof(struct flash_sector));
		for (int i = 0; i < bank->num_sectors; i++) {
			bank->sectors[i].offset = i * etacorem3_bank->pagesize;
			bank->sectors[i].size = etacorem3_bank->pagesize;
			bank->sectors[i].is_erased = -1;
			/* No flash protect in this hardware. */
			bank->sectors[i].is_protected = 0;
		}
	}

	etacorem3_bank->probed = true;
	return ERROR_OK;
}

static int etacorem3_auto_probe(struct flash_bank *bank)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;

	if (etacorem3_bank->probed)
		return ERROR_OK;

	return etacorem3_probe(bank);
}


/**
 * Display ETA chip info from probe.
 * @param bank
 * @param buf Buffer to be printed.
 * @param buf_size Size of buffer.
 * @returns success
 */
static int get_etacorem3_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;

	if (!etacorem3_bank->probed) {
		LOG_ERROR("Target not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	int printed = snprintf(buf,
			buf_size,
			"\nETA Compute %s."
			"\n\tTotal Flash: %u KB, Sram: %u KB."
			"\n\tStart Flash: 0x%08X, Sram: 0x%08X.",
			etacorem3_bank->target_name,
			(etacorem3_bank->flash_size/1024),
			(etacorem3_bank->sram_size/1024)
			,
			etacorem3_bank->flash_base,
			etacorem3_bank->sram_base
			);

	if (printed < 0)
		return ERROR_BUF_TOO_SMALL;
	return ERROR_OK;
}

/*
 * openocd command interface
 */

/**
 * Initialize etacorem3 bank info.
 * @returns success
 *
 * From target cfg file.
 * @code
 * flash_bank etacorem3 <base> <size> 0 0 <target#> [variant]
 * @endcode
 */
FLASH_BANK_COMMAND_HANDLER(etacorem3_flash_bank_command)
{
	struct etacorem3_flash_bank *etacorem3_bank;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	etacorem3_bank = calloc(1, sizeof(*etacorem3_bank));
	if (!etacorem3_bank)
		return ERROR_FLASH_OPERATION_FAILED;

	bank->driver_priv = etacorem3_bank;
	etacorem3_bank->probed = false;

	return ERROR_OK;
}

/**
 * Handle external mass erase command.
 * @returns ERROR_COMMAND_SYNTAX_ERROR or ERROR_OK.
 */
COMMAND_HANDLER(etacorem3_handle_mass_erase_command)
{

	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *bank;
	uint32_t retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	if (etacorem3_mass_erase(bank) == ERROR_OK) {
		/* set all sectors as erased */
		for (int i = 0; i < bank->num_sectors; i++)
			bank->sectors[i].is_erased = 1;

		command_print(CMD_CTX, "etacorem3 mass erase complete");
	} else
		command_print(CMD_CTX, "etacorem3 mass erase failed");

	return ERROR_OK;
}


/**
 * Register exec commands, extensions to standard OCD commands.
 * Use this for additional commands specific to this target.
 * (i.e. Commands for automation, validation, or production)
 */
static const struct command_registration etacorem3_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.usage = "<bank>",
		.handler = etacorem3_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.help = "Erase entire device",
	},
	COMMAND_REGISTRATION_DONE
};

/**
 * Register required commands and chain to optional exec commands.
 */
static const struct command_registration etacorem3_command_handlers[] = {
	{
		.name = "etacorem3",
		.mode = COMMAND_EXEC,
		.help = "etacorem3 flash command group",
		.usage = "Support for ETA Compute ecm35xx parts.",
		.chain = etacorem3_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

/**
 * Required OpenOCD flash driver commands.
 */
struct flash_driver etacorem3_flash = {
	.name = "etacorem3",
	.commands = etacorem3_command_handlers,
	.flash_bank_command = etacorem3_flash_bank_command,
	.erase = etacorem3_erase,
	.protect = etacorem3_protect,
	.write = etacorem3_write,
	.read = default_flash_read,
	.probe = etacorem3_probe,
	.auto_probe = etacorem3_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = etacorem3_protect_check,
	.info = get_etacorem3_info,
};
