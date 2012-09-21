/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2006 by Magnus Lundin                                   *
 *   lundin@mlu.mine.nu                                                    *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2009 by Dirk Behme                                      *
 *   dirk.behme@gmail.com - copy from cortex_m3                            *
 *                                                                         *
 *   Copyright (C) 2010 Øyvind Harboe                                      *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) ST-Ericsson SA 2011                                     *
 *   michel.jaouen@stericsson.com : smp minimum support                    *
 *                                                                         *
 *   Copyright (C) 2011 by Google Inc                                      *
 *   aschultz@google.com - Copy from cortex_a                              *
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
 *                                                                         *
 *   Cortex-R4(tm) TRM, ARM DDI 0363E                                      *
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "breakpoints.h"
#include "cortex_r.h"
#include "register.h"
#include "target_request.h"
#include "target_type.h"
#include "arm_opcodes.h"
#include <helper/time_support.h>

static int cortex_r4_poll(struct target *target);
static int cortex_r4_debug_entry(struct target *target);
static int cortex_r4_restore_context(struct target *target, bool bpwp);
static int cortex_r4_set_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode);
static int cortex_r4_set_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode);
static int cortex_r4_set_hybrid_breakpoint(struct target *target,
	struct breakpoint *breakpoint);
static int cortex_r4_unset_breakpoint(struct target *target,
	struct breakpoint *breakpoint);
static int cortex_r4_dap_read_coreregister_u32(struct target *target,
	uint32_t *value, int regnum);
static int cortex_r4_dap_write_coreregister_u32(struct target *target,
	uint32_t value, int regnum);
static int cortex_r4_mmu(struct target *target, int *enabled);
static int cortex_r4_virt2phys(struct target *target,
	uint32_t virt, uint32_t *phys);

/*
 * FIXME do topology discovery using the ROM; don't
 * assume this is an OMAP3.   Also, allow for multiple ARMv7-A
 * cores, with different AP numbering ... don't use a #define
 * for these numbers, use per-core armv7r state.
 */
#define swjdp_memoryap 0
#define swjdp_debugap 1

/*  restore cp15_control_reg at resume */
static int cortex_r4_restore_cp15_control_reg(struct target *target)
{
	int retval = ERROR_OK;
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);
	struct armv7r_common *armv7r = target_to_armv7r(target);

	if (cortex_r4->cp15_control_reg != cortex_r4->cp15_control_reg_curr) {
		cortex_r4->cp15_control_reg_curr = cortex_r4->cp15_control_reg;
		/* LOG_INFO("cp15_control_reg: %8.8" PRIx32, cortex_r4->cp15_control_reg); */
		retval = armv7r->arm.mcr(target, 15,
				0, 0,	/* op1, op2 */
				1, 0,	/* CRn, CRm */
				cortex_r4->cp15_control_reg);
	}
	return retval;
}

/*  check address before cortex_r4_apb read write access with mmu on
 *  remove apb predictible data abort */
static int cortex_r4_check_address(struct target *target, uint32_t address)
{
	/*
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);
	uint32_t os_border = armv7r->armv7r_mmu.os_border;
	if ((address < os_border) &&
		(armv7r->arm.core_mode == ARM_MODE_SVC)) {
		LOG_ERROR("%x access in userspace and target in supervisor", address);
		return ERROR_FAIL;
	}
	if ((address >= os_border) &&
		(cortex_r4->curr_mode != ARM_MODE_SVC)) {
		dpm_modeswitch(&armv7r->dpm, ARM_MODE_SVC);
		cortex_r4->curr_mode = ARM_MODE_SVC;
		LOG_INFO("%x access in kernel space and target not in supervisor",
			address);
		return ERROR_OK;
	}
	if ((address < os_border) &&
		(cortex_r4->curr_mode == ARM_MODE_SVC)) {
		dpm_modeswitch(&armv7r->dpm, ARM_MODE_ANY);
		cortex_r4->curr_mode = ARM_MODE_ANY;
	}
	*/
	return ERROR_OK;
}

/*
 * Cortex-r4 Basic debug access, very low level assumes state is saved
 */
static int cortex_r4_init_debug_access(struct target *target)
{
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;
	int retval;
	uint32_t dummy;

	LOG_DEBUG(" ");

	/* Unlocking the debug registers for modification
	 * The debugport might be uninitialised so try twice */
	retval = mem_ap_sel_write_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_LOCKACCESS, 0xC5ACCE55);
	if (retval != ERROR_OK) {
		/* try again */
		retval = mem_ap_sel_write_atomic_u32(swjdp, swjdp_debugap,
				armv7r->debug_base + CPUDBG_LOCKACCESS, 0xC5ACCE55);
		if (retval == ERROR_OK)
			LOG_USER(
				"Locking debug access failed on first, but succeeded on second try.");
	}
	if (retval != ERROR_OK)
		return retval;
	/* Clear Sticky Power Down status Bit in PRSR to enable access to
	   the registers in the Core Power Domain */
	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_PRSR, &dummy);
	if (retval != ERROR_OK)
		return retval;

	/* Enabling of instruction execution in debug mode is done in debug_entry code */

	/* Resync breakpoint registers */

	/* Since this is likely called from init or reset, update target state information*/
	return cortex_r4_poll(target);
}

/* To reduce needless round-trips, pass in a pointer to the current
 * DSCR value.  Initialize it to zero if you just need to know the
 * value on return from this function; or DSCR_INSTR_COMP if you
 * happen to know that no instruction is pending.
 */
static int cortex_r4_exec_opcode(struct target *target,
	uint32_t opcode, uint32_t *dscr_p)
{
	uint32_t dscr;
	int retval;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;

	dscr = dscr_p ? *dscr_p : 0;

	LOG_DEBUG("exec opcode 0x%08" PRIx32, opcode);

	/* Wait for InstrCompl bit to be set */
	long long then = timeval_ms();
	while ((dscr & DSCR_INSTR_COMP) == 0) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
				armv7r->debug_base + CPUDBG_DSCR, &dscr);
		if (retval != ERROR_OK) {
			LOG_ERROR("Could not read DSCR register, opcode = 0x%08" PRIx32, opcode);
			return retval;
		}
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for cortex_r4_exec_opcode");
			return ERROR_FAIL;
		}
	}

	retval = mem_ap_sel_write_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_ITR, opcode);
	if (retval != ERROR_OK)
		return retval;

	then = timeval_ms();
	do {
		retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
				armv7r->debug_base + CPUDBG_DSCR, &dscr);
		if (retval != ERROR_OK) {
			LOG_ERROR("Could not read DSCR register");
			return retval;
		}
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for cortex_r4_exec_opcode");
			return ERROR_FAIL;
		}
	} while ((dscr & DSCR_INSTR_COMP) == 0);	/* Wait for InstrCompl bit to be set */

	if (dscr_p)
		*dscr_p = dscr;

	return retval;
}

/**************************************************************************
Read core register with very few exec_opcode, fast but needs work_area.
This can cause problems with MMU active.
**************************************************************************/
static int cortex_r4_read_regs_through_mem(struct target *target, uint32_t address,
	uint32_t *regfile)
{
	int retval = ERROR_OK;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;

	retval = cortex_r4_dap_read_coreregister_u32(target, regfile, 0);
	if (retval != ERROR_OK)
		return retval;
	retval = cortex_r4_dap_write_coreregister_u32(target, address, 0);
	if (retval != ERROR_OK)
		return retval;
	retval = cortex_r4_exec_opcode(target, ARMV4_5_STMIA(0, 0xFFFE, 0, 0), NULL);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_read_buf_u32(swjdp, swjdp_memoryap,
			(uint8_t *)(&regfile[1]), 4*15, address);

	return retval;
}

