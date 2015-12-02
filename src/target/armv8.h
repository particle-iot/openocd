/***************************************************************************
 *   Copyright (C) 2015 by David Ung                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 ***************************************************************************/

#ifndef ARMV8_H
#define ARMV8_H

#include "arm_adi_v5.h"
#include "arm.h"
#include "armv4_5_mmu.h"
#include "armv4_5_cache.h"
#include "arm_dpm.h"


/* ------------------------------ ARMv8 ------------------------------
 * DDI0487A.f (ID032515)	ARM Architecture Reference Manual
 *							ARMv8, for ARMv8-A architecture profile
 */

/* Component offset to armv8.debug_base */
/* These component base could be detected by
 * dap_romtable_lookup_cs_component() with some modification
 * i.e: lookup the component after the CORE was found
 */
#define	ARMV8_CORE_BASE_OFST	(0x00000)
#define	ARMV8_CTI_BASE_OFST		(0x10000)
#define	ARMV8_PMU_BASE_OFST		(0x20000)
#define	ARMV8_ETM_BASE_OFST		(0x30000)


#define ARMV8_REG_EDESR			(0x020)		/* External Debug Event Status Register */
#define ARMV8_REG_EDECR			(0x024)		/* External Debug Execution Control Register */
#define ARMV8_REG_EDWAR			(0x030)		/* External Debug Watchpoint Address Register */
#define ARMV8_REG_EDWAR_LO		(0x030)		/*   LO word of EDWAR: bit[31:00] */
#define ARMV8_REG_EDWAR_HI		(0x034)		/*   HI word of EDWAR: bit[63:32] */
#define ARMV8_REG_DBGDTRRX_EL0	(0x080)		/* Debug Data Transfer Register (Receive) */
#define ARMV8_REG_EDITR			(0x084)		/* External Debug Instruction Transfer Regisgter */
#define ARMV8_REG_EDSCR			(0x088)		/* External Debug Status and Control Register */
#define ARMV8_REG_DBGDTRTX_EL0	(0x08C)		/* Debug Data Transfer Register (Transmit) */
#define ARMV8_REG_EDRCR			(0x090)		/* External Debug Reserve Control Register */
#define ARMV8_REG_EDACR			(0x094)		/* External Debug Auxiliary Control Register */
#define ARMV8_REG_EDECCR		(0x098)		/* External Debug Exception Catch Control Register */
#define ARMV8_REG_EDPCSR_LO		(0x0A0)		/* External Debug Program Counter Sample Register (LO) */
#define ARMV8_REG_EDCIDSR		(0x0A4)		/* External Debug Context ID Sample Register */
#define ARMV8_REG_EDVIDSR		(0x0A8)		/* External Debug Virtual Context Sample Register */
#define ARMV8_REG_EDPCSR_HI		(0x0AC)		/* External Debug Program Counter Sample Register (HI) */
#define ARMV8_REG_OSLAR_EL1		(0x300)		/* OS Lock Access Register */
#define ARMV8_REG_EDPRCR		(0x310)		/* External Debug Power/Reset Control Register */
#define ARMV8_REG_EDPRSR		(0x314)		/* External Debug Processor Status Register */

#define ARMV8_REG_DBGBVR_EL1(n)		(0x400 + 0x10*(n))	/* Debug Breakpoint Value Register <0..n> */
#define ARMV8_REG_DBGBVR_EL1_LO(n)	(0x400 + 0x10*(n))	/*   LO word of DBGBVR<n>_EL1 */
#define ARMV8_REG_DBGBVR_EL1_HI(n)	(0x404 + 0x10*(n))	/*   HI word of DBGBVR<n>_EL1 */
#define ARMV8_REG_DBGBCR_EL1(n)		(0x408 + 0x10*(n))	/* Debug Breakpoint Control Register <0..n> */
#define ARMV8_REG_DBGBVR0_EL1	(0x400)		/* Debug Breakpoint Value Register 0 */
#define ARMV8_REG_DBGBVR0_EL1_LO	(0x400)	/*   LO word of DBGBVR0_EL1 */
#define ARMV8_REG_DBGBVR0_EL1_HI	(0x404)	/*   HI word of DBGBVR0_EL1 */
#define ARMV8_REG_DBGBCR0_EL1	(0x408)		/* Debug Breakpoint Control Register 0 */
#define ARMV8_REG_DBGBVR1_EL1	(0x410)		/* Debug Breakpoint Value Register 1 */
#define ARMV8_REG_DBGBVR1_EL1_LO	(0x410)	/*   LO word of DBGBVR1_EL1 */
#define ARMV8_REG_DBGBVR1_EL1_HI	(0x414)	/*   HI word of DBGBVR1_EL1 */
#define ARMV8_REG_DBGBCR1_EL1	(0x418)		/* Debug Breakpoint Control Register 1 */
#define ARMV8_REG_DBGBVR2_EL1	(0x420)		/* Debug Breakpoint Value Register 2 */
#define ARMV8_REG_DBGBVR2_EL1_LO	(0x420)	/*   LO word of DBGBVR2_EL1 */
#define ARMV8_REG_DBGBVR2_EL1_HI	(0x424)	/*   HI word of DBGBVR2_EL1 */
#define ARMV8_REG_DBGBCR2_EL1	(0x428)		/* Debug Breakpoint Control Register 2 */
#define ARMV8_REG_DBGBVR3_EL1	(0x430)		/* Debug Breakpoint Value Register 3 */
#define ARMV8_REG_DBGBVR3_EL1_LO	(0x430)	/*   LO word of DBGBVR3_EL1 */
#define ARMV8_REG_DBGBVR3_EL1_HI	(0x434)	/*   HI word of DBGBVR3_EL1 */
#define ARMV8_REG_DBGBCR3_EL1	(0x438)		/* Debug Breakpoint Control Register 3 */
#define ARMV8_REG_DBGBVR4_EL1	(0x440)		/* Debug Breakpoint Value Register 4 */
#define ARMV8_REG_DBGBVR4_EL1_LO	(0x440)	/*   LO word of DBGBVR4_EL1 */
#define ARMV8_REG_DBGBVR4_EL1_HI	(0x444)	/*   HI word of DBGBVR4_EL1 */
#define ARMV8_REG_DBGBCR4_EL1	(0x448)		/* Debug Breakpoint Control Register 4 */
#define ARMV8_REG_DBGBVR5_EL1	(0x450)		/* Debug Breakpoint Value Register 5 */
#define ARMV8_REG_DBGBVR5_EL1_LO	(0x450)	/*   LO word of DBGBVR5_EL1 */
#define ARMV8_REG_DBGBVR5_EL1_HI	(0x454)	/*   HI word of DBGBVR5_EL1 */
#define ARMV8_REG_DBGBCR5_EL1	(0x458)		/* Debug Breakpoint Control Register 5 */

