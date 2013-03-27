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
#include "nds32_disassembler.h"
#include "target_type.h"

struct breakpoint syscall_breakpoint = {
	0x80,
	0,
	4,
	BKPT_SOFT,
	0,
	NULL,
	NULL,
	0x515CA11,
	0,
};

static int nds32_v3_add_breakpoint(struct target *target,
		struct breakpoint *breakpoint);
static int nds32_v3_remove_breakpoint(struct target *target,
		struct breakpoint *breakpoint);

static int nds32_v3_register_mapping(struct nds32 *nds32, int reg_no)
{
	if (reg_no == PC)
		return IR11;

	return reg_no;
}

static int nds32_v3_get_debug_reason(struct nds32 *nds32, uint32_t *reason)
{
	uint32_t edmsw;
	struct aice_port_s *aice = target_to_aice(nds32->target);
	aice->port->api->read_debug_reg(NDS_EDM_SR_EDMSW, &edmsw);

	*reason = (edmsw >> 12) & 0x0F;

	return ERROR_OK;
}

static int nds32_v3_activate_hardware_breakpoint(struct target *target)
{
	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(target);
	struct aice_port_s *aice = target_to_aice(target);
	struct breakpoint *bp;
	int32_t hbr_index = 0;

	for (bp = target->breakpoints; bp; bp = bp->next) {
		if (bp->type == BKPT_SOFT) {
			/* already set at nds32_v3_add_breakpoint() */
			continue;
		} else if (bp->type == BKPT_HARD) {
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPA0 + hbr_index, bp->address);  /* set address */
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPAM0 + hbr_index, 0);   /* set mask */
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPV0 + hbr_index, 0);   /* set value */

			if (nds32_v3->nds32.memory.address_translation)
				/* enable breakpoint (virtual address) */
				aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + hbr_index, 0x2);
			else
				/* enable breakpoint (physical address) */
				aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + hbr_index, 0xA);

			LOG_DEBUG("Add hardware BP %d at %08" PRIx32, hbr_index,
					bp->address);

			hbr_index++;
		} else {
			return ERROR_FAIL;
		}
	}

	return ERROR_OK;
}

static int nds32_v3_deactivate_hardware_breakpoint(struct target *target)
{
	struct aice_port_s *aice = target_to_aice(target);
	struct breakpoint *bp;
	int32_t hbr_index = 0;

	for (bp = target->breakpoints; bp; bp = bp->next) {
		if (bp->type == BKPT_SOFT)
			continue;
		else if (bp->type == BKPT_HARD)
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + hbr_index, 0x0); /* disable breakpoint */
		else
			return ERROR_FAIL;

		LOG_DEBUG("Remove hardware BP %d at %08" PRIx32, hbr_index,
				bp->address);

		hbr_index++;
	}

	return ERROR_OK;
}

static int nds32_v3_activate_hardware_watchpoint(struct target *target)
{
	struct aice_port_s *aice = target_to_aice(target);
	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(target);
	struct watchpoint *wp;
	int32_t wp_num = nds32_v3->next_hbr_index;
	uint32_t wp_config = 0;
	int32_t i;
	bool ld_stop, st_stop;

	if (nds32_v3->nds32.global_stop)
		ld_stop = st_stop = false;

	for (wp = target->watchpoints, i = 0; wp; wp = wp->next, i++) {

		if (i < nds32_v3->used_n_wp) {
			wp_num--;
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
			if (nds32_v3->nds32.memory.address_translation == false)
				wp_config |= 0x8;

			aice->port->api->write_debug_reg(NDS_EDM_SR_BPA0 + wp_num,
					wp->address - (wp->address % wp->length));  /* set address */
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPAM0 + wp_num, wp->mask);    /* set mask */
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + wp_num, wp_config);    /* enable watchpoint */
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPV0 + wp_num, 0);   /* set value */

			LOG_DEBUG("Add hardware wathcpoint %d at %08" PRIx32 " mask %08" PRIx32, wp_num,
					wp->address, wp->mask);
		} else if (nds32_v3->nds32.global_stop) {
			if (wp->rw == WPT_READ)
				ld_stop = true;
			else if (wp->rw == WPT_WRITE)
				st_stop = true;
			else if (wp->rw == WPT_ACCESS)
				ld_stop = st_stop = true;
		}
	}

	if (nds32_v3->nds32.global_stop) {
		uint32_t edm_ctl;
		aice->port->api->read_debug_reg(NDS_EDM_SR_EDM_CTL, &edm_ctl);
		if (ld_stop)
			edm_ctl |= 0x10;
		if (st_stop)
			edm_ctl |= 0x20;
		aice->port->api->write_debug_reg(NDS_EDM_SR_EDM_CTL, edm_ctl);
	}

	return ERROR_OK;
}

