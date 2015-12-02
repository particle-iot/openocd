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
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "aarch64.h"
#include <helper/time_support.h>
#include "breakpoints.h"
#include "register.h"
#include "target_request.h"
#include "target_type64.h"
//#include "arm_opcodes.h"
#include "armv8_opcodes.h"
#include "armv8_cti.h"


//#define	_DEBUG_OPCODE_
//#define	_DEBUG_DCC_IO_
#define	_DEBUG_BPWP_


static int aarch64_poll(struct target *target);
static int aarch64_debug_entry(struct target *target);
static int aarch64_restore_context(struct target *target, bool bpwp);
static int aarch64_set_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode);
static int aarch64_set_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode);
static int aarch64_set_hybrid_breakpoint(struct target *target,
	struct breakpoint *breakpoint);
static int aarch64_unset_breakpoint(struct target *target,
	struct breakpoint *breakpoint);
static int aarch64_mmu(struct target *target, int *enabled);
static int aarch64_virt2phys(struct target *target,
	uint64_t virt, uint64_t *phys);
static int aarch64_read_apb_ab_memory(struct target *target,
	uint64_t address, uint32_t size, uint32_t count, uint8_t *buffer);
static int aarch64_instr_write_data_r0(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t data);

static int aarch64_restore_system_control_reg(struct target *target)
{
	int retval = ERROR_OK;

	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = target_to_armv8(target);

	if (aarch64->system_control_reg != aarch64->system_control_reg_curr) {
		aarch64->system_control_reg_curr = aarch64->system_control_reg;
		retval = aarch64_instr_write_data_r0(armv8->arm.dpm,
						     0xd5181000,	/* msr sctlr_el1, x0 */
						     aarch64->system_control_reg);
	}

	return retval;
}

/*  check address before aarch64_apb read write access with mmu on
 *  remove apb predictible data abort */
static int aarch64_check_address(struct target *target, uint32_t address)
{
	/* TODO */
	return ERROR_OK;
}

/*  modify system_control_reg in order to enable or disable mmu for :
 *  - virt2phys address conversion
 *  - read or write memory in phys or virt address */
static int aarch64_mmu_modify(struct target *target, int enable)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	int retval = ERROR_OK;

	if (enable) {
		/*  if mmu enabled at target stop and mmu not enable */
		if (!(aarch64->system_control_reg & 0x1U)) {
			LOG_ERROR("trying to enable mmu on target stopped with mmu disable");
			return ERROR_FAIL;
		}
		if (!(aarch64->system_control_reg_curr & 0x1U)) {
			aarch64->system_control_reg_curr |= 0x1U;
			retval = aarch64_instr_write_data_r0(armv8->arm.dpm,
							     0xd5181000,	/* msr sctlr_el1, x0 */
							     aarch64->system_control_reg_curr);
		}
	} else {
		if (aarch64->system_control_reg_curr & 0x4U) {
			/*  data cache is active */
			aarch64->system_control_reg_curr &= ~0x4U;
			/* Flush D-Cache */
			if (armv8->armv8_mmu.armv8_cache.flush_dcache_all)
				armv8->armv8_mmu.armv8_cache.flush_dcache_all(target);
		}
		if ((aarch64->system_control_reg_curr & 0x1U)) {
			aarch64->system_control_reg_curr &= ~0x1U;
			retval = aarch64_instr_write_data_r0(armv8->arm.dpm,
							     0xd5181000,	/* msr sctlr_el1, x0 */
							     aarch64->system_control_reg_curr);
		}
	}
	return retval;
}

/*
 * Basic debug access, very low level assumes state is saved
 */
static int aarch64_init_debug_access(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval;
//	uint32_t dummy;

	LOG_DEBUG("%s", target_name(target));

	/* Unlocking the debug registers for modification
	 * The debugport might not be ready yet, so try it until timeout */
	int64_t wait = timeval_ms();
	do {
		retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			     armv8->debug_base + ARMV8_REG_EDLAR, 0xC5ACCE55);
		if (retval == ERROR_OK) {
			LOG_DEBUG("Unlock debug access successful");
			break;
		}
		if (timeval_ms() > wait + 1000) {
			LOG_USER("Timeout trying to unlock debug access");
			return ERROR_TARGET_TIMEOUT;
		}
	} while (true);

#if 0	/* Alamy: Does it really work ? */
	/* Clear Sticky Power Down status Bit in PRSR to enable access to
	   the registers in the Core Power Domain */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUDBG_PRSR, &dummy);
	if (retval != ERROR_OK)
		return retval;
#endif

#if 0	/* Alamy: Trying to clear EDSCR.MA: the code is implemented with NORMAL access code */
	dap_ap_select(swjdp, armv8->debug_ap);
	retval = mem_ap_read_atomic_u32(swjdp,
			armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
	edscr &= ~ARMV8_EDSCR_MA;
	retval = mem_ap_write_atomic_u32(swjdp,
			armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
#endif

	/* Enabling of instruction execution in debug mode is done in debug_entry code */

	/* Resync breakpoint registers */

	/* Since this is likely called from init or reset, update target state information*/
	/* Alamy: Do we need this ? */
//	return aarch64_poll(target);

	return ERROR_OK;
}

/* To reduce needless round-trips, pass in a pointer to the current
 * EDSCR value.  Initialize it to zero if you just need to know the
 * value on return from this function; or ARMV8_EDSCR_ITE if you
 * happen to know that no instruction is pending.
 * 'force' to read EDSCR at least once.
 */
static int aarch64_wait_ITE(struct armv8_common *armv8, uint32_t *edscr, bool force)
{
	int retval;
	int64_t	then;

	assert(edscr != NULL);

	/* H4.4.1 Normal access mode: EDSCR.ITE == 1 indicates that
	 * the PE is ready to accept an instruction to the ITR.
	 */
	then = timeval_ms();
	while ((*edscr & ARMV8_EDSCR_ITE) == 0 || force) {
		force = false;

		retval = mem_ap_read_atomic_u32(armv8->arm.dap,
				armv8->debug_base + ARMV8_REG_EDSCR, edscr);
		if (retval != ERROR_OK) {
			LOG_ERROR("Fail to read EDSCR");
			return retval;
		}

		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for ITR empty");
			return ERROR_TARGET_TIMEOUT;
		}
	}	/* End of while(!ARMV8_EDSCR_ITE) */

	return ERROR_OK;
}

/* To reduce needless round-trips, pass in a pointer to the current
 * EDSCR value.  Initialize it to zero if you just need to know the
 * value on return from this function; or ARMV8_EDSCR_ITE if you
 * happen to know that no instruction is pending.
 */
int aarch64_exec_opcode(struct target *target,
	uint32_t opcode, uint32_t *edscr_p)
{
	uint32_t edscr;
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);

	edscr = edscr_p ? *edscr_p : 0;

#ifdef	_DEBUG_OPCODE_
	LOG_DEBUG("exec opcode 0x%08" PRIx32, opcode);
#endif

	/* Wait for ITR to be empty (EDSCR.ITE bit to be set) */
	retval = aarch64_wait_ITE(armv8, &edscr, false);
	if (retval != ERROR_OK)
		return retval;

	/* Set ITR (opcode) for PE to execute */
	retval = mem_ap_sel_write_u32(armv8->arm.dap, armv8->debug_ap,
			armv8->debug_base + ARMV8_REG_EDITR, opcode);
	if (retval != ERROR_OK)
		return retval;

	/* Wait for ITR to be empty */
	retval = aarch64_wait_ITE(armv8, &edscr, true);

	if (edscr_p)
		*edscr_p = edscr;

	return retval;
}

/* Write to memory mapped registers directly with no cache or mmu handling */
static int aarch64_dap_write_memap_register_u32(struct target *target,
	uint32_t address,
	uint32_t value)
{
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap, address, value);

	return retval;
}

/*
 * AARCH64 implementation of Debug Programmer's Model
 *
 * NOTE the invariant:  these routines return with DSCR_INSTR_COMP set,
 * so there's no need to poll for it before executing an instruction.
 *
 * NOTE that in several of these cases the "stall" mode might be useful.
 * It'd let us queue a few operations together... prepare/finish might
 * be the places to enable/disable that mode.
 */

static inline struct aarch64_common *dpm_to_a8(struct arm_dpm *dpm)
{
	return container_of(dpm, struct aarch64_common, armv8_common.dpm);
}

static int aarch64_write_dcc(struct aarch64_common *a8, uint32_t data)
{
	LOG_DEBUG("write DCC 0x%08" PRIx32, data);
	return mem_ap_sel_write_u32(a8->armv8_common.arm.dap,
		a8->armv8_common.debug_ap, a8->armv8_common.debug_base + CPUDBG_DTRRX, data);
}

static int aarch64_write_dcc_64(struct aarch64_common *a8, uint64_t data)
{
	struct armv8_common *armv8 = &(a8->armv8_common);
	struct adiv5_dap *swjdp = armv8->arm.dap;

	uint32_t lword, hword;		/* Low/High word of 64-bit data */
	int retval;

	hword = data >> 32;
	lword = (uint32_t)data;

#ifdef	_DEBUG_DCC_IO_
	LOG_DEBUG("write DCC(lo) 0x%08" PRIx32, lword);
	LOG_DEBUG("write DCC(hi) 0x%08" PRIx32, hword);
#endif

	/* Wait for RXfull == 0 */

	/* H4.4.3 Accessing 64-bit data
	 * Although the limitation doesn't apply in Debug state, when send a
	 * 64-bit value to the target, it's still better for the external debugger
	 * to write to DBGDTRTX_EL0 before writing DBGDTRRX_EL0.
	 */
	retval = mem_ap_write_u32(swjdp,
		armv8->debug_base + ARMV8_REG_DBGDTRTX_EL0, hword);
	retval += mem_ap_write_atomic_u32(swjdp,
		armv8->debug_base + ARMV8_REG_DBGDTRRX_EL0, lword);

	return retval;
}

/*
 * Read 32-bit data through DCC (Debug Communications Channel)
 */
static int aarch64_read_dcc(struct aarch64_common *a8, uint32_t *data,
	uint32_t *dscr_p)
{
	struct adiv5_dap *swjdp = a8->armv8_common.arm.dap;
	uint32_t dscr = DSCR_INSTR_COMP;
	int retval;

	if (dscr_p)
		dscr = *dscr_p;

/* Alamy: Check this function */
LOG_WARNING("Alamy: Should NOT use this function, use dcc64() instead.");
	/* Wait for DTRRXfull */
	long long then = timeval_ms();
	while ((dscr & DSCR_DTR_TX_FULL) == 0) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, a8->armv8_common.debug_ap,
				a8->armv8_common.debug_base + CPUDBG_DSCR,
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for read dcc");
			return ERROR_TARGET_TIMEOUT;
		}
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, a8->armv8_common.debug_ap,
					    a8->armv8_common.debug_base + CPUDBG_DTRTX,
					    data);
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("read DCC 0x%08" PRIx32, *data);

	if (dscr_p)
		*dscr_p = dscr;

	return retval;
}

/*
 * Read 64-bit data through DCC (Debug Communications Channel)
 *   Normal access mode, when EDSCR.MA == 0 or the PE is in Non-debug state.
 *   Memory access mode, when EDSCR.MA == 1 and the PE is in Debug state.
 */
static int aarch64_read_dcc_64(struct aarch64_common *aarch64, uint64_t *data,
	uint32_t *edscr_p)
{
	struct armv8_common *armv8 = &(aarch64->armv8_common);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	uint32_t edscr;
	uint32_t lword, hword;		/* Low/High word of 64-bit data */
	int retval;


//LOG_DEBUG("Alamy: Debug: aarch64=%p, armv8=%p, swjdp=%p", aarch64, armv8, swjdp);
	/* EDSCR.MA is ignored if in Non-debug state and set to zero on entry to Debug state */
	/* Normal access or Memory access */
	retval = mem_ap_read_atomic_u32(swjdp,
		armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
	if (retval != ERROR_OK)	return retval;

	if (((edscr & ARMV8_EDSCR_MA) == 0) ||
		(EDSCR_STATUS(edscr) == ARMV8_EDSCR_STATUS_NDBG)) {
		/* Normal access mode */
//		aarch64_read_dcc_normal();
		LOG_DEBUG("Implement: aarch64_read_dcc_normal()");
	} else if ((edscr & ARMV8_EDSCR_MA) &&
		(EDSCR_STATUS(edscr) != ARMV8_EDSCR_STATUS_NDBG)) {
		/* Memory access mode */
//		aarch64_read_dcc_memory();
		LOG_DEBUG("Implement: aarch64_read_dcc_memory()");
	} else {
		LOG_ERROR("Unable to read DCC when EDSCR.MA=%d, PE in %s mode",
			(edscr & ARMV8_EDSCR_MA) ? 1 : 0,
			(EDSCR_STATUS(edscr) == ARMV8_EDSCR_STATUS_NDBG)
				? "Debug" : "Non-debug");
		return ERROR_TARGET_FAILURE;
	}


	edscr = edscr_p ? *edscr_p : 0;	/* Should take EDSCR.MA's reading of edscr account later */

	/* H4.4.1 Normal access mode:
	 * EDSCR.TXfull == 1 indicates that DBGDTRTX_EL0 contains a
	 * valid value that has been written by software running on the target
	 * and not yet read by an external debugger.
	 */
	/* Wait for data to be ready on DCC */
	long long then = timeval_ms();
	while ((edscr & ARMV8_EDSCR_TXFULL) == 0) {
		retval = mem_ap_read_atomic_u32(swjdp,
				armv8->debug_base + CPUDBG_DSCR,
				&edscr);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for reading DCC");
			return ERROR_TARGET_TIMEOUT;
		}
	}

	/* H4.4.3 Accessing 64-bit data
	 * Although the limitation doesn't apply in Debug state, but it's still
	 * better to read DBGDTRRX_EL0 before reading DBGDTRTX_EL0, for receiving
	 * a 64-bit value
	 */
	/* H2.4.8 Accessing registers in Debug state, Figure H2-1 (AArch64)
	 *	(Debugger)		D[63:32]	D[31:00]	Note		 |	(Software, H4.3.1)
	 *	Write register	DBGDTRTX	DBGDTRRX	Xn = D[63:0] |	Read
	 *  Read  register	DBGDTRRX	DBGDTRTX	D[63:0] = Xn |	Write
	 */
	retval = mem_ap_read_u32(swjdp,
			armv8->debug_base + ARMV8_REG_DBGDTRRX_EL0,
			&hword);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_read_atomic_u32(swjdp,
			armv8->debug_base + ARMV8_REG_DBGDTRTX_EL0,
			&lword);
	if (retval != ERROR_OK)
		return retval;

	*data = ((uint64_t)hword << 32) | (uint64_t)lword;
#ifdef	_DEBUG_DCC_IO_
	LOG_DEBUG("read DCC 0x%16.16" PRIx64, *data);
#endif

	/* CAUTION:
	 * edscr value is dirty because EDSCR.TXFfull should be cleared to 0
	 * after reading DBGDTRTX_EL0 (H9.2.7)
	 */
	if (edscr_p)
		*edscr_p = edscr;

	return retval;
}