static int cortex_r4_dap_read_coreregister_u32(struct target *target,
	uint32_t *value, int regnum)
{
	int retval = ERROR_OK;
	uint8_t reg = regnum&0xFF;
	uint32_t dscr = 0;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;

	if (reg > 17)
		return retval;

	if (reg < 15) {
		/* Rn to DCCTX, "MCR p14, 0, Rn, c0, c5, 0"  0xEE00nE15 */
		retval = cortex_r4_exec_opcode(target,
				ARMV4_5_MCR(14, 0, reg, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	} else if (reg == 15) {
		/* "MOV r0, r15"; then move r0 to DCCTX */
		retval = cortex_r4_exec_opcode(target, 0xE1A0000F, &dscr);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_r4_exec_opcode(target,
				ARMV4_5_MCR(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	} else {
		/* "MRS r0, CPSR" or "MRS r0, SPSR"
		 * then move r0 to DCCTX
		 */
		retval = cortex_r4_exec_opcode(target, ARMV4_5_MRS(0, reg & 1), &dscr);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_r4_exec_opcode(target,
				ARMV4_5_MCR(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	}

	/* Wait for DTRRXfull then read DTRRTX */
	long long then = timeval_ms();
	while ((dscr & DSCR_DTR_TX_FULL) == 0) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
				armv7r->debug_base + CPUDBG_DSCR, &dscr);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for cortex_r4_exec_opcode");
			return ERROR_FAIL;
		}
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DTRTX, value);
	LOG_DEBUG("read DCC 0x%08" PRIx32, *value);

	return retval;
}

static int cortex_r4_dap_write_coreregister_u32(struct target *target,
	uint32_t value, int regnum)
{
	int retval = ERROR_OK;
	uint8_t Rd = regnum&0xFF;
	uint32_t dscr;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;

	LOG_DEBUG("register %i, value 0x%08" PRIx32, regnum, value);

	/* Check that DCCRX is not full */
	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;
	if (dscr & DSCR_DTR_RX_FULL) {
		LOG_ERROR("DSCR_DTR_RX_FULL, dscr 0x%08" PRIx32, dscr);
		/* Clear DCCRX with MRC(p14, 0, Rd, c0, c5, 0), opcode  0xEE100E15 */
		retval = cortex_r4_exec_opcode(target, ARMV4_5_MRC(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	}

	if (Rd > 17)
		return retval;

	/* Write DTRRX ... sets DSCR.DTRRXfull but exec_opcode() won't care */
	LOG_DEBUG("write DCC 0x%08" PRIx32, value);
	retval = mem_ap_sel_write_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DTRRX, value);
	if (retval != ERROR_OK)
		return retval;

	if (Rd < 15) {
		/* DCCRX to Rn, "MRC p14, 0, Rn, c0, c5, 0", 0xEE10nE15 */
		retval = cortex_r4_exec_opcode(target, ARMV4_5_MRC(14, 0, Rd, 0, 5, 0),
				&dscr);

		if (retval != ERROR_OK)
			return retval;
	} else if (Rd == 15) {
		/* DCCRX to R0, "MRC p14, 0, R0, c0, c5, 0", 0xEE100E15
		 * then "mov r15, r0"
		 */
		retval = cortex_r4_exec_opcode(target, ARMV4_5_MRC(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_r4_exec_opcode(target, 0xE1A0F000, &dscr);
		if (retval != ERROR_OK)
			return retval;
	} else {
		/* DCCRX to R0, "MRC p14, 0, R0, c0, c5, 0", 0xEE100E15
		 * then "MSR CPSR_cxsf, r0" or "MSR SPSR_cxsf, r0" (all fields)
		 */
		retval = cortex_r4_exec_opcode(target, ARMV4_5_MRC(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_r4_exec_opcode(target, ARMV4_5_MSR_GP(0, 0xF, Rd & 1),
				&dscr);
		if (retval != ERROR_OK)
			return retval;

		/* "Prefetch flush" after modifying execution status in CPSR */
		if (Rd == 16) {
			retval = cortex_r4_exec_opcode(target,
					ARMV4_5_MCR(15, 0, 0, 7, 5, 4),
					&dscr);
			if (retval != ERROR_OK)
				return retval;
		}
	}

	return retval;
}

/* Write to memory mapped registers directly with no cache or mmu handling */
static int cortex_r4_dap_write_memap_register_u32(struct target *target,
	uint32_t address,
	uint32_t value)
{
	int retval;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;

	retval = mem_ap_sel_write_atomic_u32(swjdp, swjdp_debugap, address, value);

	return retval;
}

/*
 * Cortex-R4 implementation of Debug Programmer's Model
 *
 * NOTE the invariant:  these routines return with DSCR_INSTR_COMP set,
 * so there's no need to poll for it before executing an instruction.
 *
 * NOTE that in several of these cases the "stall" mode might be useful.
 * It'd let us queue a few operations together... prepare/finish might
 * be the places to enable/disable that mode.
 */

static inline struct cortex_r4_common *dpm_to_r4(struct arm_dpm *dpm)
{
	return container_of(dpm, struct cortex_r4_common, armv7r_common.dpm);
}

static int cortex_r4_write_dcc(struct cortex_r4_common *r4, uint32_t data)
{
	LOG_DEBUG("write DCC 0x%08" PRIx32, data);
	return mem_ap_sel_write_u32(r4->armv7r_common.arm.dap,
			swjdp_debugap, r4->armv7r_common.debug_base + CPUDBG_DTRRX, data);
}

static int cortex_r4_read_dcc(struct cortex_r4_common *r4, uint32_t *data,
	uint32_t *dscr_p)
{
	struct adiv5_dap *swjdp = r4->armv7r_common.arm.dap;
	uint32_t dscr = DSCR_INSTR_COMP;
	int retval;

	if (dscr_p)
		dscr = *dscr_p;

	/* Wait for DTRRXfull */
	long long then = timeval_ms();
	while ((dscr & DSCR_DTR_TX_FULL) == 0) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
				r4->armv7r_common.debug_base + CPUDBG_DSCR,
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for read dcc");
			return ERROR_FAIL;
		}
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			r4->armv7r_common.debug_base + CPUDBG_DTRTX, data);
	if (retval != ERROR_OK)
		return retval;
	/* LOG_DEBUG("read DCC 0x%08" PRIx32, *data); */

	if (dscr_p)
		*dscr_p = dscr;

	return retval;
}

static int cortex_r4_dpm_prepare(struct arm_dpm *dpm)
{
	struct cortex_r4_common *r4 = dpm_to_r4(dpm);
	struct adiv5_dap *swjdp = r4->armv7r_common.arm.dap;
	uint32_t dscr;
	int retval;

	/* set up invariant:  INSTR_COMP is set after ever DPM operation */
	long long then = timeval_ms();
	for (;; ) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
				r4->armv7r_common.debug_base + CPUDBG_DSCR,
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		if ((dscr & DSCR_INSTR_COMP) != 0)
			break;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for dpm prepare");
			return ERROR_FAIL;
		}
	}

	/* this "should never happen" ... */
	if (dscr & DSCR_DTR_RX_FULL) {
		LOG_ERROR("DSCR_DTR_RX_FULL, dscr 0x%08" PRIx32, dscr);
		/* Clear DCCRX */
		retval = cortex_r4_exec_opcode(
				r4->armv7r_common.arm.target,
				ARMV4_5_MRC(14, 0, 0, 0, 5, 0),
				&dscr);
		if (retval != ERROR_OK)
			return retval;
	}

	return retval;
}

static int cortex_r4_dpm_finish(struct arm_dpm *dpm)
{
	/* REVISIT what could be done here? */
	return ERROR_OK;
}

static int cortex_r4_instr_write_data_dcc(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t data)
{
	struct cortex_r4_common *r4 = dpm_to_r4(dpm);
	int retval;
	uint32_t dscr = DSCR_INSTR_COMP;

	retval = cortex_r4_write_dcc(r4, data);
	if (retval != ERROR_OK)
		return retval;

	return cortex_r4_exec_opcode(
			r4->armv7r_common.arm.target,
			opcode,
			&dscr);
}

static int cortex_r4_instr_write_data_r0(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t data)
{
	struct cortex_r4_common *r4 = dpm_to_r4(dpm);
	uint32_t dscr = DSCR_INSTR_COMP;
	int retval;

	retval = cortex_r4_write_dcc(r4, data);
	if (retval != ERROR_OK)
		return retval;

	/* DCCRX to R0, "MCR p14, 0, R0, c0, c5, 0", 0xEE000E15 */
	retval = cortex_r4_exec_opcode(
			r4->armv7r_common.arm.target,
			ARMV4_5_MRC(14, 0, 0, 0, 5, 0),
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	/* then the opcode, taking data from R0 */
	retval = cortex_r4_exec_opcode(
			r4->armv7r_common.arm.target,
			opcode,
			&dscr);

	return retval;
}

static int cortex_r4_instr_cpsr_sync(struct arm_dpm *dpm)
{
	struct target *target = dpm->arm->target;
	uint32_t dscr = DSCR_INSTR_COMP;

	/* "Prefetch flush" after modifying execution status in CPSR */
	return cortex_r4_exec_opcode(target,
			ARMV4_5_MCR(15, 0, 0, 7, 5, 4),
			&dscr);
}

static int cortex_r4_instr_read_data_dcc(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t *data)
{
	struct cortex_r4_common *r4 = dpm_to_r4(dpm);
	int retval;
	uint32_t dscr = DSCR_INSTR_COMP;

	/* the opcode, writing data to DCC */
	retval = cortex_r4_exec_opcode(
			r4->armv7r_common.arm.target,
			opcode,
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	return cortex_r4_read_dcc(r4, data, &dscr);
}


static int cortex_r4_instr_read_data_r0(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t *data)
{
	struct cortex_r4_common *r4 = dpm_to_r4(dpm);
	uint32_t dscr = DSCR_INSTR_COMP;
	int retval;

	/* the opcode, writing data to R0 */
	retval = cortex_r4_exec_opcode(
			r4->armv7r_common.arm.target,
			opcode,
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	/* write R0 to DCC */
	retval = cortex_r4_exec_opcode(
			r4->armv7r_common.arm.target,
			ARMV4_5_MCR(14, 0, 0, 0, 5, 0),
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	return cortex_r4_read_dcc(r4, data, &dscr);
}

static int cortex_r4_bpwp_enable(struct arm_dpm *dpm, unsigned index_t,
	uint32_t addr, uint32_t control)
{
	struct cortex_r4_common *r4 = dpm_to_r4(dpm);
	uint32_t vr = r4->armv7r_common.debug_base;
	uint32_t cr = r4->armv7r_common.debug_base;
	int retval;

	switch (index_t) {
		case 0 ... 15:	/* breakpoints */
			vr += CPUDBG_BVR_BASE;
			cr += CPUDBG_BCR_BASE;
			break;
		case 16 ... 31:	/* watchpoints */
			vr += CPUDBG_WVR_BASE;
			cr += CPUDBG_WCR_BASE;
			index_t -= 16;
			break;
		default:
			return ERROR_FAIL;
	}
	vr += 4 * index_t;
	cr += 4 * index_t;

	LOG_DEBUG("R4: bpwp enable, vr %08x cr %08x",
		(unsigned) vr, (unsigned) cr);

	retval = cortex_r4_dap_write_memap_register_u32(dpm->arm->target,
			vr, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = cortex_r4_dap_write_memap_register_u32(dpm->arm->target,
			cr, control);
	return retval;
}

static int cortex_r4_bpwp_disable(struct arm_dpm *dpm, unsigned index_t)
{
	struct cortex_r4_common *r4 = dpm_to_r4(dpm);
	uint32_t cr;

	switch (index_t) {
		case 0 ... 15:
			cr = r4->armv7r_common.debug_base + CPUDBG_BCR_BASE;
			break;
		case 16 ... 31:
			cr = r4->armv7r_common.debug_base + CPUDBG_WCR_BASE;
			index_t -= 16;
			break;
		default:
			return ERROR_FAIL;
	}
	cr += 4 * index_t;

	LOG_DEBUG("r4: bpwp disable, cr %08x", (unsigned) cr);

	/* clear control register */
	return cortex_r4_dap_write_memap_register_u32(dpm->arm->target, cr, 0);
}

static int cortex_r4_dpm_setup(struct cortex_r4_common *r4, uint32_t didr)
{
	struct arm_dpm *dpm = &r4->armv7r_common.dpm;
	int retval;

	dpm->arm = &r4->armv7r_common.arm;
	dpm->didr = didr;

	dpm->prepare = cortex_r4_dpm_prepare;
	dpm->finish = cortex_r4_dpm_finish;

	dpm->instr_write_data_dcc = cortex_r4_instr_write_data_dcc;
	dpm->instr_write_data_r0 = cortex_r4_instr_write_data_r0;
	dpm->instr_cpsr_sync = cortex_r4_instr_cpsr_sync;

	dpm->instr_read_data_dcc = cortex_r4_instr_read_data_dcc;
	dpm->instr_read_data_r0 = cortex_r4_instr_read_data_r0;

	dpm->bpwp_enable = cortex_r4_bpwp_enable;
	dpm->bpwp_disable = cortex_r4_bpwp_disable;

	retval = arm_dpm_setup(dpm);
	if (retval == ERROR_OK)
		retval = arm_dpm_initialize(dpm);

	return retval;
}
static struct target *get_cortex_r4(struct target *target, int32_t coreid)
{
	struct target_list *head;
	struct target *curr;

	head = target->head;
	while (head != (struct target_list *)NULL) {
		curr = head->target;
		if ((curr->coreid == coreid) && (curr->state == TARGET_HALTED))
			return curr;
		head = head->next;
	}
	return target;
}
static int cortex_r4_halt(struct target *target);

static int cortex_r4_halt_smp(struct target *target)
{
	int retval = 0;
	struct target_list *head;
	struct target *curr;
	head = target->head;
	while (head != (struct target_list *)NULL) {
		curr = head->target;
		if ((curr != target) && (curr->state != TARGET_HALTED))
			retval += cortex_r4_halt(curr);
		head = head->next;
	}
	return retval;
}

static int update_halt_gdb(struct target *target)
{
	int retval = 0;
	if (target->gdb_service->core[0] == -1) {
		target->gdb_service->target = target;
		target->gdb_service->core[0] = target->coreid;
		retval += cortex_r4_halt_smp(target);
	}
	return retval;
}

/*
 * Cortex-R4 Run control
 */

static int cortex_r4_poll(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t dscr;
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);
	struct armv7r_common *armv7r = &cortex_r4->armv7r_common;
	struct adiv5_dap *swjdp = armv7r->arm.dap;
	enum target_state prev_target_state = target->state;
	/*  toggle to another core is done by gdb as follow */
	/*  maint packet J core_id */
	/*  continue */
	/*  the next polling trigger an halt event sent to gdb */
	if ((target->state == TARGET_HALTED) && (target->smp) &&
		(target->gdb_service) &&
		(target->gdb_service->target == NULL)) {
		target->gdb_service->target =
			get_cortex_r4(target, target->gdb_service->core[1]);
		target_call_event_callbacks(target, TARGET_EVENT_HALTED);
		return retval;
	}
	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;
	cortex_r4->cpudbg_dscr = dscr;

	if (DSCR_RUN_MODE(dscr) == (DSCR_CORE_HALTED | DSCR_CORE_RESTARTED)) {
		if (prev_target_state != TARGET_HALTED) {
			/* We have a halting debug event */
			LOG_DEBUG("Target halted");
			target->state = TARGET_HALTED;
			if ((prev_target_state == TARGET_RUNNING)
				|| (prev_target_state == TARGET_RESET)) {
				retval = cortex_r4_debug_entry(target);
				if (retval != ERROR_OK)
					return retval;
				if (target->smp) {
					retval = update_halt_gdb(target);
					if (retval != ERROR_OK)
						return retval;
				}
				target_call_event_callbacks(target,
					TARGET_EVENT_HALTED);
			}
			if (prev_target_state == TARGET_DEBUG_RUNNING) {
				LOG_DEBUG(" ");

				retval = cortex_r4_debug_entry(target);
				if (retval != ERROR_OK)
					return retval;
				if (target->smp) {
					retval = update_halt_gdb(target);
					if (retval != ERROR_OK)
						return retval;
				}

				target_call_event_callbacks(target,
					TARGET_EVENT_DEBUG_HALTED);
			}
		}
	} else if (DSCR_RUN_MODE(dscr) == DSCR_CORE_RESTARTED)
		target->state = TARGET_RUNNING;
	else {
		LOG_DEBUG("Unknown target state dscr = 0x%08" PRIx32, dscr);
		target->state = TARGET_UNKNOWN;
	}

	return retval;
}

static int cortex_r4_halt(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t dscr;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;

	/*
	 * Tell the core to be halted by writing DRCR with 0x1
	 * and then wait for the core to be halted.
	 */
	retval = mem_ap_sel_write_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DRCR, DRCR_HALT);
	if (retval != ERROR_OK)
		return retval;

	/*
	 * enter halting debug mode
	 */
	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_write_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DSCR, dscr | DSCR_HALT_DBG_MODE);
	if (retval != ERROR_OK)
		return retval;

	long long then = timeval_ms();
	for (;; ) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
				armv7r->debug_base + CPUDBG_DSCR, &dscr);
		if (retval != ERROR_OK)
			return retval;
		if ((dscr & DSCR_CORE_HALTED) != 0)
			break;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for halt");
			return ERROR_FAIL;
		}
	}

	target->debug_reason = DBG_REASON_DBGRQ;

	return ERROR_OK;
}

static int cortex_r4_internal_restore(struct target *target, int current,
	uint32_t *address, int handle_breakpoints, int debug_execution)
{
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm *arm = &armv7r->arm;
	int retval;
	uint32_t resume_pc;

	if (!debug_execution)
		target_free_all_working_areas(target);

#if 0
	if (debug_execution) {
		/* Disable interrupts */
		/* We disable interrupts in the PRIMASK register instead of
		 * masking with C_MASKINTS,
		 * This is probably the same issue as Cortex-M3 Errata 377493:
		 * C_MASKINTS in parallel with disabled interrupts can cause
		 * local faults to not be taken. */
		buf_set_u32(armv7m->core_cache->reg_list[ARMV7M_PRIMASK].value, 0, 32, 1);
		armv7m->core_cache->reg_list[ARMV7M_PRIMASK].dirty = 1;
		armv7m->core_cache->reg_list[ARMV7M_PRIMASK].valid = 1;

		/* Make sure we are in Thumb mode */
		buf_set_u32(armv7m->core_cache->reg_list[ARMV7M_xPSR].value, 0, 32,
			buf_get_u32(armv7m->core_cache->reg_list[ARMV7M_xPSR].value, 0,
			32) | (1 << 24));
		armv7m->core_cache->reg_list[ARMV7M_xPSR].dirty = 1;
		armv7m->core_cache->reg_list[ARMV7M_xPSR].valid = 1;
	}
#endif

	/* current = 1: continue on current pc, otherwise continue at <address> */
	resume_pc = buf_get_u32(arm->pc->value, 0, 32);
	if (!current)
		resume_pc = *address;
	else
		*address = resume_pc;

	/* Make sure that the Armv7 gdb thumb fixups does not
	 * kill the return address
	 */
	switch (arm->core_state) {
		case ARM_STATE_ARM:
			resume_pc &= 0xFFFFFFFC;
			break;
		case ARM_STATE_THUMB:
		case ARM_STATE_THUMB_EE:
			/* When the return address is loaded into PC
			 * bit 0 must be 1 to stay in Thumb state
			 */
			resume_pc |= 0x1;
			break;
		case ARM_STATE_JAZELLE:
			LOG_ERROR("How do I resume into Jazelle state??");
			return ERROR_FAIL;
	}
	LOG_DEBUG("resume pc = 0x%08" PRIx32, resume_pc);
	buf_set_u32(arm->pc->value, 0, 32, resume_pc);
	arm->pc->dirty = 1;
	arm->pc->valid = 1;
	/* restore dpm_mode at system halt */
	dpm_modeswitch(&armv7r->dpm, ARM_MODE_ANY);
	/* called it now before restoring context because it uses cpu
	 * register r0 for restoring cp15 control register */
	retval = cortex_r4_restore_cp15_control_reg(target);
	if (retval != ERROR_OK)
		return retval;
	retval = cortex_r4_restore_context(target, handle_breakpoints);
	if (retval != ERROR_OK)
		return retval;
	target->debug_reason = DBG_REASON_NOTHALTED;
	target->state = TARGET_RUNNING;

	/* registers are now invalid */
	register_cache_invalidate(arm->core_cache);

#if 0
	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		/* Single step past breakpoint at current address */
		breakpoint = breakpoint_find(target, resume_pc);
		if (breakpoint) {
			LOG_DEBUG("unset breakpoint at 0x%8.8x", breakpoint->address);
			cortex_m3_unset_breakpoint(target, breakpoint);
			cortex_m3_single_step_core(target);
			cortex_m3_set_breakpoint(target, breakpoint);
		}
	}

#endif
	return retval;
}

static int cortex_r4_internal_restart(struct target *target)
{
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm *arm = &armv7r->arm;
	struct adiv5_dap *swjdp = arm->dap;
	int retval;
	uint32_t dscr;
	/*
	 * * Restart core and wait for it to be started.  Clear ITRen and sticky
	 * * exception flags: see ARMv7 ARM, C5.9.
	 *
	 * REVISIT: for single stepping, we probably want to
	 * disable IRQs by default, with optional override...
	 */

	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;

	if ((dscr & DSCR_INSTR_COMP) == 0)
		LOG_ERROR("DSCR InstrCompl must be set before leaving debug!");

	retval = mem_ap_sel_write_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DSCR, dscr & ~DSCR_ITR_EN);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_write_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DRCR, DRCR_RESTART |
			DRCR_CLEAR_EXCEPTIONS);
	if (retval != ERROR_OK)
		return retval;

	long long then = timeval_ms();
	for (;; ) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
				armv7r->debug_base + CPUDBG_DSCR, &dscr);
		if (retval != ERROR_OK)
			return retval;
		if ((dscr & DSCR_CORE_RESTARTED) != 0)
			break;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for resume");
			return ERROR_FAIL;
		}
	}

	target->debug_reason = DBG_REASON_NOTHALTED;
	target->state = TARGET_RUNNING;

	/* registers are now invalid */
	register_cache_invalidate(arm->core_cache);

	return ERROR_OK;
}

