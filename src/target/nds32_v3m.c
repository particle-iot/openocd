/***************************************************************************
 *   Copyright (C) 2012 Andes technology.                                  *
 *   Hsiangkai Wang <hkwang@andestech.com>                                 *
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/time_support.h>
#include <helper/binarybuffer.h>
#include "breakpoints.h"
#include "nds32_insn.h"
#include "nds32_reg.h"
#include "nds32_edm.h"
#include "nds32_cmd.h"
#include "nds32_v3.h"
#include "nds32_v3m.h"
#include "target_type.h"

static int nds32_v3m_register_mapping(struct nds32 *nds32, int reg_no)
{
	if (reg_no == PC)
		return IR11;

	return reg_no;
}

static int nds32_v3m_get_debug_reason(struct nds32 *nds32, uint32_t *reason)
{
	uint32_t edmsw;
	struct aice_port_s *aice = target_to_aice(nds32->target);
	aice->port->api->read_debug_reg(NDS_EDM_SR_EDMSW, &edmsw);

	*reason = (edmsw >> 12) & 0x0F;

	return ERROR_OK;
}

static int nds32_v3m_activate_hardware_breakpoint(struct target *target)
{
	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(target);
	struct aice_port_s *aice = target_to_aice(target);
	struct breakpoint *bp;
	unsigned brp_num = nds32_v3m->n_hbr - 1;

	for (bp = target->breakpoints; bp; bp = bp->next) {
		if (bp->type == BKPT_SOFT) {
			/* already set at nds32_v3m_add_breakpoint() */
			continue;
		} else if (bp->type == BKPT_HARD) {
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPA0 + brp_num, bp->address);  /* set address */
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPAM0 + brp_num, 0);   /* set mask */

			if (nds32_v3m->nds32.memory.address_translation)
				/* enable breakpoint (virtual address) */
				aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + brp_num, 0x2);
			else
				/* enable breakpoint (physical address) */
				aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + brp_num, 0xA);

			LOG_DEBUG("Add hardware BP %d at %08" PRIx32, brp_num,
					bp->address);

			brp_num--;
		} else {
			return ERROR_FAIL;
		}
	}

	return ERROR_OK;
}

static int nds32_v3m_deactivate_hardware_breakpoint(struct target *target)
{
	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(target);
	struct aice_port_s *aice = target_to_aice(target);
	struct breakpoint *bp;
	unsigned brp_num = nds32_v3m->n_hbr - 1;

	for (bp = target->breakpoints; bp; bp = bp->next) {
		if (bp->type == BKPT_SOFT)
			continue;
		else if (bp->type == BKPT_HARD)
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + brp_num, 0x0); /* disable breakpoint */
		else
			return ERROR_FAIL;

		LOG_DEBUG("Remove hardware BP %d at %08" PRIx32, brp_num,
				bp->address);

		brp_num--;
	}

	return ERROR_OK;
}

static int nds32_v3m_activate_hardware_watchpoint(struct target *target)
{
	struct aice_port_s *aice = target_to_aice(target);
	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(target);
	struct watchpoint *wp;
	unsigned wp_num = 0;
	uint32_t wp_config = 0;

	for (wp = target->watchpoints; wp; wp = wp->next) {

		wp->mask = wp->length - 1;
		if ((wp->address % wp->length) != 0)
			wp->mask = (wp->mask << 1) + 1;

		if (wp->rw == WPT_READ)
			wp_config = 0x3;
		else if (wp->rw == WPT_WRITE)
			wp_config = 0x5;
		else if (wp->rw == WPT_ACCESS)
			wp_config = 0x7;

		/* set/unset physical address bit of BPCn according to PSW.DT */
		if (nds32_v3m->nds32.memory.address_translation == false)
			wp_config |= 0x8;

		aice->port->api->write_debug_reg(NDS_EDM_SR_BPA0 + wp_num,
				wp->address - (wp->address % wp->length));  /* set address */
		aice->port->api->write_debug_reg(NDS_EDM_SR_BPAM0 + wp_num, wp->mask);    /* set mask */
		aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + wp_num, wp_config);    /* enable watchpoint */

		LOG_DEBUG("Add hardware wathcpoint %d at %08" PRIx32 " mask %08" PRIx32, wp_num,
				wp->address, wp->mask);

		wp_num++;
	}

	return ERROR_OK;
}