static int nds32_v3_deactivate_hardware_watchpoint(struct target *target)
{
	struct aice_port_s *aice = target_to_aice(target);
	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(target);
	int32_t wp_num = nds32_v3->next_hbr_index;
	struct watchpoint *wp;
	int32_t i;
	bool clean_global_stop = false;

	for (wp = target->watchpoints, i = 0; wp; wp = wp->next, i++) {

		if (i < nds32_v3->used_n_wp) {
			wp_num--;
			aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0 + wp_num, 0x0); /* disable watchpoint */

			LOG_DEBUG("Remove hardware wathcpoint %d at %08" PRIx32 " mask %08" PRIx32, wp_num,
					wp->address, wp->mask);
		} else if (nds32_v3->nds32.global_stop) {
			clean_global_stop = true;
		}
	}

	if (clean_global_stop) {
		uint32_t edm_ctl;
		aice->port->api->read_debug_reg(NDS_EDM_SR_EDM_CTL, &edm_ctl);
		edm_ctl = edm_ctl & (~0x30);
		aice->port->api->write_debug_reg(NDS_EDM_SR_EDM_CTL, edm_ctl);
	}

	return ERROR_OK;
}

static int nds32_v3_check_interrupt_stack(struct nds32_v3_common *nds32_v3)
{
	struct nds32 *nds32 = &(nds32_v3->nds32);
	uint32_t val_ir0;
	uint32_t value;

	/* Save interrupt level */
	nds32_get_mapped_reg(nds32, IR0, &val_ir0);
	nds32->current_interrupt_level = (val_ir0 >> 1) & 0x3;

	if (nds32_reach_max_interrupt_level(nds32)) {
		LOG_ERROR("<-- TARGET ERROR! Reaching the max interrupt stack level %d. -->", nds32->current_interrupt_level);

		return ERROR_FAIL;
	}

	/* backup $ir4 & $ir6 to avoid suppressed exception overwrite */
	nds32_get_mapped_reg(nds32, IR4, &value);
	nds32_get_mapped_reg(nds32, IR6, &value);

	return ERROR_OK;
}

static int nds32_v3_restore_interrupt_stack(struct nds32_v3_common *nds32_v3)
{
	struct nds32 *nds32 = &(nds32_v3->nds32);
	uint32_t value;

	/* get backup value from cache */
	/* then set back to make the register dirty */
	nds32_get_mapped_reg(nds32, IR0, &value);
	nds32_set_mapped_reg(nds32, IR0, value);

	nds32_get_mapped_reg(nds32, IR4, &value);
	nds32_set_mapped_reg(nds32, IR4, value);

	nds32_get_mapped_reg(nds32, IR6, &value);
	nds32_set_mapped_reg(nds32, IR6, value);

	return ERROR_OK;
}

/**
 * Save processor state.  This is called after a HALT instruction
 * succeeds, and on other occasions the processor enters debug mode
 * (breakpoint, watchpoint, etc).
 */