static int aarch64_instr_write_data_dcc(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	int retval;
	uint32_t dscr = DSCR_INSTR_COMP;

LOG_WARNING("Alamy: Should NOT use this function, 64-bit version instead.");
	retval = aarch64_write_dcc(a8, data);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
}

static int aarch64_instr_write_data_dcc_64(struct arm_dpm *dpm,
	uint32_t opcode, uint64_t data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	int retval;
	uint32_t dscr = DSCR_INSTR_COMP;

	retval = aarch64_write_dcc_64(a8, data);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
}

static int aarch64_instr_write_data_r0(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t dscr = DSCR_INSTR_COMP;
	int retval;

LOG_WARNING("Alamy: Should NOT use this function, use write_data_x0() instead.");
	retval = aarch64_write_dcc(a8, data);
	if (retval != ERROR_OK)
		return retval;

	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			0xd5330500,				/* mrs x0, dbgdtrrx_el0 */
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	/* then the opcode, taking data from R0 */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);

	return retval;
}

/*
 * CAUTION: Do not call this function directly, call dpm->arm->msr().
 * It's better to have dpm_prepare/dpm_finish to protect the session.
 *
 * Write data to DCC
 * Move data from DCC to X0
 * Copy X0 to register (opcode)
 */
static int aarch64_instr_write_data_x0(struct arm_dpm *dpm,
	uint32_t opcode, uint64_t data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t edscr;
	int retval;

#if 0
	retval = aarch64_write_dcc_64(a8, data);
	if (retval != ERROR_OK)
		return retval;

	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
//			0xd5330400,				/* mrs x0, dbgdtr_el0 */
			A64_OPCODE_MRS_DBGDTR_EL0(AARCH64_X0)	/* mrs x0, dbgdtr_el0 */
			&dscr);
	if (retval != ERROR_OK)
		return retval;

#else
	retval = aarch64_instr_write_data_dcc_64(dpm,
			A64_OPCODE_MRS_DBGDTR_EL0(AARCH64_X0),	/* mrs x0, dbgdtr_el0 */
			data);
	if (retval != ERROR_OK)
		return retval;
#endif

	/* then the opcode, taking data from X0 */
	edscr = ARMV8_EDSCR_ITE;
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&edscr);

	return retval;
}

#if 0
static int aarch64_instr_cpsr_sync(struct arm_dpm *dpm)
{
	struct target *target = dpm->arm->target;
	uint32_t dscr = DSCR_INSTR_COMP;

	/* "Prefetch flush" after modifying execution status in CPSR */
	return aarch64_exec_opcode(target,
			ARMV4_5_MCR(15, 0, 0, 7, 5, 4),
			&dscr);
}
#endif

static int aarch64_instr_read_data_dcc(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t *data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	int retval;
	uint32_t dscr = DSCR_INSTR_COMP;

LOG_WARNING("Alamy: Should NOT use this function, use read_data_dcc64() instead.");
	/* the opcode, writing data to DCC */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_read_dcc(a8, data, &dscr);
}

/*
 * Called by
 * dpm_read_reg_aarch64(), directly, to read registers. i.e.: AARCH64_X0 ... AARCH64_X30
 * dpm_read_reg_aarch64(), through aarch64_instr_read_data_x0(), to read special registers.
 */
static int aarch64_instr_read_data_dcc_64(struct arm_dpm *dpm,
	uint32_t opcode, uint64_t *data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	int retval;
	uint32_t edscr;

	/* We are sure that ITR is empty & DCC RX is empty after dpm_prepare()
	 * edscr still got updated after opcode is executed
	 */
	edscr = ARMV8_EDSCR_ITE;
	retval = aarch64_exec_opcode(dpm->arm->target, opcode, &edscr);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_read_dcc_64(a8, data, &edscr);
}

#if 0	/* Alamy: Deprecated */
static int aarch64_instr_read_data_r0(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t *data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t dscr = DSCR_INSTR_COMP;
	int retval;

	/* the opcode, writing data to R0 */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	/* write R0 to DCC */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			0xd5130400,  /* msr dbgdtr_el0, x0 */
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_read_dcc(a8, data, &dscr);
}
#endif

/* 1. Copy special register value to X0
 * 2. Send X0 to DCC
 * 3. Read value from DCC
 *
 * Note: X0 will be polluated by other register's value,
 *   so it's read first in arm_dpm_read_current_registers_64().
 */
static int aarch64_instr_read_data_x0(struct arm_dpm *dpm,
	uint32_t opcode, uint64_t *data)
{
	uint32_t edscr;
	int retval;

	/* the opcode, copy data to X0 */
	/* set 'edscr': same reason as it is in aarch64_instr_read_data_dcc_64() */
	edscr = ARMV8_EDSCR_ITE;
	retval = aarch64_exec_opcode(dpm->arm->target, opcode, &edscr);
	if (retval != ERROR_OK)
		return retval;

#if 0
LOG_DEBUG("EDSCR.ERR == %d, EDSCR.RXFULL == %d, EDSCR.TXFULL == %d\n",
	(edscr & ARMV8_EDSCR_ERR) ? 1 : 0,
	(edscr & ARMV8_EDSCR_RXFULL) ? 1 : 0,
	(edscr & ARMV8_EDSCR_TXFULL) ? 1 : 0);
LOG_DEBUG("EDSCR.ITO == %d, EDSCR.RXO == %d, EDSCR.TXU == %d\n",
	(edscr & ARMV8_EDSCR_ITO) ? 1 : 0,
	(edscr & ARMV8_EDSCR_RXO) ? 1 : 0,
	(edscr & ARMV8_EDSCR_TXU) ? 1 : 0);
#endif

	/* PE writes X0 to DCC, then Debugger reads DCC */
	return aarch64_instr_read_data_dcc_64(dpm,
			A64_OPCODE_MSR_DBGDTR_EL0(AARCH64_X0),	/* msr dbgdtr_el0, x0 */
			data);
}

static int aarch64_bpwp_enable(struct arm_dpm *dpm, unsigned index_t,
	uint32_t addr, uint32_t control)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t vr = a8->armv8_common.debug_base;
	uint32_t cr = a8->armv8_common.debug_base;
	int retval;

	LOG_ERROR("Alamy: Rewrite this function (uintmax_t, use MACRO)");
	switch (index_t) {
	/* see arm_dpm_initialize */
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
	vr += 16 * index_t;
	cr += 16 * index_t;

	LOG_DEBUG("A8: bpwp enable, vr %08x cr %08x",
		(unsigned) vr, (unsigned) cr);

	retval = aarch64_dap_write_memap_register_u32(dpm->arm->target,
			vr, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = aarch64_dap_write_memap_register_u32(dpm->arm->target,
			cr, control);
	return retval;
}

static int aarch64_bpwp_disable(struct arm_dpm *dpm, unsigned index_t)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t cr;

	LOG_ERROR("Alamy: Rewrite this function (uintmax_t, use MACRO)");
	switch (index_t) {
		case 0 ... 15:
			cr = a8->armv8_common.debug_base + CPUDBG_BCR_BASE;
			break;
		case 16 ... 31:
			cr = a8->armv8_common.debug_base + CPUDBG_WCR_BASE;
			index_t -= 16;
			break;
		default:
			return ERROR_FAIL;
	}
	cr += 16 * index_t;

	LOG_DEBUG("A8: bpwp disable, cr %08x", (unsigned) cr);

	/* clear control register */
	return aarch64_dap_write_memap_register_u32(dpm->arm->target, cr, 0);
}

static int aarch64_dpm_prepare(struct arm_dpm *dpm)
{
	struct armv8_common *armv8 = dpm_to_armv8(dpm);
	struct adiv5_dap *swjdp = dpm->arm->dap;
	uint32_t edscr;
	int retval;

	dap_ap_select(swjdp, armv8->debug_ap);

	/* Wait for ITR to be empty */
	retval = aarch64_wait_ITE(armv8, &edscr, true);
	if (retval != ERROR_OK)
		return retval;

	/* this "should never happen" ... */
	/* Debug Data Transfer Register, Receive: Debugger to PE */
	if (edscr & ARMV8_EDSCR_RXFULL) {
		LOG_ERROR("EDSCR_DTR_RX_FULL, edscr = 0x%08" PRIx32, edscr);

#if 0	/* Disable it: This will pollute X0 register */
		/* Clear RXfull: D7.3.7 Read system register DBGDTRRX_EL0 */
		/*	MRS X0, DBGDTRRX_EL0 (CAUTION: pollute X0 register) */
		retval = aarch64_exec_opcode(armv8->arm->target,
			A64_OPCODE_MRS_DBGDTRRX_EL0(AARCH64_X0),
			&edscr);
		if (retval != ERROR_OK)
			return retval;
#endif

		/* H4.4.4 ClearStickyErrors() */
		/* H9.2.40 EDRCR
		 * Clear the EDSCR.{TXU, RXO, ERR} bits, and, if the processor
		 * is in Debug state, the EDSCR.ITO bit */
		retval = mem_ap_write_atomic_u32(swjdp,
			armv8->debug_base + ARMV8_REG_EDRCR,
			ARMV8_EDRCR_CSE);
		if (retval != ERROR_OK)
			return retval;
	}

	return retval;
}

