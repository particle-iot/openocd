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

#ifndef _ETA_FLASH_COMMON_H
#define _ETA_FLASH_COMMON_H

#ifndef IOREG
#define IOREG(x)                                                              \
	(*((volatile uint32_t *)(x)))
#endif


#ifdef SIMULATION
/* use these constants for testing in chip simulation. */
#define BOOTROM_FLASH_TNVS_COUNT   (0x10)
#define BOOTROM_FLASH_TRE_COUNT    (0x28)
#define BOOTROM_FLASH_TNVH_COUNT   (0x300)
#define BOOTROM_FLASH_TRCV_COUNT   (0x30)
#define BOOTROM_FLASH_TERASE_COUNT (0x30)
#define BOOTROM_FLASH_TPGS_COUNT   (0x28)
#define BOOTROM_FLASH_TPROG_COUNT  (0x50)
#else
#define BOOTROM_FLASH_TNVS_COUNT   (0x10)
#define BOOTROM_FLASH_TRE_COUNT    (0x28)
#define BOOTROM_FLASH_TNVH_COUNT   (0x300)
#define BOOTROM_FLASH_TRCV_COUNT   (0x30)
#define BOOTROM_FLASH_TERASE_COUNT (0x800000)
#define BOOTROM_FLASH_TPGS_COUNT   (0x38)
#define BOOTROM_FLASH_TPROG_COUNT  (0x78)
#endif

#define ETA_CSP_FLASH_MASS_ERASE()      BootROM_flash_erase(0x01000000, 1, BOOTROM_FLASH_TNVS_COUNT, \
		BOOTROM_FLASH_TERASE_COUNT,                  \
		BOOTROM_FLASH_TNVH_COUNT,                    \
		BOOTROM_FLASH_TRCV_COUNT);

#define ETA_CSP_FLASH_PAGE_ERASE(ADDR)  BootROM_flash_erase(ADDR, 0, BOOTROM_FLASH_TNVS_COUNT,      \
		BOOTROM_FLASH_TERASE_COUNT,                  \
		BOOTROM_FLASH_TNVH_COUNT,                    \
		BOOTROM_FLASH_TRCV_COUNT);

#define ETA_CSP_FLASH_PROGRAM(ADDR, SRCPTR, COUNT)          \
	BootROM_flash_program(ADDR, SRCPTR, COUNT,  \
		BOOTROM_FLASH_TNVS_COUNT,           \
		BOOTROM_FLASH_TPGS_COUNT,           \
		BOOTROM_FLASH_TPROG_COUNT,          \
		BOOTROM_FLASH_TNVH_COUNT,           \
		BOOTROM_FLASH_TRCV_COUNT);

#define ETA_COMMON_SRAM_MAX  0x10020000
#define ETA_COMMON_SRAM_BASE 0x10000000
#define ETA_COMMON_SRAM_SIZE (ETA_COMMON_SRAM_MAX  - ETA_COMMON_SRAM_BASE)

#define ETA_COMMON_SRAM_TOP_ADDR (ETA_COMMON_SRAM_MAX - sizeof(eta_csp_common_sram_top_t))

#define ETA_COMMON_FLASH_MAX  0x01080000
#define ETA_COMMON_FLASH_BASE 0x01000000
#define ETA_COMMON_FLASH_SIZE (ETA_COMMON_FLASH_MAX  - ETA_COMMON_FLASH_BASE)
#define ETA_COMMON_FLASH_PAGE_SIZE 4096
#define ETA_COMMON_FLASH_PAGE_ADDR_BITS 12
#define ETA_COMMON_FLASH_PAGE_ADDR_MASK 0xFFFFF000

/*
 * SRAM Address for magic numbers.
 */

#define MAGIC_ADDR_M3ETA    (0x0001FFF0)
#define MAGIC_ADDR_ECM3501  (0x1001FFF0)

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

/*
 * Jump table for ecm35xx BootROM's.
 */
#define BOOTROM_LOADER_FLASH_M3ETA	(0x000004F8)
#define BOOTROM_LOADER_FPGA_M3ETA	(0x00000564)
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
#define CHECK_FLASH_PROGRAM_ECM3501     (0x00b086b5)

typedef void (*BootROM_FlashWSHelper_T)(uint32_t);
typedef uint32_t (*BootROM_ui32LoadHelper_T)(uint32_t);
typedef void (*BootROM_ui32StoreHelper_T)(uint32_t,
	uint32_t);
typedef void (*BootROM_flash_ref_cell_erase_T)(uint32_t, uint32_t, uint32_t,
	uint32_t);

typedef void (*BootROM_flash_erase_T)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*BootROM_flash_program_T)(uint32_t, uint32_t *, uint32_t, uint32_t, uint32_t,
	uint32_t, uint32_t, uint32_t);

#endif