static int nds32_v3_debug_entry(struct nds32 *nds32, bool enable_watchpoint)
{
	LOG_DEBUG("nds32_v3_debug_entry");

	jtag_poll_set_enabled(false);

	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(nds32->target);

	enum target_state backup_state = nds32->target->state;
	nds32->target->state = TARGET_HALTED;

	if (nds32->init_arch_info_after_halted == false) {
		/* init architecture info according to config registers */
		CHECK_RETVAL(nds32_config(nds32));

		nds32->init_arch_info_after_halted = true;
	}

	/* REVISIT entire cache should already be invalid !!! */
	register_cache_invalidate(nds32->core_cache);

	/* deactivate all hardware breakpoints */
	CHECK_RETVAL(nds32_v3_deactivate_hardware_breakpoint(nds32->target));

	if (enable_watchpoint)
		CHECK_RETVAL(nds32_v3_deactivate_hardware_watchpoint(nds32->target));

	if (nds32->virtual_hosting) {
		if (syscall_breakpoint.set) {
			/** disable virtual hosting */

			/* insert breakpoint at syscall entry */
			nds32_v3_remove_breakpoint(nds32->target, &syscall_breakpoint);
			syscall_breakpoint.set = 0;

			uint32_t value_pc;
			nds32_get_mapped_reg(nds32, PC, &value_pc);
			if (value_pc == syscall_breakpoint.address)
				/** process syscall for virtual hosting */
				nds32->hit_syscall = true;
		}
	}

	if (ERROR_OK != nds32_examine_debug_reason(nds32)) {
		nds32->target->state = backup_state;

		/* re-activate all hardware breakpoints & watchpoints */
		CHECK_RETVAL(nds32_v3_activate_hardware_breakpoint(nds32->target));

		if (enable_watchpoint)
			CHECK_RETVAL(nds32_v3_activate_hardware_watchpoint(nds32->target));

		jtag_poll_set_enabled(true);

		return ERROR_FAIL;
	}

	/* Save registers. */
	nds32_full_context(nds32);

	/* check interrupt level */
	nds32_v3_check_interrupt_stack(nds32_v3);

	/* record fpu/audio status */
	nds32_check_extension(nds32);

	return ERROR_OK;
}

/* target request support */
static int nds32_v3_target_request_data(struct target *target,
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
static int nds32_v3_leave_debug_state(struct nds32 *nds32, bool enable_watchpoint)
{
	LOG_DEBUG("nds32_v3_leave_debug_state");

	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(nds32->target);
	struct target *target = nds32->target;

	/* activate all hardware breakpoints */
	CHECK_RETVAL(nds32_v3_activate_hardware_breakpoint(target));

	if (enable_watchpoint) {
		/* activate all watchpoints */
		CHECK_RETVAL(nds32_v3_activate_hardware_watchpoint(target));
	}

	/* restore interrupt stack */
	nds32_v3_restore_interrupt_stack(nds32_v3);

	/* REVISIT once we start caring about MMU and cache state,
	 * address it here ...
	 */

	/* restore PSW, PC, and R0 ... after flushing any modified
	 * registers.
	 */
	CHECK_RETVAL(nds32_restore_context(target));

	if (nds32->virtual_hosting) {
		/** enable virtual hosting */
		uint32_t value_ir3;
		uint32_t entry_size;
		uint32_t syscall_address;

		/* get syscall entry address */
		nds32_get_mapped_reg(nds32, IR3, &value_ir3);
		entry_size = 0x4 << (((value_ir3 >> 14) & 0x3) << 1);
		syscall_address = (value_ir3 & 0xFFFF0000) + entry_size * 8; /* The index of SYSCALL is 8 */

		if (nds32->hit_syscall) {
			/* single step to skip syscall entry */
			/* use IRET to skip syscall */
			struct aice_port_s *aice = target_to_aice(target);
			uint32_t value_ir9;
			uint32_t value_ir6;
			uint32_t syscall_id;

			nds32_get_mapped_reg(nds32, IR6, &value_ir6);
			syscall_id = (value_ir6 >> 16) & 0x7FFF;

			if (syscall_id == NDS32_SYSCALL_EXIT) {
				/* If target hits exit syscall, do not use IRET to skip handler. */
				aice->port->api->step();
			} else {
				/* use api->read/write_reg to skip nds32 register cache */
				uint32_t value_dimbr;
				aice->port->api->read_debug_reg(NDS_EDM_SR_DIMBR, &value_dimbr);
				aice->port->api->write_reg(IR11, value_dimbr + 0xC);

				aice->port->api->read_reg(IR9, &value_ir9);
				value_ir9 += 4; /* syscall is always 4 bytes */
				aice->port->api->write_reg(IR9, value_ir9);

				/* backup hardware breakpoint 0 */
				uint32_t backup_bpa, backup_bpam, backup_bpc;
				aice->port->api->read_debug_reg(NDS_EDM_SR_BPA0, &backup_bpa);
				aice->port->api->read_debug_reg(NDS_EDM_SR_BPAM0, &backup_bpam);
				aice->port->api->read_debug_reg(NDS_EDM_SR_BPC0, &backup_bpc);

				/* use hardware breakpoint 0 to stop cpu after skipping syscall */
				aice->port->api->write_debug_reg(NDS_EDM_SR_BPA0, value_ir9);
				aice->port->api->write_debug_reg(NDS_EDM_SR_BPAM0, 0);
				aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0, 0xA);

				/* Execute two IRET.
				 * First IRET is used to quit debug mode.
				 * Second IRET is used to quit current syscall. */
				uint32_t dim_inst[4] = {NOP, NOP, IRET, IRET};
				aice->port->api->execute(dim_inst, 4);

				/* restore origin hardware breakpoint 0 */
				aice->port->api->write_debug_reg(NDS_EDM_SR_BPA0, backup_bpa);
				aice->port->api->write_debug_reg(NDS_EDM_SR_BPAM0, backup_bpam);
				aice->port->api->write_debug_reg(NDS_EDM_SR_BPC0, backup_bpc);
			}

			nds32->hit_syscall = false;
		}

		/* insert breakpoint at syscall entry */
		syscall_breakpoint.address = syscall_address;
		syscall_breakpoint.type = BKPT_SOFT;
		syscall_breakpoint.set = 1;
		nds32_v3_add_breakpoint(target, &syscall_breakpoint);
	}

	/* enable polling */
	jtag_poll_set_enabled(true);

	return ERROR_OK;
}

