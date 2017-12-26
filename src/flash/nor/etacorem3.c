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

#include <helper/time_support.h>
#include <target/image.h>

#include "../../../contrib/loaders/flash/etacorem3/etacorem3_flash_common.h"

/**
 * @file
 * Flash programming support for ETA ECM3xx devices.
 */

/* Driver Debug. */
#if 0
#undef LOG_DEBUG
#define LOG_DEBUG LOG_INFO
#endif

/*
 * M3ETA
 */

#define ETA_SRAM_MAX_M3ETA  (0x00020000)
#define ETA_SRAM_BASE_M3ETA (0x00010000)
#define ETA_SRAM_SIZE_M3ETA \
	(ETA_SRAM_MAX_M3ETA - ETA_SRAM_BASE_M3ETA)

#define ETA_FLASH_MAX_M3ETA  (0)
#define ETA_FLASH_BASE_M3ETA (0)
#define ETA_FLASH_SIZE_M3ETA (0)

/*
 * ECM3501
 */

#define ETA_SRAM_MAX_ECM3501            ETA_COMMON_SRAM_MAX
#define ETA_SRAM_BASE_ECM3501           ETA_COMMON_SRAM_BASE
#define ETA_SRAM_SIZE_ECM3501           ETA_COMMON_SRAM_SIZE

#define ETA_FLASH_MAX_ECM3501           ETA_COMMON_FLASH_MAX
#define ETA_FLASH_BASE_ECM3501          ETA_COMMON_FLASH_BASE
#define ETA_FLASH_SIZE_ECM3501          ETA_COMMON_FLASH_SIZE

#define ETA_FLASH_PAGE_SIZE_ECM3501     ETA_COMMON_FLASH_PAGE_SIZE
#define ETA_FLASH_NUM_PAGES_ECM3501     ETA_COMMON_FLASH_NUM_PAGES

/*
 * ECM3531
 */

#define ETA_SRAM_MAX_ECM3531            ETA_COMMON_SRAM_MAX
#define ETA_SRAM_BASE_ECM3531           ETA_COMMON_SRAM_BASE
#define ETA_SRAM_SIZE_ECM3531           ETA_COMMON_SRAM_SIZE

#define ETA_FLASH_MAX_ECM3531           ETA_COMMON_FLASH_MAX
#define ETA_FLASH_BASE_ECM3531          ETA_COMMON_FLASH_BASE
#define ETA_FLASH_SIZE_ECM3531          ETA_COMMON_FLASH_SIZE

#define ETA_FLASH_PAGE_SIZE_ECM3531     ETA_COMMON_FLASH_PAGE_SIZE
#define ETA_FLASH_NUM_PAGES_ECM3531     ETA_COMMON_FLASH_NUM_PAGES

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
	uint32_t bootrom_load_entry;	/**< BootROM_flash_load */
	uint32_t bootrom_store_entry;	/**< BootROM_flash_store */
	uint32_t bootrom_erase_entry;	/**< BootROM_flash_erase */
	uint32_t bootrom_write_entry;	/**< BootROM_flash_program */
	uint32_t bootrom_read_entry;	/**< BootROM_flash_read */
	uint32_t bootrom_version;	/**< 0-chip, 1-fpga, 2-m3eta, 3-ECM3531 */
	uint32_t branchtable_start;	/**< Start address of branch table. */

	/* Timeouts */

	uint32_t time_per_page_erase;
	uint32_t timeout_erase;
	uint32_t timeout_program;

	/* Flags and Semaphores */

	uint32_t info_semaphore;/**< option passed over to target driver call. */
	uint32_t target_buffer;	/**< user specified target buffer address. */
	bool probed;	/**< Flash bank has been probed. */
};

/**
 * @note SRAM allocations depend on -work-area-phys in target file.
 */

/** Target algorithm stack size. */
#define SRAM_STACK_SIZE         (0x00000100)

/** if return code not ok, print warning message. */
#define CHECK_STATUS(rc, msg) { \
		if (rc != ERROR_OK) \
			LOG_WARNING("status(%d):%s\n", rc, msg); }

/*
 * Global storage for driver.
 */

#if 0
/** Magic numbers loaded into SRAM before BootROM call. */
static const uint32_t magic_numbers[] = {
	0xc001c0de,
	0xc001c0de,
	0xdeadbeef,
	0xc369a517,
};
#endif

/**
 *  Bootrom Branch Table Offsets
 */
#define BRANCHTABLE_FLASH_WS            (0x00)	/* 0x98 */
#define BRANCHTABLE_FLASH_LOAD          (0x04)	/* 0x9C */
#define BRANCHTABLE_FLASH_STORE         (0x08)	/* 0xA0 */
#define BRANCHTABLE_FLASH_VERSION       (0x0C)	/* 0xA4 */
#define BRANCHTABLE_FLASH_ERASE_REF     (0x10)	/* 0xA8 */
#define BRANCHTABLE_FLASH_ERASE         (0x14)	/* 0xAC */
#define BRANCHTABLE_FLASH_PROGRAM       (0x18)	/* 0xB0 */
#define BRANCHTABLE_FLASH_READ          (0x1C)	/* 0xB4 */

/**
 *  Bootrom branch table key.
 *
 *  Version info 0x00180502 (ecm3531)
 *  First table entry 0x00001fb4.
 */
static const uint32_t branchtable_key[] = {
	0x43415445,	/**< 3 Word "ETACOMPUTE" zero terminated string. */
	0x55504d4f,
	0x00004554,
};

#if 0
/**
 *  b[0] ECM3531, Can't use this for fpga version.
 */
static const uint32_t branchtable_version[] = {
	0x00180502,	/**< ECM3531 */
};
#endif

/*
 * Utilities
 */

/**
 *  \brief Write array of 32 bit words.
 *
 *  \param [in] target OCD target information.
 *  \param [in] address Target address.
 *  \param [in] count Number of words.
 *  \param [in] srcbuf Buffer to write.
 *  \return Retval
 *
 *  \details Write 32 bit endian corrected array.
 */
static int target_write_u32_array(struct target *target,
	target_addr_t address,
	uint32_t count,
	const uint32_t *srcbuf)
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
 *  \brief Read array of 32 bit words.
 *
 *  \param [in] target Target descriptor.
 *  \param [in] address Target address.
 *  \param [in] count Number of 32 bit words.
 *  \param [in] srcbuf Buffer to read.
 *  \return Retval.
 *
 *  \details Endian correct 32 bit array reader.
 */
static int target_read_u32_array(struct target *target, target_addr_t address,
	uint32_t count, uint32_t *srcbuf)
{
	int retval = ERROR_OK;
	for (uint32_t i = 0; i < count; i++) {
		retval = target_read_u32(target, address + (i * 4), &srcbuf[i]);
		if (retval != ERROR_OK)
			break;
	}
	return retval;
}

#if 0
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

/**
 * Clear magic numbers in target sram.
 * @param bank information.
 * @returns status of write buffer.
 */