#define ARMV8_REG_DBGWVR_EL1(n)		(0x800 + 0x10*(n))	/* Debug Watchpoint Value Register <0..n> */
#define ARMV8_REG_DBGWVR_EL1_LO(n)	(0x800 + 0x10*(n))	/*   LO word of DBGWVR<n>_EL1 */
#define ARMV8_REG_DBGWVR_EL1_HI(n)	(0x804 + 0x10*(n))	/*   HI word of DBGWVR<n>_EL1 */
#define ARMV8_REG_DBGWCR_EL1(n)		(0x808 + 0x10*(n))	/* Debug Watchpoint Control Register <0..n> */
#define ARMV8_REG_DBGWVR0_EL1	(0x800)		/* Debug Watchpoint Value Register 0 */
#define ARMV8_REG_DBGWVR0_EL1_LO	(0x800)	/*   LO word of DBGWVR0_EL1 */
#define ARMV8_REG_DBGWVR0_EL1_HI	(0x804)	/*   HI word of DBGWVR0_EL1 */
#define ARMV8_REG_DBGWCR0_EL1	(0x808)		/* Debug Watchpoint Control Register 0 */
#define ARMV8_REG_DBGWVR1_EL1	(0x810)		/* Debug Watchpoint Value Register 1 */
#define ARMV8_REG_DBGWVR1_EL1_LO	(0x810)	/*   LO word of DBGWVR1_EL1 */
#define ARMV8_REG_DBGWVR1_EL1_HI	(0x814)	/*   HI word of DBGWVR1_EL1 */
#define ARMV8_REG_DBGWCR1_EL1	(0x818)		/* Debug Watchpoint Control Register 1 */
#define ARMV8_REG_DBGWVR2_EL1	(0x820)		/* Debug Watchpoint Value Register 2 */
#define ARMV8_REG_DBGWVR2_EL1_LO	(0x820)	/*   LO word of DBGWVR2_EL1 */
#define ARMV8_REG_DBGWVR2_EL1_HI	(0x824)	/*   HI word of DBGWVR2_EL1 */
#define ARMV8_REG_DBGWCR2_EL1	(0x828)		/* Debug Watchpoint Control Register 2 */
#define ARMV8_REG_DBGWVR3_EL1	(0x830)		/* Debug Watchpoint Value Register 3 */
#define ARMV8_REG_DBGWVR3_EL1_LO	(0x830)	/*   LO word of DBGWVR3_EL1 */
#define ARMV8_REG_DBGWVR3_EL1_HI	(0x834)	/*   HI word of DBGWVR3_EL1 */
#define ARMV8_REG_DBGWCR3_EL1	(0x838)		/* Debug Watchpoint Control Register 3 */

#define ARMV8_REG_MIDR_EL1			(0xD00)		/* Main ID Register */

#define ARMV8_REG_ID_AA64PFR_EL1(n)	(0xD20 + 0x20*(n))		/* AArch64 Processor Feature Register <0..n> */
#define ARMV8_REG_ID_AA64PFR_EL1_LO(n)	(0xD20 + 0x20*(n))	/*   LO word of ID_AA64PFR<n>_EL1 */
#define ARMV8_REG_ID_AA64PFR_EL1_HI(n)	(0xD24 + 0x20*(n))	/*   LI word of ID_AA64PFR<n>_EL1 */
#define ARMV8_REG_ID_AA64DFR_EL1(n)	(0xD28 + 0x20*(n))		/* AArch64 Debug Feature Register <0..n> */
#define ARMV8_REG_ID_AA64ISAR_EL1(n)(0xD30 + 0x20*(n))		/* AArch64 Instruction Set Attribute Register <0..n> */
#define ARMV8_REG_ID_AA64ISAR_EL1_LO(n)	(0xD30 + 0x20*(n))	/*   LO word of ID_AA64ISAR<n>_EL1 */
#define ARMV8_REG_ID_AA64ISAR_EL1_HI(n)	(0xD34 + 0x20*(n))	/*   LI word of ID_AA64ISAR<n>_EL1 */
#define ARMV8_REG_ID_AA64MMFR_EL1(n)(0xD38 + 0x20*(n))		/* AArch64 Memory Model Feature Register <0..n> */
#define ARMV8_REG_ID_AA64MMFR_EL1_LO(n)	(0xD38 + 0x20*(n))	/*   LO word of ID_AA64MMFR<n>_EL1 */
#define ARMV8_REG_ID_AA64MMFR_EL1_HI(n)	(0xD3C + 0x20*(n))	/*   HI word of ID_AA64MMFR<n>_EL1 */
#define ARMV8_REG_ID_AA64PFR0_EL1	(0xD20)		/* AArch64 Processor Feature Register 0 */
#define ARMV8_REG_ID_AA64PFR0_EL1_LO	(0xD20)	/*   LO word of ID_AA64PFR0_EL1 */
#define ARMV8_REG_ID_AA64PFR0_EL1_HI	(0xD24)	/*   LI word of ID_AA64PFR0_EL1 */
#define ARMV8_REG_ID_AA64DFR0_EL1	(0xD28)		/* AArch64 Debug Feature Register 0 */
#define ARMV8_REG_ID_AA64DFR0_EL1_LO	(0xD28)	/*   LO word of ID_AA64DFR0_EL1 */
#define ARMV8_REG_ID_AA64DFR0_EL1_HI	(0xD2C)	/*   HI word of ID_AA64DFR0_EL1 */
#define ARMV8_REG_ID_AA64ISAR0_EL1	(0xD30)		/* AArch64 Instruction Set Attribute Register 0 */
#define ARMV8_REG_ID_AA64ISAR0_EL1_LO	(0xD30)	/*   LO word of ID_AA64ISAR0_EL1 */
#define ARMV8_REG_ID_AA64ISAR0_EL1_HI	(0xD34)	/*   LI word of ID_AA64ISAR0_EL1 */
#define ARMV8_REG_ID_AA64MMFR0_EL1	(0xD38)		/* AArch64 Memory Model Feature Register 0 */
#define ARMV8_REG_ID_AA64MMFR0_EL1_LO	(0xD38)	/*   LO word of ID_AA64MMFR0_EL1 */
#define ARMV8_REG_ID_AA64MMFR0_EL1_HI	(0xD3C)	/*   HI word of ID_AA64MMFR0_EL1 */
#define ARMV8_REG_ID_AA64PFR1_EL1	(0xD40)		/* AArch64 Processor Feature Register 1 */
#define ARMV8_REG_ID_AA64PFR1_EL1_LO	(0xD40)	/*   LO word of ID_AA64PFR1_EL1 */
#define ARMV8_REG_ID_AA64PFR1_EL1_HI	(0xD44)	/*   LI word of ID_AA64PFR1_EL1 */
#define ARMV8_REG_ID_AA64DFR1_EL1	(0xD48)		/* AArch64 Debug Feature Register 1 */
#define ARMV8_REG_ID_AA64DFR1_EL1_LO	(0xD48)	/*   LO word of ID_AA64DFR1_EL1 */
#define ARMV8_REG_ID_AA64DFR1_EL1_HI	(0xD4C)	/*   HI word of ID_AA64DFR1_EL1 */
#define ARMV8_REG_ID_AA64ISAR1_EL1	(0xD50)		/* AArch64 Instruction Set Attribute Register 1 */
#define ARMV8_REG_ID_AA64ISAR1_EL1_LO	(0xD50)	/*   LO word of ID_AA64ISAR1_EL1 */
#define ARMV8_REG_ID_AA64ISAR1_EL1_HI	(0xD54)	/*   LI word of ID_AA64ISAR1_EL1 */
#define ARMV8_REG_ID_AA64MMFR1_EL1	(0xD58)		/* AArch64 Memory Model Feature Register 1 */
#define ARMV8_REG_ID_AA64MMFR1_EL1_LO	(0xD58)	/*   LO word of ID_AA64MMFR1_EL1 */
#define ARMV8_REG_ID_AA64MMFR1_EL1_HI	(0xD5C)	/*   HI word of ID_AA64MMFR1_EL1 */

