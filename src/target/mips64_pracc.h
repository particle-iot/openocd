/***************************************************************************
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by David T.L. Wong                                 *
 *                                                                         *
 *   Copyright (C) 2011 by Drasko DRASKOVIC                                *
 *   drasko.draskovic@gmail.com                                            *
 *                                                                         *
 *   Copyright (C) 2013 by Dongxue Zhang                                   *
 *   elta.era@gmail.com                                                    *
 *                                                                         *
 *   Copyright (C) 2013 by FengGao                                         *
 *   gf91597@gmail.com                                                     *
 *                                                                         *
 *   Copyright (C) 2013 by Jia Liu                                         *
 *   proljc@gmail.com                                                      *
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
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef MIPS64_PRACC_H
#define MIPS64_PRACC_H

#include <target/mips64.h>
#include <target/mips_ejtag.h>

#define MIPS64_PRACC_FASTDATA_AREA		0xFF200000
#define MIPS64_PRACC_BASE_ADDR			0xFF200000
#define MIPS64_PRACC_FASTDATA_SIZE		16
#define MIPS64_PRACC_TEXT				0xFF200200
#define MIPS64_PRACC_STACK				0xFF204000
#define MIPS64_PRACC_PARAM_IN			0xFF201000
#define MIPS64_PRACC_PARAM_IN_SIZE		0x1000
#define MIPS64_PRACC_PARAM_OUT			(MIPS64_PRACC_PARAM_IN + MIPS64_PRACC_PARAM_IN_SIZE)
#define MIPS64_PRACC_PARAM_OUT_SIZE		0x1000

#define MIPS64_PRACC_UPPER_BASE_ADDR			(MIPS64_PRACC_BASE_ADDR >> 16)
#define MIPS64_PRACC_TEXT_OFFSET			(MIPS64_PRACC_TEXT - MIPS64_PRACC_BASE_ADDR)
#define MIPS64_PRACC_IN_OFFSET				(MIPS64_PRACC_PARAM_IN - MIPS64_PRACC_BASE_ADDR)
#define MIPS64_PRACC_OUT_OFFSET			(MIPS64_PRACC_PARAM_OUT - MIPS64_PRACC_BASE_ADDR)
#define MIPS64_PRACC_STACK_OFFSET			(MIPS64_PRACC_STACK - MIPS64_PRACC_BASE_ADDR)

#define MIPS64_FASTDATA_HANDLER_SIZE	0x80
#define UPPER16(uint32_t)				(uint32_t >> 16)
#define LOWER16(uint32_t)				(uint32_t & 0xFFFF)
#define UPPER64_16(uint64_t)				((uint64_t >> 16) & 0xFFFF)
#define LOWER64_16(uint64_t)				(uint64_t & 0xFFFF)
#define NEG16(v)						(((~(v)) + 1) & 0xFFFF)
#define OBTAIN16(uint64_t, v)                   ((uint64_t >> (16 * v)) & 0xFFFF)
/*#define NEG18(v) (((~(v)) + 1) & 0x3FFFF)*/

struct mips64_pracc_queue_info {
	int retval;
	const int max_code;
	int code_count;
	int store_count;
	uint32_t *pracc_list;	/* Code and store addresses */
};
void mips64_pracc_queue_init(struct mips64_pracc_queue_info *ctx);
void mips64_pracc_add(struct mips64_pracc_queue_info *ctx, uint64_t addr, uint32_t instr);
void mips64_pracc_queue_free(struct mips64_pracc_queue_info *ctx);
int mips64_pracc_queue_exec(struct mips_ejtag *ejtag_info,
			    struct mips64_pracc_queue_info *ctx, uint64_t *buf);

int mips64_pracc_read_mem(struct mips_ejtag *ejtag_info,
		target_ulong addr, uint32_t size, uint32_t count, void *buf);
int mips64_pracc_write_mem(struct mips_ejtag *ejtag_info,
		target_ulong addr, uint32_t size, uint32_t count, void *buf);
int mips64_pracc_fastdata_xfer(struct mips_ejtag *ejtag_info, struct working_area *source,
		int write_t, target_ulong addr, int count, uint32_t *buf);

int mips64_pracc_read_regs(struct mips_ejtag *ejtag_info, uint64_t *regs);
int mips64_pracc_write_regs(struct mips_ejtag *ejtag_info, uint64_t *regs);

int mips64_pracc_exec(struct mips_ejtag *ejtag_info, int code_len, const uint32_t *code,
		int num_param_in, uint64_t *param_in,
		int num_param_out, uint64_t *param_out, int cycle);

/**
 * \b mips64_cp0_read
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
int mips64_cp0_read(struct mips_ejtag *ejtag_info,
		uint32_t *val, uint32_t cp0_reg, uint32_t cp0_sel);

/**
 * \b mips64_cp0_write
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
int mips64_cp0_write(struct mips_ejtag *ejtag_info,
		uint32_t val, uint32_t cp0_reg, uint32_t cp0_sel);

#endif
