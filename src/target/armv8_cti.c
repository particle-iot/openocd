/***************************************************************************
 *   Copyright (C) 2015 by Alamy Liu                                       *
 *   alamy.liu@gmail.com                                                   *
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

/* H5.3 An ARMv8 PE must include a cross-trigger interface, and the
 * implementation must include at least the input and output triggers defined
 * in this architecture.
 * The minimum number of channels in the CTM is three. (CTIDEVID.NUMCHAN)
 */

/* H5.4
 * Output trigger events
 *	0	CTI -> PE			Debug   request trigger event
 *	1	CTI -> PE			Restart request trigger event
 *	2	CTI -> IRQ			Generic CTI interrupt trigger event
 *	3	(reserved)
 *	4-7	CTI -> Trace ext.	Generic Trace external input trigger events (opt)
 *
 * Input trigger events
 *	0	PE -> CTI			Cross-halt trigger event
 *	1	PE -> CTI			Performance Monitor overflow trigger event
 *	2-3	(reserved)
 *	4-7	Trace ext. -> CTI	Generic trace external output trigger events (opt)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/time_support.h>
#include "armv8.h"
#include "armv8_opcodes.h"
#include "target.h"
#include "target_type64.h"
#include "armv8_cti.h"


#define	_DEBUG_CTI_FUNC_ENTRY_	/* "<<<") entering; ">>>") leaving */


/* H5.4.1 Debug request trigger event (CTI->PE) */
	/* The trigger event is asserted until acknowledged by the debugger.
	 * The debugger acknowledges the trigger event by
	 * writing 1 to CTIINTACK[0]
	 */

	/* A debugger must poll CTITRIGOUTSTATUS[n] until it reads as 0,
	 * to confirm that the output trigger has been deasserted.
	 */

/* H5.4.2 Restart request trigger event (CTI->PE) */
	/* If the PE is not in Debug state, the request is ignored and dropped by the CTI.
	 * Meaning: No H.W. trigger happened.
	 */
	/* Debuggers can use EDPRSR.{SDR, HALTED} to determine the Execution state of the PE */
	/* Before generating a Restart request trigger event for a PE,
	 * Debugger must ensure any Debug request trigger event targeting
	 * that PE is cleared (CTIINTACK, CTITRIGOUTSTATUS[0] == 0)
	 */
	/* Determine the execution state of the PE. EDPRSR.{SDR, HALTED} */
	/* The trigger event is self-acknowledging (No further action required) */

/* H5.4.3 Cross-halt trigger event (PE->CTI) */


/* H5.5.1 CTI reset (H8.8 External debug register resets)
 * All CTI output triggers and output channels are deasserted on an
 * External Debug reset
 */
int armv8_cti_reset(void)
{
#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG("<<<");
#endif

	/* CTI registers resides in 'External debug' reset domain */

	/* Nothing to do */

	return ERROR_OK;
}

/**
 * Initiate CTI (Cross Trigger Interface)
 *
 * @param target The TARGET
 */
int armv8_cti_init(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *dap = armv8->arm.dap;
	int rc = ERROR_FAIL;
	uint8_t restore_debug_ap = dap_ap_get_select(dap);
	uint32_t cti_base = armv8->debug_base + ARMV8_CTI_BASE_OFST;

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG("<<< target %s", target_name(target));
#endif

	/* Use mem_ap_read/write_xxx() instead of mem_ap_sel_read/write_xxx() */
	dap_ap_select(dap, armv8->debug_ap);

	/* Unlock access to CTI */
	rc = mem_ap_write_atomic_u32(dap, cti_base + CS_REG_LAR, 0xC5ACCE55);
	if (rc != ERROR_OK)	goto err;

	/* Enable CTI */
	rc = mem_ap_write_atomic_u32(dap, cti_base + ARMV8_REG_CTI_CONTROL,
		ARMV8_CTI_CONTROL_GLBEN);
	if (rc != ERROR_OK)	goto err;

	/* Disable all cross-trigger events by default */
	rc = mem_ap_write_atomic_u32(dap, cti_base + ARMV8_REG_CTI_GATE, 0);
	if (rc != ERROR_OK)	goto err;

err:
	dap_ap_select(dap, restore_debug_ap);

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG(">>> rc = %d", rc);
#endif
	return rc;
}

/**
 * Generate an event pulses on ECT channels
 *
 * @param target The TARGET
 * @param channel_events Channel events (could be 'or' together)
 */