#define ARMV8_REG_EDITCTRL			(0xF00)		/* External Debug Integration Mode Control Register */
#define ARMV8_REG_DBGCLAIMSET_EL1	(0xFA0)		/* Debug Claim Tag Set register */
#define ARMV8_REG_DBGCLAIMCLR_EL1	(0xFA4)		/* Debug Claim Tag Clear register */
#define ARMV8_REG_EDDEVAFF0			(0xFA8)		/* Multiprocessor Affinity register ?? */
#define ARMV8_REG_EDDEVAFF1			(0xFAC)		/* External Debug Device Affinity register */
#define ARMV8_REG_EDLAR				(0xFB0)		/* External Debug Lock Access Register */
#define ARMV8_REG_EDLSR				(0xFB4)		/* External Debug Lock Status Register */
#define ARMV8_REG_DBGAUTHSTATUS_EL1	(0xFB8)		/* Debug Authentication Status register */
#define ARMV8_REG_EDDEVARCH			(0xFBC)		/* External Debug Device Architecture register */
#define ARMV8_REG_EDDEVID2			(0xFC0)		/* External Debug Device ID register 2 */
#define ARMV8_REG_EDDEVID1			(0xFC4)		/* External Debug Device ID register 1 */
#define ARMV8_REG_EDDEVID			(0xFC8)		/* External Debug Device ID register 0 */
#define ARMV8_REG_EDDEVTYPE			(0xFCC)		/* External Debug Device Type register */
#define ARMV8_REG_EDPIDR4			(0xFD0)		/* Peripheral Identification Register 4 */
#define ARMV8_REG_EDPIDR5			(0xFD4)		/* Peripheral Identification Register 5 */
#define ARMV8_REG_EDPIDR6			(0xFD8)		/* Peripheral Identification Register 6 */
#define ARMV8_REG_EDPIDR7			(0xFDC)		/* Peripheral Identification Register 7 */
#define ARMV8_REG_EDPIDR0			(0xFE0)		/* Peripheral Identification Register 0 */
#define ARMV8_REG_EDPIDR1			(0xFE4)		/* Peripheral Identification Register 1 */
#define ARMV8_REG_EDPIDR2			(0xFE8)		/* Peripheral Identification Register 2 */
#define ARMV8_REG_EDPIDR3			(0xFEC)		/* Peripheral Identification Register 3 */
#define ARMV8_REG_EDCIDR0			(0xFF0)		/* Component Identification Register 0 */
#define ARMV8_REG_EDCIDR1			(0xFF4)		/* Component Identification Register 1 */
#define ARMV8_REG_EDCIDR2			(0xFF8)		/* Component Identification Register 2 */
#define ARMV8_REG_EDCIDR3			(0xFFC)		/* Component Identification Register 3 */


/* Fields of EDESR (0x020) */
#define ARMV8_EDESR_SS				(1 <<  2)	/* Halting step debug event pending */
#define ARMV8_EDESR_RC				(1 <<  1)	/* Reset catch debug event pending */
#define ARMV8_EDESR_OSUC			(1 <<  0)	/* OS unlock debug event pending */


/* Fields of EDECR (0x024) */
#define ARMV8_EDECR_SS				(1 <<  2)	/* Halting step enable */
#define ARMV8_EDECR_RCE				(1 <<  1)	/* Reset catch enable */
#define ARMV8_EDECR_OSUCE			(1 <<  0)	/* OS unlock catch enable */


/* Fields of EDSCR (0x088) */
#define ARMV8_EDSCR_RES31			(1 << 31)	/* Reserved, RES0 */
#define	ARMV8_EDSCR_RXFULL			(1 << 30)	/* ro: DTRRX full */
#define ARMV8_EDSCR_TXFULL			(1 << 29)	/* ro: DTRTX full */
#define ARMV8_EDSCR_ITO				(1 << 28)	/* ro: EDITR overrun */
#define ARMV8_EDSCR_RXO				(1 << 27)	/* ro: DTRRX overrun */
#define ARMV8_EDSCR_TXU				(1 << 26)	/* ro: DTRTX underrun */
#define ARMV8_EDSCR_PIPEADV			(1 << 25)	/* ro: Pipeline advance */
#define ARMV8_EDSCR_ITE				(1 << 24)	/* ro: ITR empty */
#define ARMV8_EDSCR_INTDIS_SHIFT	(22)		/* Interrupt disable */
#define ARMV8_EDSCR_INTDIS_MASK		(0b11 << ARMV8_EDSCR_INTDIS_SHIFT)
#define ARMV8_EDSCR_INTDIS_NONE				(0b00)
#define ARMV8_EDSCR_INTDIS_NSEL1			(0b01)
#define ARMV8_EDSCR_INTDIS_NSEL12_EXTSEL1	(0b10)
#define ARMV8_EDSCR_INTDIS_NSEL12_EXTALL	(0b11)
#define ARMV8_EDSCR_TDA				(1 << 21)	/* Trap debug registers access */
#define ARMV8_EDSCR_MA				(1 << 20)	/* Memory access mode */
#define ARMV8_EDSCR_RES19			(1 << 19)	/* Reserved, RES0 */
#define ARMV8_EDSCR_NS				(1 << 18)	/* ro: Non-secure status */
#define ARMV8_EDSCR_RES17			(1 << 17)	/* Reserved, RES0 */
#define ARMV8_EDSCR_SDD				(1 << 16)	/* ro: Secure debug disabled */
#define ARMV8_EDSCR_RES15			(1 << 15)	/* Reserved, RES0 */
#define ARMV8_EDSCR_HDE				(1 << 14)	/* Halting debug enable */
#define ARMV8_EDSCR_RW_SHIFT		(10)		/* ro: Exception level Execution state status */
#define ARMV8_EDSCR_RW_MASK			(0b1111 << ARMV8_EDSCR_RW_SHIFT)
#define ARMV8_EDSCR_RW_AA64_ALL		(0b1111)	/* All using AArch64 */
#define ARMV8_EDSCR_RW_AA32_EL0		(0b1110)	/* 0b1110 */
#define ARMV8_EDSCR_RW_AA32_EL01	(0b1100)	/* Note: 0b110x */
#define ARMV8_EDSCR_RW_AA32_EL012	(0b1000)	/* Note: 0b10xx */
#define ARMV8_EDSCR_RW_AA32_ALL		(0b0000)	/* Note: 0b0xxx */
#define ARMV8_EDSCR_EL_SHIFT		(8)			/* ro: Exception level */
#define ARMV8_EDSCR_EL_MASK			(0b11 << ARMV8_EDSCR_EL_SHIFT)
#define ARMV8_EDSCR_A				(1 <<  7)	/* ro: System Error interrupt pending */
#define ARMV8_EDSCR_ERR				(1 <<  6)
#define ARMV8_EDSCR_STATUS_SHIFT	(0)
#define ARMV8_EDSCR_STATUS_MASK		(0b111111 << ARMV8_EDSCR_STATUS_SHIFT)
#define ARMV8_EDSCR_STATUS_NDBG		(0b000010)	/* Non-debug */
#define ARMV8_EDSCR_STATUS_RESTART	(0b000001)	/* Restarting */
#define ARMV8_EDSCR_STATUS_BP		(0b000111)	/* Halt: Breakpoint */
#define ARMV8_EDSCR_STATUS_EDBGRQ	(0b010011)	/* Halt: External debug request */
#define ARMV8_EDSCR_STATUS_STEP_NORM	(0b011011)	/* Halt: Step, normal */
#define ARMV8_EDSCR_STATUS_STEP_EXCL	(0b011111)	/* Halt: Step, exclusive */
#define ARMV8_EDSCR_STATUS_OS_UL	(0b100011)	/* Halt: OS unlock catch */
#define ARMV8_EDSCR_STATUS_RESET	(0b100111)	/* Halt: Reset catch */
#define ARMV8_EDSCR_STATUS_WP		(0b101011)	/* Halt: Watchpoint */
#define ARMV8_EDSCR_STATUS_HLT		(0b101111)	/* Halt: HLT instruction */
#define ARMV8_EDSCR_STATUS_SW_ACC	(0b110011)	/* Halt: Software access to debug register */
#define ARMV8_EDSCR_STATUS_EXCPT	(0b110111)	/* Halt: Exception catch */
#define ARMV8_EDSCR_STATUS_STEP_NOSYND	(0b111011)	/* Halt: Step, no syndrome */