static int clear_magic_numbers(struct flash_bank *bank)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	const uint32_t \
		clear_magic[(sizeof(magic_numbers)/sizeof(uint32_t))] = {0,};
	/* Use endian neutral call. */
	int retval = target_write_u32_array(bank->target, etacorem3_bank->magic_address,
			(sizeof(magic_numbers)/sizeof(uint32_t)), clear_magic);
	return retval;
}
#endif

/*
 * Timeouts.
 */

/* 1.13289410199725 Seconds to 1133 Microseconds */
#define TIME_PER_PAGE_ERASE_ECM3501  (1133)
#define TIMEOUT_ERASE_ECM3501        (6000)
#define TIMEOUT_PROGRAM_ECM3501      (2000)
#define TIMEOUT_ERASE_ECM3501_FPGA   (4000)
#define TIMEOUT_PROGRAM_ECM3501_FPGA (1500)

/**
 *  \brief Find branch table, return version.
 *
 *  \param [in] bank Target descriptor.
 *  \param [in] version Version from Bootrom or 0.
 *  \return ERROR_OK or flash specific error status.
 *
 *  \details Scan first 2k of bootrom for branch table.
 */
int find_branch_table(struct flash_bank *bank, uint32_t *version)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	uint32_t romptr;
	int retval = ERROR_OK;

	*version = 0;
	/* find the branch table in bootrom (limit search to 0-2KB) */
	for (romptr = 0; romptr < 2048; romptr += sizeof(uint32_t)) {
		uint32_t romval;
		/* Search single 32 bit words. */
		retval = target_read_u32(bank->target, romptr, &romval);
		if ((retval == ERROR_OK) && (romval == branchtable_key[0])) {
			/** @todo change to struct to avoid flogging. */
			uint32_t startup_head[6];
#if 0
			0x43415445,			/**< "ETACOMPUTE" zero terminated. */
			0x55504d4f,
			0x00004554,
			0x00000000,			/**< 1 byte revsion, 3 byte part number. */
			0x00000000,			/**< ptr to branch table. */
			0x00000000			/**< FPGA: non zero bitfile build date. */
#endif
			retval = target_read_u32_array(bank->target, romptr, 6, &startup_head[0]);
			if ((retval == ERROR_OK) && (startup_head[1] == branchtable_key[1]) && \
				(startup_head[2] == branchtable_key[2])) {
				*version = BOOTROM_VERSION_ECM3531;	/* startup_head[3]; */
				etacorem3_bank->branchtable_start = startup_head[4];
				break;
			}
		}
	}
	return retval;
}

/**
 * Get BootROM variant from BootRom address contents.
 * Set default, do not return errors.
 */
static uint32_t get_variant(struct flash_bank *bank)
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


	/* Determine which bootrom version that we have. */
	if (retval == ERROR_OK) {
		if ((check_program_fpga == CHECK_FLASH_PROGRAM_FPGA) && \
			(check_erase_fpga == CHECK_FLASH_ERASE_FPGA))
			retval = 1;	/* ECM3501 fpga bootrom */
		else if ((check_program_board == CHECK_FLASH_PROGRAM_ECM3501) && \
			(check_erase_board == CHECK_FLASH_ERASE_ECM3501))
			retval = 0;	/* ECM3501 chip bootrom */
		else if ((check_flash_m3eta == CHECK_FLASH_M3ETA) && \
			(check_fpga_m3eta == CHECK_FPGA_M3ETA))
			retval = 2;	/* m3eta bootrom */
		else {
			uint32_t version;
			retval = find_branch_table(bank, &version);
			if (retval == ERROR_OK)
				retval = BOOTROM_VERSION_ECM3531;
			else {
				LOG_WARNING("Unknown BootROM version. Default to ECM3501.");
				retval = BOOTROM_VERSION_ECM3501;
			}
		}
	} else {
		LOG_WARNING("BootROM entry points could not be read (%d).", retval);
		retval = BOOTROM_VERSION_ECM3501;

	}
	LOG_DEBUG("Bootrom version: %d", retval);
	return (uint32_t) retval;
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

	/* Ignore expected error messages from breakpoint. */
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

/**
 * @brief Target must be halted and probed before commands are executed.
 * @param bank - Flash bank context.
 * @returns ERROR_OK or target not ready codes.
 *
 */
static int target_ready(struct flash_bank *bank)
{
	struct etacorem3_flash_bank *etacorem3_info = bank->driver_priv;
	int retval;

	if (etacorem3_info->probed == 0) {
		LOG_ERROR("Target not probed.");
		retval = ERROR_FLASH_BANK_NOT_PROBED;
	} else if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted.");
		retval = ERROR_TARGET_NOT_HALTED;
	} else if (bank->size == 0) {
		LOG_ERROR("Target flash bank empty.");
		retval = ERROR_FLASH_SECTOR_INVALID;
	} else
		retval = ERROR_OK;

	return retval;
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

/** Target sram wrapper code for erase. */
static const uint8_t write_sector_code[] = {
#include "../../../contrib/loaders/flash/etacorem3/write.inc"
};

/** Target sram wrapper code for read. */
static const uint8_t read_sector_code[] = {
#include "../../../contrib/loaders/flash/etacorem3/read.inc"
};

/** Target sram wrapper code for load. */
static const uint8_t load_code[] = {
#include "../../../contrib/loaders/flash/etacorem3/load.inc"
};

/** Target sram wrapper code for store. */
static const uint8_t store_code[] = {
#include "../../../contrib/loaders/flash/etacorem3/store.inc"
};

/*
 * Routines to load and run target code.
 */

/** Common code for erase commands. */
static int common_erase_run(struct flash_bank *bank,
	int param_size,
	eta_erase_interface_T sramargs)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	int retval;
	struct working_area *workarea = NULL, *paramarea = NULL;
	struct working_area *stackarea = NULL;

#if 0
	/*
	 * Clear magic numbers.
	 */
	retval = clear_magic_numbers(bank);
	if (retval != ERROR_OK) {
		CHECK_STATUS(retval, "error clearing target magic numbers.");
		return retval;
	}
#endif

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
	retval = target_write_buffer(bank->target, workarea->address,
			sizeof(erase_sector_code), erase_sector_code);
	if (retval != ERROR_OK)
		goto err_alloc_code;

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

#if 0
	/* Ignore expected error messages from breakpoint. */
	int save_debug_level = debug_level;
	debug_level = LOG_LVL_OUTPUT;
#endif

	/* Run the code. */
	retval = target_run_algorithm(bank->target,
			0, NULL,
			ARRAY_SIZE(reg_params), reg_params,
			workarea->address, 0,
			etacorem3_bank->timeout_erase, &armv7m_algo);

#if 0
	/* Restore debug output level. */
	debug_level = save_debug_level;