static int cortex_r4_restore_smp(struct target *target, int handle_breakpoints)
{
	int retval = 0;
	struct target_list *head;
	struct target *curr;
	uint32_t address;
	head = target->head;
	while (head != (struct target_list *)NULL) {
		curr = head->target;
		if ((curr != target) && (curr->state != TARGET_RUNNING)) {
			/*  resume current address , not in step mode */
			retval += cortex_r4_internal_restore(curr, 1, &address,
					handle_breakpoints, 0);
			retval += cortex_r4_internal_restart(curr);
		}
		head = head->next;

	}
	return retval;
}

static int cortex_r4_resume(struct target *target, int current,
	uint32_t address, int handle_breakpoints, int debug_execution)
{
	int retval = 0;
	/* dummy resume for smp toggle in order to reduce gdb impact  */
	if ((target->smp) && (target->gdb_service->core[1] != -1)) {
		/*   simulate a start and halt of target */
		target->gdb_service->target = NULL;
		target->gdb_service->core[0] = target->gdb_service->core[1];
		/*  fake resume at next poll we play the  target core[1], see poll*/
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		return 0;
	}
	cortex_r4_internal_restore(target, current, &address, handle_breakpoints, debug_execution);
	if (target->smp) {
		target->gdb_service->core[0] = -1;
		retval = cortex_r4_restore_smp(target, handle_breakpoints);
		if (retval != ERROR_OK)
			return retval;
	}
	cortex_r4_internal_restart(target);

	if (!debug_execution) {
		target->state = TARGET_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		LOG_DEBUG("target resumed at 0x%" PRIx32, address);
	} else {
		target->state = TARGET_DEBUG_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
		LOG_DEBUG("target debug resumed at 0x%" PRIx32, address);
	}

	return ERROR_OK;
}

