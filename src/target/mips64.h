/***************************************************************************
 *   Support for processors implementing MIPS64 instruction set            *
 *                                                                         *
 *   Copyright (C) 2014 by Andrey Sidorov <anysidorov@gmail.com>           *
 *   Copyright (C) 2014 by Aleksey Kuleshov <rndfax@yandex.ru>             *
 *   Copyright (C) 2014 by Peter Mamonov <pmamonov@gmail.com>              *
 *                                                                         *
 *   Based on the work of:                                                 *
 *       Copyright (C) 2008 by Spencer Oliver                              *
 *       Copyright (C) 2008 by David T.L. Wong                             *
 *       Copyright (C) 2010 by Konstantin Kostyukhin, Nikolay Shmyrev      *
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
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/

#ifndef OPENOCD_TARGET_MIPS64_H
#define OPENOCD_TARGET_MIPS64_H

#include "target.h"
#include "register.h"
#include "mips64_pracc.h"

#define MIPS64_COMMON_MAGIC		0xB640B640

/* offsets into mips64 core register cache */
#define MIPS64_NUM_CORE_REGS 40
#define MIPS64_NUM_FP_REGS 34
enum {
	MIPS64_PC = MIPS64_NUM_CORE_REGS - 1,
	MIPS64_FIR = MIPS64_NUM_CORE_REGS + MIPS64_NUM_FP_REGS - 1,
	MIPS64NUMCOREREGS
};

struct mips64_comparator {
	int used;
	uint64_t bp_value;
	uint64_t reg_address;
};

struct mips64_common {
	uint32_t common_magic;
	void *arch_info;
	struct reg_cache *core_cache;
	struct mips_ejtag ejtag_info;
	uint64_t core_regs[MIPS64NUMCOREREGS];

	int bp_scanned;
	int num_inst_bpoints;
	int num_data_bpoints;
	int num_inst_bpoints_avail;
	int num_data_bpoints_avail;
	struct mips64_comparator *inst_break_list;
	struct mips64_comparator *data_break_list;

	/* register cache to processor synchronization */
	int (*read_core_reg)(struct target *target, int num);
	int (*write_core_reg)(struct target *target, int num);
};

struct mips64_core_reg {
	uint32_t num;
	struct target *target;
	struct mips64_common *mips64_common;
};

#define MIPS64_OP_BEQ	0x04
#define MIPS64_OP_BNE	0x05
#define MIPS64_OP_ADDI	0x08
#define MIPS64_OP_AND	0x24
#define MIPS64_OP_LUI	0x0F
#define MIPS64_OP_LW	0x23
#define MIPS64_OP_LD	0x37
#define MIPS64_OP_LBU	0x24
#define MIPS64_OP_LHU	0x25
#define MIPS64_OP_MFHI	0x10
#define MIPS64_OP_MTHI	0x11
#define MIPS64_OP_MFLO	0x12
#define MIPS64_OP_MTLO	0x13
#define MIPS64_OP_SB	0x28
#define MIPS64_OP_SH	0x29
#define MIPS64_OP_SW	0x2B
#define MIPS64_OP_SD	0x3F
#define MIPS64_OP_ORI	0x0D

#define MIPS64_OP_COP0	0x10
#define MIPS64_OP_COP1	0x11
#define MIPS64_OP_COP2	0x12

#define MIPS64_COP_MF	0x00
#define MIPS64_COP_DMF	0x01
#define MIPS64_COP_MT	0x04
#define MIPS64_COP_DMT	0x05
#define MIPS64_COP_CF	0x02
#define MIPS64_COP_CT	0x06

#define MIPS64_R_INST(opcode, rs, rt, rd, shamt, funct) \
(((opcode) << 26) | ((rs) << 21) | ((rt) << 16) | ((rd) << 11) | ((shamt) << 6) | (funct))
#define MIPS64_I_INST(opcode, rs, rt, immd)	(((opcode) << 26) | ((rs) << 21) | ((rt) << 16) | (immd))
#define MIPS64_J_INST(opcode, addr)	(((opcode) << 26) | (addr))

