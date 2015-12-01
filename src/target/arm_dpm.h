/*
 * Copyright (C) 2009 by David Brownell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef __ARM_DPM_H
#define __ARM_DPM_H

/**
 * @file
 * This is the interface to the Debug Programmers Model for ARMv6 and
 * ARMv7 processors.  ARMv6 processors (such as ARM11xx implementations)
 * introduced a model which became part of the ARMv7-AR architecture
 * which is most familiar through the Cortex-A series parts.  While
 * specific details differ (like how to write the instruction register),
 * the high level models easily support shared code because those
 * registers are compatible.
 */

struct dpm_bpwp {
	unsigned number;
	uint32_t address;
	uint32_t control;
	/* true if hardware state needs flushing */
	bool dirty;
};

struct dpm_bp {
	struct breakpoint *bp;
	struct dpm_bpwp bpwp;
};

struct dpm_wp {
	struct watchpoint *wp;
	struct dpm_bpwp bpwp;
};

/**
 * This wraps an implementation of DPM primitives.  Each interface
 * provider supplies a structure like this, which is the glue between
 * upper level code and the lower level hardware access.
 *
 * It is a PRELIMINARY AND INCOMPLETE set of primitives, starting with
 * support for CPU register access.
 */
struct arm_dpm {
	struct arm *arm;

	/** Cache of DIDR */
	uint32_t didr;

	/** Invoke before a series of instruction operations */
	int (*prepare)(struct arm_dpm *);

	/** Invoke after a series of instruction operations */
	int (*finish)(struct arm_dpm *);

	/* EXECUTE INSTRUCTION */

	int (*exec_opcode)(struct arm_dpm *, uint32_t opcode);

	/* WRITE TO CPU */

	/** Runs one instruction, writing data to DCC before execution. */
	int (*instr_write_data_dcc)(struct arm_dpm *,
			uint32_t opcode, uint32_t data);

	int (*instr_write_data_dcc_64)(struct arm_dpm *,
			uint32_t opcode, uint64_t data);

	/** Runs one instruction, writing data to R0 before execution. */
	int (*instr_write_data_r0)(struct arm_dpm *,
			uint32_t opcode, uint32_t data);

	/** Runs one instruction, writing data to X0 before execution. */
	int (*instr_write_data_x0)(struct arm_dpm *,
			uint32_t opcode, uint64_t data);

	/** Optional core-specific operation invoked after CPSR writes. */
	int (*instr_cpsr_sync)(struct arm_dpm *dpm);

	/* READ FROM CPU */

	/** Runs one instruction, reading data from dcc after execution. */
	int (*instr_read_data_dcc)(struct arm_dpm *,
			uint32_t opcode, uint32_t *data);

	int (*instr_read_data_dcc_64)(struct arm_dpm *,
			uint32_t opcode, uint64_t *data);

	/** Runs one instruction, reading data from r0 after execution. */
	int (*instr_read_data_r0)(struct arm_dpm *,
			uint32_t opcode, uint32_t *data);

	int (*instr_read_data_x0)(struct arm_dpm *,
			uint32_t opcode, uint64_t *data);

	/* BREAKPOINT/WATCHPOINT SUPPORT */

	/**
	 * Enables one breakpoint or watchpoint by writing to the
	 * hardware registers.  The specified breakpoint/watchpoint
	 * must currently be disabled.  Indices 0..15 are used for
	 * breakpoints; indices 16..31 are for watchpoints.
	 */
	int (*bpwp_enable)(struct arm_dpm *, unsigned index_value,
			uint32_t addr, uint32_t control);

	/**
	 * Disables one breakpoint or watchpoint by clearing its
	 * hardware control registers.  Indices are the same ones
	 * accepted by bpwp_enable().
	 */
	int (*bpwp_disable)(struct arm_dpm *, unsigned index_value);

	/* The breakpoint and watchpoint arrays are private to the
	 * DPM infrastructure.  There are nbp indices in the dbp
	 * array.  There are nwp indices in the dwp array.
	 */

	unsigned nbp;
	unsigned nwp;
	struct dpm_bp *dbp;
	struct dpm_wp *dwp;

	/** Address of the instruction which triggered a watchpoint. */
	uint32_t wp_pc;

	/* FIXME -- read/write DCSR methods and symbols */
};

int arm_dpm_setup(struct arm_dpm *dpm);
int arm_dpm_initialize(struct arm_dpm *dpm);