static int cortex_r4_debug_entry(struct target *target)
{
	int i;
	uint32_t regfile[16], cpsr, dscr;
	int retval = ERROR_OK;
	struct working_area *regfile_working_area = NULL;
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm *arm = &armv7r->arm;
	struct adiv5_dap *swjdp = armv7r->arm.dap;
	struct reg *reg;

	LOG_DEBUG("dscr = 0x%08" PRIx32, cortex_r4->cpudbg_dscr);

	/* REVISIT surely we should not re-read DSCR !! */
	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;

	/* REVISIT see R4 TRM 12.11.4 steps 2..3 -- make sure that any
	 * imprecise data aborts get discarded by issuing a Data
	 * Synchronization Barrier:  ARMV4_5_MCR(15, 0, 0, 7, 10, 4).
	 */

	/* Enable the ITR execution once we are in debug mode */
	dscr |= DSCR_ITR_EN;
	retval = mem_ap_sel_write_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DSCR, dscr);
	if (retval != ERROR_OK)
		return retval;

	/* Examine debug reason */
	arm_dpm_report_dscr(&armv7r->dpm, cortex_r4->cpudbg_dscr);

	/* save address of instruction that triggered the watchpoint? */
	if (target->debug_reason == DBG_REASON_WATCHPOINT) {
		uint32_t wfar;

		retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
				armv7r->debug_base + CPUDBG_WFAR,
				&wfar);
		if (retval != ERROR_OK)
			return retval;
		arm_dpm_report_wfar(&armv7r->dpm, wfar);
	}

	/* REVISIT fast_reg_read is never set ... */

	/* Examine target state and mode */
	if (cortex_r4->fast_reg_read)
		target_alloc_working_area(target, 64, &regfile_working_area);

	/* First load register acessible through core debug port*/
	if (!regfile_working_area)
		retval = arm_dpm_read_current_registers(&armv7r->dpm);
	else {
		retval = cortex_r4_read_regs_through_mem(target,
				regfile_working_area->address, regfile);

		target_free_working_area(target, regfile_working_area);
		if (retval != ERROR_OK)
			return retval;

		/* read Current PSR */
		retval = cortex_r4_dap_read_coreregister_u32(target, &cpsr, 16);
		/*  store current cpsr */
		if (retval != ERROR_OK)
			return retval;

		LOG_DEBUG("cpsr: %8.8" PRIx32, cpsr);

		arm_set_cpsr(arm, cpsr);

		/* update cache */
		for (i = 0; i <= ARM_PC; i++) {
			reg = arm_reg_current(arm, i);

			buf_set_u32(reg->value, 0, 32, regfile[i]);
			reg->valid = 1;
			reg->dirty = 0;
		}

		/* Fixup PC Resume Address */
		if (cpsr & (1 << 5)) {
			/* T bit set for Thumb or ThumbEE state */
			regfile[ARM_PC] -= 4;
		} else {
			/* ARM state */
			regfile[ARM_PC] -= 8;
		}

		reg = arm->pc;
		buf_set_u32(reg->value, 0, 32, regfile[ARM_PC]);
		reg->dirty = reg->valid;
	}

#if 0
/* TODO, Move this */
	uint32_t cp15_control_register, cp15_cacr, cp15_nacr;
	cortex_r4_read_cp(target, &cp15_control_register, 15, 0, 1, 0, 0);
	LOG_DEBUG("cp15_control_register = 0x%08x", cp15_control_register);

	cortex_r4_read_cp(target, &cp15_cacr, 15, 0, 1, 0, 2);
	LOG_DEBUG("cp15 Coprocessor Access Control Register = 0x%08x", cp15_cacr);

	cortex_r4_read_cp(target, &cp15_nacr, 15, 0, 1, 1, 2);
	LOG_DEBUG("cp15 Nonsecure Access Control Register = 0x%08x", cp15_nacr);
#endif

	/* Are we in an exception handler */
/*	armv4_5->exception_number = 0; */
	if (armv7r->post_debug_entry) {
		retval = armv7r->post_debug_entry(target);
		if (retval != ERROR_OK)
			return retval;
	}

	return retval;
}

static int cortex_r4_post_debug_entry(struct target *target)
{
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);
	struct armv7r_common *armv7r = &cortex_r4->armv7r_common;
	int retval;

	/* MRC p15,0,<Rt>,c1,c0,0 ; Read CP15 System Control Register */
	retval = armv7r->arm.mrc(target, 15,
			0, 0,	/* op1, op2 */
			1, 0,	/* CRn, CRm */
			&cortex_r4->cp15_control_reg);
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("cp15_control_reg: %8.8" PRIx32, cortex_r4->cp15_control_reg);
	cortex_r4->cp15_control_reg_curr = cortex_r4->cp15_control_reg;

	if (armv7r->armv7r_cache.ctype == -1)
		armv7r_identify_cache(target);

	armv7r->armv7r_cache.d_u_cache_enabled =
		(cortex_r4->cp15_control_reg & 0x4U) ? 1 : 0;
	armv7r->armv7r_cache.i_cache_enabled =
		(cortex_r4->cp15_control_reg & 0x1000U) ? 1 : 0;
	cortex_r4->curr_mode = armv7r->arm.core_mode;

	return ERROR_OK;
}