static int nds32_v3_deassert_reset(struct target *target)
{
	int retval;
	struct aice_port_s *aice = target_to_aice(target);
	bool switch_to_v3_stack = false;
	uint32_t value_edm_ctl;

	aice->port->api->read_debug_reg(NDS_EDM_SR_EDM_CTL, &value_edm_ctl);
	if (((value_edm_ctl >> 6) & 0x1) == 0) { /* reset to V2 EDM mode */
		aice->port->api->write_debug_reg(NDS_EDM_SR_EDM_CTL, value_edm_ctl | (0x1 << 6));
		aice->port->api->read_debug_reg(NDS_EDM_SR_EDM_CTL, &value_edm_ctl);
		if (((value_edm_ctl >> 6) & 0x1) == 1)
			switch_to_v3_stack = true;
	} else
		switch_to_v3_stack = false;

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
		/* reset-halt */
		jtag_poll_set_enabled(false);

		struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(target);
		struct nds32 *nds32 = &(nds32_v3->nds32);
		uint32_t value;
		uint32_t interrupt_level;

		if (switch_to_v3_stack == true) {
			/* PSW.INTL-- */
			nds32_get_mapped_reg(nds32, IR0, &value);
			interrupt_level = (value >> 1) & 0x3;
			interrupt_level--;
			value &= ~(0x6);
			value |= (interrupt_level << 1);
			value |= 0x400;  /* set PSW.DEX */
			nds32_set_mapped_reg(nds32, IR0, value);

			/* copy IPC to OIPC */
			if ((interrupt_level + 1) < nds32->max_interrupt_level) {
				nds32_get_mapped_reg(nds32, IR9, &value);
				nds32_set_mapped_reg(nds32, IR11, value);
			}
		}
	}

	return ERROR_OK;
}

static int nds32_v3_soft_reset_halt(struct target *target)
{
	struct aice_port_s *aice = target_to_aice(target);
	return aice->port->api->assert_srst(AICE_RESET_HOLD);
}

static int nds32_v3_checksum_memory(struct target *target,
		uint32_t address, uint32_t count, uint32_t *checksum)
{
	LOG_WARNING("Not implemented: %s", __func__);

	return ERROR_FAIL;
}

static int nds32_v3_add_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(target);
	struct nds32 *nds32 = &(nds32_v3->nds32);
	int result;

	if (breakpoint->type == BKPT_HARD) {
		/* check hardware resource */
		if (nds32_v3->n_hbr <= nds32_v3->next_hbr_index) {
			LOG_WARNING("<-- TARGET WARNING! Insert too many "
					"hardware breakpoints/watchpoints! "
					"The limit of combined hardware "
					"breakpoints/watchpoints is %d. -->", nds32_v3->n_hbr);
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}

		/* update next place to put hardware breakpoint */
		nds32_v3->next_hbr_index++;

		/* hardware breakpoint insertion occurs before 'continue' actually */
		return ERROR_OK;
	} else if (breakpoint->type == BKPT_SOFT) {
		result = nds32_add_software_breakpoint(target, breakpoint);
		if (ERROR_OK != result) {
			/* auto convert to hardware breakpoint if failed */
			if (nds32->auto_convert_hw_bp) {
				/* convert to hardware breakpoint */
				breakpoint->type = BKPT_HARD;

				return nds32_v3_add_breakpoint(target, breakpoint);
			}
		}

		return result;
	} else /* unrecognized breakpoint type */
		return ERROR_FAIL;

	return ERROR_OK;
}