int armv8_cti_generate_events(
	struct target *target,
	int channel_events)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *dap = armv8->arm.dap;
	int rc = ERROR_FAIL;

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG("<<< target %s generate cti channel events 0x%x",
		target_name(target), channel_events);
#endif

	/* CTIAPPPULSE[n] = 1
	 * Generate a channel event on channel select channels */
	rc = mem_ap_sel_write_atomic_u32(dap, armv8->debug_ap,
		armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_APPPULSE,
		channel_events);

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG(">>> rc = %d", rc);
#endif
	return rc;
}

/**
 * Clear the trigger request by writing 1 to CTIINTACK[n]
 *
 * @param target The TARGET
 * @param out_trigger_events The events to be clear (could be 'or' together)
 */
int armv8_cti_clear_trigger_events(
	struct target *target,
	int out_trigger_events)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *dap = armv8->arm.dap;
	int rc = ERROR_FAIL;
	uint8_t restore_debug_ap = dap_ap_get_select(dap);
	uint32_t value;

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG("<<< target %s clear cti trigger events 0x%x",
		target_name(target), out_trigger_events);
#endif

	dap_ap_select(dap, armv8->debug_ap);

	rc = mem_ap_write_atomic_u32(dap,
		armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_INTACK,
		out_trigger_events);
	if (rc != ERROR_OK)
		goto err;

	/* H9.3.24 CTIINTACK
	 * A debugger must poll CTITRIGOUTSTATUS to confirm that the output
	 * trigger has been acknowledged/deasserted
	 *	CTITRIGOUTSTATUS[n] == 0b0
	 */

	int64_t t0 = timeval_ms();		/* Start to wait at time 't0' */
	do {
		rc = mem_ap_read_atomic_u32(dap,
			armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_TRIGOUTSTATUS,
			&value);
		if (rc != ERROR_OK)	goto err;
		if ((value & out_trigger_events) == 0)
			break;

		if (timeval_ms() > t0 + 1000) {
			LOG_ERROR("%s: timeout waiting for 0x%x trigger event to be deasserted",
				target_name(target), out_trigger_events);
			rc = ERROR_TARGET_TIMEOUT;
			goto err;
		}
	} while (true);

err:
	dap_ap_select(dap, restore_debug_ap);

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG(">>> rc = %d", rc);
#endif
	return rc;
}

/**
 * Halt a single core (Example H5-1 Halting a single PE)
 *
 * @param target The TARGET
 */
int armv8_cti_halt_single(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *dap = armv8->arm.dap;
	int rc = ERROR_FAIL;
	uint8_t restore_debug_ap = dap_ap_get_select(dap);

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG("<<< halting target %s", target_name(target));
#endif

	dap_ap_select(dap, armv8->debug_ap);

	/* 1. CTIGATE[0] = 0
	 * So that the CTI does not pass channel events on internal channel 0 to
	 * the CTM */
	rc = mem_ap_clear_bits_u32(dap,
		armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_GATE,
		ARMV8_CTI_CHANNEL_DEBUG);
	if (rc != ERROR_OK)	goto err;

	/* 2. CTIOUTEN0[0] = 1
	 * So that the CTI generates a Debug request trigger event in response to
	 * a channel event on channel 0 */
	rc = mem_ap_write_atomic_u32(dap,
		armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_OUTEN(ARMV8_CTI_OUT_DEBUG),
		ARMV8_CTI_CHANNEL_DEBUG);
	if (rc != ERROR_OK)	goto err;

	/* 3. CTIAPPPULSE[0] = 1
	 * Generate a channel event on channel 0 */
	rc = armv8_cti_generate_events(target, ARMV8_CTI_CHANNEL_DEBUG);
	if (rc != ERROR_OK)	goto err;


	/* When the PE has entered Debug state, clear the Debug request trigger
	 * event: CTIINTACK[0] = 1 */


err:
	dap_ap_select(dap, restore_debug_ap);

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG(">>> rc = %d", rc);
#endif
	return rc;
}

/**
 * Enable Cross-Halt for all targets in the SMP group
 * Example H5-2 Halting all PEs in a group when any one PE halts
 *
 * @param target The TARGET used to restore selected AP
 */