static int nds32_v3m_deactivate_hardware_watchpoint(struct target *target)
{
	struct aice_port_s *aice = target_to_aice(target);
	struct watchpoint *wp;
	unsigned wp_num = 0;

	for (wp = target->watchpoints; wp; wp = wp->next) {
		aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + wp_num, 0x0); /* disable watchpoint */

		LOG_DEBUG("Remove hardware wathcpoint %d at %08" PRIx32 " mask %08" PRIx32, wp_num,
				wp->address, wp->mask);

		wp_num++;
	}

	return ERROR_OK;
}

static int nds32_v3m_check_interrupt_stack(struct nds32_v3m_common *nds32_v3m)
{
	struct nds32 *nds32 = &(nds32_v3m->nds32);
	uint32_t val_ir0;
	uint32_t value;

	/* Save interrupt level */
	nds32_get_mapped_reg(nds32, IR0, &val_ir0);
	nds32->current_interrupt_level = (val_ir0 >> 1) & 0x3;

	if (nds32_reach_max_interrupt_level(nds32))
		LOG_ERROR("Reaching the max interrupt stack level %d", nds32->current_interrupt_level);

	/* backup $ir6 to avoid suppressed exception overwrite */
	nds32_get_mapped_reg(nds32, IR6, &value);

	return ERROR_OK;
}

static int nds32_v3m_restore_interrupt_stack(struct nds32_v3m_common *nds32_v3m)
{
	struct nds32 *nds32 = &(nds32_v3m->nds32);
	uint32_t value;

	/* get backup value from cache */
	/* then set back to make the register dirty */
	nds32_get_mapped_reg(nds32, IR0, &value);
	nds32_set_mapped_reg(nds32, IR0, value);

	nds32_get_mapped_reg(nds32, IR6, &value);
	nds32_set_mapped_reg(nds32, IR6, value);

	return ERROR_OK;
}

/**
 * Save processor state.  This is called after a HALT instruction
 * succeeds, and on other occasions the processor enters debug mode
 * (breakpoint, watchpoint, etc).
 */
static int nds32_v3m_debug_entry(struct nds32 *nds32, bool enable_watchpoint)
{
	LOG_DEBUG("nds32_v3m_debug_entry");

	jtag_poll_set_enabled(false);

	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(nds32->target);

	/* deactivate all hardware breakpoints */
	CHECK_RETVAL(nds32_v3m_deactivate_hardware_breakpoint(nds32->target));

	if (enable_watchpoint)
		CHECK_RETVAL(nds32_v3m_deactivate_hardware_watchpoint(nds32->target));

	nds32->target->state = TARGET_HALTED;
	nds32_examine_debug_reason(nds32);

	if (nds32->init_arch_info_after_halted == false) {
		/* init architecture info according to config registers */
		CHECK_RETVAL(nds32_config(nds32));

		nds32->init_arch_info_after_halted = true;
	}

	/* REVISIT entire cache should already be invalid !!! */
	register_cache_invalidate(nds32->core_cache);

	/* Save registers. */
	nds32_full_context(nds32);

	/* check interrupt level */
	nds32_v3m_check_interrupt_stack(nds32_v3m);

	/* record fpu/audio status */
	nds32_check_extension(nds32);

	return ERROR_OK;
}

/* target request support */
static int nds32_v3m_target_request_data(struct target *target,
		uint32_t size, uint8_t *buffer)
{
	/* AndesCore could use DTR register to communicate with OpenOCD to output messages
	 * Target data will be put in buffer
	 * The format of DTR is as follow
	 * DTR[31:16] => length, DTR[15:8] => size, DTR[7:0] => target_req_cmd
	 * target_req_cmd has three possible values:
	 *   TARGET_REQ_TRACEMSG
	 *   TARGET_REQ_DEBUGMSG
	 *   TARGET_REQ_DEBUGCHAR
	 * if size == 0, target will call target_asciimsg(), else call target_hexmsg()
	 */
	LOG_WARNING("Not implemented: %s", __func__);

	return ERROR_OK;
}

/**
 * Restore processor state.
 */