#define MIPS64_NOP			0
#define MIPS64_ADDI(tar, src, val)	MIPS64_I_INST(MIPS64_OP_ADDI, src, tar, val)
#define MIPS64_AND(reg, off, val)	MIPS64_R_INST(0, off, val, reg, 0, MIPS64_OP_AND)
#define MIPS64_B(off)			MIPS64_BEQ(0, 0, off)
#define MIPS64_BEQ(src, tar, off)	MIPS64_I_INST(MIPS64_OP_BEQ, src, tar, off)
#define MIPS64_BNE(src, tar, off)	MIPS64_I_INST(MIPS64_OP_BNE, src, tar, off)
#define MIPS64_MFC0(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP0, MIPS64_COP_MF, gpr, cpr, 0, sel)
#define MIPS64_DMFC0(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP0, MIPS64_COP_DMF, gpr, cpr, 0, sel)
#define MIPS64_MTC0(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP0, MIPS64_COP_MT, gpr, cpr, 0, sel)
#define MIPS64_DMTC0(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP0, MIPS64_COP_DMT, gpr, cpr, 0, sel)
#define MIPS64_MFC1(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP1, MIPS64_COP_MF, gpr, cpr, 0, 0)
#define MIPS64_MTC1(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP1, MIPS64_COP_MT, gpr, cpr, 0, 0)
#define MIPS64_MFC2(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP2, MIPS64_COP_MF, gpr, cpr, 0, sel)
#define MIPS64_MTC2(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP2, MIPS64_COP_MT, gpr, cpr, 0, sel)
#define MIPS64_CFC1(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP1, MIPS64_COP_CF, gpr, cpr, 0, 0)
#define MIPS64_CTC1(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP1, MIPS64_COP_CT, gpr, cpr, 0, 0)
#define MIPS64_CFC2(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP2, MIPS64_COP_CF, gpr, cpr, 0, sel)
#define MIPS64_CTC2(gpr, cpr, sel)	MIPS64_R_INST(MIPS64_OP_COP2, MIPS64_COP_CT, gpr, cpr, 0, sel)
#define MIPS64_LBU(reg, off, base)	MIPS64_I_INST(MIPS64_OP_LBU, base, reg, off)
#define MIPS64_LHU(reg, off, base)	MIPS64_I_INST(MIPS64_OP_LHU, base, reg, off)
#define MIPS64_LUI(reg, val)		MIPS64_I_INST(MIPS64_OP_LUI, 0, reg, val)
#define MIPS64_LW(reg, off, base)	MIPS64_I_INST(MIPS64_OP_LW, base, reg, off)
#define MIPS64_LD(reg, off, base)	MIPS64_I_INST(MIPS64_OP_LD, base, reg, off)
#define MIPS64_MFLO(reg)		MIPS64_R_INST(0, 0, 0, reg, 0, MIPS64_OP_MFLO)
#define MIPS64_MFHI(reg)		MIPS64_R_INST(0, 0, 0, reg, 0, MIPS64_OP_MFHI)
#define MIPS64_MTLO(reg)		MIPS64_R_INST(0, reg, 0, 0, 0, MIPS64_OP_MTLO)
#define MIPS64_MTHI(reg)		MIPS64_R_INST(0, reg, 0, 0, 0, MIPS64_OP_MTHI)
#define MIPS64_ORI(src, tar, val)	MIPS64_I_INST(MIPS64_OP_ORI, src, tar, val)
#define MIPS64_SB(reg, off, base)	MIPS64_I_INST(MIPS64_OP_SB, base, reg, off)
#define MIPS64_SH(reg, off, base)	MIPS64_I_INST(MIPS64_OP_SH, base, reg, off)
#define MIPS64_SW(reg, off, base)	MIPS64_I_INST(MIPS64_OP_SW, base, reg, off)
#define MIPS64_SD(reg, off, base)	MIPS64_I_INST(MIPS64_OP_SD, base, reg, off)
#define MIPS64_CACHE(op, reg, off)	(47 << 26 | (reg) << 21 | (op) << 16 | (off))
#define MIPS64_SYNCI(reg, off)		(1 << 26 | (reg) << 21 | 0x1f << 16 | (off))

/* ejtag specific instructions */
#define MIPS64_DRET			0x4200001F
#define MIPS64_SDBBP			0x7000003F
#define MIPS64_SDBBP_LE			0x3f000007

#define MIPS64_SYNC			0x0000000F

int mips64_arch_state(struct target *target);
int mips64_init_arch_info(struct target *target, struct mips64_common *mips64, struct jtag_tap *tap);
int mips64_restore_context(struct target *target);
int mips64_save_context(struct target *target);
struct reg_cache *mips64_build_reg_cache(struct target *target);
int mips64_run_algorithm(struct target *target, int num_mem_params, struct mem_param *mem_params,
	int num_reg_params, struct reg_param *reg_params,
	target_addr_t entry_point, target_addr_t exit_point,
	int timeout_ms, void *arch_info);
int mips64_configure_break_unit(struct target *target);
int mips64_enable_interrupts(struct target *target, int enable);
int mips64_examine(struct target *target);

int mips64_register_commands(struct command_context *cmd_ctx);
int mips64_invalidate_core_regs(struct target *target);
int mips64_get_gdb_reg_list(struct target *target,
	struct reg **reg_list[], int *reg_list_size,
	enum target_register_class reg_class);

#endif	/* OPENOCD_TARGET_MIPS64_H */
