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
 * Flash Read for OCD.
 * This is an SRAM wrapper routine to call Bootrom helper functions.
 *
 */

#include <string.h>
#include <stdint.h>
#include "etacorem3_flash_common.h"

/** Flash helper functions for load. */
BootROM_ui32LoadHelper_T BootROM_ui32LoadHelper;

uint32_t value;
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
	eta_loadstore_interface_T *flash_interface;
	/*
	 * This can also be built into a standalone executable with startup code.
	 * The startup code calls =main(0,NULL), and sram_param_start is 0.
	 * When sram_param_start is 0, the default SRAM_PARAM_START address is used.
	 */
	if (sram_param_start == 0)
		flash_interface = (eta_loadstore_interface_T *) SRAM_PARAM_START;
	else
		flash_interface = (eta_loadstore_interface_T *) sram_param_start;
#else
/**
 * Write up to a sector to flash.
 * Standalone executable with startup code.
 * Use SRAM_PARAM_START to locate parameter block.
 */

uint32_t main(int argc, char **argv)
{
	eta_loadstore_interface_T *flash_interface = \
		(eta_loadstore_interface_T *) SRAM_PARAM_START;
#endif

	uint32_t flash_address = flash_interface->flash_address;
#if 0
	uint32_t sram_buffer = (uint32_t *) flash_interface->sram_buffer;
	/* Allow a default SRAM buffer. */
	if (sram_buffer == NULL)
		sram_buffer = (uint32_t *) SRAM_BUFFER_START;
#endif
	flash_interface->retval = 1;

	/* Set our Helper function entry point from interface. */
	if (flash_interface->bootrom_entry_point) {
		BootROM_ui32LoadHelper = \
			(BootROM_ui32LoadHelper_T) flash_interface->bootrom_entry_point;
	} else {
		flash_interface->retval = 4;
		goto parameter_error;
	}
	/* Mark failure during execution. */
	flash_interface->retval = 6;
	/* The load command returns value in R0. */
	flash_interface->sram_buffer = ETA_CSP_FLASH_LOAD(flash_address);
	/* Mark successful execution. */
	flash_interface->retval = 0;

parameter_error:
#if OCD
	asm ("    BKPT      #0");
#else
	return flash_interface->retval;
#endif
}