static int aarch64_dpm_finish(struct arm_dpm *dpm)
{
	struct aarch64_common *aarch64 = dpm_to_aarch64(dpm);
	struct armv8_common *armv8 = dpm_to_armv8(dpm);
	uint32_t edscr;
	int retval;

	/* Update saved EDSCR (maybe unnecessary: 2015-10-07 */
	retval = mem_ap_sel_read_atomic_u32(armv8->arm.dap, armv8->debug_ap,
			armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
	if (retval == ERROR_OK)
		aarch64->cpudbg_edscr = edscr;


	/* REVISIT what else could be done here ? */


	return ERROR_OK;
}

static int aarch64_dpm_setup(struct aarch64_common *a8, uint32_t debug)
{
	struct arm_dpm *dpm = &a8->armv8_common.dpm;
	int retval;

	dpm->arm = &a8->armv8_common.arm;
	dpm->didr = debug;

	dpm->prepare = aarch64_dpm_prepare;
	dpm->finish = aarch64_dpm_finish;

	dpm->instr_write_data_dcc = aarch64_instr_write_data_dcc;
	dpm->instr_write_data_dcc_64 = aarch64_instr_write_data_dcc_64;
	dpm->instr_write_data_r0 = aarch64_instr_write_data_r0;	/* Alamy: get rid of r0 function */
	dpm->instr_write_data_x0 = aarch64_instr_write_data_x0;
//	dpm->instr_cpsr_sync = aarch64_instr_cpsr_sync;	/* Alamy: get rid of ARMV4_5() */

	dpm->instr_read_data_dcc = aarch64_instr_read_data_dcc;
	dpm->instr_read_data_dcc_64 = aarch64_instr_read_data_dcc_64;
//	dpm->instr_read_data_r0 = aarch64_instr_read_data_r0;
	dpm->instr_read_data_x0 = aarch64_instr_read_data_x0;

	dpm->bpwp_enable = aarch64_bpwp_enable;
	dpm->bpwp_disable = aarch64_bpwp_disable;

	retval = arm_dpm_setup(dpm);
	if (retval != ERROR_OK)
		return retval;

	retval = arm_dpm_initialize(dpm);

	return retval;
}

#if 0
static struct target *get_aarch64(struct target *target, int32_t coreid)
{
	struct target_list *head;
	struct target *curr;

	head = target->head;
	while (head != (struct target_list *)NULL) {
		curr = head->target;
LOG_DEBUG("list target=%p, curr target=%s(%p), head target=%p",
head, target_name(curr), curr, curr->head);
		if ((curr->coreid == coreid) && (curr->state == TARGET_HALTED))
			return curr;
		head = head->next;
	}
	return target;
}
#endif

#if 0
static int aarch64_halt(struct target *target);

static int aarch64_halt_smp(struct target *target)
{
	int retval = 0;
	struct target_list *head;
	struct target *curr;

	head = target->head;
LOG_DEBUG("curr target=%s (head=%p)", target_name(target), target->head);
	while (head != (struct target_list *)NULL) {
		curr = head->target;
LOG_DEBUG("list target=%s(%p), (head=%p)", target_name(curr), curr, target->head);
		if ((curr != target) && (curr->state != TARGET_HALTED))
{
LOG_DEBUG("smp halting target=%s(%p)", target_name(curr), curr);
			retval += aarch64_halt(curr);
}
		head = head->next;
	}
	return retval;
}

static int update_halt_gdb(struct target *target)
{
	int retval = 0;

LOG_DEBUG("target=%s(%p) (head target=%p), service=%p, core[0]=%d",
	target_name(target), target, target->head,
	target->gdb_service, ((target->gdb_service)?target->gdb_service->core[0]:0));
	if (target->gdb_service && target->gdb_service->core[0] == -1) {
LOG_DEBUG("Debug tag");
		target->gdb_service->target = target;
		target->gdb_service->core[0] = target->coreid;
		retval += aarch64_halt_smp(target);
	}
	return retval;
}
#endif

/*
 * Cortex-A8 Run control
 */

static int aarch64_poll_one(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t edscr;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct adiv5_dap *swjdp = armv8->arm.dap;
//	enum target_state prev_target_state = target->state;

//	LOG_DEBUG("Target %s(%p)", target_name(target), target);

	/* poll might be called from configuration file before examined */
	if (!target_was_examined(target))
		return ERROR_OK;

	/*
	 * Read EDSCR to determine running/halted state
	 * If the core is halted and was previously not halted,
	 *     then enter debug state & read core registers etc.
	 * If the core has resumed (restarted) and was previously halted,
	 *     then note that it is now running
	 */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
	if (retval != ERROR_OK)
		return retval;
	aarch64->cpudbg_edscr = edscr;

	/* Alamy: too much debugging messages: annoying */
//	LOG_DEBUG("%s EDSCR=0x%08x", target_name(target), edscr);

	if (!armv8_is_pe_status_valid(edscr)) {
		LOG_ERROR("Unknown %s state. EDSCR = 0x%08" PRIx32,
			target_name(target), edscr);
		target->state = TARGET_UNKNOWN;
		return ERROR_TARGET_INVALID;
	}

	if (! PE_STATUS_HALTED(EDSCR_STATUS(edscr)) ) {
		target->state = TARGET_RUNNING;
//		LOG_WARNING("Target %s is not halted", target_name(target);

		/* We don't want to break following JIM command(s)
		 * (ie: target configure -event ...)
		 */
		return ERROR_OK;
	}

	/* This core is halted */
	enum target_state prev_target_state = target->state;
	target->state = TARGET_HALTED;
	target->debug_reason = armv8_edscr_debug_reason(edscr);

//	LOG_DEBUG("Target %s halted due to %s",
//		target_name(target), debug_reason_name(target));

	if (prev_target_state != TARGET_HALTED) {
		/* We have a halting debug event
		 * Either by triggered, or by debugger
		 */
		unsigned event;
		if (prev_target_state == TARGET_DEBUG_RUNNING)
			event = TARGET_EVENT_DEBUG_HALTED;
		else
			event = TARGET_EVENT_HALTED;

		retval = aarch64_debug_entry(target);
		if (retval != ERROR_OK)
			return retval;

		/* Call back to target to send TARGET_EVENT_GDB_HALT,
		 * so GDB could get back to its command prompt
		 */
LOG_DEBUG("event: %s", (event==TARGET_EVENT_HALTED) ?"HALTED" :"DEBUG_HALTED");
		target_call_event_callbacks(target, event);

		/* CAUTION: Set target->state here causing Infinite eval loop
		 *   halt -> poll -> halt -> ...
		 */

	} else {
//		LOG_WARNING("Halted to halted ?");
	}

	return retval;
}
static int aarch64_poll_smp(struct target *target)
{
	int retval_one;
	int retval_smp = ERROR_OK;

	LOG_DEBUG("-");

	while (NULL != target) {
		if (target->smp) {
			retval_one = aarch64_poll_one(target);
			if (ERROR_OK != retval_one) {
				retval_smp = retval_one;
				LOG_WARNING("Failed to poll SMP target %s", target_name(target));
				/* We continue to poll other SMP target(s) */
			}
		}

		target = target->next;
	}

	/* Last error message from among the SMP targets, if any */
	return retval_smp;
}

#if 0
void aarch64_set_smp_debug_reason(enum target_debug_reason debug_reason)
{
	struct target* target;

	LOG_DEBUG("-");

	target = all_targets;
	while (NULL != target) {
		target->debug_reason = debug_reason;
	}	// End of while (target)
}
#endif

/*
 * There is no way (i.e. Interrupt) from target to notify debugger
 * if its state changed (i.e. hit a breakpoint). The only thing we could
 * do is polling.
 * Thus, we need to read the register to know the current state to determine
 * if state changed.
 */
static int aarch64_poll(struct target *target)
{
	int retval;

	LOG_DEBUG("Target %s(%p)", target_name(target), target);

#if 0
	/*
	 * Read EDSCR to determine running/halted state
	 * If the core is halted and was previously not halted,
	 *     then enter debug state & read core registers etc.
	 * If the core has resumed (restarted) and was previously halted,
	 *     then note that it is now running
	 */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
	if (retval != ERROR_OK)
		return retval;
	aarch64->cpudbg_edscr = edscr;

	if (!armv8_is_pe_status_valid(edscr)) {
		LOG_ERROR("Unknown %s state. EDSCR = 0x%08"PRIx32,
			target_name(target), edscr);
		target->state = TARGET_UNKNOWN;
		return retval;
	}
#endif

	if (target->smp) {
		/* Assume H.W. ETM is enabled
		 * If there is no ETM, one has to poll_smp to detect any
		 * cores in SMP group has triggered debug state, halt
		 * the other cores as well, and poll again
		 */
		#if defined _NO_HW_ETM_
		debug_state = aarch64_poll_smp_state(all_targets);
		if (debug_state == <any core triggered debug state>) {
			retval = aarch64_halt_smp(all_targets);
			aarch64_set_smp_debug_reason(debug_reason);
		}
		#endif
		retval = aarch64_poll_smp(all_targets);
	} else
		retval = aarch64_poll_one(target);

#if 0
	if (PE_STATUS_HALTED(EDSCR_STATUS(edscr))) {
		target->debug_reason = armv8_edscr_debug_reason(edscr);

		/* In the case that the CORE is not halted by debugger
		 * ie: previous state != TARGET_HALTED
		 * We need to halt other cores as well, if SMP
		 */
		if (	(target->debug_reason == DBG_REASON_BREAKPOINT)
			 || (target->debug_reason == DBG_REASON_WATCHPOINT)
			 || (target->debug_reason == DBG_REASON_HLT) ) {
			if (target->smp) {
				/* Software solution: Halt other cores first
				 * for ETM, just call to poll other cores
				 */
				retval = aarch64_halt_smp(all_targets);
			}
		}	// End of if (! halted by debugger)
	}	// End of if PE_STATUS_HALTED()
#endif

	return retval;
}

static int aarch64_halt_one(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t edscr;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;

	if (target->state == TARGET_HALTED) {
		LOG_WARNING("Target %s has already been halted", target_name(target));
		return ERROR_OK;
	}

LOG_DEBUG("target = %s, dbgbase=%"PRIx32, target_name(target), armv8->debug_base);

	retval = armv8_cti_halt_single(target);
	if (retval != ERROR_OK)	return retval;

	long long t0 = timeval_ms();
	for (;; ) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
		if (retval != ERROR_OK)
			return retval;
		if (PE_STATUS_HALTED(EDSCR_STATUS(edscr)))
			break;
		if (timeval_ms() > t0 + 1000) {
			LOG_ERROR("Timeout waiting for halt");
			return ERROR_TARGET_TIMEOUT;
		}
	}

	/* H5.4.1 Acknowledges the trigger event */
	retval = armv8_cti_clear_trigger_events(target, ARMV8_CTI_CHANNEL_DEBUG);
	if (retval != ERROR_OK)
		return retval;

	/* We don't set target->state = TARGET_HALTED here,
	 * for 'poll' to identify the reason (differ from WPBP)
	 */
//	target->state = TARGET_HALTED;
	target->debug_reason = DBG_REASON_DBGRQ;
	LOG_DEBUG("Target %s(%p) is now halted", target_name(target), target);

	return ERROR_OK;
}

static int aarch64_halt_smp(struct target *target)
{
	int retval_one;
	int retval_smp = ERROR_OK;	

	LOG_DEBUG("-");

	while (NULL != target) {
		if (target->smp) {
			retval_one = aarch64_halt_one(target);
			if (ERROR_OK != retval_one) {
				retval_smp = retval_one;
				LOG_WARNING("Failed to halt SMP target %s", target_name(target));
				/* We continue to halt other SMP target(s) */
			}
		}

		target = target->next;
	}

	/* Last error message from among the SMP targets, if any */
	return retval_smp;
}

static int aarch64_halt(struct target *target)
{
	int retval;

	LOG_DEBUG("-");
	if (target->smp)
		retval = aarch64_halt_smp(all_targets);
	else
		retval = aarch64_halt_one(target);

	return retval;
}

static int aarch64_internal_restore(struct target *target, int current,
	uint64_t *address, int handle_breakpoints, int debug_execution)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;
	int retval;
	uint64_t resume_pc;

LOG_DEBUG("target %s, current=%d, addr=0x%" PRIx64 ", handle_breakpoints=%d, debug_execution=%d",
	target_name(target), current, *address, handle_breakpoints, debug_execution);

	if (!debug_execution)
		target_free_all_working_areas(target);

	/* current = 1: continue on current pc, otherwise continue at <address> */
	resume_pc = buf_get_u64(arm->pc->value, 0, 64);
	if (!current)
		resume_pc = *address;
	else
		*address = resume_pc;

	switch (arm->core_state) {
		case ARM_STATE_AARCH32:
			resume_pc &= 0xFFFFFFFC;
			break;
		case ARM_STATE_AARCH64:
			resume_pc &= 0xFFFFFFFFFFFFFFFC;
			break;
		default:
			LOG_ERROR("Unsupported %d state", arm->core_state);
			return ERROR_FAIL;
	}
	LOG_DEBUG("resume pc = 0x%016" PRIx64, resume_pc);
	buf_set_u64(arm->pc->value, 0, 64, resume_pc);
	arm->pc->dirty = 1;
	arm->pc->valid = 1;
#if 0	/* Alamy */
	/* restore dpm_mode at system halt */
	dpm_modeswitch(&armv8->dpm, ARM_MODE_ANY);
#endif
	/* called it now before restoring context because it uses cpu
	 * register x0 for restoring system control register */
	retval = aarch64_restore_system_control_reg(target);
	if (retval != ERROR_OK)
		return retval;
	retval = aarch64_restore_context(target, handle_breakpoints);
	if (retval != ERROR_OK)
		return retval;

//	target->debug_reason = DBG_REASON_NOTHALTED;
//	target->state = TARGET_RUNNING;

	/* registers are now invalid */
	register_cache_invalidate(arm->core_cache);

LOG_DEBUG("WARNING(Alamy): Review step over BP");
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

static int aarch64_internal_restart(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;
	struct adiv5_dap *swjdp = arm->dap;
	int retval;
	uint32_t edscr;
	uint32_t value;

	/*
	 * * Restart core and wait for it to be started.  Clear ITRen and sticky
	 * * exception flags: see ARMv7 ARM, C5.9.
	 *
	 * REVISIT: for single stepping, we probably want to
	 * disable IRQs by default, with optional override...
	 */

	LOG_DEBUG("WARNING(Alamy): Review this function");
#if 0
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
	if (retval != ERROR_OK)
		return retval;

	/* Something wrong if Instruction register is not empty ... */
	if ((edscr & ARMV8_EDSCR_ITE) == 0)
		LOG_ERROR("EDSCR InstrCompl must be set before leaving debug!");
#endif

	/* Clear Sticky Error (H9.2.40 EDRCR)
	 * Clear the EDSCR.{TXU, RXO, ERR} bits, and, if the processor
	 * is in Debug state, the EDSCR.ITO bit */
	retval = mem_ap_sel_write_atomic_u32(armv8->arm.dap, armv8->debug_ap,
			armv8->debug_base + ARMV8_REG_EDRCR, ARMV8_EDRCR_CSE);
	if (retval != ERROR_OK)
		return retval;

	/* H5.4.2 Restart request trigger event
	 * Debuggers must program the CTI to send Restart request trigger events
	 * only to PEs that are halted.
	 */


	/* H5.4.2 Restart request trigger event
	 * Before generating a Restart request trigger evnet for a PE, a debugger
	 * must ensure any Debug request trigger event targeting that PE is cleared
	 */
	/* Drop/Deactivate Debug request trigger event : CTIINTACK[0] = 0b1 */
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_INTACK,
			ARMV8_CTI_OUT_DEBUG);
	if (retval != ERROR_OK)
		return retval;
	/* Confirm that the output trigger has been deasserted:
	 *   CTITRIGOUTSTATUS[n] == 0b0
	 */
	int64_t wait = timeval_ms();		/* Start to wait at time 'wait' */
	do {
		retval = mem_ap_read_atomic_u32(swjdp,
				armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_TRIGOUTSTATUS,
				&value);
		if (retval != ERROR_OK)
			return retval;
		if ((value & ARMV8_CTI_OUT_DEBUG) == 0)
			break;

		if (timeval_ms() > wait + 1000) {
			LOG_ERROR("Timeout waiting for all trigger to be deasserted");
			return ERROR_TARGET_TIMEOUT;
		}
	} while (true);


	/* Actually restart the PE */
	retval = armv8_cti_generate_events(target, ARMV8_CTI_CHANNEL_RESTART);

	if (retval != ERROR_OK)
		return retval;

	/* Waiting for core to leave Debug state */
	/* H5.4.2 Restart request trigger event
	 * Debuggers can use EDPRSR.{SDR, HALTED} to determine the
	 * Execution state of the PE
	 */
	/* CAUTION: Do NOT compare with EDSCR_STATUS_NDBG,
	 * PE might already be in ARMV8_EDSCR_STATUS_HALT_NOSYND status
	 * when this function is called by 'step' command
	 */
	wait = timeval_ms();
	do {
		retval = mem_ap_read_atomic_u32(swjdp,
			armv8->debug_base + ARMV8_REG_EDPRSR,
			&value);
		if (retval != ERROR_OK) {
			return retval;
		}
		if (value & ARMV8_EDPRSR_SDR)	/* Sticky debug restart */
			break;
		if (timeval_ms() > wait + 1000) {
			LOG_ERROR("Timeout waiting for resume, EDPRSR=0x%.8x, EDPRSR.SDR=%d, EDPRSR.HALTED=%d",
				value,
				(value & ARMV8_EDPRSR_SDR) ? 1 : 0,
				(value & ARMV8_EDPRSR_HALTED) ? 1 : 0
			);
			return ERROR_TARGET_TIMEOUT;
		}
	} while (true);
	/* WARNING: target might be in "halted: NoSynd" state (step) */

	/*
	 * Read EDSCR to determine running/halted state
	 */
	retval = mem_ap_read_atomic_u32(swjdp,
			armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
	if (retval != ERROR_OK)
		return retval;
	if (PE_STATUS_HALTED(EDSCR_STATUS(edscr))) {
		switch (EDSCR_STATUS(edscr)) {
		case ARMV8_EDSCR_STATUS_STEP_NOSYND:
			/* This is acceptable in 'step' case */
			break;
		case ARMV8_EDSCR_STATUS_STEP_NORM:
		case ARMV8_EDSCR_STATUS_STEP_EXCL:
			/* Would these two case stops so fast ? */
			LOG_ERROR("Target %s step halted (0x%x) so fast (correct ?)",
				target_name(target), EDSCR_STATUS(edscr));
			break;
		default:
			LOG_ERROR("Target %s should not halted (0x%x)",
				target_name(target), EDSCR_STATUS(edscr));
			break;
		}
	}


	target->debug_reason = DBG_REASON_NOTHALTED;

//	target->state = TARGET_RUNNING;

	/* registers are now invalid */
	register_cache_invalidate(arm->core_cache);

	return ERROR_OK;
}

