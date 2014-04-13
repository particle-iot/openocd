/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2014 by Andrey Smirnov                                  *
 *   andrew.smirnov@gmail.com                                              *
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
 *   along with this program; see the file COPYING.  If not see            *
 *   <http://www.gnu.org/licenses/>                                        *
 *                                                                         *
 ***************************************************************************/

#ifndef __HCS12_H__
#define __HCS12_H__

/**
 * Represents a HCS12 core
 *
 */
struct hcs12 {
	struct reg_cache *core_cache;

	/* Handle to the PC */
	struct reg *pc;
	/* Handle to the SP */
	struct reg *sp;
	/* Handle to the CCR */
	struct reg *ccr;

	/** Backpointer to the target. */
	struct target *target;

	/** Retrieve a single core register. */
	int (*read_core_reg)(struct hcs12 *hcs12, struct reg *reg);
	int (*write_core_reg)(struct hcs12 *hcs12, struct reg *reg,
			      uint16_t value);
};

enum hcs12_registers {
	HCS12_REG_MODE		= 0x00000B,

	HCS12_REG_RPAGE		= 0x000016,

	HCS12_REG_PARTIDH	= 0x00001A,
	HCS12_REG_PARTIDL	= 0x00001B,
	HCS12_REG_PARTID	= HCS12_REG_PARTIDH,

	HCS12_REG_COPCTL	= 0x00003C,

	HCS12_REG_FCLKDIV	= 0x000100,
	HCS12_REG_FSEC		= 0x000101,
	HCS12_REG_FCCOBIX	= 0x000102,
	HCS12_REG_FSTAT		= 0x000106,

	HCS12_REG_FPROT		= 0x000108,
	HCS12_REG_EPROT		= 0x000109,

	HCS12_REG_FCCOBHI	= 0x00010A,
	HCS12_REG_FCCOBLO	= 0x00010B,
	HCS12_REG_FCCOB		= HCS12_REG_FCCOBHI,
};

enum hcs12_reg_copctl_bits {
	HCS12_REG_COPCTL_RSBCK   = 1 << 6,
	HCS12_REG_COPCTL_WRTMASK = 1 << 5,
};

enum {
	HCS12_PC,
	HCS12_SP,
	HCS12_D,
	HCS12_X,
	HCS12_Y,
	HCS12_CCR,
	HCS12_NUM_REGS,
};

enum hcs12_operation_mode {
	HCS12_REG_MODE_MOD_SHIFT = 5,

	HCS12_M_SPECIAL_SINGLE_CHIP	= 0b000,
	HCS12_M_EMULATION_SINGLE_CHIP	= 0b001,
	HCS12_M_SPECIAL_TEST		= 0b010,
	HCS12_M_EMULATION_EXPANDED	= 0b011,
	HCS12_M_NORMAL_SINGLE_CHIP	= 0b100,
	HCS12_M_NORMAL_EXPANDED		= 0b101,

	HCS12_M_MODE_COUNT = 6,
};

enum hcs12_ccr_bits {
	HCS12_CCR_I_BIT = 4,
};

struct hcs12_algorithm {
	uint16_t context[HCS12_NUM_REGS];
};

static inline struct hcs12 *
target_to_hcs12(struct target *target)
{
	return target->arch_info;
}

int32_t hcs12_ram_map_global_to_local(struct target *, uint32_t);

enum hcs12_opcodes {
	HCS12_OP_BKND = 0x00,

	HCS12_OP_ANDCC = 0x10,
	HCS12_OP_WAI   = 0x3E,
	HCS12_OP_SWI   = 0x3F,
	HCS12_OP_SEI   = 0x14,
	HCS12_OP_STOP  = 0x18,
	HCS12_OP_TFR   = 0xB7,
	HCS12_OP_RTI   = 0x0B,
};

#endif
