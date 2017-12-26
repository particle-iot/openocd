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

#define BOOTROM_FLASH_TNVS_COUNT   (0x10)
#define BOOTROM_FLASH_TRE_COUNT    (0x28)
#define BOOTROM_FLASH_TNVH_COUNT   (0x300)
#define BOOTROM_FLASH_TRCV_COUNT   (0x30)
#define BOOTROM_FLASH_TERASE_COUNT (0x30)
#define BOOTROM_FLASH_TPGS_COUNT   (0x28)
#define BOOTROM_FLASH_TPROG_COUNT  (0x50)

#define ETA_CSP_FLASH_MASS_ERASE()      BootROM_flash_erase(0x01000000, 1, BOOTROM_FLASH_TNVS_COUNT, \
		BOOTROM_FLASH_TERASE_COUNT,                  \
		BOOTROM_FLASH_TNVH_COUNT,                    \
		BOOTROM_FLASH_TRCV_COUNT);

#define ETA_CSP_FLASH_PAGE_ERASE(ADDR)  BootROM_flash_erase(ADDR, 0, BOOTROM_FLASH_TNVS_COUNT,      \
		BOOTROM_FLASH_TERASE_COUNT,                  \
		BOOTROM_FLASH_TNVH_COUNT,                    \
		BOOTROM_FLASH_TRCV_COUNT);

#define ETA_CSP_FLASH_PROGRAM(ADDR, SRCPTR, COUNT)  BootROM_flash_program(ADDR, SRCPTR, COUNT,      \
		BOOTROM_FLASH_TNVS_COUNT,                    \
		BOOTROM_FLASH_TPGS_COUNT,                    \
		BOOTROM_FLASH_TPROG_COUNT,                   \
		BOOTROM_FLASH_TNVH_COUNT,                    \
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
 * Bootrom Entry points for ECM3501.
 */

#define BootROM_flash_erase_board       (0x00000385)
#define BootROM_flash_program_board     (0x000004C9)
#define BootROM_flash_erase_fpga        (0x00000249)
#define BootROM_flash_program_fpga      (0x000002CD)

/** Flash helper function for erase. */
typedef void (*BootROM_flash_erase_T)(uint32_t addr, uint32_t options,
	uint32_t Tnvs_count, uint32_t Terase_count,
	uint32_t Tnvh_count, uint32_t Trcv_count);
/** Flash helper function for write. */
typedef void (*BootROM_flash_program_T)(uint32_t addr, uint32_t *data, uint32_t num_data,
	uint32_t Tnvs_count, uint32_t Tpgs_count, uint32_t Tprog_count,
	uint32_t Tnvh_count, uint32_t T_rcv_count);

#endif