#if 0
static int aarch64_restore_smp(struct target *target, int handle_breakpoints)
{
	int retval = 0;
	struct target_list *head;
	struct target *curr;
	uint64_t address;

	head = target->head;
	while (head != (struct target_list *)NULL) {
		curr = head->target;
		if ((curr != target) && (curr->state != TARGET_RUNNING)) {
			/*  resume current address , not in step mode */
			retval += aarch64_internal_restore(curr, 1, &address,
					handle_breakpoints, 0);
			retval += aarch64_internal_restart(curr);
		}
		head = head->next;

	}
	return retval;
}
#endif

static int aarch64_resume_one(struct target *target, int current,
	uint64_t address, int handle_breakpoints, int debug_execution)
{
	int retval = 0;
	uint64_t addr = address;

	if (target->state != TARGET_HALTED)
		return ERROR_OK;

	/* Restore context */
	aarch64_internal_restore(target, current, &addr, handle_breakpoints,
		debug_execution);

	/* Kick off */
	retval = aarch64_internal_restart(target);
	/* WARNING: target might be in "halted: NoSynd" state, not running */

	if (!debug_execution) {
		target->state = TARGET_RUNNING;			/* WARNING */
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		LOG_DEBUG("target %s resumed at 0x%.16" PRIX64,
			target_name(target), addr);
	} else {
		target->state = TARGET_DEBUG_RUNNING;	/* WARNING */
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
		LOG_DEBUG("target %s debug resumed at 0x%.16" PRIX64,
			target_name(target), addr);
	}

	return ERROR_OK;
}

/*
 * Only the selected 'target' will resume at 'address', if ('current' == 0)
 */
static int aarch64_resume_smp(struct target *target, int current,
	uint64_t address, int handle_breakpoints, int debug_execution)
{
	struct target *t;
	int use_current;
#ifndef _ARMV8_CTI_
	int retval_one;
#endif
	int retval_smp = ERROR_OK;	

	LOG_DEBUG("-");

	/* H5.4.2 Restart request trigger event
	 * Debuggers must program the CTI to send Restart request trigger events
	 * only to PEs that are halted.
	 *
	 * When CTI is programmed properly, we just
	 *	call aarch64_internal_restore()
	 *  then trigger CTI
	 */

	for (t = all_targets; t; t = t->next) {
		if (! t->smp)	continue;

		/* Other core(s) resume at where they stopped */
		if (t == target)
			use_current = current;
		else
			use_current = 1;
#ifdef _ARMV8_CTI_	/* H.W. CTI */
		/* Restore context */
		aarch64_internal_restore(t, use_current, &address,
			handle_breakpoints, debug_execution);
#else
		retval_one = aarch64_resume_one(t, use_current, address,
			handle_breakpoints, debug_execution);
		if (ERROR_OK != retval_one) {
			retval_smp = retval_one;
			LOG_WARNING("Failed to resume SMP target %s", target_name(t));
			/* We continue to resume other SMP target(s) */
		}
#endif
	}

#ifdef	_ARMV8_CTI_
	retval_smp = armv8_cti_restart_smp(target);

	if (retval_smp != ERROR_OK)
		return retval_smp;

	if (!debug_execution) {
		target->state = TARGET_RUNNING;			/* WARNING */
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		LOG_DEBUG("target %s resumed at 0x%.16"PRIX64 " if %d",
			target_name(target), address, use_current);
	} else {
		target->state = TARGET_DEBUG_RUNNING;	/* WARNING */
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
		LOG_DEBUG("target %s debug resumed at 0x%.16"PRIX64 " if %d",
			target_name(target), address, use_current);
	}
#endif

	/* Last error message from among the SMP targets, if any */
	return retval_smp;
}

static int aarch64_resume(struct target *target, int current,
	uint64_t address, int handle_breakpoints, int debug_execution)
{
	int retval;

	LOG_DEBUG("-");
	if (target->smp)
		retval = aarch64_resume_smp(
			target, current, address, handle_breakpoints, debug_execution);
	else
		retval = aarch64_resume_one(
			target, current, address, handle_breakpoints, debug_execution);

	return retval;
}

/*
 * When PE enters Debug State ...
 */
static int aarch64_debug_entry(struct target *target)
{
//	uint32_t edscr;
	int retval = ERROR_OK;
//	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;

	LOG_DEBUG("WARNING(Alamy): Review this function: target=%s", target_name(target));

	/* Alamy: What is this ? */
	/* REVISIT see A8 TRM 12.11.4 steps 2..3 -- make sure that any
	 * imprecise data aborts get discarded by issuing a Data
	 * Synchronization Barrier:  ARMV4_5_MCR(15, 0, 0, 7, 10, 4).
	 */


	/* Examine debug reason */
	/* As this function is invoked by aarch64_poll() only (Oct-1, 2015),
	 * We don't re-read EDSCR which is already saved in aarch64->cpudbg_edscr.
	 * NOTE: may need to read EDSCR in the future */
//	target->debug_reason = armv8_edscr_debug_reason(aarch64->cpudbg_edscr);

	/* save address of instruction that triggered the watchpoint? */
	if (target->debug_reason == DBG_REASON_WATCHPOINT) {
		uint32_t wfar;

		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUDBG_WFAR,
				&wfar);
		if (retval != ERROR_OK)
			return retval;
		arm_dpm_report_wfar(&armv8->dpm, wfar);	/* Alamy: ***** need ARMv8 version */
	}

	/* Read X0..X30, SP, PC, and PSTATE */
	retval = arm_dpm_read_current_registers_64(&armv8->dpm);

	/* Let the post_debug_entry set the variables */
	if (armv8->post_debug_entry) {
		retval = armv8->post_debug_entry(target);
		if (retval != ERROR_OK)
			return retval;
	}

	return retval;
}

#if 0	/* Alamy: Remove this: replaced by Macro */
static uint32_t aarch64_dpm_mrs_opcode(
	uint32_t op0, uint32_t op1, uint32_t op2, uint32_t CRn, uint32_t CRm)
{
	uint32_t sys;

	sys = ((op0 & 0x3) << 19 | (op1 & 0x7) << 16 | (CRn & 0xF) << 12 |\
				(CRm & 0xF) << 8 | (op2 & 0x7) << 5);
	sys >>= 5;
	return ARMV8_MRS(sys, 0);
}
#endif

static int aarch64_post_debug_entry(struct target *target)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct armv8_mmu_common *armv8_mmu = &armv8->armv8_mmu;
	uint64_t pstate;
	uint64_t sctlr = 0;
	uint32_t edscr;
	uint32_t itr;
	int retval;

#if 1
	/* PSTATE.nRW: 0) AArch64, 1) AArch32 */
	struct reg *reg = armv8_get_reg_by_num(&(armv8->arm), AARCH64_PSTATE);
	pstate = buf_get_u64(reg->value, 0, 64);
	armv8->arm.core_state = (pstate & ARMV8_PSTATE_nRW)
		? ARM_STATE_AARCH32
		: ARM_STATE_AARCH64;
#else
	armv8->arm.core_state = ARM_STATE_AARCH64;	/* Alamy: just for debugging */
#endif

	/* Alamy: Get rid of 'is_aarch64' */
	target->is_aarch64 = (armv8->arm.core_state == ARM_STATE_AARCH64);



	/* Clear Sticky Error (H9.2.40 EDRCR)
	 * Clear the EDSCR.{TXU, RXO, ERR} bits, and, if the processor
	 * is in Debug state, the EDSCR.ITO bit */
	retval = mem_ap_sel_write_atomic_u32(armv8->arm.dap, armv8->debug_ap,
			armv8->debug_base + ARMV8_REG_EDRCR, ARMV8_EDRCR_CSE);
	if (retval != ERROR_OK)
		return retval;

	/* Read SCTRL register depending on EL */
	switch (EDSCR_EL(aarch64->cpudbg_edscr)) {
	case 0:	/* fall though */
	case 1:
		/* D7.2.81 SCTLR_EL1: Read SCTLR_EL1 into Xt
		 *	op0		op1		CRn		CRm		op2
		 * MRS <Xt>, SCTLR_EL1	11		000		0001	0000	000
		 */
		itr = A64_OPCODE_MRS(0b11, 0b000, 0b0001, 0b0000, 0b000, AARCH64_X0);
		break;

	case 2:
		/* D7.2.82 SCTLR_EL2: Read SCTLR_EL2 into Xt
		 *	op0		op1		CRn		CRm		op2
		 * MRS <Xt>, SCTLR_EL2	11		100		0001	0000	000
		 */
		itr = A64_OPCODE_MRS(0b11, 0b100, 0b0001, 0b0000, 0b000, AARCH64_X0);
		break;

	case 3:
		/* D7.2.83 SCTLR_EL3: Read SCTLR_EL3 into Xt
		 *	op0		op1		CRn		CRm		op2
		 * MRS <Xt>, SCTLR_EL3	11		110		0001	0000	000
		 */
		itr = A64_OPCODE_MRS(0b11, 0b110, 0b0001, 0b0000, 0b000, AARCH64_X0);
		break;

	default:
		/* Unknown EL: Impossible */
		assert(true);
		break;
	}	// End of switch(EL)

	retval = armv8->arm.mrs(target, itr, &sctlr);
	if (retval != ERROR_OK)
		return retval;

	LOG_DEBUG("%s EL%d sctlr = 0x%.8" PRIx32 ", SCTLR.{EE,I,SA,C,A,M}=%d,%d,%d,%d,%d,%d",
		target_name(target),
		EDSCR_EL(aarch64->cpudbg_edscr), (uint32_t)sctlr,
		(int)((sctlr >> 25) & 0b1),	/* EE: Endian */
		(int)((sctlr >> 12) & 0b1),	/*  I: Instruction cache */
		(int)((sctlr >>  3) & 0b1),	/* SA: Stack alignment check */
		(int)((sctlr >>  2) & 0b1),	/*  C: Data cacheable */
		(int)((sctlr >>  1) & 0b1),	/*  A: Load/Store register alignment check */
		(int)((sctlr >>  0) & 0b1)	/*  M: MMU enable */
		);
	aarch64->system_control_reg = sctlr;
	aarch64->system_control_reg_curr = sctlr;
	aarch64->curr_mode = armv8->arm.core_mode;

	armv8_mmu->mmu_enabled = sctlr & 0x1U ? 1 : 0;
	armv8_mmu->armv8_cache.d_u_cache_enabled = sctlr & 0x4U ? 1 : 0;
	armv8_mmu->armv8_cache.i_cache_enabled = sctlr & 0x1000U ? 1 : 0;

	if (armv8->armv8_mmu.armv8_cache.identified == false)
		armv8_identify_cache(target);


	/* ISB: flushes the pipeline in the PE */
	edscr = ARMV8_EDSCR_ITE;	/* after mrs, safe to skip waiting */
	itr = A64_OPCODE_ISB;
	/* void */ aarch64_exec_opcode(target, itr, &edscr);


	return ERROR_OK;
}

/*
 * current = 1: continue on current PC, otherwise continue at <address>
 */
static int aarch64_step(struct target *target,
	int current, uint64_t address,
	int handle_breakpoints)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	uint32_t edscr;
	int retval;
	uint32_t tmp;

	LOG_DEBUG("WARNING(Alamy): Review this function");
	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = mem_ap_read_atomic_u32(swjdp,
		armv8->debug_base + ARMV8_REG_EDSCR, &edscr);

LOG_DEBUG("current edscr=0x%x, saved edscr=0x%x", edscr, aarch64->cpudbg_edscr);
LOG_DEBUG("EL=%d, AArch[EL3..0]=0x%x", EDSCR_EL(edscr),
	((edscr & ARMV8_EDSCR_RW_MASK) >> ARMV8_EDSCR_RW_SHIFT));

#if 0
	/* Clear the pending Halting step debug event */
	retval = mem_ap_set_bits_u32(swjdp,
		armv8->debug_base + ARMV8_REG_EDESR,
		ARMV8_EDESR_SS);
	if (retval != ERROR_OK)	return retval;
#endif

#if 1
LOG_DEBUG("DBG");
	/* Enable halting step debug event */
	retval = mem_ap_set_bits_u32(swjdp,
		armv8->debug_base + ARMV8_REG_EDECR,
		ARMV8_EDECR_SS);
	if (retval != ERROR_OK)	return retval;

	/* H3.2.7 Disable interrupts while stepping */
	retval = mem_ap_set_bits_u32(swjdp,
		armv8->debug_base + ARMV8_REG_EDSCR,
		ARMV8_EDSCR_INTDIS_NSEL12_EXTALL);
	if (retval != ERROR_OK) {
		LOG_WARNING("Fail to disable Interrupt through EDSCR.INTdis\n");
		/* Alamy (***** WARNING): Might be no harm, let it go for now */
//		return retval;
	}
	/* Alamy: Should we also take care of PSTATE.{DAIF} ? */

#else
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUDBG_DECR, &tmp);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUDBG_DECR, (tmp|0x4));
	if (retval != ERROR_OK)
		return retval;
#endif

LOG_DEBUG("DBG");
	target->debug_reason = DBG_REASON_SINGLESTEP;
	retval = aarch64_resume(target, current, address, 0, 0);
	if (retval != ERROR_OK)
		return retval;

LOG_DEBUG("DBG");
	long long then = timeval_ms();
	while (target->state != TARGET_HALTED) {
#if 1
		retval = mem_ap_read_atomic_u32(swjdp,
			armv8->debug_base + CPUDBG_DESR, &tmp);
		if (retval != ERROR_OK)	return retval;
#else
		mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUDBG_DESR, &tmp);
#endif
		LOG_DEBUG("DESR = %#x", tmp);
		retval = aarch64_poll(target);
		if (retval != ERROR_OK)
			return retval;