#endif

	/* Read return code from sram parameter area. */
	uint32_t retvalT;
	int retval1 = target_read_u32(bank->target,
			(paramarea->address + offsetof(eta_erase_interface_T, retval)),
			&retvalT);
	if ((retval != ERROR_OK) || (retval1 != ERROR_OK) || (retvalT != 0)) {
		LOG_ERROR(
			"error executing flash erase %d, RC1 %d, TRC %d.",
			retval,
			retval1,
			retvalT);
		retval = ERROR_FLASH_OPERATION_FAILED;
		goto err_run;
	}

	/* error after register parameters allocated. */
err_run:
	for (unsigned i = 0; i < ARRAY_SIZE(reg_params); i++)
		destroy_reg_param(&reg_params[i]);

	/* error after buffer(s) have been allocated. */
err_alloc_code:
	if (workarea != NULL)
		target_free_working_area(bank->target, workarea);
	if (paramarea != NULL)
		target_free_working_area(bank->target, paramarea);
	if (stackarea != NULL)
		target_free_working_area(bank->target, stackarea);

	return retval;
}

/**
 * @brief Erase info space. [ECM3531]
 * @param bank
 * @returns
 *
 */
static int etacorem3_info_erase(struct flash_bank *bank)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;

	int retval = target_ready(bank);
	if (retval != ERROR_OK)
		return retval;

	/*
	 * Load SRAM Parameters.
	 */

	eta_erase_interface_T sramargs = {
		etacorem3_bank->flash_base,	/**< Start of flash. */
		0x00000000,	/**< Length 0 for all. */
		0x00000002,	/**< Option 0x2, info space erase. */
		etacorem3_bank->bootrom_erase_entry,	/**< bootrom entry point. */
		etacorem3_bank->bootrom_version,/**< ecm3501 chip or fpga, m3eta, ecm3531 */
		BREAKPOINT	/**< Return code from bootrom. */
	};

	/* Common erase execution code. */
	retval = common_erase_run(bank,
			sizeof(eta_erase_interface_T), sramargs);

	/* if successful, mark sectors as erased */
	if (retval == ERROR_OK)
		write_is_erased(bank,
			etacorem3_bank->flash_base,
			etacorem3_bank->flash_base + etacorem3_bank->flash_size,
			1);

	LOG_DEBUG("Mass erase on bank %d.", bank->bank_number);

	return retval;
}

/**
 * Mass erase entire flash bank.
 * @param bank Pointer to the flash bank descriptor.
 * @return retval
 *
 */
static int etacorem3_mass_erase(struct flash_bank *bank)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;

	int retval = target_ready(bank);
	if (retval != ERROR_OK)
		return retval;

	/*
	 * Load SRAM Parameters.
	 */

	eta_erase_interface_T sramargs = {
		etacorem3_bank->flash_base,	/**< Start of flash. */
		0x00000000,	/**< Length 0 for all. */
		0x00000001,	/**< Option 1, mass erase. */
		etacorem3_bank->bootrom_erase_entry,	/**< bootrom entry point. */
		etacorem3_bank->bootrom_version,/**< ecm3501 chip or fpga, m3eta, ecm3531 */
		BREAKPOINT	/**< Return code from bootrom. */
	};

	/* Common erase execution code. */
	retval = common_erase_run(bank,
			sizeof(eta_erase_interface_T), sramargs);

	/* if successful, mark sectors as erased */
	if (retval == ERROR_OK)
		write_is_erased(bank,
			etacorem3_bank->flash_base,
			etacorem3_bank->flash_base + etacorem3_bank->flash_size,
			1);

	LOG_DEBUG("Mass erase on bank %d.", bank->bank_number);

	return retval;
}

/**
 * Erase sectors in flash.
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
	int retval;

	retval = target_ready(bank);
	if (retval != ERROR_OK)
		return retval;

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

	eta_erase_interface_T sramargs = {
		etacorem3_bank->flash_base + (first * etacorem3_bank->pagesize),
		(last - first + 1) * etacorem3_bank->pagesize,	/**< Length in bytes. */
		0x00000000,	/**< Request page erase. */
		etacorem3_bank->bootrom_erase_entry,	/**< bootrom entry point. */
		etacorem3_bank->bootrom_version,/**< chip fpga or ecm3531*/
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
			sizeof(eta_erase_interface_T), sramargs);

	/* if successful, mark sectors as erased */
	if (retval == ERROR_OK)
		write_is_erased(bank, first, last, 1);

	return retval;
}

/**
 * Write pages to flash from buffer.
 * @param bank - flash descriptor.
 * @param buffer - data to write.
 * @param offset  - 32 bit aligned byte offset from base address.
 * @param count - 32 bit aligned Number of bytes.
 * @returns success ERROR_OK
 */
