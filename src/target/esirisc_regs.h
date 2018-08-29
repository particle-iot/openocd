/***************************************************************************
 *   Copyright (C) 2018 by Square, Inc.                                    *
 *   Steven Stallion <stallion@squareup.com>                               *
 *   James Zhao <hjz@squareup.com>                                         *
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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifndef OPENOCD_TARGET_ESIRISC_REGS_H
#define OPENOCD_TARGET_ESIRISC_REGS_H

enum esirisc_reg_num {
	REG_SP,
	REG_RA,
	REG_R2,
	REG_R3,
	REG_R4,
	REG_R5,
	REG_R6,
	REG_R7,
	REG_R8,
	REG_R9,
	REG_R10,
	REG_R11,
	REG_R12,
	REG_R13,
	REG_R14,
	REG_R15,
	REG_R16,
	REG_R17,
	REG_R18,
	REG_R19,
	REG_R20,
	REG_R21,
	REG_R22,
	REG_R23,
	REG_R24,
	REG_R25,
	REG_R26,
	REG_R27,
	REG_R28,
	REG_R29,
	REG_R30,
	REG_R31,

	REG_V0,
	REG_V1,
	REG_V2,
	REG_V3,
	REG_V4,
	REG_V5,
	REG_V6,
	REG_V7,
	REG_V8,
	REG_V9,
	REG_V10,
	REG_V11,
	REG_V12,
	REG_V13,
	REG_V14,
	REG_V15,
	REG_V16,
	REG_V17,
	REG_V18,
	REG_V19,
	REG_V20,
	REG_V21,
	REG_V22,
	REG_V23,
	REG_V24,
	REG_V25,
	REG_V26,
	REG_V27,
	REG_V28,
	REG_V29,
	REG_V30,
	REG_V31,

	REG_A0,
	REG_A1,
	REG_A2,
	REG_A3,
	REG_A4,
	REG_A5,
	REG_A6,
	REG_A7,

	REG_PC,
	REG_CAS,
	REG_TC,
	REG_ETA,
	REG_ETC,
	REG_EPC,
	REG_ECAS,
	REG_EID,
	REG_ED,
	REG_IP,
	REG_IM,
	REG_IS,
	REG_IT,

	ESIRISC_NUM_REGS,
};

/* CSR Banks */
#define CSR_THREAD					0x00
#define CSR_INTERRUPT				0x01
#define CSR_DEBUG					0x04
#define CSR_CONFIG					0x05
#define CSR_TRACE					0x09

/* Thread CSRs */
#define CSR_THREAD_TC				0x00	/* Thread Control */
#define CSR_THREAD_PC				0x01	/* Program Counter */
#define CSR_THREAD_CAS				0x02	/* Comparison & Arithmetic Status */
#define CSR_THREAD_AC				0x03	/* Arithmetic Control */
#define CSR_THREAD_LF				0x04	/* Locked Flag */
#define CSR_THREAD_LA				0x05	/* Locked Address */
#define CSR_THREAD_ETA				0x07	/* Exception Table Address */
#define CSR_THREAD_ETC				0x08	/* Exception TC */
#define CSR_THREAD_EPC				0x09	/* Exception PC */
#define CSR_THREAD_ECAS				0x0a	/* Exception CAS */
#define CSR_THREAD_EID				0x0b	/* Exception ID */
#define CSR_THREAD_ED				0x0c	/* Exception Data */

/* Interrupt CSRs */
#define CSR_INTERRUPT_IP			0x00	/* Interrupt Pending */
#define CSR_INTERRUPT_IA			0x01	/* Interrupt Acknowledge */
#define CSR_INTERRUPT_IM			0x02	/* Interrupt Mask */
#define CSR_INTERRUPT_IS			0x03	/* Interrupt Sense */
#define CSR_INTERRUPT_IT			0x04	/* Interrupt Trigger */

/* Debug CSRs */
#define CSR_DEBUG_DC				0x00	/* Debug Control */
#define CSR_DEBUG_IBC				0x01	/* Instruction Breakpoint Control */
#define CSR_DEBUG_DBC				0x02	/* Data Breakpoint Control */
#define CSR_DEBUG_HWDC				0x03	/* Hardware Debug Control */
#define CSR_DEBUG_DBS				0x04	/* Data Breakpoint Size */
#define CSR_DEBUG_DBR				0x05	/* Data Breakpoint Range */
#define CSR_DEBUG_IBAn				0x08	/* Instruction Breakpoint Address [0..7] */
#define CSR_DEBUG_DBAn				0x10	/* Data Breakpoint Address [0..7] */

/* Configuration CSRs */
#define CSR_CONFIG_ARCH0			0x00	/* Architectural Configuration 0 */
#define CSR_CONFIG_ARCH1			0x01	/* Architectural Configuration 1 */
#define CSR_CONFIG_ARCH2			0x02	/* Architectural Configuration 2 */
#define CSR_CONFIG_ARCH3			0x03	/* Architectural Configuration 3 */
#define CSR_CONFIG_MEM				0x04	/* Memory Configuration */
#define CSR_CONFIG_IC				0x05	/* Instruction Cache Configuration */
#define CSR_CONFIG_DC				0x06	/* Data Cache Configuration */
#define CSR_CONFIG_INT				0x07	/* Interrupt Configuration */
#define	CSR_CONFIG_ISAn				0x08	/* Instruction Set Configuration [0..6] */
#define CSR_CONFIG_DBG				0x0f	/* Debug Configuration */
#define CSR_CONFIG_MID				0x10	/* Manufacturer ID */
#define CSR_CONFIG_REV				0x11	/* Revision Number */
#define CSR_CONFIG_MPID				0x12	/* Mulitprocessor ID */
#define CSR_CONFIG_FREQn			0x13	/* Frequency [0..2] */
#define CSR_CONFIG_TRACE			0x16	/* Trace Configuration */

/* Trace CSRs */
#define CSR_TRACE_CONTROL			0x00
#define CSR_TRACE_STATUS			0x01
#define CSR_TRACE_BUFFER_START		0x02
#define CSR_TRACE_BUFFER_END		0x03
#define CSR_TRACE_BUFFER_CUR		0x04
#define CSR_TRACE_TRIGGER			0x05
#define CSR_TRACE_START_DATA		0x06
#define CSR_TRACE_START_MASK		0x07
#define CSR_TRACE_STOP_DATA			0x08
#define CSR_TRACE_STOP_MASK			0x09
#define CSR_TRACE_DELAY				0x0a

#endif /* OPENOCD_TARGET_ESIRISC_REGS_H */