LOG_DEBUG("DBG");
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("timeout waiting for target halt");
			return ERROR_TARGET_TIMEOUT;
		}
	}

LOG_DEBUG("DBG");
	/* Clear the pending Halting step debug event */
	retval = mem_ap_set_bits_u32(swjdp,
		armv8->debug_base + ARMV8_REG_EDESR,
		ARMV8_EDESR_SS);
	if (retval != ERROR_OK)	return retval;

LOG_DEBUG("DBG");
	/* Disable halting step debug event */
	retval = mem_ap_clear_bits_u32(swjdp,
		armv8->debug_base + ARMV8_REG_EDECR,
		ARMV8_EDECR_SS);
	if (retval != ERROR_OK)	return retval;
#if 0
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUDBG_DECR, (tmp&(~0x4)));
	if (retval != ERROR_OK)
		return retval;
#endif

LOG_DEBUG("DBG");
	target_call_event_callbacks(target, TARGET_EVENT_HALTED);
	if (target->state == TARGET_HALTED)
		LOG_DEBUG("target stepped");

	return ERROR_OK;
}

static int aarch64_restore_context(struct target *target, bool bpwp)
{
	struct armv8_common *armv8 = target_to_armv8(target);

	LOG_DEBUG(" ");

	if (armv8->pre_restore_context)
		armv8->pre_restore_context(target);

	return arm_dpm_write_dirty_registers(&armv8->dpm, bpwp);
}

/*
 * Cortex-A8 Breakpoint and watchpoint functions
 */

/* Setup hardware Breakpoint Register Pair */
static int aarch64_set_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t bptype)
{
	int retval;
	int brp_i = 0;
	uint32_t control;
	uint8_t byte_addr_select = 0x0F;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct aarch64_brp *brp_list = aarch64->brp_list;
	uint32_t dscr;

#ifdef	_DEBUG_BPWP_
	LOG_DEBUG("%s: type=0x%x, addr=0x%.16"PRIXMAX,
		target_name(target), bptype, breakpoint->address);
#endif

	if (breakpoint->set) {
		LOG_WARNING("breakpoint already set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		uint32_t addr_hi, addr_lo;
		while (brp_list[brp_i].used && (brp_i < aarch64->brp_num))
			brp_i++;
		if (brp_i >= aarch64->brp_num) {
			LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
		breakpoint->set = brp_i + 1;
		if (breakpoint->length == 2)
			byte_addr_select = (3 << (breakpoint->address & 0x02));
		control = ((bptype & 0b1111) << ARMV8_DBGBCR_BT_SHIFT)
			| ARMV8_DBGBCR_HMC
			| (byte_addr_select << ARMV8_DBGBCR_BAS_SHIFT)
			| (3 << ARMV8_DBGBCR_PMC_SHIFT)
			| ARMV8_DBGBCR_E;
		brp_list[brp_i].used = 1;
		brp_list[brp_i].value = breakpoint->address & 0xFFFFFFFFFFFFFFFC;
		brp_list[brp_i].control = control;

		addr_hi = brp_list[brp_i].value >> 32;
		addr_lo = (uint32_t)(brp_list[brp_i].value);

		retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ ARMV8_REG_DBGBVR_EL1_LO(brp_list[brp_i].BRPn), addr_lo);
		if (retval != ERROR_OK)
			return retval;
		retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ ARMV8_REG_DBGBVR_EL1_HI(brp_list[brp_i].BRPn), addr_hi);
		if (retval != ERROR_OK)
			return retval;
		retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ ARMV8_REG_DBGBCR_EL1(brp_list[brp_i].BRPn),
			brp_list[brp_i].control);
		if (retval != ERROR_OK)
			return retval;
#ifdef	_DEBUG_BPWP_
		LOG_DEBUG("brp %i control 0x%0" PRIx32 "(BT:0x%02x,LBN:0x%02x,BAS:0x%02x,E:%d) value 0x%.16" PRIx64, brp_i,
			brp_list[brp_i].control,
			(control & ARMV8_DBGBCR_BT_MASK) >> ARMV8_DBGBCR_BT_SHIFT,
			(control & ARMV8_DBGBCR_LBN_MASK) >> ARMV8_DBGBCR_LBN_SHIFT,
			(control & ARMV8_DBGBCR_BAS_MASK) >> ARMV8_DBGBCR_BAS_SHIFT,
			(control & ARMV8_DBGBCR_E) ? 1 : 0,
			brp_list[brp_i].value);
#endif
	} else if (breakpoint->type == BKPT_SOFT) {
		uint8_t code[4];
		buf_set_u32(code, 0, 32, A64_OPCODE_BRK(0x11));
LOG_DEBUG("set BRK code: 0x%x", A64_OPCODE_BRK(0x11));
		retval = target_read_memory(target,
				breakpoint->address & 0xFFFFFFFFFFFFFFFE,
				breakpoint->length, 1,
				breakpoint->orig_instr);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_memory(target,
				breakpoint->address & 0xFFFFFFFFFFFFFFFE,
				breakpoint->length, 1, code);
		if (retval != ERROR_OK)
			return retval;
		breakpoint->set = 0x11;	/* Any nice value but 0 */
	}

	retval = mem_ap_sel_read_atomic_u32(armv8->arm.dap, armv8->debug_ap,
					    armv8->debug_base + CPUDBG_DSCR, &dscr);
	/* Ensure that halting debug mode is enable */
	dscr = dscr | DSCR_HALT_DBG_MODE;
	retval = mem_ap_sel_write_atomic_u32(armv8->arm.dap, armv8->debug_ap,
					     armv8->debug_base + CPUDBG_DSCR, dscr);
	if (retval != ERROR_OK) {
		LOG_DEBUG("Failed to set DSCR.HDE");
		return retval;
	}

	return ERROR_OK;
}

static int aarch64_set_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode)
{
	int retval = ERROR_FAIL;
	int brp_i = 0;
	uint32_t control;
	uint8_t byte_addr_select = 0x0F;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct aarch64_brp *brp_list = aarch64->brp_list;

	LOG_DEBUG("WARNING(Alamy): Review this function");
	if (breakpoint->set) {
		LOG_WARNING("breakpoint already set");
		return retval;
	}
	/*check available context BRPs*/
	while ((brp_list[brp_i].used ||
		(brp_list[brp_i].type != BRP_CONTEXT)) && (brp_i < aarch64->brp_num))
		brp_i++;

	if (brp_i >= aarch64->brp_num) {
		LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
		return ERROR_FAIL;
	}

	breakpoint->set = brp_i + 1;
	control = ((matchmode & 0x7) << 20)
		| (1 << 13)
		| (byte_addr_select << 5)
		| (3 << 1) | 1;
	brp_list[brp_i].used = 1;
	brp_list[brp_i].value = (breakpoint->asid);
	brp_list[brp_i].control = control;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUDBG_BVR_BASE + 16 * brp_list[brp_i].BRPn,
			brp_list[brp_i].value);
	if (retval != ERROR_OK)
		return retval;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUDBG_BCR_BASE + 16 * brp_list[brp_i].BRPn,
			brp_list[brp_i].control);
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("brp %i control 0x%0" PRIx32 " value 0x%.16" PRIx64,
		brp_i, brp_list[brp_i].control, brp_list[brp_i].value);
	return ERROR_OK;

}

static int aarch64_set_hybrid_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	int retval = ERROR_FAIL;
	int brp_1 = 0;	/* holds the contextID pair */
	int brp_2 = 0;	/* holds the IVA pair */
	uint32_t control_CTX, control_IVA;
	uint8_t CTX_byte_addr_select = 0x0F;
	uint8_t IVA_byte_addr_select = 0x0F;
	uint8_t CTX_machmode = 0x03;
	uint8_t IVA_machmode = 0x01;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct aarch64_brp *brp_list = aarch64->brp_list;

	LOG_DEBUG("WARNING(Alamy): Review this function");
	if (breakpoint->set) {
		LOG_WARNING("breakpoint already set");
		return retval;
	}
	/*check available context BRPs*/
	while ((brp_list[brp_1].used ||
		(brp_list[brp_1].type != BRP_CONTEXT)) && (brp_1 < aarch64->brp_num))
		brp_1++;

	printf("brp(CTX) found num: %d\n", brp_1);
	if (brp_1 >= aarch64->brp_num) {
		LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
		return ERROR_FAIL;
	}

	while ((brp_list[brp_2].used ||
		(brp_list[brp_2].type != BRP_NORMAL)) && (brp_2 < aarch64->brp_num))
		brp_2++;

	printf("brp(IVA) found num: %d\n", brp_2);
	if (brp_2 >= aarch64->brp_num) {
		LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
		return ERROR_FAIL;
	}

	breakpoint->set = brp_1 + 1;
	breakpoint->linked_BRP = brp_2;
	control_CTX = ((CTX_machmode & 0x7) << 20)
		| (brp_2 << 16)
		| (0 << 14)
		| (1 << 13)
		| (CTX_byte_addr_select << 5)
		| (3 << 1) | 1;
	brp_list[brp_1].used = 1;
	brp_list[brp_1].value = (breakpoint->asid);
	brp_list[brp_1].control = control_CTX;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUDBG_BVR_BASE + 16 * brp_list[brp_1].BRPn,
			brp_list[brp_1].value);
	if (retval != ERROR_OK)
		return retval;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUDBG_BCR_BASE + 16 * brp_list[brp_1].BRPn,
			brp_list[brp_1].control);
	if (retval != ERROR_OK)
		return retval;

	control_IVA = ((IVA_machmode & 0x7) << 20)
		| (brp_1 << 16)
		| (1 << 13)
		| (IVA_byte_addr_select << 5)
		| (3 << 1) | 1;
	brp_list[brp_2].used = 1;
	brp_list[brp_2].value = (breakpoint->address & 0xFFFFFFFC);
	brp_list[brp_2].control = control_IVA;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUDBG_BVR_BASE + 16 * brp_list[brp_2].BRPn,
			brp_list[brp_2].value);
	if (retval != ERROR_OK)
		return retval;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUDBG_BCR_BASE + 16 * brp_list[brp_2].BRPn,
			brp_list[brp_2].control);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int aarch64_unset_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	int retval;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct aarch64_brp *brp_list = aarch64->brp_list;

	LOG_DEBUG("WARNING(Alamy): Review this function");
#ifdef	_DEBUG_BPWP_
	LOG_DEBUG("%s: addr=0x%.16"PRIXMAX,
		target_name(target), breakpoint->address);
#endif

	if (!breakpoint->set) {
		LOG_WARNING("breakpoint not set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		if ((breakpoint->address != 0) && (breakpoint->asid != 0)) {
			int brp_i = breakpoint->set - 1;
			int brp_j = breakpoint->linked_BRP;
			if ((brp_i < 0) || (brp_i >= aarch64->brp_num)) {
				LOG_DEBUG("Invalid BRP number in breakpoint");
				return ERROR_OK;
			}
			LOG_DEBUG("rbp %i control 0x%0" PRIx32 " value 0x%.16" PRIx64,
				brp_i, brp_list[brp_i].control, brp_list[brp_i].value);
			brp_list[brp_i].used = 0;
			brp_list[brp_i].value = 0;
			brp_list[brp_i].control = 0;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
				+ ARMV8_REG_DBGBCR_EL1(brp_list[brp_i].BRPn),
				brp_list[brp_i].control);
			if (retval != ERROR_OK)
				return retval;
			if ((brp_j < 0) || (brp_j >= aarch64->brp_num)) {
				LOG_DEBUG("Invalid BRP number in breakpoint");
				return ERROR_OK;
			}
			LOG_DEBUG("rbp %i control 0x%0" PRIx32 " value 0x%.16" PRIx64,
				brp_j, brp_list[brp_j].control, brp_list[brp_j].value);
			brp_list[brp_j].used = 0;
			brp_list[brp_j].value = 0;
			brp_list[brp_j].control = 0;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
				+ ARMV8_REG_DBGBCR_EL1(brp_list[brp_j].BRPn),
				brp_list[brp_j].control);
			if (retval != ERROR_OK)
				return retval;
			breakpoint->linked_BRP = 0;
			breakpoint->set = 0;
			return ERROR_OK;

		} else {
			int brp_i = breakpoint->set - 1;
			if ((brp_i < 0) || (brp_i >= aarch64->brp_num)) {
				LOG_DEBUG("Invalid BRP number in breakpoint");
				return ERROR_OK;
			}
			LOG_DEBUG("rbp %i control 0x%0" PRIx32 " value 0x%.16" PRIx64,
				brp_i, brp_list[brp_i].control, brp_list[brp_i].value);
			brp_list[brp_i].used = 0;
			brp_list[brp_i].value = 0;
			brp_list[brp_i].control = 0;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
				+ ARMV8_REG_DBGBCR_EL1(brp_list[brp_i].BRPn),
				brp_list[brp_i].control);
			if (retval != ERROR_OK)
				return retval;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
				+ CPUDBG_BVR_BASE + 16 * brp_list[brp_i].BRPn,
					brp_list[brp_i].value);
			if (retval != ERROR_OK)
				return retval;
			breakpoint->set = 0;
			return ERROR_OK;
		}
	} else {
		/* restore original instruction (kept in target endianness) */
		if (breakpoint->length == 4) {
			retval = target_write_memory(target,
					breakpoint->address & 0xFFFFFFFFFFFFFFFE,
					4, 1, breakpoint->orig_instr);
			if (retval != ERROR_OK)
				return retval;
		} else {
			retval = target_write_memory(target,
					breakpoint->address & 0xFFFFFFFFFFFFFFFE,
					2, 1, breakpoint->orig_instr);
			if (retval != ERROR_OK)
				return retval;
		}
	}
	breakpoint->set = 0;

	return ERROR_OK;
}

static int aarch64_add_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);

	if ((breakpoint->type == BKPT_HARD) && (aarch64->brp_num_available < 1)) {
		LOG_INFO("no hardware breakpoint available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
		aarch64->brp_num_available--;

	return aarch64_set_breakpoint(target, breakpoint, 0x00);	/* Exact match */
}

static int aarch64_add_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);

	if ((breakpoint->type == BKPT_HARD) && (aarch64->brp_num_available < 1)) {
		LOG_INFO("no hardware breakpoint available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
		aarch64->brp_num_available--;

	return aarch64_set_context_breakpoint(target, breakpoint, 0x02);	/* asid match */
}

static int aarch64_add_hybrid_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);

	if ((breakpoint->type == BKPT_HARD) && (aarch64->brp_num_available < 1)) {
		LOG_INFO("no hardware breakpoint available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
		aarch64->brp_num_available--;

	return aarch64_set_hybrid_breakpoint(target, breakpoint);	/* ??? */
}


static int aarch64_remove_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);