static int etacorem3_write(struct flash_bank *bank,
	const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	uint32_t address = bank->base + offset;
	uint32_t sram_buffer;
	uint32_t thisrun_count;
	struct working_area *workarea = NULL, *paramarea = NULL;
	struct working_area *bufferarea = NULL, *stackarea = NULL;
	int retval = ERROR_OK;

	/*
	* Bootrom uses 32 bit boundaries, 64 bit count.
	* Force 32 bit count here.
	*/
	if (((count%4) != 0) || ((offset%4) != 0)) {
		LOG_ERROR("write block must be multiple of 4 bytes in offset & length");
		return ERROR_FAIL;
	}

#if 0
	/*
	 * Clear Magic Numbers before write.
	 */
	retval = clear_magic_numbers(bank);
	if (retval != ERROR_OK) {
		CHECK_STATUS(retval, "error clearing target magic numbers.");
		return retval;
	}
#endif

	/*
	 * Max buffer size for this device...
	 * Chip Bootrom can only write 512 bytes at a time.
	 * Target side code will block the write into 512 bytes.
	 */
	uint32_t maxbuffer = SRAM_BUFFER_SIZE;

	sram_buffer = etacorem3_bank->target_buffer;
	if (sram_buffer == 0) {
		/*
		 * Allocate space on target for write buffer.
		 */
		retval = target_alloc_working_area(bank->target,
				maxbuffer, &bufferarea);
		LOG_DEBUG("bufferarea address: " TARGET_ADDR_FMT ", retval %d.",
			bufferarea->address,
			retval);
		if (retval != ERROR_OK) {
			LOG_ERROR("No buffer area available.");
			retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
			goto err_alloc_code;
		}
		sram_buffer = bufferarea->address;
	}

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
		goto err_alloc_code;

	/*
	 * Allocate sram parameter area.
	 */
	retval = target_alloc_working_area(bank->target,
			sizeof(eta_write_interface_T), &paramarea);
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
		retval = target_write_buffer(bank->target, sram_buffer, thisrun_count, buffer);
		if (retval != ERROR_OK) {
			LOG_ERROR("status(%d):%s\n", retval, "error writing buffer to target.");
			break;
		}

		/*
		 * Load SRAM Parameters.
		 */

		uint32_t write512_option = 0;
		if (etacorem3_bank->bootrom_version == BOOTROM_VERSION_ECM3501)
			write512_option = 1;
		/* check for info space option. */
		if (etacorem3_bank->bootrom_version == BOOTROM_VERSION_ECM3531)
			write512_option |= (etacorem3_bank->info_semaphore & 2);

		eta_write_interface_T sramargs = {
			address,	/**< Start address in flash. */
			thisrun_count,	/**< Length in bytes. */
			sram_buffer,
			write512_option,	/**< 1, write 512 byte blocks. 2, info space. */
			etacorem3_bank->bootrom_write_entry,	/**< bootrom entry point. */
			etacorem3_bank->bootrom_version,/**< chip or fpga */
			BREAKPOINT	/**< Return code from bootrom. */
		};

		retval = target_write_u32_array(bank->target,
				paramarea->address,
				(sizeof(eta_write_interface_T)/sizeof(uint32_t)),
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

#if 0
		/* Ignore expected error messages from breakpoint. */
		int save_debug_level = debug_level;
		debug_level = LOG_LVL_OUTPUT;
#endif

		/* Run the code. */
		retval = target_run_algorithm(bank->target,
				0, NULL,
				ARRAY_SIZE(reg_params), reg_params,
				workarea->address, 0,
				etacorem3_bank->timeout_program, &armv7m_algo);

#if 0
		/* Restore debug output level. */
		debug_level = save_debug_level;
#endif

		/* Read return code from sram parameter area. */
		uint32_t retvalT;
		int retval1 = target_read_u32(bank->target,
				(paramarea->address + offsetof(eta_write_interface_T, retval)),
				&retvalT);
		if ((retval != ERROR_OK) || (retval1 != ERROR_OK) || (retvalT != 0)) {
			LOG_ERROR(
				"error executing flash write %d, RC1 %d, TRC %d.",
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

	/* error after register parameters allocated. */
err_run:
	for (unsigned i = 0; i < ARRAY_SIZE(reg_params); i++)
		destroy_reg_param(&reg_params[i]);

	/* error after buffer(s) have been allocated. */
err_alloc_code:
	if (workarea != NULL)
		target_free_working_area(bank->target, workarea);
	if (paramarea != NULL)
		target_free_working_area(bank->target, paramarea);
	if (stackarea != NULL)
		target_free_working_area(bank->target, stackarea);
	if (bufferarea != NULL)
		target_free_working_area(bank->target, bufferarea);

#if 0
	if (retval == ERROR_OK) {
		/*
		 * Set Magic Numbers to run a program.
		 */
		retval = set_magic_numbers(bank);
		if (retval != ERROR_OK) {
			CHECK_STATUS(retval, "error setting target magic numbers.");
			return retval;
		}
	}
#endif

	return retval;
}
/** Write info space [ECM3531].
@param flash_bank bank - descriptor
@param uint offset - 0 for info space.
@param uint count - 32
@param uint instance.
 */
static int etacorem3_write_info(struct flash_bank *bank,
	const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;

	int retval = target_ready(bank);
	if (retval != ERROR_OK)
		return retval;

	etacorem3_bank->target_buffer = 0;
	etacorem3_bank->info_semaphore = 2;
	retval = etacorem3_write(bank, buffer, offset, count);
	etacorem3_bank->target_buffer = 0;
	etacorem3_bank->info_semaphore = 0;

	return retval;
}

/** Write info from target buffer. */
static int etacorem3_write_info_target(struct flash_bank *bank,
	uint32_t target_buffer, uint32_t offset, uint32_t count)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;

	int retval = target_ready(bank);
	if (retval != ERROR_OK)
		return retval;

	etacorem3_bank->target_buffer = target_buffer;
	etacorem3_bank->info_semaphore = 2;
	retval = etacorem3_write(bank, 0, offset, count);
	etacorem3_bank->target_buffer = 0;
	etacorem3_bank->info_semaphore = 0;
	return retval;
}

static int etacorem3_store(struct flash_bank *bank, uint32_t address, uint32_t value)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	struct working_area *workarea = NULL, *paramarea = NULL;
	struct working_area *stackarea = NULL;

	/* R0 and SP for algorithm. */
	struct reg_param reg_params[2];
	LOG_DEBUG("*** ADVAL 0x%08x 0x%08x", address, value);
	/*
	 * Allocate space on target for code.
	 */
	int retval = target_alloc_working_area(bank->target,
			sizeof(store_code), &workarea);
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
			sizeof(store_code), store_code);
	if (retval != ERROR_OK)
		goto err_alloc_code;

	/*
	 * Allocate sram parameter area.
	 */
	retval = target_alloc_working_area(bank->target,
			sizeof(eta_loadstore_interface_T), &paramarea);
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

	/*
	 * Load SRAM Parameters.
	 */
	eta_loadstore_interface_T sramargs = {
		address,/**< Start address in flash. */
		value,
		etacorem3_bank->bootrom_store_entry,	/**< bootrom entry point. */
		BREAKPOINT	/**< Return code from bootrom. */
	};

	retval = target_write_u32_array(bank->target,
			paramarea->address,
			(sizeof(eta_loadstore_interface_T)/sizeof(uint32_t)),
			(uint32_t *)&sramargs);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to load sram parameters.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}

	struct armv7m_algorithm armv7m_algo;

	armv7m_algo.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_algo.core_mode = ARM_MODE_THREAD;

	/* allocate registers sp, and r0. */
	init_reg_param(&reg_params[0], "sp", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r0", 32, PARAM_OUT);	/* address, value */

	/* Set the sram stack in sp. */
	buf_set_u32(reg_params[0].value, 0, 32,
		(stackarea->address + SRAM_STACK_SIZE));
	/* Set the sram parameter address in r0. */
	buf_set_u32(reg_params[1].value, 0, 32, paramarea->address);

	/* Run the code. */
	retval = target_run_algorithm(bank->target,
			0, NULL,
			ARRAY_SIZE(reg_params), reg_params,
			workarea->address, 0,
			etacorem3_bank->timeout_program, &armv7m_algo);

	/* Read return code from sram parameter area. */
	uint32_t retvalT;
	int retval1 = target_read_u32(bank->target,
			(paramarea->address + offsetof(eta_loadstore_interface_T, retval)),
			&retvalT);

	/* Accumulated errors retval, retval1, and retvalT (target routine). */

	if ((retval != ERROR_OK) || (retval1 != ERROR_OK) || (retvalT != 0)) {
		LOG_ERROR(
			"error executing store %d, RC1 %d, TRC %d.",
			retval,
			retval1,
			retvalT);
		LOG_DEBUG("address: 0x%08X", address);
		retval = ERROR_FLASH_OPERATION_FAILED;
		goto err_run;
	}

	/* error after register parameters allocated. */
err_run:
	for (unsigned i = 0; i < ARRAY_SIZE(reg_params); i++)
		destroy_reg_param(&reg_params[i]);

	/* error after buffer(s) have been allocated. */
err_alloc_code:
	if (workarea != NULL)
		target_free_working_area(bank->target, workarea);
	if (paramarea != NULL)
		target_free_working_area(bank->target, paramarea);
	if (stackarea != NULL)
		target_free_working_area(bank->target, stackarea);

	return retval;
}