#define	EDSCR_RW(edscr)	\
	(((edscr) & ARMV8_EDSCR_RW_MASK) >> ARMV8_EDSCR_RW_SHIFT)
#define	EDSCR_STATUS(edscr)	\
	(((edscr) & ARMV8_EDSCR_STATUS_MASK) >> ARMV8_EDSCR_STATUS_SHIFT)
#define	EDSCR_EL(edscr)	\
	(((edscr) & ARMV8_EDSCR_EL_MASK) >> ARMV8_EDSCR_EL_SHIFT)

/* H2.2.8 Halted() */
#define	PE_STATUS_HALTED(s) \
	(((s) != ARMV8_EDSCR_STATUS_NDBG) && ((s) != ARMV8_EDSCR_STATUS_RESTART))
/* H9.2.39 EDPRSR.HALT when (EDPRSR.PU == 0b1) */


/* Fields of EDRCR (0x090) */
#define ARMV8_EDRCR_CSE				(1 << 2)	/* Clear Sticky Error */
#define ARMV8_EDRCR_CSPA			(1 << 3)	/* Clear Sticky Pipeline Advance */
#define ARMV8_EDRCR_CBRRQ			(1 << 4)	/* Allow imprecise entry to Debug state */


/* Fields of EDPRCR (0x310) */
#define ARMV8_EDPRCR_COREPURQ		(1 << 3)	/* Core powerup request */
#define ARMV8_EDPRCR_CWRR			(1 << 1)	/* wo: Warm reset request */
#define ARMV8_EDPRCR_CORENPDRQ		(1 << 0)	/* Core no powerdown request */


/* Fields of EDPRSR (0x314) */
#define ARMV8_EDPRSR_SDR			(1 << 11)	/* Sticky debug restart */
#define ARMV8_EDPRSR_SPMAD			(1 << 10)	/* Sticky EPMAD error */
#define ARMV8_EDPRSR_EPMAD			(1 <<  9)	/* External Performance Monitors access disable status */
#define ARMV8_EDPRSR_SDAD			(1 <<  8)	/* Sticky EDAD error */
#define ARMV8_EDPRSR_EDAD			(1 <<  7)	/* External debug access disable status */
#define ARMV8_EDPRSR_DLK			(1 <<  6)	/* OS Double Lock status */
#define ARMV8_EDPRSR_OSLK			(1 <<  5)	/* OS lock status */
#define ARMV8_EDPRSR_HALTED			(1 <<  4)	/* Halted status */
#define ARMV8_EDPRSR_SR				(1 <<  3)	/* Sticky core reset status */
#define ARMV8_EDPRSR_R				(1 <<  2)	/* Core reset status */
#define ARMV8_EDPRSR_SPD			(1 <<  1)	/* Sticky core powerdown status */
#define ARMV8_EDPRSR_PU				(1 <<  0)	/* Core powerup status (access debug registers) */

/* Fields of PSTATE (D1.6.4 SPSRs) */
#define ARMV8_PSTATE_N			(1 << 31)
#define ARMV8_PSTATE_Z			(1 << 30)
#define ARMV8_PSTATE_C			(1 << 29)
#define ARMV8_PSTATE_V			(1 << 28)
#define ARMV8_PSTATE_Q			(1 << 27)	/* AArch32 only */
#define ARMV8_PSTATE_IT10_SHIFT		(1 << 25)	/* AArch32 only */
#define ARMV8_PSTATE_IT10_MASK		(0b11 << ARMV8_PSTATE_IT10_SHIFT)
#define ARMV8_PSTATE_SS			(1 << 21)	/* Software Step */
#define ARMV8_PSTATE_IL			(1 << 20)
#define ARMV8_PSTATE_GE_SHIFT		(1 << 16)	/* AArch32 only */
#define ARMV8_PSTATE_GE_MASK		(0b1111 << ARMV8_PSTATE_GE_SHIFT)
#define ARMV8_PSTATE_IT72_SHIFT		(1 << 10)	/* AArch32 only */
#define ARMV8_PSTATE_IT72_MASK		(0b111111 << ARMV8_PSTATE_IT72_SHIFT)
#define ARMV8_PSTATE_D			(1 <<  9)	/* AArch64 only */
#define ARMV8_PSTATE_E			(1 <<  9)	/* AArch32 only */
#define ARMV8_PSTATE_A			(1 <<  8)
#define ARMV8_PSTATE_I			(1 <<  7)
#define ARMV8_PSTATE_F			(1 <<  6)
#define ARMV8_PSTATE_T			(1 <<  5)	/* AArch32 only */
#define ARMV8_PSTATE_nRW		(1 <<  4)	/* MODE[4] encodes the value of PSTATE.nRW */
#define ARMV8_PSTATE_MODE_SHIFT		(0)
#define ARMV8_PSTATE_MODE_MASK		(0b11111 << ARMV8_PSTATE_MODE_SHIFT)

#define ARMV8_PSTATE_MODE(pstate) \
	(((pstate) & ARMV8_PSTATE_MODE_MASK) >> ARMV8_PSTATE_MODE_SHIFT)


/* ------------------------------------------------------------------ */
/* Breakpoint/Watchpoint related registers */

