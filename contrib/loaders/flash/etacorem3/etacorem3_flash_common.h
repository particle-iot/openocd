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
 */

#ifndef _ETA_FLASH_COMMON_H
#define _ETA_FLASH_COMMON_H

#ifndef IOREG
#define IOREG(x)                                                              \
	(*((volatile uint32_t *)(x)))
#endif

/*
 * Driver Defaults.
 */

#define DEFAULT_TARGET_BUFFER   (0x10002000)
/** Location wrapper functions look for parameters, and top of stack. */
#define SRAM_PARAM_START        (0x10001000)
/** Target buffer start address for write operations. */
#define SRAM_BUFFER_START       DEFAULT_TARGET_BUFFER
/** Target buffer size. */
#define SRAM_BUFFER_SIZE        (0x00002000)
/** 4k target info buffer. */
#define SRAM_INFO_START         (0x10005000)

/*
 * Useful bootrom versions.
 */

#define BOOTROM_VERSION_ECM3501      (0)
#define BOOTROM_VERSION_ECM3501_FPGA (1)
#define BOOTROM_VERSION_M3ETA        (2)
#define BOOTROM_VERSION_ECM3531      (3)

/* Bootrom Options passed to read, write, erase. */

#define OPTION_SPACE_INFO   (0x2)
#define OPTION_MASS_ERASE   (0x1)
#define OPTION_WRITE512     (0x1)

#define NORMAL_SPACE        (0x0)
#define INFO_SPACE          (0x1)
#define MASS_ERASE          (0x1)

/* ECM3531 Flash timing arguments. */

#define ECM3531_FLASH_TNVS_COUNT   (0x6)
#define ECM3531_FLASH_TRE_COUNT    (0x186A0)
#define ECM3531_FLASH_TNVH_COUNT   (0x6)
#define ECM3531_FLASH_TNVH1_COUNT  (0x6E)
#define ECM3531_FLASH_TRCV_COUNT   (0xB)
#define ECM3531_FLASH_TERASE_COUNT (0x16000)
#define ECM3531_FLASH_TPGS_COUNT   (0xB)
#define ECM3531_FLASH_TPROG_COUNT  (0xC)
#define ECM3531_FLASH_TME_COUNT    (0x16000)

/* ECM3501 Flash timing arguments. */

#define ECM3501_FLASH_TNVS_COUNT   (0x10)
#define ECM3501_FLASH_TRE_COUNT    (0x28)
#define ECM3501_FLASH_TNVH_COUNT   (0x300)
#define ECM3501_FLASH_TNVH1_COUNT  (0x3000)
#define ECM3501_FLASH_TRCV_COUNT   (0x30)
#define ECM3501_FLASH_TERASE_COUNT (0x800000)
#define ECM3501_FLASH_TPGS_COUNT   (0x38)
#define ECM3501_FLASH_TPROG_COUNT  (0x78)

/* Common SRAM sizes. */

#define ETA_COMMON_SRAM_MAX     (0x10020000)
#define ETA_COMMON_SRAM_BASE    (0x10000000)
#define ETA_COMMON_SRAM_SIZE    \
	(ETA_COMMON_SRAM_MAX  - ETA_COMMON_SRAM_BASE)

/* Common Flash sizes. */

#define ETA_COMMON_FLASH_MAX            (0x01080000)
#define ETA_COMMON_FLASH_BASE           (0x01000000)
#define ETA_COMMON_FLASH_SIZE   \
	(ETA_COMMON_FLASH_MAX  - ETA_COMMON_FLASH_BASE)
#define ETA_COMMON_FLASH_PAGE_SIZE      (4096)
#define ETA_COMMON_FLASH_PAGE_ADDR_BITS (12)
#define ETA_COMMON_FLASH_PAGE_ADDR_MASK (0xFFFFF000)

/*
 * SRAM Start Address for magic numbers.
 * @see magic_numbers[]
 */

#define MAGIC_ADDR_M3ETA    (0x0001FFF0)
#define MAGIC_ADDR_ECM35xx  (0x1001FFF0)