static int etacorem3_load(struct flash_bank *bank, uint32_t address)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	struct working_area *workarea = NULL, *paramarea = NULL;
	struct working_area *stackarea = NULL;

	/* R0 and SP for algorithm. */
	struct reg_param reg_params[2];

	/*
	 * Allocate space on target for code.
	 */
	int retval = target_alloc_working_area(bank->target,
			sizeof(load_code), &workarea);
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
			sizeof(load_code), load_code);
	if (retval != ERROR_OK)
		goto err_alloc_code;

	/*
	 * Allocate sram parameter area.
	 */
	retval = target_alloc_working_area(bank->target,
			sizeof(eta_loadstore_interface_T), &paramarea);
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

	/*
	 * Load SRAM Parameters.
	 */
	eta_loadstore_interface_T sramargs = {
		address,/**< Start address in flash. */
		0,
		etacorem3_bank->bootrom_load_entry,	/**< bootrom entry point. */
		BREAKPOINT	/**< Return code from bootrom. */
	};

	retval = target_write_u32_array(bank->target,
			paramarea->address,
			(sizeof(eta_loadstore_interface_T)/sizeof(uint32_t)),
			(uint32_t *)&sramargs);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to load sram parameters.");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto err_alloc_code;
	}

	struct armv7m_algorithm armv7m_algo;

	armv7m_algo.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_algo.core_mode = ARM_MODE_THREAD;

	/* allocate registers sp, and r0. */
	init_reg_param(&reg_params[0], "sp", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r0", 32, PARAM_OUT);	/* address, value */

	/* Set the sram stack in sp. */
	buf_set_u32(reg_params[0].value, 0, 32,
		(stackarea->address + SRAM_STACK_SIZE));
	/* Set the sram parameter address in r0. */
	buf_set_u32(reg_params[1].value, 0, 32, paramarea->address);

	/* Run the code. */
	retval = target_run_algorithm(bank->target,
			0, NULL,
			ARRAY_SIZE(reg_params), reg_params,
			workarea->address, 0,
			etacorem3_bank->timeout_program, &armv7m_algo);

	/* Read return code from sram parameter area. */
	uint32_t retvalT;
	int retval1 = target_read_u32(bank->target,
			(paramarea->address + offsetof(eta_loadstore_interface_T, retval)),
			&retvalT);

	/* Accumulated errors retval, retval1, and retvalT (target routine). */

	if ((retval != ERROR_OK) || (retval1 != ERROR_OK) || (retvalT != 0)) {
		LOG_ERROR(
			"error executing load %d, RC1 %d, TRC %d.",
			retval,
			retval1,
			retvalT);
		LOG_DEBUG("address: 0x%08X", address);
		retval = ERROR_FLASH_OPERATION_FAILED;
		goto err_run;
	}

	/* Read value returned in sram_buffer. */

	uint32_t sram_buffer;

	retval = target_read_u32(bank->target,
			(paramarea->address + offsetof(eta_loadstore_interface_T, sram_buffer)),
			&sram_buffer);
	if (retval != ERROR_OK)
		goto err_run;
	/* mdw format. */
	LOG_INFO("0x%08x: %08x", address, sram_buffer);

	/* error after register parameters allocated. */
err_run:
	for (unsigned i = 0; i < ARRAY_SIZE(reg_params); i++)
		destroy_reg_param(&reg_params[i]);

	/* error after buffer(s) have been allocated. */
err_alloc_code:
	if (workarea != NULL)
		target_free_working_area(bank->target, workarea);
	if (paramarea != NULL)
		target_free_working_area(bank->target, paramarea);
	if (stackarea != NULL)
		target_free_working_area(bank->target, stackarea);

	return retval;
}

/**
 * BootROM Read from flash to SRAM buffer.
 * @param bank - flash descriptor.
 * @param buffer - data to read.
 * @param offset  - 128 bit aligned byte offset from base address.
 * @param count - 128 bit aligned Number of bytes.
 * @returns success ERROR_OK
 */
static int etacorem3_read_buffer(struct flash_bank *bank, target_addr_t address,
	uint32_t count, uint8_t *buffer)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;
	uint32_t sram_buffer;
	uint32_t thisrun_count;
	struct working_area *workarea = NULL, *paramarea = NULL;
	struct working_area *bufferarea = NULL, *stackarea = NULL;
	int retval = ERROR_OK;


	/*
	 * Bootrom uses 32 bit boundaries, 64 bit count.
	 * Force 32 bit count here.
	 */
	if ((count%32) != 0) {
		LOG_ERROR("read block must be multiple of 32 bytes in offset & length");
		return ERROR_FAIL;
	}

#if 0
	/*
	 * Clear Magic numbers required for bootrom help function execution.
	 */
	retval = clear_magic_numbers(bank);
	if (retval != ERROR_OK) {
		CHECK_STATUS(retval, "error writing magic numbers to target.");
		return retval;
	}
#endif

	sram_buffer = etacorem3_bank->target_buffer;
	if (sram_buffer == 0) {
		/*
		 * Allocate space on target for read buffer.
		 */
		retval = target_alloc_working_area(bank->target,
				ETA_FLASH_PAGE_SIZE_ECM3531, &bufferarea);
		LOG_DEBUG("bufferarea address: " TARGET_ADDR_FMT ", retval %d.",
			bufferarea->address,
			retval);
		if (retval != ERROR_OK) {
			LOG_ERROR("No buffer area available.");
			retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
			goto err_alloc_code;
		}
		sram_buffer = bufferarea->address;
	}

	/* R0 and SP for algorithm. */
	struct reg_param reg_params[2];

	/*
	 * Allocate space on target for code.
	 */
	retval = target_alloc_working_area(bank->target,
			sizeof(read_sector_code), &workarea);
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
			sizeof(read_sector_code), read_sector_code);
	if (retval != ERROR_OK)
		goto err_alloc_code;

	/*
	 * Allocate sram parameter area.
	 */
	retval = target_alloc_working_area(bank->target,
			sizeof(eta_read_interface_T), &paramarea);
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
		if (count > ETA_FLASH_PAGE_SIZE_ECM3531)
			thisrun_count = ETA_FLASH_PAGE_SIZE_ECM3531;
		else
			thisrun_count = count;

		/*
		 * Load SRAM Parameters.
		 */
		eta_read_interface_T sramargs = {
			address,/**< Start address in flash. */
			thisrun_count,	/**< Length in bytes. */
			sram_buffer,
			etacorem3_bank->info_semaphore,	/**< 2 - Info or Normal space */
			etacorem3_bank->bootrom_read_entry,	/**< bootrom entry point. */
			etacorem3_bank->bootrom_version,/**< chip or fpga */
			BREAKPOINT	/**< Return code from bootrom. */
		};

		retval = target_write_u32_array(bank->target,
				paramarea->address,
				(sizeof(eta_read_interface_T)/sizeof(uint32_t)),
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

#if 0
		/* Ignore expected error messages from breakpoint. */
		int save_debug_level = debug_level;
		debug_level = LOG_LVL_OUTPUT;
#endif

		/* Run the code. */
		retval = target_run_algorithm(bank->target,
				0, NULL,
				ARRAY_SIZE(reg_params), reg_params,
				workarea->address, 0,
				etacorem3_bank->timeout_program, &armv7m_algo);

#if 0
		/* Restore debug output level. */
		debug_level = save_debug_level;
#endif

		/* Read return code from sram parameter area. */
		uint32_t retvalT;
		int retval1 = target_read_u32(bank->target,
				(paramarea->address + offsetof(eta_read_interface_T, retval)),
				&retvalT);
		if ((retval != ERROR_OK) || (retval1 != ERROR_OK) || (retvalT != 0)) {
			LOG_ERROR(
				"error executing flash read %d, RC1 %d, TRC %d.",
				retval,
				retval1,
				retvalT);
			retval = ERROR_FLASH_OPERATION_FAILED;
			goto err_run;
		}
		/*
		 * Load target Read Buffer.
		 */
		retval = target_read_buffer(bank->target, sram_buffer, thisrun_count, buffer);
		if (retval != ERROR_OK) {
			LOG_ERROR("status(%d):%s\n", retval, "error writing buffer to target.");
			break;
		}

		buffer += thisrun_count;
		address += thisrun_count;
		count -= thisrun_count;
	}

	/* error after register parameters allocated. */
