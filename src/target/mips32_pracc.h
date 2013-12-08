/***************************************************************************
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by David T.L. Wong                                 *
 *                                                                         *
 *   Copyright (C) 2011 by Drasko DRASKOVIC                                *
 *   drasko.draskovic@gmail.com                                            *
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
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifndef MIPS32_PRACC_H
#define MIPS32_PRACC_H

#define ERROR_PRACC_TEXT_JUMP -2013	/* should not conflict with jtag error codes */

#include <target/mips32.h>
#include <target/mips_ejtag.h>

#define MIPS32_PRACC_FASTDATA_AREA		0xFF200000
#define MIPS32_PRACC_FASTDATA_SIZE		16
#define MIPS32_PRACC_BASE_ADDR			0xFF200000
#define MIPS32_PRACC_TEXT				0xFF200200
#define MIPS32_PRACC_PARAM_OUT			0xFF202000

#define PRACC_UPPER_BASE_ADDR			(MIPS32_PRACC_BASE_ADDR >> 16)
#define PRACC_OUT_OFFSET			(MIPS32_PRACC_PARAM_OUT - MIPS32_PRACC_BASE_ADDR)

#define MIPS32_FASTDATA_HANDLER_SIZE	0x80
#define UPPER16(uint32_t)				(uint32_t >> 16)
#define LOWER16(uint32_t)				(uint32_t & 0xFFFF)
#define NEG16(v)						(((~(v)) + 1) & 0xFFFF)
/*#define NEG18(v) (((~(v)) + 1) & 0x3FFFF)*/

/* Exception codes for 24kc, from Mips doc MD00343 Revision 03.11 December 19, 2008 */
#define DEBUG_EXCEPTION_CODE_LIST \
	"00 Interrupt",\
	"01 Mod TLB modification exception",\
	"02 TLB exception (load or instruction fetch)",\
	"03 TLBS TLB exception (store)",\
	"04 AdEL Address error exception (load or instruction fetch)",\
	"05 AdES Address error exception (store)",\
	"06 IBE Bus error exception (instruction fetch)",\
	"07 DBE Bus error exception (data reference: load or store)",\
	"08 Sys Syscall exception",\
	"09 Bp Breakpoint exception / SDBBP in Debug Mode",\
	"10 RI Reserved instruction exception",\
	"11 CpU Coprocessor Unusable exception",\
	"12 Ov Arithmetic Overflow exception",\
	"13 Tr Trap exception",\
	"14 Reserved",\
	"15 FPE Floating point exception",\
	"16 IS1 Coprocessor 2 implementation specific exception",\
	"17 CEU CorExtend Unusable",\
	"18 C2E Precise Coprocessor 2 exception",\
	"19 Reserved",\
	"20 Reserved",\
	"21 Reserved",\
	"22 Reserved",\
	"23 WATCH Reference to WatchHi/WatchLo address",\
	"24 MCheck Machine checkcore",\
	"25 Reserved",\
	"26 Reserved",\
	"27 Reserved",\
	"28 Reserved",\
	"29 Reserved",\
	"30 CacheErr Cache error",\
	"31 Reserved"

struct pracc_queue_info {
	int retval;
	const int max_code;
	int code_count;
	int store_count;
	uint32_t *pracc_list;	/* Code and store addresses */
};
void pracc_queue_init(struct pracc_queue_info *ctx);
void pracc_add(struct pracc_queue_info *ctx, uint32_t addr, uint32_t instr);
void pracc_queue_free(struct mips_ejtag *ejtag_info, struct pracc_queue_info *ctx);
int mips32_pracc_queue_exec(struct mips_ejtag *ejtag_info,
			    struct pracc_queue_info *ctx, uint32_t *buf);

int mips32_pracc_read_mem(struct mips_ejtag *ejtag_info,
		uint32_t addr, int size, int count, void *buf);
int mips32_pracc_write_mem(struct mips_ejtag *ejtag_info,
		uint32_t addr, int size, int count, const void *buf);
int mips32_pracc_fastdata_xfer(struct mips_ejtag *ejtag_info, struct working_area *source,
		int write_t, uint32_t addr, int count, uint32_t *buf);

int mips32_pracc_read_regs(struct mips_ejtag *ejtag_info, uint32_t *regs);
int mips32_pracc_write_regs(struct mips_ejtag *ejtag_info, uint32_t *regs);

int mips32_pracc_exec(struct mips_ejtag *ejtag_info, struct pracc_queue_info *ctx, uint32_t *param_out);

/**
 * \b mips32_cp0_read
 *
 * Simulates mfc0 ASM instruction (Move From C0),
 * i.e. implements copro C0 Register read.
 *
 * @param[in] ejtag_info
 * @param[in] val Storage to hold read value
 * @param[in] cp0_reg Number of copro C0 register we want to read
 * @param[in] cp0_sel Select for the given C0 register
 *
 * @return ERROR_OK on Sucess, ERROR_FAIL otherwise
 */
int mips32_cp0_read(struct mips_ejtag *ejtag_info,
		uint32_t *val, uint32_t cp0_reg, uint32_t cp0_sel);

/**
 * \b mips32_cp0_write
 *
 * Simulates mtc0 ASM instruction (Move To C0),
 * i.e. implements copro C0 Register read.
 *
 * @param[in] ejtag_info
 * @param[in] val Value to be written
 * @param[in] cp0_reg Number of copro C0 register we want to write to
 * @param[in] cp0_sel Select for the given C0 register
 *
 * @return ERROR_OK on Sucess, ERROR_FAIL otherwise
 */
int mips32_cp0_write(struct mips_ejtag *ejtag_info,
		uint32_t val, uint32_t cp0_reg, uint32_t cp0_sel);

#endif