static int cortex_r4_step(struct target *target, int current, uint32_t address,
	int handle_breakpoints)
{
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm *arm = &armv7r->arm;
	struct breakpoint *breakpoint = NULL;
	struct breakpoint stepbreakpoint;
	struct reg *r;
	int retval;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* current = 1: continue on current pc, otherwise continue at <address> */
	r = arm->pc;
	if (!current)
		buf_set_u32(r->value, 0, 32, address);
	else
		address = buf_get_u32(r->value, 0, 32);

	/* The front-end may request us not to handle breakpoints.
	 * But since Cortex-R4 uses breakpoint for single step,
	 * we MUST handle breakpoints.
	 */
	handle_breakpoints = 1;
	if (handle_breakpoints) {
		breakpoint = breakpoint_find(target, address);
		if (breakpoint)
			cortex_r4_unset_breakpoint(target, breakpoint);
	}

	/* Setup single step breakpoint */
	stepbreakpoint.address = address;
	stepbreakpoint.length = (arm->core_state == ARM_STATE_THUMB)
		? 2 : 4;
	stepbreakpoint.type = BKPT_HARD;
	stepbreakpoint.set = 0;

	/* Break on IVA mismatch */
	cortex_r4_set_breakpoint(target, &stepbreakpoint, 0x04);

	target->debug_reason = DBG_REASON_SINGLESTEP;

	retval = cortex_r4_resume(target, 1, address, 0, 0);
	if (retval != ERROR_OK)
		return retval;

	long long then = timeval_ms();
	while (target->state != TARGET_HALTED) {
		retval = cortex_r4_poll(target);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("timeout waiting for target halt");
			return ERROR_FAIL;
		}
	}

	cortex_r4_unset_breakpoint(target, &stepbreakpoint);

	target->debug_reason = DBG_REASON_BREAKPOINT;

	if (breakpoint)
		cortex_r4_set_breakpoint(target, breakpoint, 0);

	if (target->state != TARGET_HALTED)
		LOG_DEBUG("target stepped");

	return ERROR_OK;
}

static int cortex_r4_restore_context(struct target *target, bool bpwp)
{
	struct armv7r_common *armv7r = target_to_armv7r(target);

	LOG_DEBUG(" ");

	if (armv7r->pre_restore_context)
		armv7r->pre_restore_context(target);

	return arm_dpm_write_dirty_registers(&armv7r->dpm, bpwp);
}

/*
 * Cortex-R4 Breakpoint and watchpoint functions
 */

/* Setup hardware Breakpoint Register Pair */
static int cortex_r4_set_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode)
{
	int retval;
	int brp_i = 0;
	uint32_t control;
	uint8_t byte_addr_select = 0x0F;
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);
	struct armv7r_common *armv7r = &cortex_r4->armv7r_common;
	struct cortex_r4_brp *brp_list = cortex_r4->brp_list;

	if (breakpoint->set) {
		LOG_WARNING("breakpoint already set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		while (brp_list[brp_i].used && (brp_i < cortex_r4->brp_num))
			brp_i++;
		if (brp_i >= cortex_r4->brp_num) {
			LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
		breakpoint->set = brp_i + 1;
		if (breakpoint->length == 2)
			byte_addr_select = (3 << (breakpoint->address & 0x02));
		control = ((matchmode & 0x7) << 20)
			| (byte_addr_select << 5)
			| (3 << 1) | 1;
		brp_list[brp_i].used = 1;
		brp_list[brp_i].value = (breakpoint->address & 0xFFFFFFFC);
		brp_list[brp_i].control = control;
		retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
				+ CPUDBG_BVR_BASE + 4 * brp_list[brp_i].BRPn,
				brp_list[brp_i].value);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
				+ CPUDBG_BCR_BASE + 4 * brp_list[brp_i].BRPn,
				brp_list[brp_i].control);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("brp %i control 0x%0" PRIx32 " value 0x%0" PRIx32, brp_i,
			brp_list[brp_i].control,
			brp_list[brp_i].value);
	} else if (breakpoint->type == BKPT_SOFT) {
		uint8_t code[4];
		if (breakpoint->length == 2)
			buf_set_u32(code, 0, 32, ARMV5_T_BKPT(0x11));
		else
			buf_set_u32(code, 0, 32, ARMV5_BKPT(0x11));
		retval = target->type->read_memory(target,
				breakpoint->address & 0xFFFFFFFE,
				breakpoint->length, 1,
				breakpoint->orig_instr);
		if (retval != ERROR_OK)
			return retval;
		retval = target->type->write_memory(target,
				breakpoint->address & 0xFFFFFFFE,
				breakpoint->length, 1, code);
		if (retval != ERROR_OK)
			return retval;
		breakpoint->set = 0x11;	/* Any nice value but 0 */
	}

	return ERROR_OK;
}

static int cortex_r4_set_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode)
{
	int retval = ERROR_FAIL;
	int brp_i = 0;
	uint32_t control;
	uint8_t byte_addr_select = 0x0F;
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);
	struct armv7r_common *armv7r = &cortex_r4->armv7r_common;
	struct cortex_r4_brp *brp_list = cortex_r4->brp_list;

	if (breakpoint->set) {
		LOG_WARNING("breakpoint already set");
		return retval;
	}
	/*check available context BRPs*/
	while ((brp_list[brp_i].used ||
		(brp_list[brp_i].type != BRP_CONTEXT)) && (brp_i < cortex_r4->brp_num))
		brp_i++;

	if (brp_i >= cortex_r4->brp_num) {
		LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
		return ERROR_FAIL;
	}

	breakpoint->set = brp_i + 1;
	control = ((matchmode & 0x7) << 20)
		| (byte_addr_select << 5)
		| (3 << 1) | 1;
	brp_list[brp_i].used = 1;
	brp_list[brp_i].value = (breakpoint->asid);
	brp_list[brp_i].control = control;
	retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
			+ CPUDBG_BVR_BASE + 4 * brp_list[brp_i].BRPn,
			brp_list[brp_i].value);
	if (retval != ERROR_OK)
		return retval;
	retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
			+ CPUDBG_BCR_BASE + 4 * brp_list[brp_i].BRPn,
			brp_list[brp_i].control);
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("brp %i control 0x%0" PRIx32 " value 0x%0" PRIx32, brp_i,
		brp_list[brp_i].control,
		brp_list[brp_i].value);
	return ERROR_OK;

}