int arm_dpm_read_current_registers(struct arm_dpm *);
int arm_dpm_read_current_registers_64(struct arm_dpm *);
int dpm_modeswitch(struct arm_dpm *dpm, enum arm_mode mode);


int arm_dpm_write_dirty_registers(struct arm_dpm *, bool bpwp);

void arm_dpm_report_wfar(struct arm_dpm *, uint32_t wfar);

/* DSCR bits; see ARMv7a arch spec section C10.3.1.
 * Not all v7 bits are valid in v6.
 */
#define DSCR_CORE_HALTED            (0x1 <<  0)
#define DSCR_CORE_RESTARTED         (0x1 <<  1)
#define DSCR_ENTRY_MASK             (0xF <<  2)
#define DSCR_STICKY_ABORT_PRECISE   (0x1 <<  6)
#define DSCR_STICKY_ABORT_IMPRECISE (0x1 <<  7)
#define DSCR_STICKY_UNDEFINED       (0x1 <<  8)
#define DSCR_DBG_NOPWRDWN           (0x1 <<  9) /* v6 only */
#define DSCR_DBG_ACK                (0x1 << 10)
#define DSCR_INT_DIS                (0x1 << 11)
#define DSCR_CP14_USR_COMMS         (0x1 << 12)
#define DSCR_ITR_EN                 (0x1 << 13)
#define DSCR_HALT_DBG_MODE          (0x1 << 14)
#define DSCR_MON_DBG_MODE           (0x1 << 15)
#define DSCR_SEC_PRIV_INVASV_DIS    (0x1 << 16)
#define DSCR_SEC_PRIV_NINVASV_DIS   (0x1 << 17)
#define DSCR_NON_SECURE             (0x1 << 18)
#define DSCR_DSCRD_IMPRECISE_ABORT  (0x1 << 19)
#define DSCR_EXT_DCC_MASK           (0x3 << 20) /* DTR mode */  /* bits 22, 23 are reserved */
#define DSCR_INSTR_COMP             (0x1 << 24)
#define DSCR_PIPE_ADVANCE           (0x1 << 25)
#define DSCR_DTRTX_FULL_LATCHED     (0x1 << 26)
#define DSCR_DTRRX_FULL_LATCHED     (0x1 << 27) /* bit 28 is reserved */
#define DSCR_DTR_TX_FULL            (0x1 << 29)
#define DSCR_DTR_RX_FULL            (0x1 << 30) /* bit 31 is reserved */

#define DSCR_ENTRY(dscr)            (((dscr) >> 2) & 0xf)
#define DSCR_RUN_MODE(dscr)         ((dscr) & (DSCR_CORE_HALTED | DSCR_CORE_RESTARTED))


/* Methods of entry into debug mode */
#define DSCR_ENTRY_HALT_REQ           (0x0 << 2)
#define DSCR_ENTRY_BREAKPOINT         (0x1 << 2)
#define DSCR_ENTRY_IMPRECISE_WATCHPT  (0x2 << 2)
#define DSCR_ENTRY_BKPT_INSTR         (0x3 << 2)
#define DSCR_ENTRY_EXT_DBG_REQ        (0x4 << 2)
#define DSCR_ENTRY_VECT_CATCH         (0x5 << 2)
#define DSCR_ENTRY_D_SIDE_ABORT       (0x6 << 2)  /* v6 only */
#define DSCR_ENTRY_I_SIDE_ABORT       (0x7 << 2)  /* v6 only */
#define DSCR_ENTRY_OS_UNLOCK          (0x8 << 2)
#define DSCR_ENTRY_PRECISE_WATCHPT    (0xA << 2)

/* DTR modes */
#define DSCR_EXT_DCC_NON_BLOCKING     (0x0 << 20)
#define DSCR_EXT_DCC_STALL_MODE       (0x1 << 20)
#define DSCR_EXT_DCC_FAST_MODE        (0x2 << 20)  /* bits 22, 23 are reserved */

/* DRCR (debug run control register) bits */
#define DRCR_HALT				(1 << 0)
#define DRCR_RESTART			(1 << 1)
#define DRCR_CLEAR_EXCEPTIONS	(1 << 2)


void arm_dpm_report_dscr(struct arm_dpm *dpm, uint32_t dcsr);
enum target_debug_reason armv8_edscr_debug_reason(uint32_t edscr);


#endif /* __ARM_DPM_H */