#if 0
/* It is perfectly possible to remove breakpoints while the target is running */
	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}
#endif

	if (breakpoint->set) {
		aarch64_unset_breakpoint(target, breakpoint);
		if (breakpoint->type == BKPT_HARD)
			aarch64->brp_num_available++;
	}

	return ERROR_OK;
}

/*
 * AArch64 Reset functions
 */

static int aarch64_assert_reset(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);

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
		LOG_ERROR("%s: how to reset? (hint: <target> configure -event reset-assert ...)",
			target_name(target));
		return ERROR_FAIL;
	}

	/* registers are now invalid */
	register_cache_invalidate(armv8->arm.core_cache);

	target->state = TARGET_RESET;

	return ERROR_OK;
}

static int aarch64_deassert_reset(struct target *target)
{
	int retval;

	LOG_DEBUG(" ");

	/* be certain SRST is off */
	jtag_add_reset(0, 0);

	retval = aarch64_poll(target);
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

/* ------------------------------------------------------------------ */

/*
 * Read 32-bit from 'addr' (32-bit aligned)
 * Copy 'size' bytes from 'buffer' to 'ofst'-th byte.
 * Write it back
 *
 * i.e.: ofst = 1, size = 2
 *	[addr]            [addr+2]
 *	+--------+--------+--------+--------+
 *	|        | ////// | ////// |        |
 *	+--------+--------+--------+--------+
 *	         buffer[0] buffer[1]
 */
static int aarch64_write_apb_ab_memory_misaligned_u32(
	struct armv8_common *armv8,
	uint64_t addr, uint32_t ofst, uint32_t size, const uint8_t *buffer)
{
	struct arm_dpm *dpm = &armv8->dpm;	/* or arm->dpm */

	int retval = ERROR_FAIL;
	uint32_t itr;				/* Instruction */
	uint64_t value;				/* [addr] --> tmp32 -> value -> X0 */
	uint8_t tmp32[4];			/* [buffer] --/                    */

LOG_DEBUG("addr=%"PRIx64", ofst=%d, size=%d\n", addr, ofst, size);
	/* 'addr' is 32-bit aligned */
	assert((addr & 0x3) == 0);
	assert((ofst < 4) && (size < 4));

	/* Clear any sticky error */
	retval = mem_ap_sel_write_atomic_u32(armv8->arm.dap, armv8->debug_ap,
		armv8->debug_base + ARMV8_REG_EDRCR, ARMV8_EDRCR_CSE);
	if (retval != ERROR_OK) {
		LOG_WARNING("Fail to clear sticky error");
		/* We could keep going, this is not a critical problem */
	}

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto dpm_done;

	/* 1. Read 32-bit data at 'addr' into 'tmp32' */
	/* x1 = addr: addr -> DCC -> X1 */
	retval = aarch64_instr_write_data_dcc_64(dpm,
			A64_OPCODE_MRS_DBGDTR_EL0(AARCH64_X1),	/* mrs x1, dbgdtr_el0 */
			addr);
	if (retval != ERROR_OK)
		goto dpm_done;

	/* tmp32 = value = X0 */
	itr = 0xb9400020;	/* ldr	w0, [x1] */		/* <-- EDSCR.ERR == 1 */
//	itr = 0xf9400020;	/* ldr	x0, [x1] */		/* <-- EDSCR.ERR == 1 */
	itr = 0xb8404420;	/* ldr	w0, [x1],#4 */	/* <-- GOOD */

#if 0
LDXR
STXR
LDR : C6-720
STR : C6-539
#endif

	retval = aarch64_instr_read_data_x0(dpm, itr, &value);
	if (retval != ERROR_OK)
		goto dpm_done;
	buf_set_u32(tmp32, 0, 32, (uint32_t)value);

	/* 2. Override with new data */
	memcpy(&(tmp32[ofst]), buffer, size);
	value = (uint64_t)buf_get_u32(tmp32, 0, 32);

	/* 3. Write 32-bit data back to 'addr' */
	/* Note: X1 still holds the address */
	itr = 0xb9000020;	/* str	w0, [x1] */		/* <-- EDSCR.ERR == 1 */
	itr = 0xb81fc020;	/* stur	w0, [x1,#-4] */

	retval = aarch64_instr_write_data_x0(dpm, itr, value);
	if (retval != ERROR_OK)
		goto dpm_done;

dpm_done:
	/* (void) */ dpm->finish(dpm);

	/* Clear any sticky error */
	/* (void) */ mem_ap_sel_write_atomic_u32(armv8->arm.dap, armv8->debug_ap,
		armv8->debug_base + ARMV8_REG_EDRCR, ARMV8_EDRCR_CSE);

	return retval;
}
static int aarch64_write_apb_ab_memory(struct target *target,
	uint64_t address, uint32_t size,
	uint32_t count, const uint8_t *buffer)
{
	/* write memory through APB-AP */
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;
	struct adiv5_dap *swjdp = armv8->arm.dap;
	struct arm_dpm *dpm = &armv8->dpm;	/* or arm->dpm */
	unsigned int total_bytes = count * size;
	unsigned int total_u32;
	uint32_t start_ofst = address & 0x3;
	uint32_t end_ofst   = (address + total_bytes) & 0x3;
	struct reg *reg;
	uint32_t itr;
	uint64_t value;
//	uint32_t dscr;
//	uint8_t *tmp_buff = NULL;
//	uint32_t i = 0;

	LOG_DEBUG("Writing APB-AP memory address 0x%" PRIx64 " size %" PRIu32 " count %" PRIu32,
			  address, size, count);
	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* In the case of total_bytes == 0 and address&3 == 1 | 2 | 3,
	 * DIV_ROUND_UP() would return 1, which is incorrect.
	 * Check total_bytes first to avoid error
	 */
	if (total_bytes == 0)
		return ERROR_OK;
	total_u32 = DIV_ROUND_UP((address & 3) + total_bytes, 4);

	/* Mark register X0 and X1 as dirty, as they will be used
	 * to transfer data.
	 * 'dirty' register value(s) will be restored automatically when
	 * exiting debug mode
	 */
	reg = armv8_get_reg_by_num(arm, AARCH64_X1);
	reg->dirty = true;

	reg = armv8_get_reg_by_num(arm, AARCH64_X0);
	reg->dirty = true;

	/* Handle first and last misaligned 32-bit data before entering dpm scope.
	 * Reason:
	 *  misaligned We need to call aarch64_read_apb_ab_memory() to read memory data.
	 *  It has its own dpm and error handling that we want to avoid
	 *  to cause recursive-alike situation in this function.
	 */
	if (start_ofst != 0) {
		uint32_t adj_size = (4 - start_ofst);

		retval = aarch64_write_apb_ab_memory_misaligned_u32(armv8,
			(address & ~0x3), start_ofst, adj_size, buffer);
		if (retval != ERROR_OK)
			return retval;

		/* Adjustment */
		address		+= adj_size;
		buffer		+= adj_size;
		total_bytes	-= adj_size;
		total_u32--;
	}
	if (total_bytes == 0)
		return ERROR_OK;

	assert((address & 0x3) == 0);
	if (end_ofst != 0) {
		assert(total_u32 != 0);

		retval = aarch64_write_apb_ab_memory_misaligned_u32(armv8,
			(address + total_bytes) & ~0x3, 0, end_ofst,
			buffer+total_bytes-end_ofst);
		if (retval != ERROR_OK)
			return retval;

		/* Adjustment */
		total_bytes -= end_ofst;
		total_u32--;
	}
	if (total_bytes == 0)
		return ERROR_OK;

	/* Now we should have address and size aligned to 32-bit boundary */
	assert((total_bytes & 0x3) == 0);
	assert(total_bytes == (total_u32 << 2));


	/* Clear any sticky error */
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
		armv8->debug_base + ARMV8_REG_EDRCR, ARMV8_EDRCR_CSE);
	if (retval != ERROR_OK) {
		LOG_WARNING("Fail to clear sticky error");
		/* We could keep going, this is not a critical problem */
	}

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto dpm_done;

	/* x1 = addr: addr -> DCC -> X1 */
	retval = aarch64_instr_write_data_dcc_64(dpm,
			A64_OPCODE_MRS_DBGDTR_EL0(AARCH64_X1),	/* mrs x1, dbgdtr_el0 */
			address);
	if (retval != ERROR_OK)
		goto dpm_done;

	while (total_u32) {
		if (total_u32 > 1) {
			value = buf_get_u64(buffer, 0, 64);
			itr = 0xf8008420;	/* str	x0, [x1],#8 */
			total_bytes = 8;
		} else {
			value = (uint64_t)buf_get_u32(buffer, 0, 32);
			itr = 0xb8004420;	/* str	w0, [x1],#4 */
			total_bytes = 4;
		}

		retval = aarch64_instr_write_data_x0(dpm, itr, value);
		if (retval != ERROR_OK)
			break;

		buffer += total_bytes;
		total_u32 -= total_bytes >> 2;

	}	/* End of while(total_u32) */

	goto dpm_done;

#if 0	/* Alamy: Get rid of this, not using malloc() */
	/* This algorithm comes from either :
	 * Cortex-A TRM Example 12-25
	 * Cortex-R4 TRM Example 11-26
	 * (slight differences)
	 */

	/* The algorithm only copies 32 bit words, so the buffer
	 * should be expanded to include the words at either end.
	 * The first and last words will be read first to avoid
	 * corruption if needed.
	 */

	tmp_buff = malloc(total_u32 * 4);
	if (tmp_buff == NULL) {
		LOG_ERROR("Unable to allocate memory");
		return ERROR_FAIL;
	}

	if ((start_ofst != 0) && (total_u32 > 1)) {
		/* First bytes not aligned - read the 32 bit word to avoid corrupting
		 * the other bytes in the word.
		 */
		retval = aarch64_read_apb_ab_memory(target, (address & ~0x3), 4, 1, tmp_buff);
		if (retval != ERROR_OK)
			goto error_free_buff_w;
	}

	/* If end of write is not aligned, or the write is less than 4 bytes */
	if ((end_ofst != 0) ||
		((total_u32 == 1) && (total_bytes != 4))) {

		/* Read the last word to avoid corruption during 32 bit write */
		int mem_offset = (total_u32-1) * 4;
		retval = aarch64_read_apb_ab_memory(target, (address & ~0x3) + mem_offset, 4, 1, &tmp_buff[mem_offset]);
		if (retval != ERROR_OK)
			goto error_free_buff_w;
	}

	/* Copy the write buffer over the top of the temporary buffer */
	memcpy(&tmp_buff[start_ofst], buffer, total_bytes);

	/* We now have a 32 bit aligned buffer that can be written */

	/* Read DSCR */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_w;

	/* Set DTR mode to Normal*/
	dscr = (dscr & ~DSCR_EXT_DCC_MASK) | DSCR_EXT_DCC_NON_BLOCKING;
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUDBG_DSCR, dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_w;

	if (size > 4) {
		LOG_WARNING("reading size >4 bytes not yet supported");
		goto error_unset_dtr_w;
	}

	/* mrs x1, dbgdtr_el0 */
	retval = aarch64_instr_write_data_dcc_64(arm->dpm, 0xd5330401, address+4);
	if (retval != ERROR_OK)
		goto error_unset_dtr_w;

	dscr = DSCR_INSTR_COMP;
	while (i < count * size) {
		uint32_t val;

		memcpy(&val, &buffer[i], size);
		/* mrs x0, dbgdtrrx_el0 */
		retval = aarch64_instr_write_data_dcc(arm->dpm, 0xd5330500, val);
		if (retval != ERROR_OK)
			goto error_unset_dtr_w;

		/* str w0, [x1, #-4] */
		retval = aarch64_exec_opcode(target, 0xb81fc020, &dscr);
		if (retval != ERROR_OK)
			goto error_unset_dtr_w;

		/* add x1, x1, #0x4 */
		retval = aarch64_exec_opcode(target, 0x91001021, &dscr);
		if (retval != ERROR_OK)
			goto error_unset_dtr_w;

		i += 4;
	}

	/* Check for sticky abort flags in the DSCR */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUDBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_w;
	if (dscr & (DSCR_STICKY_ABORT_PRECISE | DSCR_STICKY_ABORT_IMPRECISE)) {
		/* Abort occurred - clear it and exit */
		LOG_ERROR("abort occurred - dscr = 0x%08" PRIx32, dscr);
		mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					armv8->debug_base + CPUDBG_DRCR, 1<<2);
		goto error_free_buff_w;
	}

	/* Done */
	free(tmp_buff);
	return ERROR_OK;

error_unset_dtr_w:
	/* Unset DTR mode */
	mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUDBG_DSCR, &dscr);
	dscr = (dscr & ~DSCR_EXT_DCC_MASK) | DSCR_EXT_DCC_NON_BLOCKING;
	mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUDBG_DSCR, dscr);
error_free_buff_w:
	LOG_ERROR("error");
	free(tmp_buff);
	return ERROR_FAIL;
#endif

dpm_done:
	/* (void) */ dpm->finish(dpm);

	/* Clear any sticky error */
	/* (void) */ mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
		armv8->debug_base + ARMV8_REG_EDRCR, ARMV8_EDRCR_CSE);

	/* Done */
	return retval;
}