int armv8_cti_enable_cross_halt(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);;
	struct adiv5_dap *dap = armv8->arm.dap;;
	int rc = ERROR_FAIL;
	uint8_t restore_debug_ap = dap_ap_get_select(dap);

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG("<<<");
#endif

	for (target = all_targets; target; target = target->next) {
		if (! target->smp)	continue;

		armv8 = target_to_armv8(target);
		dap = armv8->arm.dap;

		dap_ap_select(dap, armv8->debug_ap);

		/* 1. CTIGATE[2] = 1
		 * So that each CTI passes channel events on internal channel 2
		 * to the CTM */
		rc = mem_ap_set_bits_u32(dap,
			armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_GATE,
			ARMV8_CTI_CHANNEL_CROSS_HALT);
		if (rc != ERROR_OK)	goto err;

		/* 2. CTIINEN0[2] = 1
		 * So that each CTI generates a channel event on channel 2
		 * in response to a Cross-halt trigger event */
		rc = mem_ap_write_atomic_u32(dap,
			armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_INEN(ARMV8_CTI_IN_CROSS_HALT),
			ARMV8_CTI_CHANNEL_CROSS_HALT);
		if (rc != ERROR_OK)	goto err;

		/* 3. CTIOUTEN0[2] = 1
		 * So that each CTI generates a Debug request trigger event
		 * in response to an channel event on channel 2 */
		rc = mem_ap_write_atomic_u32(dap,
			armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_OUTEN(ARMV8_CTI_OUT_DEBUG),
			ARMV8_CTI_CHANNEL_CROSS_HALT);
		if (rc != ERROR_OK)	goto err;
	}	/* End of for(target) */


	/* When a PE has halted, clear the Debug request trigger event by
	 * write a value of 1 to CTIINTACK[0] */


err:
	dap_ap_select(dap, restore_debug_ap);

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG(">>> rc = %d", rc);
#endif
	return rc;
}

/**
 * Enable Cross-Halt and Cross-Restart for all targets in the SMP group
 * Combine the cross-halt & restart code together
 *
 * @param target The TARGET used to restore selected AP
 */
int armv8_cti_enable_cross_restart(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);;
	struct adiv5_dap *dap = armv8->arm.dap;;
	int rc = ERROR_FAIL;
	uint8_t restore_debug_ap = dap_ap_get_select(dap);

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG("<<<");
#endif

	for (target = all_targets; target; target = target->next) {
		if (! target->smp)	continue;

		armv8 = target_to_armv8(target);
		dap = armv8->arm.dap;

		dap_ap_select(dap, armv8->debug_ap);

		/* 1. If the PE was halted because of Debug request trigger event,
		 * the debugger must ensure the trigger event is deasserted.
		 * a. CTIINTACK[0] = 1: clear the Debug request trigger event
		 * b. while(CTITRIGOUTSTATUS[0] != 0): confirm that the trigger event
		 * has been deasserted. */
		/* H5.4.2 Restart request trigger event
		 * Before generating a Restart request trigger evnet for a PE, a debugger
		 * must ensure any Debug request trigger event targeting that PE is cleared
		 */
		/* Alamy: WARNING: Should it be ARMV8_CTI_CHANNEL_DEBUG ? */
		rc = armv8_cti_clear_trigger_events(target, ARMV8_CTI_CHANNEL_DEBUG);
		if (rc != ERROR_OK)
			goto err;

		/* 2. CTIGATE[1] = 1
		 * So that each CTI passes channel events on internal channel 1
		 * to the CTM */
		rc = mem_ap_set_bits_u32(dap,
			armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_GATE,
			ARMV8_CTI_CHANNEL_RESTART);
		if (rc != ERROR_OK)
			goto err;

		/* 3. CTIOUTEN1[1] = 1
		 * So that each CTI generates a Restart request trigger event
		 * in response to a channel event on channel 1 */
		rc = mem_ap_set_bits_u32(dap,
			armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_OUTEN(ARMV8_CTI_OUT_RESTART),
			ARMV8_CTI_CHANNEL_RESTART);
		if (rc != ERROR_OK)
			goto err;
	}	/* End of for(target) */

err:
	dap_ap_select(dap, restore_debug_ap);

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG(">>> rc = %d", rc);
#endif

	return rc;
}

/**
 * Restart all targets in the SMP group
 * Example H5-3 Synchronously restarting a group of PEs
 *
 * @param target The TARGET used to restore selected AP
 *
 * CAUTION:
 * Make sure it's SMP before calling this function, or 'armv8' would be NULL.
 */