static int cortex_r4_set_hybrid_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	int retval = ERROR_FAIL;
	int brp_1 = 0;	/* holds the contextID pair */
	int brp_2 = 0;	/* holds the IVA pair */
	uint32_t control_CTX, control_IVA;
	uint8_t CTX_byte_addr_select = 0x0F;
	uint8_t IVA_byte_addr_select = 0x0F;
	uint8_t CTX_machmode = 0x03;
	uint8_t IVA_machmode = 0x01;
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);
	struct armv7r_common *armv7r = &cortex_r4->armv7r_common;
	struct cortex_r4_brp *brp_list = cortex_r4->brp_list;

	if (breakpoint->set) {
		LOG_WARNING("breakpoint already set");
		return retval;
	}
	/*check available context BRPs*/
	while ((brp_list[brp_1].used ||
		(brp_list[brp_1].type != BRP_CONTEXT)) && (brp_1 < cortex_r4->brp_num))
		brp_1++;

	printf("brp(CTX) found num: %d\n", brp_1);
	if (brp_1 >= cortex_r4->brp_num) {
		LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
		return ERROR_FAIL;
	}

	while ((brp_list[brp_2].used ||
		(brp_list[brp_2].type != BRP_NORMAL)) && (brp_2 < cortex_r4->brp_num))
		brp_2++;

	printf("brp(IVA) found num: %d\n", brp_2);
	if (brp_2 >= cortex_r4->brp_num) {
		LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
		return ERROR_FAIL;
	}

	breakpoint->set = brp_1 + 1;
	breakpoint->linked_BRP = brp_2;
	control_CTX = ((CTX_machmode & 0x7) << 20)
		| (brp_2 << 16)
		| (0 << 14)
		| (CTX_byte_addr_select << 5)
		| (3 << 1) | 1;
	brp_list[brp_1].used = 1;
	brp_list[brp_1].value = (breakpoint->asid);
	brp_list[brp_1].control = control_CTX;
	retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
			+ CPUDBG_BVR_BASE + 4 * brp_list[brp_1].BRPn,
			brp_list[brp_1].value);
	if (retval != ERROR_OK)
		return retval;
	retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
			+ CPUDBG_BCR_BASE + 4 * brp_list[brp_1].BRPn,
			brp_list[brp_1].control);
	if (retval != ERROR_OK)
		return retval;

	control_IVA = ((IVA_machmode & 0x7) << 20)
		| (brp_1 << 16)
		| (IVA_byte_addr_select << 5)
		| (3 << 1) | 1;
	brp_list[brp_2].used = 1;
	brp_list[brp_2].value = (breakpoint->address & 0xFFFFFFFC);
	brp_list[brp_2].control = control_IVA;
	retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
			+ CPUDBG_BVR_BASE + 4 * brp_list[brp_2].BRPn,
			brp_list[brp_2].value);
	if (retval != ERROR_OK)
		return retval;
	retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
			+ CPUDBG_BCR_BASE + 4 * brp_list[brp_2].BRPn,
			brp_list[brp_2].control);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int cortex_r4_unset_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	int retval;
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);
	struct armv7r_common *armv7r = &cortex_r4->armv7r_common;
	struct cortex_r4_brp *brp_list = cortex_r4->brp_list;

	if (!breakpoint->set) {
		LOG_WARNING("breakpoint not set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		if ((breakpoint->address != 0) && (breakpoint->asid != 0)) {
			int brp_i = breakpoint->set - 1;
			int brp_j = breakpoint->linked_BRP;
			if ((brp_i < 0) || (brp_i >= cortex_r4->brp_num)) {
				LOG_DEBUG("Invalid BRP number in breakpoint");
				return ERROR_OK;
			}
			LOG_DEBUG("rbp %i control 0x%0" PRIx32 " value 0x%0" PRIx32, brp_i,
				brp_list[brp_i].control, brp_list[brp_i].value);
			brp_list[brp_i].used = 0;
			brp_list[brp_i].value = 0;
			brp_list[brp_i].control = 0;
			retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
					+ CPUDBG_BCR_BASE + 4 * brp_list[brp_i].BRPn,
					brp_list[brp_i].control);
			if (retval != ERROR_OK)
				return retval;
			retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
					+ CPUDBG_BVR_BASE + 4 * brp_list[brp_i].BRPn,
					brp_list[brp_i].value);
			if (retval != ERROR_OK)
				return retval;
			if ((brp_j < 0) || (brp_j >= cortex_r4->brp_num)) {
				LOG_DEBUG("Invalid BRP number in breakpoint");
				return ERROR_OK;
			}
			LOG_DEBUG("rbp %i control 0x%0" PRIx32 " value 0x%0" PRIx32, brp_j,
				brp_list[brp_j].control, brp_list[brp_j].value);
			brp_list[brp_j].used = 0;
			brp_list[brp_j].value = 0;
			brp_list[brp_j].control = 0;
			retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
					+ CPUDBG_BCR_BASE + 4 * brp_list[brp_j].BRPn,
					brp_list[brp_j].control);
			if (retval != ERROR_OK)
				return retval;
			retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
					+ CPUDBG_BVR_BASE + 4 * brp_list[brp_j].BRPn,
					brp_list[brp_j].value);
			if (retval != ERROR_OK)
				return retval;
			breakpoint->linked_BRP = 0;
			breakpoint->set = 0;
			return ERROR_OK;

		} else {
			int brp_i = breakpoint->set - 1;
			if ((brp_i < 0) || (brp_i >= cortex_r4->brp_num)) {
				LOG_DEBUG("Invalid BRP number in breakpoint");
				return ERROR_OK;
			}
			LOG_DEBUG("rbp %i control 0x%0" PRIx32 " value 0x%0" PRIx32, brp_i,
				brp_list[brp_i].control, brp_list[brp_i].value);
			brp_list[brp_i].used = 0;
			brp_list[brp_i].value = 0;
			brp_list[brp_i].control = 0;
			retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
					+ CPUDBG_BCR_BASE + 4 * brp_list[brp_i].BRPn,
					brp_list[brp_i].control);
			if (retval != ERROR_OK)
				return retval;
			retval = cortex_r4_dap_write_memap_register_u32(target, armv7r->debug_base
					+ CPUDBG_BVR_BASE + 4 * brp_list[brp_i].BRPn,
					brp_list[brp_i].value);
			if (retval != ERROR_OK)
				return retval;
			breakpoint->set = 0;
			return ERROR_OK;
		}
	} else {
		/* restore original instruction (kept in target endianness) */
		if (breakpoint->length == 4) {
			retval = target->type->write_memory(target,
					breakpoint->address & 0xFFFFFFFE,
					4, 1, breakpoint->orig_instr);
			if (retval != ERROR_OK)
				return retval;
		} else {
			retval = target->type->write_memory(target,
					breakpoint->address & 0xFFFFFFFE,
					2, 1, breakpoint->orig_instr);
			if (retval != ERROR_OK)
				return retval;
		}
	}
	breakpoint->set = 0;

	return ERROR_OK;
}

static int cortex_r4_add_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);

	if ((breakpoint->type == BKPT_HARD) && (cortex_r4->brp_num_available < 1)) {
		LOG_INFO("no hardware breakpoint available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
		cortex_r4->brp_num_available--;

	return cortex_r4_set_breakpoint(target, breakpoint, 0x00);	/* Exact match */
}

static int cortex_r4_add_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);

	if ((breakpoint->type == BKPT_HARD) && (cortex_r4->brp_num_available < 1)) {
		LOG_INFO("no hardware breakpoint available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
		cortex_r4->brp_num_available--;

	return cortex_r4_set_context_breakpoint(target, breakpoint, 0x02);	/* asid match */
}

static int cortex_r4_add_hybrid_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);

	if ((breakpoint->type == BKPT_HARD) && (cortex_r4->brp_num_available < 1)) {
		LOG_INFO("no hardware breakpoint available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
		cortex_r4->brp_num_available--;

	return cortex_r4_set_hybrid_breakpoint(target, breakpoint);	/* ??? */
}


static int cortex_r4_remove_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);

#if 0
/* It is perfectly possible to remove breakpoints while the target is running */
	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}
#endif

	if (breakpoint->set) {
		cortex_r4_unset_breakpoint(target, breakpoint);
		if (breakpoint->type == BKPT_HARD)
			cortex_r4->brp_num_available++;
	}


	return ERROR_OK;
}

/*
 * Cortex-r4 Reset functions
 */

static int cortex_r4_assert_reset(struct target *target)
{
	struct armv7r_common *armv7r = target_to_armv7r(target);

	LOG_DEBUG(" ");

	/* FIXME when halt is requested, make it work somehow... */

	/* Issue some kind of warm reset. */
	if (target_has_event_action(target, TARGET_EVENT_RESET_ASSERT))
		target_handle_event(target, TARGET_EVENT_RESET_ASSERT);
	else if (jtag_get_reset_config() & RESET_HAS_SRST) {
		/* REVISIT handle "pulls" cases, if there's
		 * hardware that needs them to work.
		 */
		jtag_add_reset(0, 1);
	} else {
		LOG_ERROR("%s: how to reset?", target_name(target));
		return ERROR_FAIL;
	}

	/* registers are now invalid */
	register_cache_invalidate(armv7r->arm.core_cache);

	target->state = TARGET_RESET;

	return ERROR_OK;
}

static int cortex_r4_deassert_reset(struct target *target)
{
	int retval;

	LOG_DEBUG(" ");

	/* be certain SRST is off */
	jtag_add_reset(0, 0);

	retval = cortex_r4_poll(target);
	if (retval != ERROR_OK)
		return retval;

	if (target->reset_halt) {
		if (target->state != TARGET_HALTED) {
			LOG_WARNING("%s: ran after reset and before halt ...",
				target_name(target));
			retval = target_halt(target);
			if (retval != ERROR_OK)
				return retval;
		}
	}

	return ERROR_OK;
}

static int cortex_r4_write_apb_ab_memory(struct target *target,
	uint32_t address, uint32_t size,
	uint32_t count, const uint8_t *buffer)
{
	/* write memory through APB-AP */

	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm *arm = &armv7r->arm;
	int total_bytes = count * size;
	int start_byte, nbytes_to_write, i;
	struct reg *reg;
	union _data {
		uint8_t uc_a[4];
		uint32_t ui;
	} data;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	reg = arm_reg_current(arm, 0);
	reg->dirty = 1;
	reg = arm_reg_current(arm, 1);
	reg->dirty = 1;

	retval = cortex_r4_dap_write_coreregister_u32(target, address & 0xFFFFFFFC, 0);
	if (retval != ERROR_OK)
		return retval;

	start_byte = address & 0x3;

	while (total_bytes > 0) {

		nbytes_to_write = 4 - start_byte;
		if (total_bytes < nbytes_to_write)
			nbytes_to_write = total_bytes;

		if (nbytes_to_write != 4) {

			/* execute instruction LDR r1, [r0] */
			retval = cortex_r4_exec_opcode(target,  ARMV4_5_LDR(1, 0), NULL);
			if (retval != ERROR_OK)
				return retval;

			retval = cortex_r4_dap_read_coreregister_u32(target, &data.ui, 1);
			if (retval != ERROR_OK)
				return retval;
		}

		for (i = 0; i < nbytes_to_write; ++i)
			data.uc_a[i + start_byte] = *buffer++;

		retval = cortex_r4_dap_write_coreregister_u32(target, data.ui, 1);
		if (retval != ERROR_OK)
			return retval;

		/* execute instruction STRW r1, [r0], 1 (0xe4801004) */
		retval = cortex_r4_exec_opcode(target, ARMV4_5_STRW_IP(1, 0), NULL);
		if (retval != ERROR_OK)
			return retval;

		total_bytes -= nbytes_to_write;
		start_byte = 0;
	}

	return retval;
}


static int cortex_r4_read_apb_ab_memory(struct target *target,
	uint32_t address, uint32_t size,
	uint32_t count, uint8_t *buffer)
{

	/* read memory through APB-AP */

	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm *arm = &armv7r->arm;
	int total_bytes = count * size;
	int start_byte, nbytes_to_read, i;
	struct reg *reg;
	union _data {
		uint8_t uc_a[4];
		uint32_t ui;
	} data;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	reg = arm_reg_current(arm, 0);
	reg->dirty = 1;
	reg = arm_reg_current(arm, 1);
	reg->dirty = 1;

	retval = cortex_r4_dap_write_coreregister_u32(target, address & 0xFFFFFFFC, 0);
	if (retval != ERROR_OK)
		return retval;

	start_byte = address & 0x3;

	while (total_bytes > 0) {

		/* execute instruction LDRW r1, [r0], 4 (0xe4901004)  */
		retval = cortex_r4_exec_opcode(target,  ARMV4_5_LDRW_IP(1, 0), NULL);
		if (retval != ERROR_OK)
			return retval;

		retval = cortex_r4_dap_read_coreregister_u32(target, &data.ui, 1);
		if (retval != ERROR_OK)
			return retval;

		nbytes_to_read = 4 - start_byte;
		if (total_bytes < nbytes_to_read)
			nbytes_to_read = total_bytes;

		for (i = 0; i < nbytes_to_read; ++i)
			*buffer++ = data.uc_a[i + start_byte];

		total_bytes -= nbytes_to_read;
		start_byte = 0;
	}

	return retval;
}