/* read memory through APB-AP */
static int aarch64_read_apb_ab_memory(struct target *target,
	uint64_t address, uint32_t size,
	uint32_t count, uint8_t *buffer)
{
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	struct arm *arm = &armv8->arm;
	struct arm_dpm *dpm = &armv8->dpm;	/* or arm->dpm */
	struct reg *reg;
	uint32_t itr, bytes;
	uint64_t value;

	LOG_DEBUG("Reading APB-AP memory address 0x%" PRIx64 " size %" PRIu32 " count %" PRIu32,
			  address, size, count);
	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Support 4-byte accessing only (for now?) */
	if (((address & 0x3) != 0) || (size != 4)) {
		LOG_WARNING("Unaligned access at 0x%.16" PRIx64, address);
		return ERROR_TARGET_UNALIGNED_ACCESS;
	}

	/* Mark register X0 and X1 as dirty, as they will be used
	 * to transfer data.
	 * 'dirty' register value(s) will be restored automatically when
	 * exiting debug mode
	 */
	reg = armv8_get_reg_by_num(arm, AARCH64_X1);
	reg->dirty = true;

	reg = armv8_get_reg_by_num(arm, AARCH64_X0);
	reg->dirty = true;

	/* Clear any sticky error */
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
		armv8->debug_base + ARMV8_REG_EDRCR, ARMV8_EDRCR_CSE);
	if (retval != ERROR_OK) {
		LOG_WARNING("Fail to clear sticky error");
		/* We could keep going, this is not a critical problem */
	}


	/* Basic 4-byte alignment reading loop */
	/* while() {
	 *	x1 = addr
	 *	ldr w0, [x1]
	 *	value = w0
	 * }
	 */

	/* Coding consideration:
	 * *) We could load addr to X0, then "mov X1, X0" with dpm->arm->msr()
	 *   dpm->arm->msr(target, opcode <mov X1, X0>, address);
	 * Here we load 'address' into X1 directly, but we need
	 * dpm->prepare & dpm->finish to encapsulate the operations ourself.
	 * *) Use X1 as address base and take the advantage of 'LDR' instruction
	 *   to increase the address after loading, only one function call is
	 *   needed in the loop.
	 * *) Load as many bytes as possible (ldr x0)
	 *
	 * Note:
	 *   It still works without dpm->prepare/finish as they don't do much
	 * at the moment, but it's a bad coding style. Others would not know
	 * what/how to follow. (Oct-19, 2015)
	 */

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto dpm_done;

	/* x1 = addr: addr -> DCC -> X1 */
	retval = aarch64_instr_write_data_dcc_64(dpm,
			A64_OPCODE_MRS_DBGDTR_EL0(AARCH64_X1),	/* mrs x1, dbgdtr_el0 */
			address);
	if (retval != ERROR_OK)
		goto dpm_done;

	while (count) {
		/* ldr x0/w0, [x1] */
		if (count > 1) {
			itr = 0xf8408420;	/* ldr	x0, [x1],#8 */
			bytes = 8;			/* two words */
		} else {
			itr = 0xb8404420;	/* ldr	w0, [x1],#4 */
			bytes = 4;			/* one word */
		}

		/* value = X0 */
		retval = aarch64_instr_read_data_x0(dpm, itr, &value);
		if (retval != ERROR_OK)
			break;

		if (count > 1)
			buf_set_u64(buffer, 0, 64, value);
		else
			buf_set_u32(buffer, 0, 32, (uint32_t)value);

		buffer += bytes;
		count  -= bytes >> 2;

	}	/* End of while(count--) */

dpm_done:
	/* (void) */ dpm->finish(dpm);


	/* Clear any sticky error */
	/* (void) */ mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
		armv8->debug_base + ARMV8_REG_EDRCR, ARMV8_EDRCR_CSE);

	/* Done */
	return retval;
}

static int aarch64_read_phys_memory(struct target *target,
	uint64_t address, uint32_t size,
	uint32_t count, uint8_t *buffer)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	uint8_t apsel = swjdp->apsel;
	LOG_DEBUG("Reading memory at real address 0x%.16" PRIX64 "; size %" PRId32
		"; count %" PRId32, address, size, count);

	if (count && buffer) {

		if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {

			/* read memory through AHB-AP */
			retval = mem_ap_sel_read_buf(swjdp, armv8->memory_ap, buffer, size, count, address);
		} else {
			/* read memory through APB-AP */
			retval = aarch64_mmu_modify(target, 0);
			if (retval != ERROR_OK)
				return retval;
			retval = aarch64_read_apb_ab_memory(target, address, size, count, buffer);
		}
	}
	return retval;
}

static int aarch64_read_memory(struct target *target, uint64_t address,
	uint32_t size, uint32_t count, uint8_t *buffer)
{
	int mmu_enabled = 0;
	uint64_t virt, phys;
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	uint8_t apsel = swjdp->apsel;

	LOG_DEBUG("WARNING(Alamy): Review this function");
	/* aarch64 handles unaligned memory access */
	LOG_DEBUG("Reading memory at address 0x%.16" PRIX64 "; size %" PRId32
		  "; count %" PRId32, address, size, count);

	/* determine if MMU was enabled on target stop */
	/* Alamy: Error */
	retval = aarch64_mmu(target, &mmu_enabled);
	if (retval != ERROR_OK)
		return retval;

	if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {
		if (mmu_enabled) {
			virt = address;
			retval = aarch64_virt2phys(target, virt, &phys);
			if (retval != ERROR_OK)
				return retval;

			LOG_DEBUG("Reading at virtual address. Translating virt:0x%.16"
				PRIX64 " to phys:0x%.16" PRIX64, virt, phys);
			address = phys;
		}
		retval = aarch64_read_phys_memory(target, address, size, count,
						  buffer);
	} else {
		if (mmu_enabled) {
			retval = aarch64_check_address(target, address);
			if (retval != ERROR_OK)
				return retval;
			/* enable MMU as we could have disabled it for phys
			   access */
			retval = aarch64_mmu_modify(target, 1);
			if (retval != ERROR_OK)
				return retval;
		}
		retval = aarch64_read_apb_ab_memory(target, address, size,
						    count, buffer);
	}
	return retval;
}

static int aarch64_write_phys_memory(struct target *target,
	uint64_t address, uint32_t size,
	uint32_t count, const uint8_t *buffer)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	uint8_t apsel = swjdp->apsel;

	LOG_DEBUG("WARNING(Alamy): Review this function");
	LOG_DEBUG("Writing memory to real address 0x%.16" PRIX64 "; size %" PRId32
		"; count %" PRId32, address, size, count);

	if (count && buffer) {

		if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {

			/* write memory through AHB-AP */
			retval = mem_ap_sel_write_buf(swjdp, armv8->memory_ap, buffer, size, count, address);
		} else {

			/* write memory through APB-AP */
			retval = aarch64_mmu_modify(target, 0);
			if (retval != ERROR_OK)
				return retval;
			return aarch64_write_apb_ab_memory(target, address, size, count, buffer);
		}
	}


	/* REVISIT this op is generic ARMv7-A/R stuff */
	if (retval == ERROR_OK && target->state == TARGET_HALTED) {
//		struct arm_dpm *dpm = armv8->arm.dpm;
		struct armv8_cache_common *armv8_cache = &(armv8->armv8_mmu.armv8_cache);

#if 0	/* Alamy: we have dpm in I-/D- cache flushing functions */
		retval = dpm->prepare(dpm);
		if (retval != ERROR_OK)
			return retval;
#endif

		/* The Cache handling will NOT work with MMU active, the
		 * wrong addresses will be invalidated!
		 *
		 * For both ICache and DCache, walk all cache lines in the
		 * address range. Cortex-A8 has fixed 64 byte line length.
		 *
		 * REVISIT per ARMv7, these may trigger watchpoints ...
		 */

		/* invalidate I-Cache */
		if (armv8_cache->i_cache_enabled) {
			retval = armv8_invalidate_icache(target);
			if (retval != ERROR_OK)
				return retval;
		}

		/* invalidate D-Cache */
		if ((armv8_cache->d_u_cache_enabled) &&
			(armv8_cache->flush_dcache_all)) {
			armv8_cache->flush_dcache_all(target);
		}

#if 0
		/* (void) */ dpm->finish(dpm);
#endif
	}

	return retval;
}

static int aarch64_write_memory(struct target *target, uint64_t address,
	uint32_t size, uint32_t count, const uint8_t *buffer)
{
	int mmu_enabled = 0;
	uint64_t virt, phys;
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	uint8_t apsel = swjdp->apsel;

	LOG_DEBUG("WARNING(Alamy): Review this function");
	/* aarch64 handles unaligned memory access */
	LOG_DEBUG("Writing memory at address 0x%.16" PRIX64 "; size %" PRId32
		  "; count %" PRId32, address, size, count);

	/* determine if MMU was enabled on target stop */
	retval = aarch64_mmu(target, &mmu_enabled);
	if (retval != ERROR_OK)
		return retval;

	if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {
		LOG_DEBUG("Writing memory to address 0x%.16" PRIX64 "; size %"
			PRId32 "; count %" PRId32, address, size, count);
		if (mmu_enabled) {
			virt = address;
			retval = aarch64_virt2phys(target, virt, &phys);
			if (retval != ERROR_OK)
				return retval;

			LOG_DEBUG("Writing to virtual address. Translating virt:0x%.16"
				PRIX64 " to phys:0x%.16" PRIX64, virt, phys);
			address = phys;
		}
		retval = aarch64_write_phys_memory(target, address, size,
				count, buffer);
	} else {
		if (mmu_enabled) {
			retval = aarch64_check_address(target, address);
			if (retval != ERROR_OK)
				return retval;
			/* enable MMU as we could have disabled it for phys access */
			retval = aarch64_mmu_modify(target, 1);
			if (retval != ERROR_OK)
				return retval;
		}
		retval = aarch64_write_apb_ab_memory(target, address, size, count, buffer);
	}
	return retval;
}

/* ------------------------------------------------------------------ */

static int aarch64_load_core_reg_u64(struct target *target,
	uint32_t num, uint64_t *value)
{
	/* I found that there is no use of this function in ARMv8
	 * Log an error for future investigation */
	LOG_ERROR("Implement this function");

	return ERROR_OK;
}

static int aarch64_store_core_reg_u64(struct target *target,
	uint32_t num, uint64_t value)
{
	/* I found that there is no use of this function in ARMv8
	 * Log an error for future investigation */
	LOG_ERROR("Implement this function");

	return ERROR_OK;
}

static int aarch64_handle_target_request(void *priv)
{
	struct target *target = priv;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval;

//	LOG_WARNING("Alamy: review this function: 64-bit data ?");
	if (!target_was_examined(target))
		return ERROR_OK;
	if (!target->dbg_msg_enabled)
		return ERROR_OK;

	if (target->state == TARGET_RUNNING) {
		uint32_t request;
		uint32_t edscr;
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + ARMV8_REG_EDSCR, &edscr);

		/* Almay: refer to read_dcc_64
		 * read both ARMV8_REG_DBGDTRRX_EL0 and ARMV8_REG_DBGDTRTX_EL0 ?
		 */

		/* check if we have data */
		while ((edscr & ARMV8_EDSCR_TXFULL) && (retval == ERROR_OK)) {
			retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					armv8->debug_base + CPUDBG_DTRTX, &request);
			if (retval == ERROR_OK) {
				target_request(target, request);
				retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
						armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
			}
		}
	}

	return ERROR_OK;
}

/*
   Experience version to examine Juno r1 platform.
   - Juno r1 has two TAPs
     TAP0 - Cortex-M3 Versatile Express: System Power management ?

     TAP1 - ARMv8
         AP0 (IDR=0x14770004): Rev=1, JEP106(cont,code)=(4,3b), class=MEM-AP, ID(var,type)=(0,AXI-AP)
         AP1 (IDR=0x44770002): Rev=4, JEP106(cont,code)=(4,3b), class=MEM-AP, ID(var,type)=(0,APB-AP)

   - Skip dbgbase stuff (target CPU determination).

*/
static int aarch64_examine_first(struct target *target)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval = ERROR_OK;
	uint32_t pfr, debug, cpuid, edscr, edprsr;
	int i;

	LOG_DEBUG("WARNING(Alamy): Review this function");
#if 0	/* Alamy */
	/* We do one extra read to ensure DAP is configured,
	 * we call ahbap_debugport_init(swjdp) instead
	 */
	retval = ahbap_debugport_init(swjdp);
	if (retval != ERROR_OK)
		return retval;
#else
LOG_DEBUG("Debug: target=%s(%p), aarch64=%p, armv8=%p, dap=%p", target_name(target), target, aarch64, armv8, swjdp);
	retval = debugport_init(swjdp);
	if (retval != ERROR_OK)
		return retval;
#endif

#if 0
	/* Alamy: This code is not perfect to detect every platform */
	/*  Juno r1 (ARMv8) has AXI-AP & APB-AP, not AHB-AP */

	/* Search for the APB-AB - it is needed for access to debug registers */
	retval = dap_find_ap(swjdp, AP_TYPE_APB_AP, &armv8->debug_ap);
	if (retval != ERROR_OK) {
		LOG_ERROR("Could not find APB-AP for debug access");
		return retval;
	}
	/* Search for the AHB-AB */
	retval = dap_find_ap(swjdp, AP_TYPE_AHB_AP, &armv8->memory_ap);
	if (retval != ERROR_OK) {
		/* AHB-AP not found - use APB-AP */
		LOG_DEBUG("Could not find AHB-AP - using APB-AP for memory access");
		armv8->memory_ap_available = false;
	} else {
		armv8->memory_ap_available = true;
	}
#endif

	/* Assign armv8->debug_base */
	if (!target->dbgbase_set) {
		uint32_t dbgbase;
		/* Get ROM Table base */
		uint32_t apid;
		int32_t coreidx = target->coreid;

		LOG_DEBUG("[-dbgbase] not set for core %d, trying to detect ...", coreidx);
		/* Alamy: WARNING: scan tap to find a debug port (Juno r1 at AP==1) */
		retval = dap_get_debugbase(swjdp, 1, &dbgbase, &apid);
		if (retval != ERROR_OK)
			return retval;

		dbgbase &= ~0x3;
		LOG_DEBUG("dbgbase=%x", dbgbase);


		/* Alamy: ***** WARNING *****
		 *	Need to verify it's ROM Table (but it is)
		 *	One should scan all APs (So far we know it's in AP1)
		 */
		dap_ap_select(swjdp, 1);	/* Alamy(***** WARNING *****): Why it's in DAP1 ? (Juno) */
		/* devtype(0x5,0x1) = Debug Logic - Processor */
		/* ARM Cortex-A57: PID=0x00000004002BBD07 --> 0x4BB, 0xD07 */
		/* ARM Cortex-A53: PID=0x00000004003BBD03 --> 0x4BB, 0xD03 */
		retval = dap_romtable_lookup_cs_component(swjdp,
				dbgbase, 0x15,
				JEP106_ARM, PID_PART_CORTEX_A53,
				&coreidx,
				&armv8->debug_base);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("Detected core %" PRId32 " dbgbase: %08" PRIx32,
			  coreidx, armv8->debug_base);
	} else
		armv8->debug_base = target->dbgbase;

	/* Alamy: Hacking for Juno r1
		armv8->debug_ap == 0, but we need it to be 1
	 */
	armv8->debug_ap = 1;

	LOG_DEBUG("target %s(%p), debug_ap=%d, dbgbase=0x%x, coreidx=%d",
		target_name(target), target,
		armv8->debug_ap,
		armv8->debug_base,
		target->coreid);

	/* Unlock access to CORE, CTI, PMU, and ETM components */
	retval = mem_ap_write_u32(swjdp,
			armv8->debug_base + ARMV8_CORE_BASE_OFST + CS_REG_LAR,
			0xC5ACCE55);

	if (retval != ERROR_OK)
		goto err_fail_unlock;
