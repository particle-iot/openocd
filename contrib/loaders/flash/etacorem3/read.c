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

/** Flash helper function for read (ECM3531). */
BootROM_flash_read_T BootROM_flash_read;

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
	eta_read_interface_T *flash_interface;

	/*
	 * This can also be built into a standalone executable with startup code.
	 * The startup code calls =main(0,NULL), and sram_param_start is 0.
	 * When sram_param_start is 0, the default SRAM_PARAM_START address is used.
	 */
	if (sram_param_start == 0)
		flash_interface = (eta_read_interface_T *) SRAM_PARAM_START;
	else
		flash_interface = (eta_read_interface_T *) sram_param_start;
#else
/**
 * Write up to a sector to flash.
 * Standalone executable with startup code.
 * Use SRAM_PARAM_START to locate parameter block.
 */
int main(void)
{
	eta_read_interface_T *flash_interface = \
		(eta_read_interface_T *) SRAM_PARAM_START;
#endif

	uint32_t flash_address = flash_interface->flash_address;
	uint32_t flash_length = flash_interface->flash_length;
	uint32_t flash_address_max = flash_address + flash_length;
	uint32_t options = flash_interface->options;
	uint32_t bootrom_version = flash_interface->bootrom_version;
	uint32_t *sram_buffer = (uint32_t *) flash_interface->sram_buffer;
	/* Allow a default SRAM buffer. */
	if (sram_buffer == NULL)
		sram_buffer = (uint32_t *) SRAM_BUFFER_START;

	/* ecm3531 only. */
	if (bootrom_version != BOOTROM_VERSION_ECM3531) {
		flash_interface->retval = 11;
		goto parameter_error;
	}
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
		BootROM_flash_read = \
			(BootROM_flash_read_T) flash_interface->bootrom_entry_point;
	} else {
		flash_interface->retval = 4;
		goto parameter_error;
	}

	uint32_t space_option = ((options & 2)>>1);

	/*
	 * Read 4 32 bit word blocks from addess into buffer.
	 */
	uint32_t count = flash_length;
	/* RC=6, Fails on first call. */
	flash_interface->retval = 6;
	for (uint32_t I = 0; I < count; I += 16) {
		/*
		 * 32 bytes returned each call.
		 * count is the entire number of bytes.
		 * increment flash address by 16.
		 * increment sram_buffer by 4.
		 */
		ETA_CSP_FLASH_READ(
			flash_address + I,
			space_option,
			sram_buffer + I/4);
		/* RC=i/16. Failed on the I'th call (except 1). */
		flash_interface->retval = (I >> 4);
	}
	/* if second call fails, give user a mulligan. */
	flash_interface->retval = 0;

parameter_error:
#if OCD
	asm ("    BKPT      #0");
#else
	return flash_interface->retval;
#endif
}