/*
 * Cortex-R4 Memory access
 *
 * This is same Cortex M3 but we must also use the correct
 * ap number for every access.
 */

static int cortex_r4_read_phys_memory(struct target *target,
	uint32_t address, uint32_t size,
	uint32_t count, uint8_t *buffer)
{
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	uint8_t apsel = swjdp->apsel;
	LOG_DEBUG("Reading memory at real address 0x%x; size %d; count %d",
		address, size, count);

	if (count && buffer) {

		if (apsel == swjdp_memoryap) {

			/* read memory through AHB-AP */

			switch (size) {
				case 4:
					retval = mem_ap_sel_read_buf_u32(swjdp, swjdp_memoryap,
						buffer, 4 * count, address);
					break;
				case 2:
					retval = mem_ap_sel_read_buf_u16(swjdp, swjdp_memoryap,
						buffer, 2 * count, address);
					break;
				case 1:
					retval = mem_ap_sel_read_buf_u8(swjdp, swjdp_memoryap,
						buffer, count, address);
					break;
			}
		} else {

			/* read memory through APB-AP */
			retval = cortex_r4_read_apb_ab_memory(target, address, size, count, buffer);
		}
	}
	return retval;
}

static int cortex_r4_read_memory(struct target *target, uint32_t address,
	uint32_t size, uint32_t count, uint8_t *buffer)
{
	int retval;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;
	uint8_t apsel = swjdp->apsel;

	/* cortex_r4 handles unaligned memory access */
	LOG_DEBUG("Reading memory at address 0x%x; size %d; count %d", address,
		size, count);
	if (apsel == swjdp_memoryap) {
		retval = cortex_r4_read_phys_memory(target, address, size, count, buffer);
	} else {
		retval = cortex_r4_check_address(target, address);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_r4_read_apb_ab_memory(target, address, size, count, buffer);
	}
	return retval;
}

static int cortex_r4_write_phys_memory(struct target *target,
	uint32_t address, uint32_t size,
	uint32_t count, const uint8_t *buffer)
{
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	uint8_t apsel = swjdp->apsel;

	LOG_DEBUG("Writing memory to real address 0x%x; size %d; count %d", address,
		size, count);

	if (count && buffer) {

		if (apsel == swjdp_memoryap) {

			/* write memory through AHB-AP */

			switch (size) {
				case 4:
					retval = mem_ap_sel_write_buf_u32(swjdp, swjdp_memoryap,
						buffer, 4 * count, address);
					break;
				case 2:
					retval = mem_ap_sel_write_buf_u16(swjdp, swjdp_memoryap,
						buffer, 2 * count, address);
					break;
				case 1:
					retval = mem_ap_sel_write_buf_u8(swjdp, swjdp_memoryap,
						buffer, count, address);
					break;
			}

		} else {

			/* write memory through APB-AP */
			return cortex_r4_write_apb_ab_memory(target, address, size, count, buffer);
		}
	}


	/* REVISIT this op is generic ARMv7-A/R stuff */
	if (retval == ERROR_OK && target->state == TARGET_HALTED) {
		struct arm_dpm *dpm = armv7r->arm.dpm;

		retval = dpm->prepare(dpm);
		if (retval != ERROR_OK)
			return retval;

		/* For both ICache and DCache, walk all cache lines in the
		 * address range. Cortex-r4 has fixed 64 byte line length.
		 *
		 * REVISIT per ARMv7, these may trigger watchpoints ...
		 */

		/* invalidate I-Cache */
		if (armv7r->armv7r_cache.i_cache_enabled) {
			/* ICIMVAU - Invalidate Cache single entry
			 * with MVA to PoU
			 *      MCR p15, 0, r0, c7, c5, 1
			 */
			for (uint32_t cacheline = address;
				cacheline < address + size * count;
				cacheline += 64) {
				retval = dpm->instr_write_data_r0(dpm,
						ARMV4_5_MCR(15, 0, 0, 7, 5, 1),
						cacheline);
				if (retval != ERROR_OK)
					return retval;
			}
		}

		/* invalidate D-Cache */
		if (armv7r->armv7r_cache.d_u_cache_enabled) {
			/* DCIMVAC - Invalidate data Cache line
			 * with MVA to PoC
			 *      MCR p15, 0, r0, c7, c6, 1
			 */
			for (uint32_t cacheline = address;
				cacheline < address + size * count;
				cacheline += 64) {
				retval = dpm->instr_write_data_r0(dpm,
						ARMV4_5_MCR(15, 0, 0, 7, 6, 1),
						cacheline);
				if (retval != ERROR_OK)
					return retval;
			}
		}

		/* (void) */ dpm->finish(dpm);
	}

	return retval;
}

static int cortex_r4_write_memory(struct target *target, uint32_t address,
	uint32_t size, uint32_t count, const uint8_t *buffer)
{
	int retval;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;
	uint8_t apsel = swjdp->apsel;
	/* cortex_r4 handles unaligned memory access */
	LOG_DEBUG("Reading memory at address 0x%x; size %d; count %d", address,
		size, count);
	if (apsel == swjdp_memoryap) {

		LOG_DEBUG("Writing memory to address 0x%x; size %d; count %d", address, size,
			count);

		retval = cortex_r4_write_phys_memory(target, address, size,
				count, buffer);
	} else {
		retval = cortex_r4_check_address(target, address);
		if (retval != ERROR_OK)
			return retval;
		retval = cortex_r4_write_apb_ab_memory(target, address, size, count, buffer);
	}
	return retval;
}

static int cortex_r4_bulk_write_memory(struct target *target, uint32_t address,
	uint32_t count, const uint8_t *buffer)
{
	return cortex_r4_write_memory(target, address, 4, count, buffer);
}

static int cortex_r4_handle_target_request(void *priv)
{
	struct target *target = priv;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct adiv5_dap *swjdp = armv7r->arm.dap;
	int retval;

	if (!target_was_examined(target))
		return ERROR_OK;
	if (!target->dbg_msg_enabled)
		return ERROR_OK;

	if (target->state == TARGET_RUNNING) {
		uint32_t request;
		uint32_t dscr;
		retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
				armv7r->debug_base + CPUDBG_DSCR, &dscr);

		/* check if we have data */
		while ((dscr & DSCR_DTR_TX_FULL) && (retval == ERROR_OK)) {
			retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
					armv7r->debug_base + CPUDBG_DTRTX, &request);
			if (retval == ERROR_OK) {
				target_request(target, request);
				retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
						armv7r->debug_base + CPUDBG_DSCR, &dscr);
			}
		}
	}

	return ERROR_OK;
}

/*
 * Cortex-r4 target information and configuration
 */

static int cortex_r4_examine_first(struct target *target)
{
	struct cortex_r4_common *cortex_r4 = target_to_cortex_r4(target);
	struct armv7r_common *armv7r = &cortex_r4->armv7r_common;
	struct adiv5_dap *swjdp = armv7r->arm.dap;
	int i;
	int retval = ERROR_OK;
	uint32_t didr, ctypr, ttypr, cpuid;

	/* We do one extra read to ensure DAP is configured,
	 * we call ahbap_debugport_init(swjdp) instead
	 */
	retval = ahbap_debugport_init(swjdp);
	if (retval != ERROR_OK)
		return retval;

	if (!target->dbgbase_set) {
		uint32_t dbgbase;
		/* Get ROM Table base */
		uint32_t apid;
		retval = dap_get_debugbase(swjdp, 1, &dbgbase, &apid);
		if (retval != ERROR_OK)
			return retval;
		/* Lookup 0x15 -- Processor DAP */
		retval = dap_lookup_cs_component(swjdp, 1, dbgbase, 0x15,
				&armv7r->debug_base);
		if (retval != ERROR_OK)
			return retval;
	} else
		armv7r->debug_base = target->dbgbase;

	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_CPUID, &cpuid);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_CPUID, &cpuid);
	if (retval != ERROR_OK) {
		LOG_DEBUG("Examine %s failed", "CPUID");
		return retval;
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_CTYPR, &ctypr);
	if (retval != ERROR_OK) {
		LOG_DEBUG("Examine %s failed", "CTYPR");
		return retval;
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_TTYPR, &ttypr);
	if (retval != ERROR_OK) {
		LOG_DEBUG("Examine %s failed", "TTYPR");
		return retval;
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, swjdp_debugap,
			armv7r->debug_base + CPUDBG_DIDR, &didr);
	if (retval != ERROR_OK) {
		LOG_DEBUG("Examine %s failed", "DIDR");
		return retval;
	}

	LOG_DEBUG("cpuid = 0x%08" PRIx32, cpuid);
	LOG_DEBUG("ctypr = 0x%08" PRIx32, ctypr);
	LOG_DEBUG("ttypr = 0x%08" PRIx32, ttypr);
	LOG_DEBUG("didr = 0x%08" PRIx32, didr);

	armv7r->arm.core_type = ARM_MODE_MON;
	retval = cortex_r4_dpm_setup(cortex_r4, didr);
	if (retval != ERROR_OK)
		return retval;

	/* Setup Breakpoint Register Pairs */
	cortex_r4->brp_num = ((didr >> 24) & 0x0F) + 1;
	cortex_r4->brp_num_context = ((didr >> 20) & 0x0F) + 1;
	cortex_r4->brp_num_available = cortex_r4->brp_num;
	cortex_r4->brp_list = calloc(cortex_r4->brp_num, sizeof(struct cortex_r4_brp));