/* Fields of DBGBCRn_EL1 (H9.2.2 Debug Breakpoint Control Register) */
#define ARMV8_DBGBCR_BT_SHIFT		(20)
#define ARMV8_DBGBCR_BT_MASK		(0b1111 << ARMV8_DBGBCR_BT_SHIFT)
#define ARMV8_DBGBCR_BT_UNLINK_INSTADDR		(0b0000)
#define ARMV8_DBGBCR_BT_LINK_INSTADDR		(0b0001)
#define ARMV8_DBGBCR_BT_UNLINK_CTXID		(0b0010)
#define ARMV8_DBGBCR_BT_LINK_CTXID			(0b0011)
#define ARMV8_DBGBCR_BT_UNLINK_nINSTADDR	(0b0100)
#define ARMV8_DBGBCR_BT_LINK_nINSTADDR		(0b0101)
#define ARMV8_DBGBCR_BT_UNLINK_VMID			(0b1000)
#define ARMV8_DBGBCR_BT_LINK_VMID			(0b1001)
#define ARMV8_DBGBCR_BT_UNLINK_VMID_CTXID	(0b1010)
#define ARMV8_DBGBCR_BT_LINK_VMID_CTXID		(0b1011)
#define ARMV8_DBGBCR_LBN_SHIFT		(16)
#define ARMV8_DBGBCR_LBN_MASK		(0b1111 << ARMV8_DBGBCR_LBN_SHIFT)
#define ARMV8_DBGBCR_SSC_SHIFT		(14)
#define ARMV8_DBGBCR_SSC_MASK		(0b11 << ARMV8_DBGBCR_SSC_SHIFT)
#define ARMV8_DBGBCR_HMC			(1 << 13)
#define ARMV8_DBGBCR_BAS_SHIFT		(5)
#define ARMV8_DBGBCR_BAS_MASK		(0b1111 << ARMV8_DBGBCR_BAS_SHIFT)
#define ARMV8_DBGBCR_PMC_SHIFT		(1)
#define ARMV8_DBGBCR_PMC_MASK		(0b11 << ARMV8_DBGBCR_PMC_SHIFT)
#define ARMV8_DBGBCR_E				(1 <<  0)


/* ------------------------------------------------------------------ */
/* Cache related registers */

/* Fields of CCSIDR_EL1 (D7.2.14 Current Cache Size ID Register) */
#define ARMV8_CCSIDR_WT			(1 << 31)	/* Write-through */
#define ARMV8_CCSIDR_WB			(1 << 30)	/* Write-back */
#define ARMV8_CCSIDR_RA			(1 << 29)	/* Read-allocation */
#define ARMV8_CCSIDR_WA			(1 << 28)	/* Write-allocation */
#define ARMV8_CCSIDR_NUMSETS_SHIFT	(13)
#define ARMV8_CCSIDR_NUMSETS_MASK	(0x7FFF << ARMV8_CCSIDR_NUMSETS_SHIFT)
#define ARMV8_CCSIDR_ASSOCIATIVITY_SHIFT	(3)
#define ARMV8_CCSIDR_ASSOCIATIVITY_MASK	(0x3FF << ARMV8_CCSIDR_ASSOCIATIVITY_SHIFT)
#define ARMV8_CCSIDR_LINESIZE_SHIFT	(0)
#define ARMV8_CCSIDR_LINESIZE_MASK	(0b111 << ARMV8_CCSIDR_LINESIZE_SHIFT)

#define ARMV8_CCSIDR_NUMSETS(cssidr) \
	(((cssidr) & ARMV8_CCSIDR_NUMSETS_MASK) >> ARMV8_CCSIDR_NUMSETS_SHIFT)
#define ARMV8_CCSIDR_ASSOCIATIVITY(cssidr) \
	(((cssidr) & ARMV8_CCSIDR_ASSOCIATIVITY_MASK) >> ARMV8_CCSIDR_ASSOCIATIVITY_SHIFT)
#define ARMV8_CCSIDR_LINESIZE(cssidr) \
	(((cssidr) & ARMV8_CCSIDR_LINESIZE_MASK) >> ARMV8_CCSIDR_LINESIZE_SHIFT)

/* WAY: alias of ASSOCIATIVITY */
#define ARMV8_CCSIDR_NUMWAYS_SHIFT	ARMV8_CCSIDR_ASSOCIATIVITY_SHIFT
#define ARMV8_CCSIDR_NUMWAYS_MASK	ARMV8_CCSIDR_ASSOCIATIVITY_MASK
#define ARMV8_CCSIDR_NUMWAYS		ARMV8_CCSIDR_ASSOCIATIVITY


/* Fields of CLIDR_EL1 (D7.2.15 Cache Level ID Register) */
#define ARMV8_CLIDR_ICB_SHIFT		(30)
#define ARMV8_CLIDR_ICB_MASK		(0b111 << ARMV8_CLIDR_ICB_SHIFT)
#define ARMV8_CLIDR_LOUU_SHIFT		(27)
#define ARMV8_CLIDR_LOUU_MASK		(0b111 << ARMV8_CLIDR_LOUU_SHIFT)
#define ARMV8_CLIDR_LOC_SHIFT		(24)
#define ARMV8_CLIDR_LOC_MASK		(0b111 << ARMV8_CLIDR_LOC_SHIFT)
#define ARMV8_CLIDR_LOUIS_SHIFT		(21)
#define ARMV8_CLIDR_LOUIS_MASK		(0b111 << ARMV8_CLIDR_LOUIS_SHIFT)
#define ARMV8_CLIDR_CTYPE_SHIFT(n)	(((n)-1)*3)
#define ARMV8_CLIDR_CTYPE_MASK(n)	( 0b111 << (((n)-1)*3) )
#define ARMV8_CLIDR_CTYPE7_SHIFT	(18)
#define ARMV8_CLIDR_CTYPE7_MASK		(0b111 << ARMV8_CLIDR_CTYPE7_SHIFT)
#define ARMV8_CLIDR_CTYPE6_SHIFT	(15)
#define ARMV8_CLIDR_CTYPE6_MASK		(0b111 << ARMV8_CLIDR_CTYPE6_SHIFT)
#define ARMV8_CLIDR_CTYPE5_SHIFT	(12)
#define ARMV8_CLIDR_CTYPE5_MASK		(0b111 << ARMV8_CLIDR_CTYPE5_SHIFT)
#define ARMV8_CLIDR_CTYPE4_SHIFT	(9)
#define ARMV8_CLIDR_CTYPE4_MASK		(0b111 << ARMV8_CLIDR_CTYPE4_SHIFT)
#define ARMV8_CLIDR_CTYPE3_SHIFT	(6)
#define ARMV8_CLIDR_CTYPE3_MASK		(0b111 << ARMV8_CLIDR_CTYPE3_SHIFT)
#define ARMV8_CLIDR_CTYPE2_SHIFT	(3)
#define ARMV8_CLIDR_CTYPE2_MASK		(0b111 << ARMV8_CLIDR_CTYPE2_SHIFT)
#define ARMV8_CLIDR_CTYPE1_SHIFT	(0)
#define ARMV8_CLIDR_CTYPE1_MASK		(0b111 << ARMV8_CLIDR_CTYPE1_SHIFT)
#define ARMV8_CLIDR_ICB(clidr) \
	(((clidr) & ARMV8_CLIDR_ICB_MASK) >> ARMV8_CLIDR_ICB_SHIFT)