err_run:
	for (unsigned i = 0; i < ARRAY_SIZE(reg_params); i++)
		destroy_reg_param(&reg_params[i]);

	/* error after buffer(s) have been allocated. */
err_alloc_code:
	if (workarea != NULL)
		target_free_working_area(bank->target, workarea);
	if (paramarea != NULL)
		target_free_working_area(bank->target, paramarea);
	if (stackarea != NULL)
		target_free_working_area(bank->target, stackarea);
	if (bufferarea != NULL)
		target_free_working_area(bank->target, bufferarea);

	return retval;
}

static int etacorem3_read_info_buffer(struct flash_bank *bank, target_addr_t address,
	uint32_t count, uint8_t *buffer)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;

	etacorem3_bank->target_buffer = 0;
	etacorem3_bank->info_semaphore = 2;
	int retval = etacorem3_read_buffer(bank, address, count, buffer);
	etacorem3_bank->target_buffer = 0;
	etacorem3_bank->info_semaphore = 0;
	return retval;
}

/** Read info into target buffer. */
static int etacorem3_read_info_target(struct flash_bank *bank,
	uint32_t target_buffer, uint32_t offset, uint32_t count)
{
	struct etacorem3_flash_bank *etacorem3_bank = bank->driver_priv;

	int retval = target_ready(bank);
	if (retval != ERROR_OK)
		return retval;

	uint32_t address = bank->base + offset;
	etacorem3_bank->target_buffer = target_buffer;
	etacorem3_bank->info_semaphore = 2;
	retval = etacorem3_read_buffer(bank, address, count, NULL);
	etacorem3_bank->target_buffer = 0;
	etacorem3_bank->info_semaphore = 0;

	return retval;
}

/**
 * Can't protect/unprotect pages on etacorem3.
 * @param bank - descriptor.
 * @param set - on or off, always off.
 * @param first - starting page.
 * @param last - ending page.
 * @returns - OK.
 */
static int etacorem3_protect(struct flash_bank *bank, int set, int first, int last)
{
	/*
	 * Can't protect/unprotect on ECM35xx parts.
	 */
	LOG_WARNING("Cannot protect/unprotect flash.");
	return ERROR_OK;
}

/**
 * Sectors are always unprotected.
 * @param bank - descriptor
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
	etacorem3_bank->pagesize = ETA_COMMON_FLASH_PAGE_SIZE;
	etacorem3_bank->magic_address = MAGIC_ADDR_ECM35xx;
	etacorem3_bank->info_semaphore = 0;
	etacorem3_bank->target_buffer = 0;

	etacorem3_bank->bootrom_version = get_variant(bank);

	/*
	 * Load call addresses from version of bootrom found.
	 */

	/* ECM3501 */
	if (etacorem3_bank->bootrom_version == BOOTROM_VERSION_ECM3501) {
		etacorem3_bank->target_name = "ECM3501";
		etacorem3_bank->bootrom_load_entry = BOOTROM_FLASH_LOAD_ECM3501;
		etacorem3_bank->bootrom_store_entry = BOOTROM_FLASH_STORE_ECM3501;
		etacorem3_bank->bootrom_erase_entry = BOOTROM_FLASH_ERASE_ECM3501;
		etacorem3_bank->bootrom_write_entry = BOOTROM_FLASH_PROGRAM_ECM3501;
		etacorem3_bank->timeout_erase = TIMEOUT_ERASE_ECM3501;
		etacorem3_bank->timeout_program = TIMEOUT_PROGRAM_ECM3501;
		etacorem3_bank->time_per_page_erase = TIME_PER_PAGE_ERASE_ECM3501;
		/* Load for ECM3501. */
		etacorem3_bank->sram_base = ETA_SRAM_BASE_ECM3501;
		etacorem3_bank->sram_max = ETA_SRAM_MAX_ECM3501;
		etacorem3_bank->flash_base = ETA_FLASH_BASE_ECM3501;
		etacorem3_bank->flash_max = ETA_FLASH_MAX_ECM3501;

	} else if (etacorem3_bank->bootrom_version == BOOTROM_VERSION_ECM3501_FPGA) {
		etacorem3_bank->target_name = "ECM3501 FPGA";
		etacorem3_bank->bootrom_erase_entry = BOOTROM_FLASH_ERASE_FPGA;
		etacorem3_bank->bootrom_write_entry = BOOTROM_FLASH_PROGRAM_FPGA;
		etacorem3_bank->timeout_erase = TIMEOUT_ERASE_ECM3501_FPGA;
		etacorem3_bank->timeout_program = TIMEOUT_PROGRAM_ECM3501_FPGA;
		etacorem3_bank->time_per_page_erase = 0;
		/* Load for ECM3501. */
		etacorem3_bank->sram_base = ETA_SRAM_BASE_ECM3501;
		etacorem3_bank->sram_max = ETA_SRAM_MAX_ECM3501;
		etacorem3_bank->flash_base = ETA_FLASH_BASE_ECM3501;
		etacorem3_bank->flash_max = ETA_FLASH_MAX_ECM3501;

	} else if (etacorem3_bank->bootrom_version == BOOTROM_VERSION_M3ETA) {
		etacorem3_bank->target_name = "M3ETA";
		etacorem3_bank->bootrom_load_entry = 0;
		etacorem3_bank->bootrom_store_entry = 0;
		etacorem3_bank->bootrom_erase_entry = 0;
		etacorem3_bank->bootrom_write_entry = 0;
		etacorem3_bank->bootrom_read_entry = 0;
		etacorem3_bank->pagesize = 0;
		etacorem3_bank->magic_address = MAGIC_ADDR_M3ETA;
		etacorem3_bank->time_per_page_erase = 0;
		/* Load for M3ETA. */
		etacorem3_bank->sram_base = ETA_SRAM_BASE_M3ETA;
		etacorem3_bank->sram_max = ETA_SRAM_MAX_M3ETA;
		etacorem3_bank->flash_base = ETA_FLASH_BASE_M3ETA;
		etacorem3_bank->flash_max = ETA_FLASH_MAX_M3ETA;
		/*
		 * Parts with branch tables:
		 * ECM3531
		 */
	} else if (etacorem3_bank->bootrom_version == BOOTROM_VERSION_ECM3531) {
		etacorem3_bank->target_name = "ECM3531";
		if (etacorem3_bank->branchtable_start != 0) {
			target_read_u32(bank->target,
				etacorem3_bank->branchtable_start + BRANCHTABLE_FLASH_LOAD,
				&etacorem3_bank->bootrom_load_entry);
			target_read_u32(bank->target,
				etacorem3_bank->branchtable_start + BRANCHTABLE_FLASH_STORE,
				&etacorem3_bank->bootrom_store_entry);
			target_read_u32(bank->target,
				etacorem3_bank->branchtable_start + BRANCHTABLE_FLASH_ERASE,
				&etacorem3_bank->bootrom_erase_entry);
			target_read_u32(bank->target,
				etacorem3_bank->branchtable_start + BRANCHTABLE_FLASH_PROGRAM,
				&etacorem3_bank->bootrom_write_entry);
			target_read_u32(bank->target,
				etacorem3_bank->branchtable_start + BRANCHTABLE_FLASH_READ,
				&etacorem3_bank->bootrom_read_entry);
		}
		etacorem3_bank->timeout_erase = TIMEOUT_ERASE_ECM3501_FPGA;
		etacorem3_bank->timeout_program = TIMEOUT_PROGRAM_ECM3501_FPGA;
		/* Shared Chip/FPGA, so it needs slower chip value. */
		etacorem3_bank->time_per_page_erase = TIME_PER_PAGE_ERASE_ECM3501;
		/* Load for ECM3501. */
		etacorem3_bank->sram_base = ETA_SRAM_BASE_ECM3501;
		etacorem3_bank->sram_max = ETA_SRAM_MAX_ECM3501;
		etacorem3_bank->flash_base = ETA_FLASH_BASE_ECM3501;
		etacorem3_bank->flash_max = ETA_FLASH_MAX_ECM3501;
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
			(etacorem3_bank->sram_size/1024),
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
COMMAND_HANDLER(handle_etacorem3_mass_erase_command)
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
		command_print(CMD_CTX, "etacorem3 mass erase complete.");
	} else
		command_print(CMD_CTX, "etacorem3 mass erase failed.");

	return ERROR_OK;
}