static int nds32_v3m_leave_debug_state(struct nds32 *nds32, bool enable_watchpoint)
{
	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(nds32->target);

	/* activate all hardware breakpoints */
	CHECK_RETVAL(nds32_v3m_activate_hardware_breakpoint(nds32->target));

	if (enable_watchpoint) {
		/* activate all watchpoints */
		CHECK_RETVAL(nds32_v3m_activate_hardware_watchpoint(nds32->target));
	}

	/* restore interrupt stack */
	nds32_v3m_restore_interrupt_stack(nds32_v3m);

	/* REVISIT once we start caring about MMU and cache state,
	 * address it here ...
	 */

	/* restore PSW, PC, and R0 ... after flushing any modified
	 * registers.
	 */
	CHECK_RETVAL(nds32_restore_context(nds32->target));

	register_cache_invalidate(nds32->core_cache);

	/* enable polling */
	jtag_poll_set_enabled(true);

	return ERROR_OK;
}

static int nds32_v3m_soft_reset_halt(struct target *target)
{
	struct aice_port_s *aice = target_to_aice(target);
	return aice->port->api->assert_srst(AICE_RESET_HOLD);
}

static int nds32_v3m_deassert_reset(struct target *target)
{
	int retval;

	CHECK_RETVAL(nds32_poll(target));

	if (target->state != TARGET_HALTED) {
		/* reset only */
		LOG_WARNING("%s: ran after reset and before halt ...",
				target_name(target));
		retval = target_halt(target);
		if (retval != ERROR_OK)
			return retval;
		/* call target_poll() to avoid "Halt timed out" */
		CHECK_RETVAL(target_poll(target));
	} else {
		jtag_poll_set_enabled(false);
	}

	return ERROR_OK;
}

static int nds32_v3m_checksum_memory(struct target *target,
		uint32_t address, uint32_t count, uint32_t *checksum)
{
	LOG_WARNING("Not implemented: %s", __func__);

	return ERROR_FAIL;
}

static int nds32_v3m_add_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(target);
	struct nds32 *nds32 = &(nds32_v3m->nds32);
	int result;

	if (breakpoint->type == BKPT_HARD) {
		/* check hardware resource */
		if (nds32_v3m->next_hbr_index < nds32_v3m->next_hwp_index) {
			LOG_WARNING("<-- TARGET WARNING! Insert too many "
					"hardware breakpoints/watchpoints! "
					"The limit of combined hardware "
					"breakpoints/watchpoints is %d. -->", nds32_v3m->n_hbr);
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}

		/* update next place to put hardware breakpoint */
		nds32_v3m->next_hbr_index--;

		/* hardware breakpoint insertion occurs before 'continue' actually */
		return ERROR_OK;
	} else if (breakpoint->type == BKPT_SOFT) {
		result = nds32_add_software_breakpoint(target, breakpoint);
		if (ERROR_OK != result) {
			/* auto convert to hardware breakpoint if failed */
			if (nds32->auto_convert_hw_bp) {
				/* convert to hardware breakpoint */
				breakpoint->type = BKPT_HARD;

				return nds32_v3m_add_breakpoint(target, breakpoint);
			}
		}

		return result;
	} else /* unrecognized breakpoint type */
		return ERROR_FAIL;

	return ERROR_OK;
}

static int nds32_v3m_remove_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(target);

	if (breakpoint->type == BKPT_HARD) {
		if (nds32_v3m->next_hbr_index >= nds32_v3m->n_hbr - 1)
			return ERROR_FAIL;

		/* update next place to put hardware breakpoint */
		nds32_v3m->next_hbr_index++;

		/* hardware breakpoint removal occurs after 'halted' actually */
		return ERROR_OK;
	} else if (breakpoint->type == BKPT_SOFT) {
		return nds32_remove_software_breakpoint(target, breakpoint);
	} else /* unrecognized breakpoint type */
		return ERROR_FAIL;

	return ERROR_OK;
}