#define ARMV8_CLIDR_LOUU(clidr) \
	(((clidr) & ARMV8_CLIDR_LOUU_MASK) >> ARMV8_CLIDR_LOUU_SHIFT)
#define ARMV8_CLIDR_LOC(clidr) \
	(((clidr) & ARMV8_CLIDR_LOC_MASK) >> ARMV8_CLIDR_LOC_SHIFT)
#define ARMV8_CLIDR_LOUIS(clidr) \
	(((clidr) & ARMV8_CLIDR_LOUIS_MASK) >> ARMV8_CLIDR_LOUIS_SHIFT)
#define ARMV8_CLIDR_CTYPE(n, clidr) \
	(((clidr) & ARMV8_CLIDR_CTYPE_MASK(n)) >> ARMV8_CLIDR_CTYPE_SHIFT(n))
#define ARMV8_CLIDR_CTYPE7(clidr) \
	(((clidr) & ARMV8_CLIDR_CTYPE7_MASK) >> ARMV8_CLIDR_CTYPE7_SHIFT)
#define ARMV8_CLIDR_CTYPE6(clidr) \
	(((clidr) & ARMV8_CLIDR_CTYPE6_MASK) >> ARMV8_CLIDR_CTYPE6_SHIFT)
#define ARMV8_CLIDR_CTYPE5(clidr) \
	(((clidr) & ARMV8_CLIDR_CTYPE5_MASK) >> ARMV8_CLIDR_CTYPE5_SHIFT)
#define ARMV8_CLIDR_CTYPE4(clidr) \
	(((clidr) & ARMV8_CLIDR_CTYPE4_MASK) >> ARMV8_CLIDR_CTYPE4_SHIFT)
#define ARMV8_CLIDR_CTYPE3(clidr) \
	(((clidr) & ARMV8_CLIDR_CTYPE3_MASK) >> ARMV8_CLIDR_CTYPE3_SHIFT)
#define ARMV8_CLIDR_CTYPE2(clidr) \
	(((clidr) & ARMV8_CLIDR_CTYPE2_MASK) >> ARMV8_CLIDR_CTYPE2_SHIFT)
#define ARMV8_CLIDR_CTYPE1(clidr) \
	(((clidr) & ARMV8_CLIDR_CTYPE1_MASK) >> ARMV8_CLIDR_CTYPE1_SHIFT)



/* ------------------------------------------------------------------ */
/* CTI: Cross Trigger Interface */

#define ARMV8_REG_CTI_CONTROL		(0x000)		/* CTI Control register */
#define ARMV8_REG_CTI_INTACK		(0x010)		/* CTI Output Trigger Acknowledge register */
#define ARMV8_REG_CTI_APPSET		(0x014)		/* CTI Application Trigger Set register */
#define ARMV8_REG_CTI_APPCLEAR		(0x018)		/* CTI Application Trigger Clear register */
#define ARMV8_REG_CTI_APPPULSE		(0x01C)		/* CTI Application Pulse register */
#define ARMV8_REG_CTI_INEN(n)		(0x020 + 0x4*(n))
#define ARMV8_REG_CTI_INEN0		(0x020)		/* CTI Input Trigger to */
#define ARMV8_REG_CTI_INEN1		(0x024)		/*     Output Channel */
#define ARMV8_REG_CTI_INEN2		(0x028)		/*     Enable registers */
#define ARMV8_REG_CTI_INEN3		(0x02C)
#define ARMV8_REG_CTI_INEN4		(0x030)
#define ARMV8_REG_CTI_INEN5		(0x034)
#define ARMV8_REG_CTI_INEN6		(0x038)
#define ARMV8_REG_CTI_INEN7		(0x03C)
#define ARMV8_REG_CTI_OUTEN(n)		(0x0A0 + 0x4*(n))
#define ARMV8_REG_CTI_OUTEN0		(0x0A0)		/* CTI Input Channel to */
#define ARMV8_REG_CTI_OUTEN1		(0x0A4)		/*     Output Trigger */
#define ARMV8_REG_CTI_OUTEN2		(0x0A8)		/*     Enable registers */
#define ARMV8_REG_CTI_OUTEN3		(0x0AC)
#define ARMV8_REG_CTI_OUTEN4		(0x0B0)
#define ARMV8_REG_CTI_OUTEN5		(0x0B4)
#define ARMV8_REG_CTI_OUTEN6		(0x0B8)
#define ARMV8_REG_CTI_OUTEN7		(0x0BC)

#define ARMV8_REG_CTI_TRIGINSTATUS	(0x130)		/* CTI Trigger In Status register */
#define ARMV8_REG_CTI_TRIGOUTSTATUS	(0x134)		/* CTI Trigger Out Status register */
#define ARMV8_REG_CTI_CHINSTATUS	(0x138)		/* CTI Channel In Status register */
#define ARMV8_REG_CTI_CHOUTSTATUS	(0x13C)		/* CTI Channel Out Status register */
#define ARMV8_REG_CTI_GATE			(0x140)		/* CTI Channel Gate Enable register */
#define ARMV8_REG_CTI_ASICCTL		(0x144)		/* CTI External Multiplexer Control register */

/* H5.4 CTI trigger events */
#if 0
#define ARMV8_CTI_OUT_DEBUG		(0b1 << 0)	/* CTI to PE: Debug request trigger event */
#define ARMV8_CTI_OUT_RESTART		(0b1 << 1)	/* CTI to PE: Restart request trigger event */
#define ARMV8_CTI_OUT_IRQ		(0b1 << 2)	/* Generic CTI interrupt trigger event */
#define ARMV8_CTI_OUT_TRACE_EXT(n)	(0b1 << ((n)+4))	/* 4..7 */
#define ARMV8_CTI_OUT_TRACE_EXT0	(0b1 << 4)	/* Optional: */
#define ARMV8_CTI_OUT_TRACE_EXT1	(0b1 << 5)	/* Generic trace external input trigger event */
#define ARMV8_CTI_OUT_TRACE_EXT2	(0b1 << 6)
#define ARMV8_CTI_OUT_TRACE_EXT3	(0b1 << 7)

#define ARMV8_CTI_IN_CROSS_HALT		(0b1 << 0)	/* Cross-halt trigger event */
#define ARMV8_CTI_IN_PMO		(0b1 << 1)	/* Performance Monitors overflow trigger event */
#define ARMV8_CTI_IN_TRACE_EXT(n)	(0b1 << ((n)+4)		/* 4..7 */
#define ARMV8_CTI_IN_TRACE_EXT0		(0b1 << 4)	/* Optional: */
#define ARMV8_CTI_IN_TRACE_EXT1		(0b1 << 5)	/* Generic trace external output trigger event */
#define ARMV8_CTI_IN_TRACE_EXT2		(0b1 << 6)
#define ARMV8_CTI_IN_TRACE_EXT3		(0b1 << 7)