/**
 * @brief Load using bootrom helper.
 * @returns OK, Command Syntax, or Failed to read.
 *
 */
COMMAND_HANDLER(handle_etacorem3_load_command)
{
	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	target_addr_t address = ETA_COMMON_FLASH_BASE;
	struct flash_bank *bank;
	struct target *target = get_current_target(CMD_CTX);
	int retval = get_flash_bank_by_addr(target, address, true, &bank);
	if (retval != ERROR_OK)
		return retval;

	uint32_t taddress;
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], taddress);

	if (etacorem3_load(bank, taddress) != ERROR_OK)
		command_print(CMD_CTX, "etacorem3 load failed.");

	return ERROR_OK;
}

/**
 * @brief Load using bootrom helper.
 * @returns OK, Command Syntax, or Failed to read.
 *
 */
COMMAND_HANDLER(handle_etacorem3_store_command)
{
	if (CMD_ARGC != 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	target_addr_t address = ETA_COMMON_FLASH_BASE;
	struct flash_bank *bank;
	struct target *target = get_current_target(CMD_CTX);
	int retval = get_flash_bank_by_addr(target, address, true, &bank);
	if (retval != ERROR_OK)
		return retval;

	uint32_t taddress, tvalue;
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], taddress);
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], tvalue);

	if (etacorem3_store(bank, taddress, tvalue) != ERROR_OK)
		command_print(CMD_CTX, "etacorem3 store failed.");

	return ERROR_OK;
}

/**
 * @brief Erase info space. [ECM3531]
 * @param handle_etacorem3_erase_info_command
 * @returns OK, Command Syntax, or Flash Bank not found.
 *
 */
COMMAND_HANDLER(handle_etacorem3_erase_info_command)
{
	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *bank;
	uint32_t retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	etacorem3_info_erase(bank);

	return ERROR_OK;
}

/**
 * @brief write info space from target buffer. [ECM3531]
 * @param etacompute handle_write_info_command
 * @returns
 *
 */