int armv8_cti_restart_smp(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);;
	struct adiv5_dap *dap = armv8->arm.dap;;
	int rc = ERROR_FAIL;
	uint8_t restore_debug_ap = dap_ap_get_select(dap);
	uint32_t value;

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG("<<<");
#endif

	for (target = all_targets; target; target = target->next) {
		if (! target->smp)	continue;

		armv8 = target_to_armv8(target);
		dap = armv8->arm.dap;

		dap_ap_select(dap, armv8->debug_ap);

		/* 1. If the PE was halted because of Debug request trigger event,
		 * the debugger must ensure the trigger event is deasserted.
		 * a. CTIINTACK[0] = 1: clear the Debug request trigger event
		 * b. while(CTITRIGOUTSTATUS[0] != 0): confirm that the trigger event
		 * has been deasserted. */
		/* H5.4.2 Restart request trigger event
		 * Before generating a Restart request trigger evnet for a PE, a debugger
		 * must ensure any Debug request trigger event targeting that PE is cleared
		 */
		/* Alamy: WARNING: Should it be ARMV8_CTI_CHANNEL_DEBUG ? */
		rc = armv8_cti_clear_trigger_events(target, ARMV8_CTI_CHANNEL_DEBUG);
		if (rc != ERROR_OK)
			goto err;

		/* 2. CTIGATE[1] = 1
		 * So that each CTI passes channel events on internal channel 1
		 * to the CTM */
		rc = mem_ap_set_bits_u32(dap,
			armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_GATE,
			ARMV8_CTI_CHANNEL_RESTART);
		if (rc != ERROR_OK)
			goto err;

		/* 3. CTIOUTEN1[1] = 1
		 * So that each CTI generates a Restart request trigger event
		 * in response to a channel event on channel 1 */
		rc = mem_ap_write_atomic_u32(dap,
			armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_OUTEN(ARMV8_CTI_OUT_RESTART),
			ARMV8_CTI_CHANNEL_RESTART);
		if (rc != ERROR_OK)
			goto err;
	}	/* End of for(target) */

	/* 4. CTIAPPPULSE[1] = 1 on any one PE in the group
	 * To generate a channel event on channel 1 */
	assert(armv8 != NULL);	/* 'armv8' should point to the last smp target */
	rc = mem_ap_write_atomic_u32(dap,
		armv8->debug_base + ARMV8_CTI_BASE_OFST + ARMV8_REG_CTI_APPPULSE,
		ARMV8_CTI_CHANNEL_RESTART);

	/* Determine the execution state of the PE. EDPRSR.{SDR, HALTED} */
	int64_t t0;
	uint32_t edscr;
	for (target = all_targets; target; target = target->next) {
		if (! target->smp)	continue;

		armv8 = target_to_armv8(target);
		dap = armv8->arm.dap;

		dap_ap_select(dap, armv8->debug_ap);

		t0 = timeval_ms();		/* Start to wait at time 't0' */
		do {
			rc = mem_ap_read_atomic_u32(dap,
				armv8->debug_base + ARMV8_REG_EDPRSR, &value);
			if (rc != ERROR_OK)
				goto err;
			if (value & ARMV8_EDPRSR_SDR)	/* Sticky debug restart */
				break;
			if (timeval_ms() > t0 + 1000) {
				LOG_ERROR("Timeout waiting %s to restart, EDPRSR.{SDR,HALTED}={%d,%d}",
					target_name(target),
					(value & ARMV8_EDPRSR_SDR) ? 1 : 0,
					(value & ARMV8_EDPRSR_HALTED) ? 1 : 0
				);
				/* continue to check next target(core) */
			}
		} while (true);
		/* WARNING: target might be HALTED.
		 * i.e.: "halted: NoSynd" of 'step' execution */

		/*
		 * Read EDSCR to determine running/halted state
		 */
		rc = mem_ap_read_atomic_u32(dap,
			armv8->debug_base + ARMV8_REG_EDSCR, &edscr);
		if (rc != ERROR_OK)
			goto err;
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
	}	/* End of for(target) */

err:
	dap_ap_select(dap, restore_debug_ap);

#ifdef	_DEBUG_CTI_FUNC_ENTRY_
	LOG_DEBUG(">>> rc = %d", rc);
#endif
	return rc;
}