/*	cortex_r4->brb_enabled = ????; */
	for (i = 0; i < cortex_r4->brp_num; i++) {
		cortex_r4->brp_list[i].used = 0;
		if (i < (cortex_r4->brp_num-cortex_r4->brp_num_context))
			cortex_r4->brp_list[i].type = BRP_NORMAL;
		else
			cortex_r4->brp_list[i].type = BRP_CONTEXT;
		cortex_r4->brp_list[i].value = 0;
		cortex_r4->brp_list[i].control = 0;
		cortex_r4->brp_list[i].BRPn = i;
	}

	LOG_DEBUG("Configured %i hw breakpoints", cortex_r4->brp_num);

	target_set_examined(target);
	return ERROR_OK;
}

static int cortex_r4_examine(struct target *target)
{
	int retval = ERROR_OK;

	/* don't re-probe hardware after each reset */
	if (!target_was_examined(target))
		retval = cortex_r4_examine_first(target);

	/* Configure core debug access */
	if (retval == ERROR_OK)
		retval = cortex_r4_init_debug_access(target);

	return retval;
}

/*
 *	Cortex-r4 target creation and initialization
 */

static int cortex_r4_init_target(struct command_context *cmd_ctx,
	struct target *target)
{
	/* examine_first() does a bunch of this */
	return ERROR_OK;
}

static int cortex_r4_init_arch_info(struct target *target,
	struct cortex_r4_common *cortex_r4, struct jtag_tap *tap)
{
	struct armv7r_common *armv7r = &cortex_r4->armv7r_common;
	struct adiv5_dap *dap = &armv7r->dap;

	armv7r->arm.dap = dap;

	/* Setup struct cortex_r4_common */
	cortex_r4->common_magic = CORTEX_R4_COMMON_MAGIC;
	/*  tap has no dap initialized */
	if (!tap->dap) {
		armv7r->arm.dap = dap;
		/* Setup struct cortex_r4_common */

		/* prepare JTAG information for the new target */
		cortex_r4->jtag_info.tap = tap;
		cortex_r4->jtag_info.scann_size = 4;

		/* Leave (only) generic DAP stuff for debugport_init() */
		dap->jtag_info = &cortex_r4->jtag_info;

		/* Number of bits for tar autoincrement, impl. dep. at least 10 */
		dap->tar_autoincr_block = (1 << 10);
		dap->memaccess_tck = 80;
		tap->dap = dap;
	} else
		armv7r->arm.dap = tap->dap;

	cortex_r4->fast_reg_read = 0;

	/* register arch-specific functions */
	armv7r->examine_debug_reason = NULL;

	armv7r->post_debug_entry = cortex_r4_post_debug_entry;

	armv7r->pre_restore_context = NULL;

	/* REVISIT v7r setup should be in a v7r-specific routine */
	armv7r_init_arch_info(target, armv7r);
	target_register_timer_callback(cortex_r4_handle_target_request, 1, 1, target);

	return ERROR_OK;
}

static int cortex_r4_target_create(struct target *target, Jim_Interp *interp)
{
	struct cortex_r4_common *cortex_r4 = calloc(1, sizeof(struct cortex_r4_common));

	return cortex_r4_init_arch_info(target, cortex_r4, target->tap);
}



static int cortex_r4_mmu(struct target *target, int *enabled)
{
	if (target->state != TARGET_HALTED) {
		LOG_ERROR("%s: target not halted", __func__);
		return ERROR_TARGET_INVALID;
	}

	*enabled = 0;
	return ERROR_OK;
}

static int cortex_r4_virt2phys(struct target *target,
	uint32_t virt, uint32_t *phys)
{
	*phys = virt;
	return ERROR_OK;
}

COMMAND_HANDLER(cortex_r4_handle_cache_info_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct armv7r_common *armv7r = target_to_armv7r(target);

	return armv7r_handle_cache_info_command(CMD_CTX,
			&armv7r->armv7r_cache);
}


COMMAND_HANDLER(cortex_r4_handle_dbginit_command)
{
	struct target *target = get_current_target(CMD_CTX);
	if (!target_was_examined(target)) {
		LOG_ERROR("target not examined yet");
		return ERROR_FAIL;
	}

	return cortex_r4_init_debug_access(target);
}
COMMAND_HANDLER(cortex_r4_handle_smp_off_command)
{
	struct target *target = get_current_target(CMD_CTX);
	/* check target is an smp target */
	struct target_list *head;
	struct target *curr;
	head = target->head;
	target->smp = 0;
	if (head != (struct target_list *)NULL) {
		while (head != (struct target_list *)NULL) {
			curr = head->target;
			curr->smp = 0;
			head = head->next;
		}
		/*  fixes the target display to the debugger */
		target->gdb_service->target = target;
	}
	return ERROR_OK;
}

COMMAND_HANDLER(cortex_r4_handle_smp_on_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct target_list *head;
	struct target *curr;
	head = target->head;
	if (head != (struct target_list *)NULL) {
		target->smp = 1;
		while (head != (struct target_list *)NULL) {
			curr = head->target;
			curr->smp = 1;
			head = head->next;
		}
	}
	return ERROR_OK;
}

COMMAND_HANDLER(cortex_r4_handle_smp_gdb_command)
{
	struct target *target = get_current_target(CMD_CTX);
	int retval = ERROR_OK;
	struct target_list *head;
	head = target->head;
	if (head != (struct target_list *)NULL) {
		if (CMD_ARGC == 1) {
			int coreid = 0;
			COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], coreid);
			if (ERROR_OK != retval)
				return retval;
			target->gdb_service->core[1] = coreid;

		}
		command_print(CMD_CTX, "gdb coreid  %d -> %d", target->gdb_service->core[0]
			, target->gdb_service->core[1]);
	}
	return ERROR_OK;
}

static const struct command_registration cortex_r4_exec_command_handlers[] = {
	{
		.name = "cache_info",
		.handler = cortex_r4_handle_cache_info_command,
		.mode = COMMAND_EXEC,
		.help = "display information about target caches",
		.usage = "",
	},
	{
		.name = "dbginit",
		.handler = cortex_r4_handle_dbginit_command,
		.mode = COMMAND_EXEC,
		.help = "Initialize core debug",
		.usage = "",
	},
	{   .name = "smp_off",
	    .handler = cortex_r4_handle_smp_off_command,
	    .mode = COMMAND_EXEC,
	    .help = "Stop smp handling",
	    .usage = "",},
	{
		.name = "smp_on",
		.handler = cortex_r4_handle_smp_on_command,
		.mode = COMMAND_EXEC,
		.help = "Restart smp handling",
		.usage = "",
	},
	{
		.name = "smp_gdb",
		.handler = cortex_r4_handle_smp_gdb_command,
		.mode = COMMAND_EXEC,
		.help = "display/fix current core played to gdb",
		.usage = "",
	},


	COMMAND_REGISTRATION_DONE
};
static const struct command_registration cortex_r4_command_handlers[] = {
	{
		.chain = arm_command_handlers,
	},
	{
		.chain = armv7r_command_handlers,
	},
	{
		.name = "cortex_r4",
		.mode = COMMAND_ANY,
		.help = "Cortex-R4 command group",
		.usage = "",
		.chain = cortex_r4_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct target_type cortexr4_target = {
	.name = "cortex_r4",

	.poll = cortex_r4_poll,
	.arch_state = armv7r_arch_state,

	.target_request_data = NULL,

	.halt = cortex_r4_halt,
	.resume = cortex_r4_resume,
	.step = cortex_r4_step,

	.assert_reset = cortex_r4_assert_reset,
	.deassert_reset = cortex_r4_deassert_reset,
	.soft_reset_halt = NULL,

	/* REVISIT allow exporting VFP3 registers ... */
	.get_gdb_reg_list = arm_get_gdb_reg_list,

	.read_memory = cortex_r4_read_memory,
	.write_memory = cortex_r4_write_memory,
	.bulk_write_memory = cortex_r4_bulk_write_memory,

	.checksum_memory = arm_checksum_memory,
	.blank_check_memory = arm_blank_check_memory,

	.run_algorithm = armv4_5_run_algorithm,

	.add_breakpoint = cortex_r4_add_breakpoint,
	.add_context_breakpoint = cortex_r4_add_context_breakpoint,
	.add_hybrid_breakpoint = cortex_r4_add_hybrid_breakpoint,
	.remove_breakpoint = cortex_r4_remove_breakpoint,
	.add_watchpoint = NULL,
	.remove_watchpoint = NULL,

	.commands = cortex_r4_command_handlers,
	.target_create = cortex_r4_target_create,
	.init_target = cortex_r4_init_target,
	.examine = cortex_r4_examine,

	.read_phys_memory = cortex_r4_read_phys_memory,
	.write_phys_memory = cortex_r4_write_phys_memory,
	.mmu = cortex_r4_mmu,
	.virt2phys = cortex_r4_virt2phys,
};