static int nds32_v3m_add_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(target);

	/* check hardware resource */
	if (nds32_v3m->next_hwp_index >= nds32_v3m->n_hwp) {
		LOG_WARNING("<-- TARGET WARNING! Insert too many hardware "
				"watchpoints! The limit of hardware watchpoints "
				"is %d. -->", nds32_v3m->n_hwp);
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (nds32_v3m->next_hwp_index > nds32_v3m->next_hbr_index) {
		LOG_WARNING("<-- TARGET WARNING! Insert too many hardware "
				"breakpoints/watchpoints! The limit of combined "
				"hardware breakpoints/watchpoints is %d. -->", nds32_v3m->n_hbr);
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	/* update next place to put hardware watchpoint */
	nds32_v3m->next_hwp_index++;

	return ERROR_OK;
}

static int nds32_v3m_remove_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(target);

	if (nds32_v3m->next_hwp_index <= 0)
		return ERROR_FAIL;

	/* update next place to put hardware breakpoint */
	nds32_v3m->next_hwp_index--;

	return ERROR_OK;
}

static int nds32_v3m_get_exception_address(struct nds32 *nds32, uint32_t *address, uint32_t reason)
{
	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(nds32->target);
	struct aice_port_s *aice = target_to_aice(nds32->target);
	uint32_t edmsw;
	uint32_t match_bits;
	uint32_t match_count;
	int32_t i;

	aice->port->api->read_debug_reg(NDS_EDM_SR_EDMSW, &edmsw);

	/* clear matching bits (write-one-clear) */
	aice->port->api->write_debug_reg(NDS_EDM_SR_EDMSW, edmsw);
	match_bits = (edmsw >> 4) & 0xFF;
	match_count = 0;
	for (i = 0 ; i < nds32_v3m->n_hwp ; i++) {
		if (match_bits & (1 << i)) {
			aice->port->api->read_debug_reg(NDS_EDM_SR_BPA0 + i, address);
			match_count++;
		}
	}

	if (match_count != 1) /* multiple hits or no hit */
		*address = 0;

	return ERROR_OK;
}

/**
 * find out which watchpoint hits
 * get exception address and compare the address to watchpoints
 */
static int nds32_v3m_hit_watchpoint(struct target *target,
		struct watchpoint **hit_watchpoint)
{
	uint32_t exception_address;
	struct watchpoint *wp;
	static struct watchpoint scan_all_watchpoint;
	struct nds32 *nds32 = target_to_nds32(target);

	scan_all_watchpoint.address = 0;
	scan_all_watchpoint.rw = WPT_WRITE;
	scan_all_watchpoint.next = 0;
	scan_all_watchpoint.unique_id = 0x5CA8;

	exception_address = nds32->watched_address;

	if (exception_address == 0) {
		*hit_watchpoint = &scan_all_watchpoint;
		return ERROR_OK;
	}

	for (wp = target->watchpoints; wp; wp = wp->next) {
		if (((exception_address ^ wp->address) & (~wp->mask)) == 0) {
			*hit_watchpoint = wp;
			return ERROR_OK;
		}
	}

	return ERROR_FAIL;
}

static int nds32_v3m_run_algorithm(struct target *target,
		int num_mem_params,
		struct mem_param *mem_params,
		int num_reg_params,
		struct reg_param *reg_params,
		uint32_t entry_point,
		uint32_t exit_point,
		int timeout_ms,
		void *arch_info)
{
	LOG_WARNING("Not implemented: %s", __func__);

	return ERROR_FAIL;
}

static int nds32_v3m_target_create(struct target *target, Jim_Interp *interp)
{
	struct nds32_v3m_common *nds32_v3m;

	nds32_v3m = calloc(1, sizeof(*nds32_v3m));
	if (!nds32_v3m)
		return ERROR_FAIL;

	nds32_v3m->nds32.register_map = nds32_v3m_register_mapping;
	nds32_v3m->nds32.get_debug_reason = nds32_v3m_get_debug_reason;
	nds32_v3m->nds32.enter_debug_state = nds32_v3m_debug_entry;
	nds32_v3m->nds32.leave_debug_state = nds32_v3m_leave_debug_state;
	nds32_v3m->nds32.get_watched_address = nds32_v3m_get_exception_address;

	nds32_init_arch_info(target, &(nds32_v3m->nds32));

	return ERROR_OK;
}

static int nds32_v3m_init_target(struct command_context *cmd_ctx,
		struct target *target)
{
	/* Initialize anything we can set up without talking to the target */

	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(target);
	struct nds32 *nds32 = &(nds32_v3m->nds32);

