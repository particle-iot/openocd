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
#include "eta_flash_common.h"
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
	uint32_t flashAddress;
	uint32_t flashLength;
	uint32_t sramBuffer;
	uint32_t BootROM_entry_point;
	uint32_t options;
	uint32_t retval;
} eta_write_interface;

#if OCD
/** Flash helper function for erase. */
BootROM_flash_program_T BootROM_flash_program;
#else
SET_MAGIC_NUMBERS
#endif

/** Write up to a sector to flash. */
int main()
{
	eta_write_interface *pFlashInterface = (eta_write_interface *) SRAM_PARAM_START;
	uint32_t flashAddress = pFlashInterface->flashAddress;
	uint32_t flashLength = pFlashInterface->flashLength;
	uint32_t flashAddressMax = flashAddress + flashLength;
	uint32_t *sramBuffer = (uint32_t *) pFlashInterface->sramBuffer;

	if (flashLength == 0) {
		pFlashInterface->retval = 0;
		goto parameter_error;
	}

	/* allow a default value. */
	if (sramBuffer == NULL)
		sramBuffer = (uint32_t *) SRAM_BUFFER_START;

	if (flashAddress < ETA_COMMON_FLASH_BASE) {
		pFlashInterface->retval = 1;
		goto parameter_error;
	}
	/* Breakpoint is -2, use different numbers. */
	if (flashAddress >= ETA_COMMON_FLASH_MAX) {
		pFlashInterface->retval = 2;
		pFlashInterface->flashAddress;
		goto parameter_error;
	}
	if (flashAddressMax > ETA_COMMON_FLASH_MAX) {
		pFlashInterface->retval = 3;
		goto parameter_error;
	}

#if OCD
	/* Set our Helper function entry point from interface. */
	if (pFlashInterface->BootROM_entry_point) {
		BootROM_flash_program = \
			(BootROM_flash_program_T) pFlashInterface->BootROM_entry_point;
	} else {
		BootROM_flash_program = \
			(BootROM_flash_program_T) BootROM_flash_program_fpga;
	}
#endif

	/*
	 * Board and FPGA BootrROMs use 64 bit counts for length.
	 */
	uint32_t Count = (flashLength >> 3);

	if (pFlashInterface->options == 1) {

		const uint32_t BlockSize = 64;	/* DWord count. */
		const uint32_t IncrementSize = 512;	/* Bytes to increment Flash Address. */
		/*
		 * Due to a bug in this version of the helper, we have to program
		 * the whole page in blocks of 512 bytes.
		 */
		uint32_t NumExtra = Count%BlockSize;
		uint32_t NumBlock = Count/BlockSize + (NumExtra ? 1 : 0);

		uint32_t TmpAdr = flashAddress;
		uint32_t *TmpSrc = sramBuffer;

		for (int I = 0; I < NumBlock; I++) {

			if ((NumExtra != 0) && \
				(I == (NumBlock - 1))) {
				ETA_CSP_FLASH_PROGRAM(TmpAdr, TmpSrc, NumExtra);
			} else
				ETA_CSP_FLASH_PROGRAM(TmpAdr, TmpSrc, BlockSize);

			TmpAdr += IncrementSize;	/* Always bytes. */
			TmpSrc += 128;	/* Address Increment. */
		}
	} else {
		ETA_CSP_FLASH_PROGRAM(flashAddress, sramBuffer, Count);
	}

	/* Can't get an RC from bootrom, guess it worked. */
	pFlashInterface->retval = 0;

parameter_error:
#if OCD
	asm ("    BKPT      #0");
#else
	return pFlashInterface->retval;
#endif
}