static int nds32_v3_remove_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(target);

	if (breakpoint->type == BKPT_HARD) {
		if (nds32_v3->next_hbr_index <= 0)
			return ERROR_FAIL;

		/* update next place to put hardware breakpoint */
		nds32_v3->next_hbr_index--;

		/* hardware breakpoint removal occurs after 'halted' actually */
		return ERROR_OK;
	} else if (breakpoint->type == BKPT_SOFT) {
		return nds32_remove_software_breakpoint(target, breakpoint);
	} else /* unrecognized breakpoint type */
		return ERROR_FAIL;

	return ERROR_OK;
}

static int nds32_v3_add_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(target);

	/* check hardware resource */
	if (nds32_v3->n_hbr <= nds32_v3->next_hbr_index) {
		/* No hardware resource */
		if (nds32_v3->nds32.global_stop) {
			LOG_WARNING("<-- TARGET WARNING! The number of "
					"watchpoints exceeds the hardware "
					"resources. Stop at every load/store "
					"instruction to check for watchpoint matches. -->");
			return ERROR_OK;
		}

		LOG_WARNING("<-- TARGET WARNING! Insert too many hardware "
				"breakpoints/watchpoints! The limit of combined "
				"hardware breakpoints/watchpoints is %d. -->", nds32_v3->n_hbr);

		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	/* update next place to put hardware watchpoint */
	nds32_v3->next_hbr_index++;
	nds32_v3->used_n_wp++;

	return ERROR_OK;
}

static int nds32_v3_remove_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(target);

	if (nds32_v3->next_hbr_index <= 0) {
		if (nds32_v3->nds32.global_stop)
			return ERROR_OK;

		return ERROR_FAIL;
	}

	/* update next place to put hardware breakpoint */
	nds32_v3->next_hbr_index--;
	nds32_v3->used_n_wp--;

	return ERROR_OK;
}

static int nds32_v3_get_exception_address(struct nds32 *nds32, uint32_t *address, uint32_t reason)
{
	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(nds32->target);
	struct aice_port_s *aice = target_to_aice(nds32->target);
	struct target *target = nds32->target;
	uint32_t edmsw;
	uint32_t match_bits;
	uint32_t match_count;
	int32_t i;

	aice->port->api->read_debug_reg(NDS_EDM_SR_EDMSW, &edmsw);
	/* clear matching bits (write-one-clear) */
	aice->port->api->write_debug_reg(NDS_EDM_SR_EDMSW, edmsw);
	match_bits = (edmsw >> 4) & 0xFF;
	match_count = 0;
	for (i = 0 ; i < nds32_v3->n_hbr ; i++) {
		if (match_bits & (1 << i)) {
			aice->port->api->read_debug_reg(NDS_EDM_SR_BPA0 + i, address);
			match_count++;
		}
	}

	if (match_count > 1) { /* multiple hits */
		*address = 0;
		return ERROR_OK;
	} else if (match_count == 1) {
		/* dispel false match */
		uint32_t val_pc;
		uint32_t opcode;
		struct nds32_instruction instruction;
		struct watchpoint *wp;
		bool hit;

		nds32_get_mapped_reg(nds32, PC, &val_pc);

		if ((NDS32_DEBUG_DATA_ADDR_WATCHPOINT_NEXT_PRECISE == reason) ||
				(NDS32_DEBUG_DATA_VALUE_WATCHPOINT_NEXT_PRECISE == reason)) {
			if (edmsw & 0x4) /* check EDMSW.IS_16BIT */
				val_pc -= 2;
			else
				val_pc -= 4;
		}

		nds32_read_opcode(nds32, val_pc, &opcode);
		nds32_evaluate_opcode(nds32, opcode, val_pc, &instruction);

		LOG_DEBUG("PC: 0x%08x, access start: 0x%08x, end: 0x%08x", val_pc,
				instruction.access_start, instruction.access_end);

		hit = false;
		for (wp = target->watchpoints; wp; wp = wp->next) {
			if (((*address ^ wp->address) & (~wp->mask)) == 0) {
				uint32_t watch_start;
				uint32_t watch_end;

				watch_start = wp->address;
				watch_end = wp->address + wp->length;

				if ((watch_end <= instruction.access_start) ||
						(instruction.access_end <= watch_start))
					continue;

				hit = true;
				break;
			}
		}

		if (hit)
			return ERROR_OK;
		else
			return ERROR_FAIL;
	} else if (match_count == 0) {
		/* global stop is precise exception */
		if ((NDS32_DEBUG_LOAD_STORE_GLOBAL_STOP == reason) && nds32_v3->nds32.global_stop) {
			/* parse instruction to get correct access address */
			uint32_t val_pc;
			uint32_t opcode;
			struct nds32_instruction instruction;

			nds32_get_mapped_reg(nds32, PC, &val_pc);
			nds32_read_opcode(nds32, val_pc, &opcode);
			nds32_evaluate_opcode(nds32, opcode, val_pc, &instruction);

			*address = instruction.access_start;

			return ERROR_OK;
		}
	}

	*address = 0xFFFFFFFF;
	return ERROR_FAIL;
}

