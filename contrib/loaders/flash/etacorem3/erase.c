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
 * Flash Erase for OCD.
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

/** SRAM parameters for erase. */
typedef struct {
	uint32_t flash_address;
	uint32_t flash_length;
	uint32_t options;	/**< 1 - mass erase. */
	uint32_t bootrom_entry_point;
	int32_t bootrom_version;	/**< 0-chip, 1-fpga, 2-m3eta. */
	uint32_t retval;
} eta_erase_interface;

/** Flash helper function for erase. */
BootROM_flash_erase_T BootROM_flash_erase;
#ifndef OCD
SET_MAGIC_NUMBERS;
#endif

int main()
{
	eta_erase_interface *flash_interface = (eta_erase_interface *) SRAM_PARAM_START;
	uint32_t flash_address = flash_interface->flash_address;
	uint32_t flash_length = flash_interface->flash_length;
	uint32_t flash_address_max = flash_address + flash_length;
	uint32_t bootrom_version = flash_interface->bootrom_version;

	if (flash_address <  ETA_COMMON_FLASH_BASE) {
		flash_interface->retval = 1;
		goto parameter_error;
	}
	/* Breakpoint is -2, use different numbers. */
	if (flash_address >= ETA_COMMON_FLASH_MAX) {
		flash_interface->retval = 2;
		goto parameter_error;
	}
	if (flash_address_max > ETA_COMMON_FLASH_MAX) {
		flash_interface->retval = 3;
		goto parameter_error;
	}

	/* Set our Helper function entry point from interface. */
	if (flash_interface->bootrom_entry_point) {
		BootROM_flash_erase = \
			(BootROM_flash_erase_T) flash_interface->bootrom_entry_point;
	} else {
		flash_interface->retval = 4;
		goto parameter_error;
	}

	/* erase all the pages we will work with. */
	if (flash_interface->options == 1) {
		flash_interface->retval = 5;
		ETA_CSP_FLASH_MASS_ERASE();
	} else {
		flash_interface->retval = 6;
		while (flash_address < flash_address_max) {
			uint32_t erase_address;
			erase_address = (flash_address & ETA_COMMON_FLASH_PAGE_ADDR_MASK);
			ETA_CSP_FLASH_PAGE_ERASE(erase_address);
			flash_address += ETA_COMMON_FLASH_PAGE_SIZE;
		}
	}
	flash_interface->retval = 0;

parameter_error:
#if OCD
	asm ("    BKPT      #0");
#else
	return flash_interface->retval;
#endif
}
