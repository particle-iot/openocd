/*
 * Copyright (C) 2017-2018 by Rick Foos
 * rfoos@solengtech.com
 *
 * Copyright (C) 2016-2018 by Eta Compute, Inc.
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

/**
 * @file
 * Flash Write for OCD.
 * This is an SRAM wrapper routine to call Bootrom helper functions.
 *
 */

#include <string.h>
#include <stdint.h>

#if OCD
#include "etacorem3_flash_common.h"
#else
#include "eta_chip.h"
#endif

/**
 * Load and entry points of wrapper function.
 * @note Depends on -work-area-phys in target file.
 */
#define SRAM_ENTRY_POINT        (0x10000000)
/** Location wrapper functions look for parameters, and top of stack. */
#define SRAM_PARAM_START        (0x10001000)
/** Target buffer start address for write operations. */
#define SRAM_BUFFER_START       (0x10002000)
/** Target buffer size. */
#define SRAM_BUFFER_SIZE        (0x00004000)

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

/** Flash helper function for erase. */
BootROM_flash_program_T BootROM_flash_program;
#ifndef OCD
SET_MAGIC_NUMBERS;
#endif

/** Write up to a sector to flash. */
int main()
{
	eta_write_interface *flash_interface = (eta_write_interface *) SRAM_PARAM_START;
	uint32_t flash_address = flash_interface->flash_address;
	uint32_t flash_length = flash_interface->flash_length;
	uint32_t flash_address_max = flash_address + flash_length;
	uint32_t *sram_buffer = (uint32_t *) flash_interface->sram_buffer;
	uint32_t bootrom_version = flash_interface->bootrom_version;

	if (flash_length == 0) {
		flash_interface->retval = 0;
		goto parameter_error;
	}

	/* allow a default value. */
	if (sram_buffer == NULL)
		sram_buffer = (uint32_t *) SRAM_BUFFER_START;

	if (flash_address < ETA_COMMON_FLASH_BASE) {
		flash_interface->retval = 1;
		goto parameter_error;
	}
	/* Breakpoint is -2, don't use negative number returns. */
	if (flash_address >= ETA_COMMON_FLASH_MAX) {
		flash_interface->retval = 2;
		flash_interface->flash_address;
		goto parameter_error;
	}
	if (flash_address_max > ETA_COMMON_FLASH_MAX) {
		flash_interface->retval = 3;
		goto parameter_error;
	}

#if 1
	/* Set our Helper function entry point from interface. */
	if (flash_interface->bootrom_entry_point) {
		BootROM_flash_program = \
			(BootROM_flash_program_T) flash_interface->bootrom_entry_point;
	} else {
		BootROM_flash_program = \
			(BootROM_flash_program_T) BOOTROM_FLASH_PROGRAM_FPGA;
	}
#endif

	/*
	 * Board and FPGA BootrROMs use 64 bit counts for length.
	 */
	uint32_t count = (flash_length >> 3);

	if (flash_interface->options == 1) {

		const uint32_t block_size = 64;	/* DWord count. */
		const uint32_t increment_size = 512;	/* Bytes to increment Flash Address. */
		/*
		 * Due to a bug in this version of the helper, we have to program
		 * the whole page in blocks of 512 bytes.
		 */
		uint32_t num_extra = count%block_size;
		uint32_t num_block = count/block_size + (num_extra ? 1 : 0);


		uint32_t tmp_adr = flash_address;
		uint32_t *tmp_src = sram_buffer;

		for (int I = 0; I < num_block; I++) {

			if ((num_extra != 0) && \
				(I == (num_block - 1))) {
				ETA_CSP_FLASH_PROGRAM(tmp_adr, tmp_src,
					((bootrom_version == 0) ? num_extra * 2 : num_extra));
			} else
				ETA_CSP_FLASH_PROGRAM(tmp_adr, tmp_src,
					((bootrom_version == 0) ? block_size * 2 : block_size));

			tmp_adr += increment_size;	/* Always bytes. */
			tmp_src += 128;	/* Address Increment. */
		}
	} else
		ETA_CSP_FLASH_PROGRAM(flash_address, sram_buffer, count);

	/* Can't get an RC from bootrom, guess it worked. */
	flash_interface->retval = 0;

parameter_error:
#if OCD
	asm ("    BKPT      #0");
#else
	return flash_interface->retval;
#endif
}
