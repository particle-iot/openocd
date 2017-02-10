/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2017 by Angelo Dureghello                               *
 *   angelo@sysam.it                                                       *
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

#ifndef OPENOCD_TARGET_MCF5441X_H
#define OPENOCD_TARGET_MCF5441X_H

/**
 * Represents a coldfire v4 core
 *
 */
struct mcf5441x {
	struct reg_cache *core_cache;

	struct reg *pc;
	struct reg *sp;
	struct reg *ccr;

	/** Backpointer to the target. */
	struct target *target;

	/** Retrieve a single core register. */
	int (*read_core_reg)(struct mcf5441x *mcf5441x, struct reg *reg);
	int (*write_core_reg)(struct mcf5441x *mcf5441x, struct reg *reg,
			      uint8_t *value);
};

enum {
	CF_D0,
	CF_D1,
	CF_D2,
	CF_D3,
	CF_D4,
	CF_D5,
	CF_D6,
	CF_D7,
	CF_A0,
	CF_A1,
	CF_A2,
	CF_A3,
	CF_A4,
	CF_A5,
	CF_FP,
	CF_SP,
	CF_PS,
	CF_PC,
	CF_VBR,
	CF_NUM_REGS,
};

enum {
	CF_OP_HALT = 0x4ac8,
	CF_OP_TRAP = 0x4e40,
};


#define XCSR_HALTED	(1 << 31)

#define CSR_SSM		(1 << 4)
#define CSR_IPI		(1 << 5)
#define CSR_NPL		(1 << 6)
#define CSR_EMULATION	(1 << 14)
#define CSR_MAP		(1 << 15)
#define CSR_BPKT	(1 << 24)
#define CSR_HALT	(1 << 25)
#define CSR_TRG		(1 << 26)

static inline struct mcf5441x *target_to_mcf5441x(struct target *target)
{
	return target->arch_info;
}

static int mcf5441x_debug_entry(struct target *target);
int32_t mcf5441x_ram_map_global_to_local(struct target *, uint32_t);
static int mcf5441x_set_breakpoint(struct target *target,
				 struct breakpoint *breakpoint);
static int mcf5441x_unset_breakpoint(struct target *target,
				 struct breakpoint *breakpoint);

#endif /* OPENOCD_TARGET_MCF5441X_H */