#if 0
	retval = mem_ap_write_u32(swjdp,
			armv8->debug_base + ARMV8_CTI_BASE_OFST + CS_REG_LAR,
			0xC5ACCE55);
#else
	retval = armv8_cti_init(target);
#endif
	if (retval != ERROR_OK)
		goto err_fail_unlock;
	retval = mem_ap_write_u32(swjdp,
			armv8->debug_base + ARMV8_PMU_BASE_OFST + CS_REG_LAR,
			0xC5ACCE55);
	if (retval != ERROR_OK)
		goto err_fail_unlock;
	retval = mem_ap_write_u32(swjdp,
			armv8->debug_base + ARMV8_ETM_BASE_OFST + CS_REG_LAR,
			0xC5ACCE55);
	if (retval != ERROR_OK)
		goto err_fail_unlock;

	retval = dap_run(swjdp);
	if (retval != ERROR_OK) {
err_fail_unlock:
		LOG_ERROR("Fail to unlock Core/CTI/PMU/ETM access");
		return retval;
	}

	/* Unlock External Debugger accesses to ETM registers */
	/* Alamy: Same as CS_REG_LAR ? above */
	retval = mem_ap_write_atomic_u32(swjdp,
			armv8->debug_base + ARMV8_REG_OSLAR_EL1, 0);
	if (retval != ERROR_OK) {
		LOG_DEBUG("Fail to unlock ETM access");
		return retval;
	}


	/* Dump some useful information */
	retval = mem_ap_read_u32(swjdp,
		armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
	if (retval != ERROR_OK)
		goto err_fail_dump_regs;
	retval = mem_ap_read_u32(swjdp,
		armv8->debug_base + ARMV8_REG_EDPRSR, &edprsr);
	if (retval != ERROR_OK)
		goto err_fail_dump_regs;
	retval = mem_ap_read_u32(swjdp,
		armv8->debug_base + ARMV8_REG_MIDR_EL1, &cpuid);
	if (retval != ERROR_OK)
		goto err_fail_dump_regs;
	retval = mem_ap_read_u32(swjdp,
		armv8->debug_base + ARMV8_REG_ID_AA64PFR0_EL1, &pfr);
	if (retval != ERROR_OK)
		goto err_fail_dump_regs;
	retval = mem_ap_read_u32(swjdp,
		armv8->debug_base + ARMV8_REG_ID_AA64DFR0_EL1, &debug);
	if (retval != ERROR_OK)
		goto err_fail_dump_regs;

	retval = dap_run(swjdp);
	if (retval != ERROR_OK) {
err_fail_dump_regs:
		LOG_ERROR("Fail to dump core registers");
		return retval;
	}
	LOG_DEBUG("EDSCR  = 0x%08" PRIx32, edscr);
	LOG_DEBUG("EDPRSR = 0x%08" PRIx32, edprsr);
	LOG_DEBUG("cpuid  = 0x%08" PRIx32, cpuid);
	LOG_DEBUG("ID_AA64PFR0_EL1 = 0x%08" PRIx32, pfr);
	LOG_DEBUG("ID_AA64DFR0_EL1 = 0x%08" PRIx32, debug);


	armv8->arm.core_type = ARM_MODE_MON;	/* Alamy: check this */
	/* Execution state may change every time entering Debug State.
	 * It's just a default value here. see debug_entry for detail */
	armv8->arm.core_state = ARM_STATE_AARCH64;

	retval = aarch64_dpm_setup(aarch64, debug);
	if (retval != ERROR_OK)
		return retval;

	/* Setup Breakpoint Register Pairs */
	aarch64->brp_num = ((debug >> 12) & 0x0F) + 1;
	aarch64->brp_num_context = ((debug >> 28) & 0x0F) + 1;

	/* hack - no context bpt support yet */
	aarch64->brp_num_context = 0;

	aarch64->brp_num_available = aarch64->brp_num;
	aarch64->brp_list = calloc(aarch64->brp_num, sizeof(struct aarch64_brp));
	if (aarch64->brp_list == NULL)
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;

	for (i = 0; i < aarch64->brp_num; i++) {
		aarch64->brp_list[i].used = 0;
		if (i < (aarch64->brp_num-aarch64->brp_num_context))
			aarch64->brp_list[i].type = BRP_NORMAL;
		else
			aarch64->brp_list[i].type = BRP_CONTEXT;
		aarch64->brp_list[i].value = 0;
		aarch64->brp_list[i].control = 0;
		aarch64->brp_list[i].BRPn = i;
	}

	LOG_DEBUG("Configured %i hw breakpoints", aarch64->brp_num);

	target_set_examined(target);
	return ERROR_OK;
}

static int aarch64_examine(struct target *target)
{
	int retval = ERROR_OK;

LOG_DEBUG("Alamy");
	/* don't re-probe hardware after each reset */
	if (!target_was_examined(target))
		retval = aarch64_examine_first(target);

LOG_DEBUG("Alamy");
	/* Configure core debug access */
	if (retval == ERROR_OK)
		retval = aarch64_init_debug_access(target);

	return retval;
}

/*
 *	Cortex-A8 target creation and initialization
 */

static int aarch64_init_target(struct command_context *cmd_ctx,
	struct target *target)
{
	LOG_DEBUG("%s", target_name(target));
	/* examine_first() does a bunch of this */
	return ERROR_OK;
}

static int aarch64_init_arch_info(struct target *target,
	struct aarch64_common *aarch64, struct jtag_tap *tap)
{
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct adiv5_dap *dap = &armv8->dap;

	/* Setup struct aarch64_common */
	aarch64->common_magic = AARCH64_COMMON_MAGIC;
	/*  tap has no dap initialized */
	if (!tap->dap) {
		armv8->arm.dap = dap;
		/* Setup struct aarch64_common */

		/* prepare JTAG information for the new target */
		aarch64->jtag_info.tap = tap;
		aarch64->jtag_info.scann_size = 4;

		/* Leave (only) generic DAP stuff for debugport_init() */
		dap->jtag_info = &aarch64->jtag_info;

		/* Number of bits for tar autoincrement, impl. dep. at least 10 */
		dap->tar_autoincr_block = (1 << 10);
		dap->memaccess_tck = 80;
		tap->dap = dap;
	} else
		armv8->arm.dap = tap->dap;

	aarch64->fast_reg_read = 0;

	/* register arch-specific functions */
	armv8->load_core_reg_u64 = aarch64_load_core_reg_u64;
	armv8->store_core_reg_u64 = aarch64_store_core_reg_u64;

	armv8->examine_debug_reason = NULL;

	armv8->post_debug_entry = aarch64_post_debug_entry;

	armv8->pre_restore_context = NULL;

	armv8->armv8_mmu.read_physical_memory = aarch64_read_phys_memory;

	/* REVISIT v7a setup should be in a v7a-specific routine */
	armv8_init_arch_info(target, armv8);
	target_register_timer_callback(aarch64_handle_target_request, 1, 1, target);

	return ERROR_OK;
}

static int aarch64_target_create(struct target *target, Jim_Interp *interp)
{
	struct aarch64_common *aarch64 = calloc(1, sizeof(struct aarch64_common));

	if (aarch64 == NULL) {
		return ERROR_FAIL;
	}

	return aarch64_init_arch_info(target, aarch64, target->tap);
}

static void aarch64_deinit_target(struct target *target)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct arm_dpm *dpm = &aarch64->armv8_common.dpm;

	free(aarch64->brp_list);
	free(dpm->dbp);
	free(dpm->dwp);
	free(aarch64);
}

static int aarch64_mmu(struct target *target, int *enabled)
{
	if (target->state != TARGET_HALTED) {
		LOG_ERROR("target %s not halted", target_name(target));
		return ERROR_TARGET_INVALID;
	}

	*enabled = target_to_aarch64(target)->armv8_common.armv8_mmu.mmu_enabled;
	return ERROR_OK;
}

static int aarch64_virt2phys(struct target *target, uint64_t virt,
			     uint64_t *phys)
{
	int retval = ERROR_FAIL;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	uint8_t apsel = swjdp->apsel;

	if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {
		uint32_t ret;
		retval = armv8_mmu_translate_va(target,
				virt, &ret);
		if (retval != ERROR_OK)
			goto done;
		*phys = ret;
	} else {/*  use this method if armv8->memory_ap not selected
		 *  mmu must be enable in order to get a correct translation */
		retval = aarch64_mmu_modify(target, 1);
		if (retval != ERROR_OK)
			goto done;
		retval = armv8_mmu_translate_va_pa(target, virt,  phys, 1);
	}
done:
	return retval;
}

COMMAND_HANDLER(aarch64_handle_cache_info_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct armv8_common *armv8 = target_to_armv8(target);

	return armv8_handle_cache_info_command(CMD_CTX,
			&armv8->armv8_mmu.armv8_cache);
}


COMMAND_HANDLER(aarch64_handle_dbginit_command)
{
	struct target *target = get_current_target(CMD_CTX);

#if 0
unsigned i;
LOG_DEBUG("-");
for (i = 0; i < CMD_ARGC; ++i) {
LOG_DEBUG("CMD_ARGV[%d] = %s", i, CMD_ARGV[i]);
}
	/* Hacking: get the target passed by 'dbginit' command */
	if (CMD_ARGC >= 1)
		target = get_target(CMD_ARGV[0]);
#endif

	if (!target_was_examined(target)) {
		LOG_ERROR("target %s not examined yet", target_name(target));
		return ERROR_FAIL;
	}

	return aarch64_init_debug_access(target);
}
COMMAND_HANDLER(aarch64_handle_smp_off_command)
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

COMMAND_HANDLER(aarch64_handle_smp_on_command)
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

COMMAND_HANDLER(aarch64_handle_smp_gdb_command)
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
		command_print(CMD_CTX, "gdb coreid  %" PRId32 " -> %" PRId32, target->gdb_service->core[0]
			, target->gdb_service->core[1]);
	}
	return ERROR_OK;
}


extern const struct command_registration aarch64_debug_subcommand_handlers[];

static const struct command_registration aarch64_exec_command_handlers[] = {
	{
		.name = "cache_info",
		.handler = aarch64_handle_cache_info_command,
		.mode = COMMAND_EXEC,
		.help = "display information about target caches",
		.usage = "",
	},
	{
		.name = "dbginit",
		.handler = aarch64_handle_dbginit_command,
		.mode = COMMAND_EXEC,
		.help = "Initialize core debug",
		.usage = "",
	},
	{   .name = "smp_off",
	    .handler = aarch64_handle_smp_off_command,
	    .mode = COMMAND_EXEC,
	    .help = "Stop smp handling",
	    .usage = "",},
	{
		.name = "smp_on",
		.handler = aarch64_handle_smp_on_command,
		.mode = COMMAND_EXEC,
		.help = "Restart smp handling",
		.usage = "",
	},
	{
		.name = "smp_gdb",
		.handler = aarch64_handle_smp_gdb_command,
		.mode = COMMAND_EXEC,
		.help = "display/fix current core played to gdb",
		.usage = "",
	},

	{
		.name = "debug",
//		.handler = aarch64_handle_debug_command,
		.mode = COMMAND_ANY,
		.help = "debugging opcode",
		.chain = aarch64_debug_subcommand_handlers,
		.usage = "",
	},


	COMMAND_REGISTRATION_DONE
};

static const struct command_registration aarch64_command_handlers[] = {
	{	/* Register ARMv8 commands first to override commands in armv4_5.c
		 * i.e.: "disassemble"
		 */
		.chain = armv8_command_handlers,
	},
	{
		.chain = arm_command_handlers,
	},
	{
		.name = "aarch64",
		.mode = COMMAND_ANY,
		.help = "AArch64 command group",
		.usage = "",
		.chain = aarch64_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct target_type_64 aarch64_target = {
	.name = "aarch64",

	.poll = aarch64_poll,
	.arch_state = armv8_arch_state,

	.halt = aarch64_halt,
	.resume = aarch64_resume,
	.step = aarch64_step,

	.assert_reset = aarch64_assert_reset,
	.deassert_reset = aarch64_deassert_reset,

	/* REVISIT allow exporting VFP3 registers ... */
	.get_gdb_reg_list = armv8_get_gdb_reg_list,

	.read_memory = aarch64_read_memory,
	.write_memory = aarch64_write_memory,

#if 0	/* Alamy: Enable later */
	.checksum_memory = arm_checksum_memory,
	.blank_check_memory = arm_blank_check_memory,

	.run_algorithm = armv4_5_run_algorithm,
#endif

	.add_breakpoint = aarch64_add_breakpoint,
	.add_context_breakpoint = aarch64_add_context_breakpoint,
	.add_hybrid_breakpoint = aarch64_add_hybrid_breakpoint,
	.remove_breakpoint = aarch64_remove_breakpoint,
	.add_watchpoint = NULL,
	.remove_watchpoint = NULL,

	.commands = aarch64_command_handlers,
	.target_create = aarch64_target_create,
	.init_target = aarch64_init_target,
	.deinit_target = aarch64_deinit_target,
	.examine = aarch64_examine,

	.read_phys_memory = aarch64_read_phys_memory,
	.write_phys_memory = aarch64_write_phys_memory,
	.mmu = aarch64_mmu,
	.virt2phys = aarch64_virt2phys,
};