COMMAND_HANDLER(handle_etacorem3_write_info_target_command)
{
	struct flash_bank *bank;
	/* initialize to default values. */
	uint32_t target_buffer = DEFAULT_TARGET_BUFFER;
	uint32_t offset = 0, count = 0;

	if (CMD_ARGC < 1 || CMD_ARGC > 4)
		return ERROR_COMMAND_SYNTAX_ERROR;
	if (CMD_ARGC > 1)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], target_buffer);
	if (CMD_ARGC > 2)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], offset);
	if (CMD_ARGC > 3)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[3], count);

	CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);

	etacorem3_write_info_target(bank, target_buffer, offset, count);

	return ERROR_OK;
}
COMMAND_HANDLER(handle_etacorem3_write_info_image_command)
{
	uint32_t address;
	uint8_t *buffer;
	uint32_t size;
	struct fileio *fileio;

	if (CMD_ARGC < 1 || CMD_ARGC > 3)
		return ERROR_COMMAND_SYNTAX_ERROR;


	if (CMD_ARGC > 1)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], address);
	else
		address = ETA_COMMON_FLASH_BASE;

	if (CMD_ARGC > 2)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], size);
	else
		size = ETA_COMMON_FLASH_PAGE_SIZE;

	struct flash_bank *bank;
	struct target *target = get_current_target(CMD_CTX);
	int retval = get_flash_bank_by_addr(target, address, true, &bank);
	if (retval != ERROR_OK)
		return retval;

	struct duration bench;
	duration_start(&bench);

	if (fileio_open(&fileio, CMD_ARGV[0], FILEIO_READ, FILEIO_BINARY) != ERROR_OK)
		return ERROR_FAIL;

	size_t filesize;
	retval = fileio_size(fileio, &filesize);
	if (retval != ERROR_OK) {
		fileio_close(fileio);
		return retval;
	}

	uint32_t length = MIN(filesize, size);

	if (!length) {
		LOG_INFO("Nothing to write to flash bank");
		fileio_close(fileio);
		return ERROR_OK;
	}

	if (length != filesize)
		LOG_INFO("File content exceeds flash bank size. Only writing the "
			"first %u bytes of the file", length);

	target_addr_t start_addr = address;
	target_addr_t aligned_start = flash_write_align_start(bank, start_addr);
	target_addr_t end_addr = start_addr + length - 1;
	target_addr_t aligned_end = flash_write_align_end(bank, end_addr);
	uint32_t aligned_size = aligned_end + 1 - aligned_start;
	uint32_t padding_at_start = start_addr - aligned_start;
	uint32_t padding_at_end = aligned_end - end_addr;

	buffer = malloc(aligned_size);
	if (buffer == NULL) {
		fileio_close(fileio);
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}

	if (padding_at_start) {
		memset(buffer, bank->default_padded_value, padding_at_start);
		LOG_WARNING("Start offset 0x%08" PRIx32
			" breaks the required alignment of flash bank %s",
			address, bank->name);
		LOG_WARNING("Padding %" PRId32 " bytes from " TARGET_ADDR_FMT,
			padding_at_start, aligned_start);
	}

	uint8_t *ptr = buffer + padding_at_start;
	size_t buf_cnt;
	if (fileio_read(fileio, length, ptr, &buf_cnt) != ERROR_OK) {
		free(buffer);
		fileio_close(fileio);
		return ERROR_FAIL;
	}

	if (buf_cnt != length) {
		LOG_ERROR("Short read");
		free(buffer);
		return ERROR_FAIL;
	}

	ptr += length;

	if (padding_at_end) {
		memset(ptr, bank->default_padded_value, padding_at_end);
		LOG_INFO("Padding at " TARGET_ADDR_FMT " with %" PRId32
			" bytes (bank write end alignment)",
			end_addr + 1, padding_at_end);
	}

	retval = etacorem3_write_info(bank, buffer, aligned_start - bank->base, aligned_size);

	free(buffer);

	if ((ERROR_OK == retval) && (duration_measure(&bench) == ERROR_OK)) {
		command_print(CMD_CTX, "wrote %u bytes from file %s to flash bank %u"
			" at address 0x%8.8" PRIx32 " in %fs (%0.3f KiB/s)",
			length, CMD_ARGV[0], bank->bank_number, address,
			duration_elapsed(&bench), duration_kbps(&bench, length));
	}

	fileio_close(fileio);

	return retval;
}
COMMAND_HANDLER(handle_etacorem3_dump_info_image_command)
{
	struct fileio *fileio;
	uint8_t *buffer;
	target_addr_t address, size;

	if (CMD_ARGC < 1 || CMD_ARGC > 3)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (CMD_ARGC > 1)
		COMMAND_PARSE_NUMBER(u64, CMD_ARGV[1], address);
	else
		address = ETA_COMMON_FLASH_BASE;

	if (CMD_ARGC > 2)
		COMMAND_PARSE_NUMBER(u64, CMD_ARGV[2], size);
	else
		size = ETA_COMMON_FLASH_PAGE_SIZE;

	struct flash_bank *bank;
	struct target *target = get_current_target(CMD_CTX);
	int retval = get_flash_bank_by_addr(target, address, true, &bank);
	if (retval != ERROR_OK)
		return retval;

	uint32_t buf_size = \
		(size > ETA_COMMON_FLASH_PAGE_SIZE) ? ETA_COMMON_FLASH_PAGE_SIZE : size;
	buffer = malloc(buf_size);
	if (!buffer)
		return ERROR_FAIL;

	retval = fileio_open(&fileio, CMD_ARGV[0], FILEIO_WRITE, FILEIO_BINARY);
	if (retval != ERROR_OK) {
		free(buffer);
		return retval;
	}

	struct duration bench;
	duration_start(&bench);

	while (size > 0) {
		size_t size_written;
		uint32_t this_run_size = (size > buf_size) ? buf_size : size;
		retval = etacorem3_read_info_buffer(bank, address, this_run_size, buffer);
		if (retval != ERROR_OK)
			break;

		retval = fileio_write(fileio, this_run_size, buffer, &size_written);
		if (retval != ERROR_OK)
			break;

		size -= this_run_size;
		address += this_run_size;
	}

	free(buffer);

	if ((ERROR_OK == retval) && (duration_measure(&bench) == ERROR_OK)) {
		size_t filesize;
		retval = fileio_size(fileio, &filesize);
		if (retval != ERROR_OK)
			return retval;
		command_print(CMD_CTX,
			"dumped %zu bytes in %fs (%0.3f KiB/s)", filesize,
			duration_elapsed(&bench), duration_kbps(&bench, filesize));
	}

	int retvaltemp = fileio_close(fileio);
	if (retvaltemp != ERROR_OK)
		return retvaltemp;

	return retval;
}

/**
 * @brief read info space to target buffer. [ECM3531]
 * @param etacompute handle_read_info_command
 * @returns
 * @note ARGV 0 is bank number, and ARGC is 1 when set.
 */
COMMAND_HANDLER(handle_etacorem3_read_info_target_command)
{
	struct flash_bank *bank;
	/* initialize to default values. */
	uint32_t target_buffer = DEFAULT_TARGET_BUFFER;
	uint32_t offset = 0, count = 0;

	if (CMD_ARGC < 1 || CMD_ARGC > 4)
		return ERROR_COMMAND_SYNTAX_ERROR;
	if (CMD_ARGC > 1)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], target_buffer);
	if (CMD_ARGC > 2)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], offset);
	if (CMD_ARGC > 3)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[3], count);

	CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);

	etacorem3_read_info_target(bank, target_buffer, offset, count);

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
		.handler = handle_etacorem3_mass_erase_command,
		.mode = COMMAND_EXEC,
		.help = "Erase entire device",
	},
	{
		.name = "erase_info",
		.handler = handle_etacorem3_erase_info_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank>",
		.help = "Erase info space. [ECM3531]",
	},
	{
		.name = "write_info_target",
		.handler = handle_etacorem3_write_info_target_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> <target-buffer> <offset> <count>",
		.help = "Write info space from target buffer. [ECM3531]",
	},
	{
		.name = "read_info_target",
		.handler = handle_etacorem3_read_info_target_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> <target-buffer> <offset> <count>",
		.help =
			"Read info space to sram target buffer. [ECM3531]",
	},
	{
		.name = "dump_info_image",
		.handler = handle_etacorem3_dump_info_image_command,
		.mode = COMMAND_EXEC,
		.usage = "filename address size",
		.help = "Read info space to file. [ECM3531]",
	},
	{
		.name = "write_info_image",
		.handler = handle_etacorem3_write_info_image_command,
		.mode = COMMAND_EXEC,
		.usage = "filename address size",
		.help = "Write info space from file. [ECM3531]",
	},
	{
		.name = "load",
		.handler = handle_etacorem3_load_command,
		.mode = COMMAND_EXEC,
		.usage = "<address>",
		.help = "Read using bootrom helper.",
	},
	{
		.name = "store",
		.handler = handle_etacorem3_store_command,
		.mode = COMMAND_EXEC,
		.usage = "<address> <value>",
		.help = "Write using bootrom helper.",
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
	.free_driver_priv = default_flash_free_driver_priv,
};