#else
#define ARMV8_CTI_OUT_DEBUG		(0)	/* CTI to PE: Debug request trigger event */
#define ARMV8_CTI_OUT_RESTART		(1)	/* CTI to PE: Restart request trigger event */
#define ARMV8_CTI_OUT_IRQ		(2)	/* Generic CTI interrupt trigger event */
#define ARMV8_CTI_OUT_TRACE_EXT(n)	(((n)+4))	/* 4..7 */
#define ARMV8_CTI_OUT_TRACE_EXT0	(4)	/* Optional: */
#define ARMV8_CTI_OUT_TRACE_EXT1	(5)	/* Generic trace external input trigger event */
#define ARMV8_CTI_OUT_TRACE_EXT2	(6)
#define ARMV8_CTI_OUT_TRACE_EXT3	(7)

#define ARMV8_CTI_IN_CROSS_HALT		(0)	/* Cross-halt trigger event */
#define ARMV8_CTI_IN_PMO		(1)	/* Performance Monitors overflow trigger event */
#define ARMV8_CTI_IN_TRACE_EXT(n)	(((n)+4)		/* 4..7 */
#define ARMV8_CTI_IN_TRACE_EXT0		(4)	/* Optional: */
#define ARMV8_CTI_IN_TRACE_EXT1		(5)	/* Generic trace external output trigger event */
#define ARMV8_CTI_IN_TRACE_EXT2		(6)
#define ARMV8_CTI_IN_TRACE_EXT3		(7)
#endif


/* H9.3.18 CTIDEVID */
#define ARMV8_CTIDEVID_INOUT_SHIFT	(24)
#define ARMV8_CTIDEVID_INOUT_MASK	(0b11 << ARMV8_CTIDEVID_INOUT_SHIFT)
#define ARMV8_CTIDEVID_INOUT_NOGATE	(0b00)
#define ARMV8_CTIDEVID_INOUT_GATED	(0b01)
#define ARMV8_CTIDEVID_NUMCHAN_SHIFT	(16)
#define ARMV8_CTIDEVID_NUMCHAN_MASK	(0b111111 << ARMV8_CTIDEVID_NUMCHAN_SHIFT)
#define ARMV8_CTIDEVID_NUMTRIG_SHIFT	(8)
#define ARMV8_CTIDEVID_NUMTRIG_MASK	(0b111111 << ARMV8_CTIDEVID_NUMTRIG_SHIFT)
#define ARMV8_CTIDEVID_EXTMUXNUM_SHIFT	(0)
#define ARMV8_CTIDEVID_EXTMUXNUM_MASK	(0b11111 << ARMV8_CTIDEVID_EXTMUXNUM_SHIFT)

#define	ARMV8_CTIDEVID_NUMCHAN(ctidevid)	\
	(((ctidevid) & ARMV8_CTIDEVID_NUMCHAN_MASK) >> ARMV8_CTIDEVID_NUMCHAN_SHIFT)
#define	ARMV8_CTIDEVID_NUMTRIG(ctidevid)	\
	(((ctidevid) & ARMV8_CTIDEVID_NUMTRIG_MASK) >> ARMV8_CTIDEVID_NUMTRIG_SHIFT)

/* ARM CoreSight component has 0xF00..0xFFF implemented */


#if 0
enum {
	ARMV8_R0,
	ARMV8_R1,
	ARMV8_R2,
	ARMV8_R3,
	ARMV8_R4,
	ARMV8_R5,
	ARMV8_R6,
	ARMV8_R7,
	ARMV8_R8,
	ARMV8_R9,
	ARMV8_R10,
	ARMV8_R11,
	ARMV8_R12,
	ARMV8_R13,
	ARMV8_R14,
	ARMV8_R15,
	ARMV8_R16,
	ARMV8_R17,
	ARMV8_R18,
	ARMV8_R19,
	ARMV8_R20,
	ARMV8_R21,
	ARMV8_R22,
	ARMV8_R23,
	ARMV8_R24,
	ARMV8_R25,
	ARMV8_R26,
	ARMV8_R27,
	ARMV8_R28,
	ARMV8_R29,
	ARMV8_R30,
	ARMV8_R31,

	ARMV8_PC = 32,
	ARMV8_xPSR = 33,

	ARMV8_LAST_REG,
};
#endif

enum {
	/* AArch32: 13 32-bit general purpose registers, and a 32-bit PC, SP,
	   and link register (LR). Some of these registers have multiple banked
	   instances for use in different processor modes */
	AARCH32_R0 = 0,
	AARCH32_R1,
	AARCH32_R2,
	AARCH32_R3,
	AARCH32_R4,
	AARCH32_R5,
	AARCH32_R6,
	AARCH32_R7,
	AARCH32_R8,
	AARCH32_R9,
	AARCH32_R10,
	AARCH32_R11,
	AARCH32_R12,

	AARCH32_SP,
	AARCH32_PC,
	AARCH32_LR,

	/* A1.3.1 AArch64 (DDI0487A)
	   31 64-bit general purpose registers, with a 64-bit Program
	   Counter (PC), Stack Pointer (SPs), and Exception Link Registers (ELRs)
	 */
	AARCH64_X0 = 0,
	AARCH64_X1,
	AARCH64_X2,
	AARCH64_X3,
	AARCH64_X4,
	AARCH64_X5,
	AARCH64_X6,
	AARCH64_X7,
	AARCH64_X8,
	AARCH64_X9,
	AARCH64_X10,
	AARCH64_X11,
	AARCH64_X12,
	AARCH64_X13,
	AARCH64_X14,
	AARCH64_X15,
	AARCH64_X16,
	AARCH64_X17,
	AARCH64_X18,
	AARCH64_X19,
	AARCH64_X20,
	AARCH64_X21,
	AARCH64_X22,
	AARCH64_X23,
	AARCH64_X24,
	AARCH64_X25,
	AARCH64_X26,
	AARCH64_X27,
	AARCH64_X28,
	AARCH64_X29,
	AARCH64_X30,	/* PLR (Procedure Link Register) */

	/* SP and PC are not general purpose registers */
	/*
				EL0			EL1			EL2			EL3
		---------------------------------------------------
		SP		SP_EL0		  SP_EL1	  SP_EL2	  SP_EL3
		ELR					 ELR_EL1	 ELR_EL2	 ELR_EL3	(PC)
		SPSR				SPSR_EL1	SPSR_EL2	SPSR_EL3	(PSTATE / CPSR)

		SP   = Stack Pointer
		ELR  = Exception Link Register
		SPSR = Saved/Current Process Status Register
	*/
	AARCH64_SP,
	AARCH64_PC,
	AARCH64_PSTATE,

	/* FPU registers */
	AARCH64_V0 = 34,
	AARCH64_V1,
	AARCH64_V2,
	AARCH64_V3,
	AARCH64_V4,
	AARCH64_V5,
	AARCH64_V6,
	AARCH64_V7,
	AARCH64_V8,
	AARCH64_V9,
	AARCH64_V10,
	AARCH64_V11,
	AARCH64_V12,
	AARCH64_V13,
	AARCH64_V14,
	AARCH64_V15,
	AARCH64_V16,
	AARCH64_V17,
	AARCH64_V18,
	AARCH64_V19,
	AARCH64_V20,
	AARCH64_V21,
	AARCH64_V22,
	AARCH64_V23,
	AARCH64_V24,
	AARCH64_V25,
	AARCH64_V26,
	AARCH64_V27,
	AARCH64_V28,
	AARCH64_V29,
	AARCH64_V30,
	AARCH64_V31,