/*
 * Hard coded addresses for m3eta and ecm3501 BootROM's.
 * M3ETA from firmware/m3eta/bootrom/bin/bootrom.txt
 * ECM3501 from firmware/subz/bootrom.txt
 * ECM3531 from jump table.
 * (Address Bit 0 set for thumb mode)
 */
#define BOOTROM_LOADER_FLASH_M3ETA      (0x000004F9)
#define BOOTROM_LOADER_FPGA_M3ETA       (0x00000565)
#define BOOTROM_FLASH_REF_CELL_ERASE_ECM3501    (0x00000285)
#define BOOTROM_FLASH_WS_ECM3501        (0x0000009D)
#define BOOTROM_FLASH_LOAD_ECM3501      (0x000000E5)
#define BOOTROM_FLASH_STORE_ECM3501     (0x000000FD)
#define BOOTROM_FLASH_ERASE_ECM3501     (0x00000385)
#define BOOTROM_FLASH_PROGRAM_ECM3501   (0x000004C9)
#define BOOTROM_FLASH_ERASE_FPGA        (0x00000249)
#define BOOTROM_FLASH_PROGRAM_FPGA      (0x000002CD)

/*
 * Check for BootROM version using values at hard coded locations.
 */

#define CHECK_FLASH_M3ETA               (0xb08cb580)
#define CHECK_FPGA_M3ETA                (0xb08cb580)
#define CHECK_FLASH_ERASE_FPGA          (0x00b089b4)
#define CHECK_FLASH_PROGRAM_FPGA        (0x00b089b4)
#define CHECK_FLASH_ERASE_ECM3501       (0x00b086b5)
#define CHECK_FLASH_PROGRAM_ECM3501     (0x00b086b5)

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BootROM entry point typedefs.
 * ECM3531 only: xxx_space, _read.
 */

typedef void (*BootROM_FlashWSHelper_T)(uint32_t);
typedef uint32_t (*BootROM_ui32LoadHelper_T)(uint32_t);
typedef void (*BootROM_ui32StoreHelper_T)
	(uint32_t, uint32_t);
typedef uint32_t (*BootROM_ui32VersionHelper_T)(void);
typedef void (*BootROM_flash_ref_cell_erase_T)
	(uint32_t, uint32_t, uint32_t, uint32_t);
typedef int (*BootROM_flash_erase_space_T)
	(uint32_t, uint32_t, uint32_t,
	uint32_t, uint32_t, uint32_t, uint32_t);
typedef int (*BootROM_flash_erase_T)
	(uint32_t, uint32_t, uint32_t,
	uint32_t, uint32_t, uint32_t);
typedef int (*BootROM_flash_program_space_T)
	(uint32_t, uint32_t *, uint32_t, uint32_t, uint32_t,
	uint32_t, uint32_t, uint32_t, uint32_t);
typedef int (*BootROM_flash_program_T)
	(uint32_t, uint32_t *, uint32_t, uint32_t, uint32_t,
	uint32_t, uint32_t, uint32_t);
typedef void (*BootROM_flash_read_T)
	(uint32_t, uint32_t, uint32_t *);

/** SRAM parameters for write. */
typedef struct {
	uint32_t flash_address;
	uint32_t flash_length;
	uint32_t sram_buffer;
	uint32_t options;	/**< 1 - Write 512, 2 - info space. */
	uint32_t bootrom_entry_point;
	int32_t bootrom_version;	/**< 0-chip, 1-fpga, 2-m3eta, 3-3531 fpga. */
	uint32_t retval;
} eta_write_interface_T;

/** SRAM parameters for erase. */
typedef struct {
	uint32_t flash_address;
	uint32_t flash_length;
	uint32_t options;	/**< 1 - mass erase. 2 - info space. */
	uint32_t bootrom_entry_point;
	int32_t bootrom_version;	/**< 0-chip, 1-fpga, 2-m3eta, 3-3531 fpga. */
	uint32_t retval;
} eta_erase_interface_T;