	nds32_init(nds32);

	nds32->virtual_hosting = false;

	return ERROR_OK;
}

/* talk to the target and set things up */
static int nds32_v3m_examine(struct target *target)
{
	struct nds32_v3m_common *nds32_v3m = target_to_nds32_v3m(target);
	struct nds32 *nds32 = &(nds32_v3m->nds32);
	struct aice_port_s *aice = target_to_aice(target);

	if (!target_was_examined(target)) {
		CHECK_RETVAL(nds32_edm_config(nds32));

		if (nds32->reset_halt_as_examine)
			CHECK_RETVAL(nds32_reset_halt(nds32));
	}

	uint32_t edm_cfg;
	aice->port->api->read_debug_reg(NDS_EDM_SR_EDM_CFG, &edm_cfg);

	/* get the number of hardware breakpoints */
	nds32_v3m->n_hbr = (edm_cfg & 0x7) + 1;

	/* get the number of hardware watchpoints */
	/* If the WP field is hardwired to zero, it means this is a
	 * simple breakpoint.  Otherwise, if the WP field is writable
	 * then it means this is a regular watchpoints. */
	nds32_v3m->n_hwp = 0;
	for (int32_t i = 0 ; i < nds32_v3m->n_hbr ; i++) {
		/** check the hardware breakpoint is simple or not */
		uint32_t tmp_value;
		aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + i, 0x1);
		aice->port->api->read_debug_reg(NDS_EDM_SR_BPC0 + i, &tmp_value);

		if (tmp_value)
			nds32_v3m->n_hwp++;
	}
	/* hardware breakpoint is inserted from high index to low index */
	nds32_v3m->next_hbr_index = nds32_v3m->n_hbr - 1;
	/* hardware watchpoint is inserted from low index to high index */
	nds32_v3m->next_hwp_index = 0;

	LOG_INFO("%s: total hardware breakpoint %d (simple breakpoint %d)", target_name(target),
			nds32_v3m->n_hbr, nds32_v3m->n_hbr - nds32_v3m->n_hwp);
	LOG_INFO("%s: total hardware watchpoint %d", target_name(target), nds32_v3m->n_hwp);

	nds32->target->state = TARGET_RUNNING;
	nds32->target->debug_reason = DBG_REASON_NOTHALTED;

	target_set_examined(target);

	/* TODO: remove this message */
	LOG_INFO("ICEman is ready to run.");

	return ERROR_OK;
}

/** Holds methods for NDS32 V3m targets. */
struct target_type nds32_v3m_target = {
	.name = "nds32_v3m",

	.poll = nds32_poll,
	.arch_state = nds32_arch_state,

	.target_request_data = nds32_v3m_target_request_data,

	.halt = nds32_halt,
	.resume = nds32_resume,
	.step = nds32_step,

	.assert_reset = nds32_assert_reset,
	.deassert_reset = nds32_v3m_deassert_reset,
	.soft_reset_halt = nds32_v3m_soft_reset_halt,

	/* register access */
	.get_gdb_general_reg_list = nds32_get_gdb_general_reg_list,
	.get_gdb_reg_list = nds32_get_gdb_reg_list,
	.get_gdb_target_description = nds32_get_gdb_target_description,

	/* memory access */
	.read_buffer = nds32_read_buffer,
	.write_buffer = nds32_write_buffer,
	.read_memory = nds32_read_memory,
	.write_memory = nds32_write_memory,

	.checksum_memory = nds32_v3m_checksum_memory,

	/* breakpoint/watchpoint */
	.add_breakpoint = nds32_v3m_add_breakpoint,
	.remove_breakpoint = nds32_v3m_remove_breakpoint,
	.add_watchpoint = nds32_v3m_add_watchpoint,
	.remove_watchpoint = nds32_v3m_remove_watchpoint,
	.hit_watchpoint = nds32_v3m_hit_watchpoint,

	.run_algorithm = nds32_v3m_run_algorithm,

	.commands = nds32_command_handlers,
	.target_create = nds32_v3m_target_create,
	.init_target = nds32_v3m_init_target,
	.examine = nds32_v3m_examine,
};