	/* Floating-point Status/Control Registers */
	AARCH64_FPCR,	/* 66 */	/* P272 */
	AARCH64_FPSR,	/* 67 */	/* P276 */
};


#define ARMV8_COMMON_MAGIC 0x0A450AAA

/* VA to PA translation operations opc2 values*/
#define V2PCWPR  0
#define V2PCWPW  1
#define V2PCWUR  2
#define V2PCWUW  3
#define V2POWPR  4
#define V2POWPW  5
#define V2POWUR  6
#define V2POWUW  7
/*   L210/L220 cache controller support */
struct armv8_l2x_cache {
	uint32_t base;
	uint32_t way;
};

struct armv8_cachesize {
	uint32_t level_num;
	/*  cache dimensionning */
	uint32_t linelen;
	uint32_t associativity;
	uint32_t nsets;
	uint32_t cachesize;
	/* info for set way operation on cache */
	uint32_t index;
	uint32_t index_shift;
	uint32_t way;
	uint32_t way_shift;
};

struct armv8_common;

struct armv8_cache_common {
	/* *_identify_cache() set 'identified' to true after cache is identified
	 * and varaibles, in this structure are set (was 'ctype' before) */
	bool identified;			/* Has cache type been identified ? */

	uint64_t clidr;		/* Be used to flush D-Cache later */
	struct armv8_cachesize d_u_size;	/* data cache */
	struct armv8_cachesize i_size;		/* instruction cache */
	int i_cache_enabled;
	int d_u_cache_enabled;
	/* l2 external unified cache if some */
//	void *l2_cache;
//	int (*flush_all_data_cache)(struct target *target);
	int (*display_cache_info)(struct command_context *cmd_ctx,
			struct armv8_cache_common *armv8_cache);
	int (*flush_dcache_all)(struct target *target);
//	int (*flush_cache_all)(struct armv8_cache_common *armv8_cache);
};

struct armv8_mmu_common {
	/* following field mmu working way */
	int32_t ttbr1_used; /*  -1 not initialized, 0 no ttbr1 1 ttbr1 used and  */
	uint32_t ttbr0_mask;/*  masked to be used  */
	uint32_t os_border;

	int (*read_physical_memory)(struct target *target, uint64_t address,
			uint32_t size, uint32_t count, uint8_t *buffer);
	struct armv8_cache_common armv8_cache;
	uint32_t mmu_enabled;
};

struct armv8_common {
	struct arm arm;
	int common_magic;
	struct reg_cache *core_cache;

	struct adiv5_dap dap;

	/* Core Debug Unit */
	struct arm_dpm dpm;
	uint32_t debug_base;
	uint8_t debug_ap;
	uint8_t memory_ap;
	bool memory_ap_available;
	/* mdir */
	uint8_t multi_processor_system;
	uint8_t cluster_id;
	uint8_t cpu_id;

	/* cache specific to V7 Memory Management Unit compatible with v4_5*/
	struct armv8_mmu_common armv8_mmu;

	/* Direct processor core register read and writes */
	/* Alamy(2015-1009): I found that there is no use of these functions in ARMv8 */
	int (*load_core_reg_u64)(struct target *target, uint32_t num, uint64_t *value);
	int (*store_core_reg_u64)(struct target *target, uint32_t num, uint64_t value);

	int (*examine_debug_reason)(struct target *target);
	int (*post_debug_entry)(struct target *target);

	void (*pre_restore_context)(struct target *target);
};

static inline struct armv8_common *
target_to_armv8(struct target *target)
{
	return container_of(target->arch_info, struct armv8_common, arm);
}

static inline enum arm_state armv8_edscr_to_core_state(uint32_t edscr)
{
	uint8_t	edscr_rw = EDSCR_RW(edscr);
	uint8_t edscr_el = EDSCR_EL(edscr);

	/* In Debug state, each bit gives the current Execution state of each EL */
	if ((edscr_rw >> edscr_el) & 0b1)
		return ARM_STATE_AARCH64;
	else
		return ARM_STATE_ARM;	/* AARCH32 */
}

/*
 * DDI0487A_f_armv8_arm.pdf
 * H9.2.41 EDSCR, External Debug Status and Control Register
 *
 * Valid PE status values are:
 *	bit[5:4]	bit[3:0]
 *	00			0001  0010  0111
 *	01			0011,       1011, 1111
 *	10			0011, 0111, 1011, 1111
 *	11			0011, 0111, 1011
 */
static inline bool armv8_is_pe_status_valid(uint32_t edscr)
{
	uint16_t status = (edscr & 0b111111);
	uint16_t status_bit54 = (status & 0b110000) >> 4;	/* bits[5:4] */
	uint16_t status_bit30 = (status & 0b001111);		/* bits[3:0] */
	uint16_t status_bit10 = (status & 0b000011);		/* bits[1:0] */

	if (status_bit54 == 0b00) {
		/* 3 special cases */
		if ((status_bit30 == 0b0010) || (status == 0b0001) || (status == 0b0111))
			return true;
		else
			return false;
	} else if (status_bit10 == 0b11) {
		/* Mostly valid, except 0b010011 & 0b111111 */
		if ((status == 0b010111) || (status == 0b111111))
			return false;
		else
			return true;
	} else {
		/* All other values are reserved: invalid */
		return false;
	}

	return false;
}


/* register offsets from armv8.debug_base */

#define CPUDBG_WFAR		0x018
#define CPUDBG_DESR		0x020
#define CPUDBG_DECR		0x024
/* PCSR at 0x084 -or- 0x0a0 -or- both ... based on flags in DIDR */
#define CPUDBG_DSCR		0x088
#define CPUDBG_DRCR		0x090
#define CPUDBG_PRCR		0x310
#define CPUDBG_PRSR		0x314

#define CPUDBG_DTRRX		0x080
#define CPUDBG_ITR		0x084
#define CPUDBG_DTRTX		0x08c

#define CPUDBG_BVR_BASE		0x400
#define CPUDBG_BCR_BASE		0x408
#define CPUDBG_WVR_BASE		0x180	/* Alamy: should not be 0x800 ? */
#define CPUDBG_WCR_BASE		0x1C0
#define CPUDBG_VCR		0x01C

#define CPUDBG_OSLAR		0x300
#define CPUDBG_OSLSR		0x304
#define CPUDBG_OSSRR		0x308
#define CPUDBG_ECR		0x024

#define CPUDBG_DSCCR		0x028

#define CPUDBG_AUTHSTATUS	0xFB8

int armv8_arch_state(struct target *target);
int armv8_identify_cache(struct target *target);
int armv8_init_arch_info(struct target *target, struct armv8_common *armv8);
int armv8_mmu_translate_va_pa(struct target *target, uint64_t va,
		uint64_t *val, int meminfo);
int armv8_mmu_translate_va(struct target *target,  uint32_t va, uint32_t *val);

int armv8_invalidate_icache(struct target *target);
int armv8_flush_cache_all(struct target *target);	/* both D-Cache & I-Cache */
int armv8_handle_cache_info_command(struct command_context *cmd_ctx,
		struct armv8_cache_common *armv8_cache);

extern const struct command_registration armv8_command_handlers[];

#endif
