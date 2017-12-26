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
#include "etacorem3_flash_common.h"

/** Flash helper functions for erase. */
BootROM_flash_erase_T BootROM_flash_erase;
BootROM_flash_erase_space_T BootROM_flash_erase_space;

#if OCD
/**
 * Write up to a sector to flash.
 * A non-zero value in R0 contains address of parameter block.
 *
 * The purpose of sram_param_start is to capture the parameter in R0 and
 * not the typical argc, argv of main.
 */
int main(uint32_t sram_param_start)
{
	eta_erase_interface_T *flash_interface;

	/*
	 * This can also be built into a standalone executable with startup code.
	 * The startup code calls =main(0,NULL), and sram_param_start is 0.
	 * When sram_param_start is 0, the default SRAM_PARAM_START address is used.
	 */
	if (sram_param_start == 0)
		flash_interface = (eta_erase_interface_T *) SRAM_PARAM_START;
	else
		flash_interface = (eta_erase_interface_T *) sram_param_start;
#else
/**
 * Write up to a sector to flash.
 * Standalone executable with startup code.
 * Use SRAM_PARAM_START to locate parameter block.
 */
int main(void)
{
	eta_erase_interface_T *flash_interface = \
		(eta_erase_interface_T *) SRAM_PARAM_START;
#endif

	uint32_t flash_address = flash_interface->flash_address;
	uint32_t flash_length = flash_interface->flash_length;
	uint32_t flash_address_max = flash_address + flash_length;
	uint32_t options = flash_interface->options;
	uint32_t bootrom_version = flash_interface->bootrom_version;
	/* ecm3531 same size as ecm3501 chip. */
	if (flash_address <  ETA_COMMON_FLASH_BASE) {
		flash_interface->retval = 1;
		goto parameter_error;
	}
	/* Breakpoint is -2, use different retval numbers. */
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
		BootROM_flash_erase_space = \
			(BootROM_flash_erase_space_T) flash_interface->bootrom_entry_point;
	} else {
		flash_interface->retval = 4;
		goto parameter_error;
	}

	uint32_t space_option = ((options & 2)>>1);

	/* mass_erase, info erase, or multiple page erase. */
	if ((flash_interface->options & 0x1) == 1) {
		flash_interface->retval = 5;
		if (bootrom_version == BOOTROM_VERSION_ECM3531)
			ETA_CSP_FLASH_MASS_ERASE_SPACE(space_option);
		else
			ETA_CSP_FLASH_MASS_ERASE();
	} else if ((bootrom_version == BOOTROM_VERSION_ECM3531) && \
		(space_option)) {
		flash_interface->retval = 6;
		ETA_CSP_FLASH_PAGE_ERASE_SPACE(ETA_COMMON_FLASH_BASE, space_option);
	} else {
		flash_interface->retval = 6;
		while (flash_address < flash_address_max) {
			uint32_t erase_address;
			erase_address = (flash_address & ETA_COMMON_FLASH_PAGE_ADDR_MASK);
			/* is ECM3531, is info? */
			if (bootrom_version == BOOTROM_VERSION_ECM3531)
				ETA_CSP_FLASH_PAGE_ERASE(erase_address);
			else
				ETA_CSP_FLASH_PAGE_ERASE_SPACE(erase_address, space_option);
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