/** SRAM parameters for read. */
typedef struct {
	uint32_t flash_address;
	uint32_t flash_length;
	uint32_t sram_buffer;
	uint32_t options;	/**< 2 - info space. */
	uint32_t bootrom_entry_point;
	int32_t bootrom_version;	/**< 0-chip, 1-fpga, 2-m3eta, 3-3531 fpga. */
	uint32_t retval;
} eta_read_interface_T;

/** SRAM parameters for load and store. */
typedef struct {
	uint32_t flash_address;
	uint32_t sram_buffer;
	uint32_t bootrom_entry_point;
	uint32_t retval;
} eta_loadstore_interface_T;

/*
 * Function call wrappers.
 */

#define ETA_CSP_FLASH_LOAD(ADDR) \
	BootROM_ui32LoadHelper(ADDR);

#define ETA_CSP_FLASH_STORE(ADDR, VALUE) \
	BootROM_ui32StoreHelper(ADDR, VALUE);

/*
 * Function call wrappers include additional flash timing parameters.
 */

#define ETA_CSP_FLASH_MASS_ERASE_SPACE(SPACE)  \
	BootROM_flash_erase_space(ETA_COMMON_FLASH_BASE, MASS_ERASE, SPACE, \
		ECM3531_FLASH_TNVS_COUNT, \
		ECM3531_FLASH_TME_COUNT,                  \
		ECM3531_FLASH_TNVH1_COUNT,                    \
		ECM3531_FLASH_TRCV_COUNT);

#define ETA_CSP_FLASH_MASS_ERASE() \
	BootROM_flash_erase(ETA_COMMON_FLASH_BASE, MASS_ERASE, \
		ECM3501_FLASH_TNVS_COUNT, \
		ECM3501_FLASH_TERASE_COUNT,                  \
		ECM3501_FLASH_TNVH1_COUNT,                    \
		ECM3501_FLASH_TRCV_COUNT);

#define ETA_CSP_FLASH_PAGE_ERASE_SPACE(ADDR, SPACE)  \
	BootROM_flash_erase_space(ADDR, 0, SPACE, \
		ECM3531_FLASH_TNVS_COUNT,      \
		ECM3531_FLASH_TERASE_COUNT,                  \
		ECM3531_FLASH_TNVH_COUNT,                    \
		ECM3531_FLASH_TRCV_COUNT);

#define ETA_CSP_FLASH_PAGE_ERASE(ADDR)  \
	BootROM_flash_erase(ADDR, 0, ECM3501_FLASH_TNVS_COUNT,      \
		ECM3501_FLASH_TERASE_COUNT,                  \
		ECM3501_FLASH_TNVH_COUNT,                    \
		ECM3501_FLASH_TRCV_COUNT);

#define ETA_CSP_FLASH_PROGRAM_SPACE(ADDR, SRCPTR, COUNT, SPACE) \
	BootROM_flash_program_space(ADDR, SRCPTR, COUNT, SPACE, \
		ECM3531_FLASH_TNVS_COUNT,           \
		ECM3531_FLASH_TPGS_COUNT,           \
		ECM3531_FLASH_TPROG_COUNT,          \
		ECM3531_FLASH_TNVH_COUNT,           \
		ECM3531_FLASH_TRCV_COUNT);

#define ETA_CSP_FLASH_PROGRAM(ADDR, SRCPTR, COUNT)          \
	BootROM_flash_program(ADDR, SRCPTR, COUNT,  \
		ECM3501_FLASH_TNVS_COUNT,           \
		ECM3501_FLASH_TPGS_COUNT,           \
		ECM3501_FLASH_TPROG_COUNT,          \
		ECM3501_FLASH_TNVH_COUNT,           \
		ECM3501_FLASH_TRCV_COUNT);

#define ETA_CSP_FLASH_READ(ADDR, SPACE, RESULT)  \
	BootROM_flash_read(ADDR, SPACE, RESULT);

#ifdef __cplusplus
}
#endif

#endif