/**
 * find out which watchpoint hits
 * get exception address and compare the address to watchpoints
 */
static int nds32_v3_hit_watchpoint(struct target *target,
		struct watchpoint **hit_watchpoint)
{
	static struct watchpoint scan_all_watchpoint;

	uint32_t exception_address;
	struct watchpoint *wp;
	struct nds32 *nds32 = target_to_nds32(target);

	exception_address = nds32->watched_address;

	if (exception_address == 0xFFFFFFFF)
		return ERROR_FAIL;

	if (exception_address == 0) {
		scan_all_watchpoint.address = 0;
		scan_all_watchpoint.rw = WPT_WRITE;
		scan_all_watchpoint.next = 0;
		scan_all_watchpoint.unique_id = 0x5CA8;

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

static int nds32_v3_run_algorithm(struct target *target,
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

static int nds32_v3_target_create(struct target *target, Jim_Interp *interp)
{
	struct nds32_v3_common *nds32_v3;

	nds32_v3 = calloc(1, sizeof(*nds32_v3));
	if (!nds32_v3)
		return ERROR_FAIL;

	nds32_v3->nds32.register_map = nds32_v3_register_mapping;
	nds32_v3->nds32.get_debug_reason = nds32_v3_get_debug_reason;
	nds32_v3->nds32.enter_debug_state = nds32_v3_debug_entry;
	nds32_v3->nds32.leave_debug_state = nds32_v3_leave_debug_state;
	nds32_v3->nds32.get_watched_address = nds32_v3_get_exception_address;

	nds32_init_arch_info(target, &(nds32_v3->nds32));

	return ERROR_OK;
}

static int nds32_v3_init_target(struct command_context *cmd_ctx,
		struct target *target)
{
	/* Initialize anything we can set up without talking to the target */

	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(target);
	struct nds32 *nds32 = &(nds32_v3->nds32);

	nds32_init(nds32);

	target->fileio_info = malloc(sizeof(struct gdb_fileio_info));
	target->fileio_info->identifier = NULL;

	return ERROR_OK;
}

/* talk to the target and set things up */
static int nds32_v3_examine(struct target *target)
{
	struct nds32_v3_common *nds32_v3 = target_to_nds32_v3(target);
	struct nds32 *nds32 = &(nds32_v3->nds32);
	struct aice_port_s *aice = target_to_aice(target);

	if (!target_was_examined(target)) {
		CHECK_RETVAL(nds32_edm_config(nds32));

		if (nds32->reset_halt_as_examine)
			CHECK_RETVAL(nds32_reset_halt(nds32));
	}

	uint32_t edm_cfg;
	aice->port->api->read_debug_reg(NDS_EDM_SR_EDM_CFG, &edm_cfg);

	/* get the number of hardware breakpoints */
	nds32_v3->n_hbr = (edm_cfg & 0x7) + 1;

	/* low interference profiling */
	if (edm_cfg & 0x100)
		nds32_v3->low_interference_profile = true;
	else
		nds32_v3->low_interference_profile = false;

	nds32_v3->next_hbr_index = 0;
	nds32_v3->used_n_wp = 0;

	LOG_INFO("%s: total hardware breakpoint %d", target_name(target),
			nds32_v3->n_hbr);

	nds32->target->state = TARGET_RUNNING;
	nds32->target->debug_reason = DBG_REASON_NOTHALTED;

	target_set_examined(target);

	/* TODO: remove this message */
	LOG_INFO("ICEman is ready to run.");

	return ERROR_OK;
}

int nds32_v3_read_buffer(struct target *target, uint32_t address,
		uint32_t size, uint8_t *buffer)
{
	struct nds32 *nds32 = target_to_nds32(target);
	struct nds32_memory *memory = &(nds32->memory);

	if ((NDS_MEMORY_ACC_CPU == memory->access_channel) &&
			(target->state != TARGET_HALTED)) {
		LOG_WARNING("target was not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	uint32_t physical_address;
	/* BUG: If access range crosses multiple pages, the translation will not correct
	 * for second page or so. */

	/* When DEX is set to one, hardware will enforce the following behavior without
	 * modifying the corresponding control bits in PSW.
	 *
	 * Disable all interrupts
	 * Become superuser mode
	 * Turn off IT/DT
	 * Use MMU_CFG.DE as the data access endian
	 * Use MMU_CFG.DRDE as the device register access endian if MMU_CTL.DREE is asserted
	 * Disable audio special features
	 * Disable inline function call
	 *
	 * Because hardware will turn off IT/DT by default, it MUST translate virtual address
	 * to physical address.
	 */
	if (ERROR_OK == target->type->virt2phys(target, address, &physical_address))
		address = physical_address;
	else
		return ERROR_FAIL;

	return nds32_read_buffer(target, address, size, buffer);
}

int nds32_v3_write_buffer(struct target *target, uint32_t address,
		uint32_t size, const uint8_t *buffer)
{
	struct nds32 *nds32 = target_to_nds32(target);
	struct nds32_memory *memory = &(nds32->memory);

	if ((NDS_MEMORY_ACC_CPU == memory->access_channel) &&
			(target->state != TARGET_HALTED)) {
		LOG_WARNING("target was not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	uint32_t physical_address;
	/* BUG: If access range crosses multiple pages, the translation will not correct
	 * for second page or so. */

	/* When DEX is set to one, hardware will enforce the following behavior without
	 * modifying the corresponding control bits in PSW.
	 *
	 * Disable all interrupts
	 * Become superuser mode
	 * Turn off IT/DT
	 * Use MMU_CFG.DE as the data access endian
	 * Use MMU_CFG.DRDE as the device register access endian if MMU_CTL.DREE is asserted
	 * Disable audio special features
	 * Disable inline function call
	 *
	 * Because hardware will turn off IT/DT by default, it MUST translate virtual address
	 * to physical address.
	 */
	if (ERROR_OK == target->type->virt2phys(target, address, &physical_address))
		address = physical_address;
	else
		return ERROR_FAIL;

	if (nds32->hit_syscall) {
		/* Use bus mode to access memory during virtual hosting */
		struct aice_port_s *aice = target_to_aice(target);
		enum nds_memory_access origin_access_channel;
		int result;

		origin_access_channel = memory->access_channel;
		memory->access_channel = NDS_MEMORY_ACC_BUS;
		aice->port->api->memory_access(NDS_MEMORY_ACC_BUS);

		result = nds32_gdb_fileio_write_memory(nds32, address, size, buffer);

		memory->access_channel = origin_access_channel;
		aice->port->api->memory_access(origin_access_channel);

		return result;
	}

	return nds32_write_buffer(target, address, size, buffer);
}

int nds32_v3_read_memory(struct target *target, uint32_t address,
		uint32_t size, uint32_t count, uint8_t *buffer)
{
	struct nds32 *nds32 = target_to_nds32(target);
	struct nds32_memory *memory = &(nds32->memory);

	if ((NDS_MEMORY_ACC_CPU == memory->access_channel) &&
			(target->state != TARGET_HALTED)) {
		LOG_WARNING("target was not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	uint32_t physical_address;
	/* BUG: If access range crosses multiple pages, the translation will not correct
	 * for second page or so. */

	/* When DEX is set to one, hardware will enforce the following behavior without
	 * modifying the corresponding control bits in PSW.
	 *
	 * Disable all interrupts
	 * Become superuser mode
	 * Turn off IT/DT
	 * Use MMU_CFG.DE as the data access endian
	 * Use MMU_CFG.DRDE as the device register access endian if MMU_CTL.DREE is asserted
	 * Disable audio special features
	 * Disable inline function call
	 *
	 * Because hardware will turn off IT/DT by default, it MUST translate virtual address
	 * to physical address.
	 */
	if (ERROR_OK == target->type->virt2phys(target, address, &physical_address))
		address = physical_address;
	else
		return ERROR_FAIL;

	struct aice_port_s *aice = target_to_aice(target);
	/* give arbitrary initial value to avoid warning messages */
	enum nds_memory_access origin_access_channel = NDS_MEMORY_ACC_CPU;
	int result;

	if (nds32->hit_syscall) {
		/* Use bus mode to access memory during virtual hosting */
		origin_access_channel = memory->access_channel;
		memory->access_channel = NDS_MEMORY_ACC_BUS;
		aice->port->api->memory_access(NDS_MEMORY_ACC_BUS);
	}

	result = nds32_read_memory(target, address, size, count, buffer);

	if (nds32->hit_syscall) {
		/* Restore access_channel after virtual hosting */
		memory->access_channel = origin_access_channel;
		aice->port->api->memory_access(origin_access_channel);
	}

	return result;
}

int nds32_v3_write_memory(struct target *target, uint32_t address,
		uint32_t size, uint32_t count, const uint8_t *buffer)
{
	struct nds32 *nds32 = target_to_nds32(target);
	struct nds32_memory *memory = &(nds32->memory);

	if ((NDS_MEMORY_ACC_CPU == memory->access_channel) &&
			(target->state != TARGET_HALTED)) {
		LOG_WARNING("target was not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	uint32_t physical_address;
	/* BUG: If access range crosses multiple pages, the translation will not correct
	 * for second page or so. */

	/* When DEX is set to one, hardware will enforce the following behavior without
	 * modifying the corresponding control bits in PSW.
	 *
	 * Disable all interrupts
	 * Become superuser mode
	 * Turn off IT/DT
	 * Use MMU_CFG.DE as the data access endian
	 * Use MMU_CFG.DRDE as the device register access endian if MMU_CTL.DREE is asserted
	 * Disable audio special features
	 * Disable inline function call
	 *
	 * Because hardware will turn off IT/DT by default, it MUST translate virtual address
	 * to physical address.
	 */
	if (ERROR_OK == target->type->virt2phys(target, address, &physical_address))
		address = physical_address;
	else
		return ERROR_FAIL;

	return nds32_write_memory(target, address, size, count, buffer);
}

/** Holds methods for Andes1337 targets. */
struct target_type nds32_v3_target = {
	.name = "nds32_v3",

	.poll = nds32_poll,
	.arch_state = nds32_arch_state,

	.target_request_data = nds32_v3_target_request_data,

	.halt = nds32_halt,
	.resume = nds32_resume,
	.step = nds32_step,

	.assert_reset = nds32_assert_reset,
	.deassert_reset = nds32_v3_deassert_reset,
	.soft_reset_halt = nds32_v3_soft_reset_halt,

	/* register access */
	.get_gdb_general_reg_list = nds32_get_gdb_general_reg_list,
	.get_gdb_reg_list = nds32_get_gdb_reg_list,
	.get_gdb_target_description = nds32_get_gdb_target_description,

	/* memory access */
	.read_buffer = nds32_v3_read_buffer,
	.write_buffer = nds32_v3_write_buffer,
	.read_memory = nds32_v3_read_memory,
	.write_memory = nds32_v3_write_memory,

	.checksum_memory = nds32_v3_checksum_memory,

	/* breakpoint/watchpoint */
	.add_breakpoint = nds32_v3_add_breakpoint,
	.remove_breakpoint = nds32_v3_remove_breakpoint,
	.add_watchpoint = nds32_v3_add_watchpoint,
	.remove_watchpoint = nds32_v3_remove_watchpoint,
	.hit_watchpoint = nds32_v3_hit_watchpoint,

	/* MMU */
	.mmu = nds32_mmu,
	.virt2phys = nds32_virtual_to_physical,
	.read_phys_memory = nds32_read_phys_memory,
	.write_phys_memory = nds32_write_phys_memory,

	.run_algorithm = nds32_v3_run_algorithm,

	.commands = nds32_command_handlers,
	.target_create = nds32_v3_target_create,
	.init_target = nds32_v3_init_target,
	.examine = nds32_v3_examine,

	.get_gdb_fileio_info = nds32_get_gdb_fileio_info,
	.gdb_fileio_end = nds32_gdb_fileio_end,
};
