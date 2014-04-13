/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2014 by Andrey Smirnov                                  *
 *   andrew.smirnov@gmail.com                                              *
 *                                                                         *
 *   Some commentary in this files are taken directly from the source      *
 *   code of the 'usbdm-eclipse-makefiles-build' project availible         *
 *   here:                                                                 *
 *   https://github.com/podonoghue/usbdm-eclipse-makefiles-build.git       *
 *   Copyright (C) 2008  Peter O'Donoghue                                  *
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/jtag.h>
#include <jtag/interface.h>
#include <binarybuffer.h>

#include "breakpoints.h"
#include "target.h"
#include "algorithm.h"
#include "register.h"
#include "target_type.h"
#include "bdm.h"
#include "hcs12.h"
#include "log.h"


enum {
	HCS12_FAMILY_ID = 0xC1,
};

/*
  CCR is different from other registers because there's no dedicated
  BDM command to retreive it's contents and insted it is exposed as
  one(actually two) of the BDM internal registers the contents of
  which is loaded into the CPU's state register upon exiting BDM mode.

  Also for some reason HCS12's BDM implementation, through that pair
  of registers, presents you with 10 bits of status but the only
  description of the CPU's status registers I can find mention only 8
  bits, so at this point I am unclear what is the point of
  BDM_REG_BDMCCRH and what useful things it brings to the table
*/
static int32_t hcs12_read_ccr(struct target *target)
{
	int ret;

	ret = bdm_read_bd_byte(target, BDM_REG_BDMCCRL);
	if (ret < 0) {
		LOG_ERROR("Falied to read BDMCCRL register(%d)", ret);
		return ret;
	}

	const uint16_t bdmccrl = ret & 0xFF;

	ret = bdm_read_bd_byte(target, BDM_REG_BDMCCRH);
	if (ret < 0) {
		LOG_ERROR("Falied to read BDMCCRH register(%d)", ret);
		return ret;
	}

	const uint16_t bdmccrh = ret & 0xFF;

	return (bdmccrh << 8) | bdmccrl;
}

static int hcs12_write_ccr(struct target *target, uint16_t value)
{
	int ret;

	const uint16_t bdmccrl = value & 0xFF;

	ret = bdm_write_bd_byte(target, BDM_REG_BDMCCRL, bdmccrl);
	if (ret < 0) {
		LOG_ERROR("Falied to write BDMCCRH register(%d)", ret);
		return ret;
	}

	const uint16_t bdmccrh = (value >> 8) & 0x03;

	ret = bdm_write_bd_byte(target, BDM_REG_BDMCCRH, bdmccrh);
	if (ret < 0) {
		LOG_ERROR("Falied to write BDMCCRH register(%d)", ret);
		return ret;
	}

	return ERROR_OK;
}

static const struct {
	const char *name;
	enum reg_type type;
	const char *group;
	const char *feature;

	int    (*write) (struct target *, uint16_t);
	int32_t (*read) (struct target *);

} hcs12_registers_table[] = {
	/* For now the feature field is complete fiction since
	 * there's no dedicated xml file in GDB to describe hcs12 */
	[HCS12_PC]  = { "pc",  REG_TYPE_CODE_PTR, "general", "org.gnu.gdb.hcs12", bdm_write_pc, bdm_read_pc },
	[HCS12_SP]  = { "sp",  REG_TYPE_DATA_PTR, "general", "org.gnu.gdb.hcs12", bdm_write_sp, bdm_read_sp },
	[HCS12_D]   = { "d",   REG_TYPE_INT16,    "general", "org.gnu.gdb.hcs12", bdm_write_d,  bdm_read_d  },
	[HCS12_X]   = { "x",   REG_TYPE_INT16,    "general", "org.gnu.gdb.hcs12", bdm_write_x,  bdm_read_x  },
	[HCS12_Y]   = { "y",   REG_TYPE_INT16,    "general", "org.gnu.gdb.hcs12", bdm_write_y,  bdm_read_y  },
	[HCS12_CCR] = { "ccr", REG_TYPE_INT16,    "general", "org.gnu.gdb.hcs12", hcs12_write_ccr,  hcs12_read_ccr  },
};

struct hcs12_register {
	struct target *target;
	int32_t (*read)  (struct target *);
	int     (*write) (struct target *, uint16_t);
};

static int hcs12_restore_context(struct target *target)
{
	int i, ret;
	struct hcs12 *hcs12 = target_to_hcs12(target);
	struct reg_cache *cache = hcs12->core_cache;

	LOG_DEBUG(" ");

	for (i = HCS12_NUM_REGS - 1; i >= 0; i--) {
		if (cache->reg_list[i].dirty) {
			uint32_t value = buf_get_u16(cache->reg_list[i].value, 0, 16);
			ret = hcs12->write_core_reg(hcs12, &cache->reg_list[i], value);
			if (ret != ERROR_OK) {
				LOG_ERROR("Error during cache synchronization");
				return ERROR_FAIL;
			}
		}
	}

	return ERROR_OK;
}

static int hcs12_debug_entry(struct target *target)
{

	int i;
	int ret;
	struct hcs12 *hcs12 = target_to_hcs12(target);
	struct reg *r;
	uint16_t ccr;

	const int num_regs = hcs12->core_cache->num_regs;

	for (i = 0; i < num_regs; i++) {
		r = &hcs12->core_cache->reg_list[i];
		if (!r->valid) {
			ret = hcs12->read_core_reg(hcs12, r);
			if (ret != ERROR_OK)
				return ret;
		}
	}

	r   = hcs12->ccr;
	ccr = buf_get_u16(r->value, 0, 16);

	/*
	  If we reset into special single-chip mode CCR value is set
	  to a special value 0xd8, instead of what it is actuallly
	  supposed to be in that mode 0xd0, so if we detect this we
	  reset the value of the cached CCR to it's normal state.

	  See note in section 7.3.2.2 "BDM CCR LOW Holding Register (BDMCCRL)"
	 */
	if (ccr == 0xd8) {
		LOG_DEBUG("hcs12 is in special single-chip reset mode");
		buf_set_u16(r->value, 0, 16, 0xd0);
	}

	return ERROR_OK;
}

static int hcs12_get_core_reg(struct reg *reg)
{
	struct hcs12_register *hcsreg;
	struct target *target;
	struct hcs12  *hcs12;

	hcsreg = reg->arch_info;
	target = hcsreg->target;
	hcs12  = target_to_hcs12(target);

	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;

	return hcs12->read_core_reg(hcs12, reg);
}

static int hcs12_set_core_reg(struct reg *reg, uint8_t *buf)
{
	struct hcs12_register *hcsreg;
	struct target *target;
	uint32_t value;

	hcsreg = reg->arch_info;
	target = hcsreg->target;

	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;

	value = buf_get_u16(buf, 0, 16);

	buf_set_u16(reg->value, 0, 16, value);
	reg->dirty = 1;
	reg->valid = 1;

	return ERROR_OK;
}

static int hcs12_read_core_reg(struct hcs12 *hcs12, struct reg *reg)
{
	assert(reg->number < hcs12->core_cache->num_regs);

	int32_t ret;
	struct target *target = hcs12->target;

	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;

	struct hcs12_register *hcsreg = reg->arch_info;

	ret = hcsreg->read(target);
	if (ret < 0)
		return ret;

	buf_set_u16(reg->value, 0, 16, ret);
	reg->valid = 1;
	reg->dirty = 0;

	return ERROR_OK;
}

static int hcs12_write_core_reg(struct hcs12 *hcs12, struct reg *reg, uint16_t value)
{
	assert(reg->number < hcs12->core_cache->num_regs);

	int ret;
	struct hcs12_register *hcsreg = reg->arch_info;
	struct target *target = hcs12->target;

	ret = hcsreg->write(target, value);
	if (ret != ERROR_OK) {
		LOG_ERROR("BDM failure");
		reg->dirty = reg->valid;
		return ERROR_JTAG_DEVICE_ERROR;
	}

	reg->valid = 1;
	reg->dirty = 0;

	return ERROR_OK;
}

static const struct reg_arch_type hcs12_register_type = {
	.get = hcs12_get_core_reg,
	.set = hcs12_set_core_reg,
};

static int hcs12_poll(struct target *target)
{
	int ret;
	enum target_state prev_target_state = target->state;

	ret = bdm_read_bd_byte(target, BDM_REG_BDMSTS);
	if (ret < 0) {
		target->state = TARGET_UNKNOWN;
		return ret;
	}

	const uint8_t bdmsts = 0xFF & ret;

	/* FIXME: We need to handle secured part properly */

	if ((bdmsts & (BDM_REG_BDMSTS_ENBDM | BDM_REG_BDMSTS_BDMACT)) ==
	    (BDM_REG_BDMSTS_ENBDM | BDM_REG_BDMSTS_BDMACT)) {
		target->state = TARGET_HALTED;

		if ((prev_target_state == TARGET_RUNNING) || (prev_target_state == TARGET_RESET)) {
			ret = hcs12_debug_entry(target);
			if (ret != ERROR_OK)
				return ret;


			target_call_event_callbacks(target, TARGET_EVENT_HALTED);
		}

		if (prev_target_state == TARGET_DEBUG_RUNNING) {
			ret = hcs12_debug_entry(target);
			if (ret != ERROR_OK)
				return ret;


			target_call_event_callbacks(target, TARGET_EVENT_DEBUG_HALTED);
		}
	} else {
		target->state = TARGET_RUNNING;
	}

	return ERROR_OK;
}

static int hcs12_arch_state(struct target *target)
{
	struct hcs12 *hcs12 = target_to_hcs12(target);

	LOG_USER("target halted due to %s\n"
		 "pc: %#8.8" PRIx32 " sp: %#8.8" PRIx32,
		 debug_reason_name(target),
		 buf_get_u32(hcs12->pc->value, 0, 32),
		 buf_get_u32(hcs12->sp->value, 0, 32));

	return ERROR_OK;
}

static int hcs12_set_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	return ERROR_FAIL;
}

static int hcs12_unset_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	return ERROR_OK;
}

static int hcs12_target_request_data(struct target *target, uint32_t size, uint8_t *buffer)
{
	return ERROR_FAIL;
}

static int hcs12_add_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	return ERROR_FAIL;
}

static int hcs12_remove_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	return ERROR_OK;
}

static void hcs12_enable_breakpoints(struct target *target)
{
	int ret;
	struct breakpoint *breakpoint = target->breakpoints;

	/* set any pending breakpoints */
	while (breakpoint) {
		if (!breakpoint->set) {
			ret = hcs12_set_breakpoint(target, breakpoint);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to set watchpoing");
				break;
			}
		}
		breakpoint = breakpoint->next;
	}
}


static int hcs12_set_watchpoint(struct target *target, struct watchpoint *watchpoint)
{
	return ERROR_FAIL;
}

static int hcs12_unset_watchpoint(struct target *target, struct watchpoint *watchpoint)
{
	return ERROR_OK;
}

static int hcs12_add_watchpoint(struct target *target, struct watchpoint *breakpoint)
{
	return ERROR_FAIL;
}

static int hcs12_remove_watchpoint(struct target *target, struct watchpoint *watchpoint)
{
	if (watchpoint->set)
		hcs12_unset_watchpoint(target, watchpoint);

	return ERROR_OK;
}

static void hcs12_enable_watchpoints(struct target *target)
{
	int ret;
	struct watchpoint *watchpoint = target->watchpoints;

	/* set any pending watchpoints */
	while (watchpoint) {
		if (!watchpoint->set) {
			ret = hcs12_set_watchpoint(target, watchpoint);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to set watchpoing");
				break;
			}
		}
		watchpoint = watchpoint->next;
	}
}

static int32_t hcs12_find_next_non_bgnd_instr_after(struct target *target, uint16_t address)
{
	int ret;
	uint8_t  op;
	uint16_t pc;

	for (pc = address; pc != 0x0000; pc++) {
		ret = target_read_u8(target, BDM_LOCAL_ADDR(pc), &op);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to fetch instruction @ 0x%04" PRIx16, pc);
			return ret;
		}

		if (op != HCS12_OP_BKND)
			return pc;
	}

	/*
	  If resume_pc rolled over then we scaned all memory and
	  didn't find  any non-BGND instructions
	 */
	LOG_ERROR("Non non-BGKND instructions at or after 0x%04" PRIx16, address);
	return ERROR_FAIL;
}


static const char *hcs12_get_operation_mode_name(enum hcs12_operation_mode mode)
{
	assert(mode < HCS12_M_MODE_COUNT);

	static const char * const hcs12_opeation_mode_names[] = {
		[HCS12_M_SPECIAL_SINGLE_CHIP]	= "Special Single-Chip (SS)",
		[HCS12_M_EMULATION_SINGLE_CHIP] = "Emulation Single-Chip (ES)",
		[HCS12_M_SPECIAL_TEST]		= "Special Test (ST)",
		[HCS12_M_EMULATION_EXPANDED]	= "Emulation Expanded (EX)",
		[HCS12_M_NORMAL_SINGLE_CHIP]	= "Normal Single-Chip (NS)",
		[HCS12_M_NORMAL_EXPANDED]	= "Normal Expanded (NX)",
	};

	return hcs12_opeation_mode_names[mode];
}

static int hcs12_switch_to_mode(struct target *target,
				enum hcs12_operation_mode next_mode)
{
	int ret;
	assert(next_mode < HCS12_M_MODE_COUNT);

	static const bool hcs12_mode_transition_table[HCS12_M_MODE_COUNT][HCS12_M_MODE_COUNT] = {
		[HCS12_M_SPECIAL_SINGLE_CHIP] = {
			[HCS12_M_SPECIAL_SINGLE_CHIP]	= true,
			[HCS12_M_EMULATION_SINGLE_CHIP] = true,
			[HCS12_M_SPECIAL_TEST]		= true,
			[HCS12_M_EMULATION_EXPANDED]	= true,
			[HCS12_M_NORMAL_SINGLE_CHIP]	= true,
			[HCS12_M_NORMAL_EXPANDED]	= true,
		},

		[HCS12_M_EMULATION_SINGLE_CHIP] = {
			[HCS12_M_SPECIAL_SINGLE_CHIP]	= false,
			[HCS12_M_EMULATION_SINGLE_CHIP] = false,
			[HCS12_M_SPECIAL_TEST]		= false,
			[HCS12_M_EMULATION_EXPANDED]	= true,
			[HCS12_M_NORMAL_SINGLE_CHIP]	= false,
			[HCS12_M_NORMAL_EXPANDED]	= false,
		},

		[HCS12_M_SPECIAL_TEST] = {
			[HCS12_M_SPECIAL_SINGLE_CHIP]	= true,
			[HCS12_M_EMULATION_SINGLE_CHIP] = true,
			[HCS12_M_SPECIAL_TEST]		= true,
			[HCS12_M_EMULATION_EXPANDED]	= true,
			[HCS12_M_NORMAL_SINGLE_CHIP]	= true,
			[HCS12_M_NORMAL_EXPANDED]	= true,
		},

		[HCS12_M_EMULATION_EXPANDED] = {
			[HCS12_M_SPECIAL_SINGLE_CHIP]	= false,
			[HCS12_M_EMULATION_SINGLE_CHIP] = false,
			[HCS12_M_SPECIAL_TEST]		= false,
			[HCS12_M_EMULATION_EXPANDED]	= false,
			[HCS12_M_NORMAL_SINGLE_CHIP]	= false,
			[HCS12_M_NORMAL_EXPANDED]	= false,
		},

		[HCS12_M_NORMAL_SINGLE_CHIP] = {
			[HCS12_M_SPECIAL_SINGLE_CHIP]	= false,
			[HCS12_M_EMULATION_SINGLE_CHIP] = false,
			[HCS12_M_SPECIAL_TEST]		= false,
			[HCS12_M_EMULATION_EXPANDED]	= false,
			[HCS12_M_NORMAL_SINGLE_CHIP]	= false,
			[HCS12_M_NORMAL_EXPANDED]	= true,
		},

		[HCS12_M_NORMAL_EXPANDED] = {
			[HCS12_M_SPECIAL_SINGLE_CHIP]	= false,
			[HCS12_M_EMULATION_SINGLE_CHIP] = false,
			[HCS12_M_SPECIAL_TEST]		= false,
			[HCS12_M_EMULATION_EXPANDED]	= false,
			[HCS12_M_NORMAL_SINGLE_CHIP]	= false,
			[HCS12_M_NORMAL_EXPANDED]	= false,
		},
	};

	uint8_t mode;
	ret = target_read_u8(target, HCS12_REG_MODE, &mode);
	if (ret < 0) {
		LOG_ERROR("Failed to read MODE register, %d", ret);
		return ret;
	}

	const enum hcs12_operation_mode current_mode = mode >> HCS12_REG_MODE_MOD_SHIFT;

	if (current_mode >= HCS12_M_MODE_COUNT) {
		LOG_ERROR("Unknown Mode of operation");
		return ERROR_FAIL;
	}

	if (current_mode == next_mode)
		return ERROR_OK;


	if (!hcs12_mode_transition_table[current_mode][next_mode]) {
		LOG_ERROR("Transition from %s to %s is not allowed",
			  hcs12_get_operation_mode_name(current_mode),
			  hcs12_get_operation_mode_name(next_mode));
		return ERROR_FAIL;
	}

	LOG_DEBUG("Switching from %s to %s is not allowed",
		  hcs12_get_operation_mode_name(current_mode),
		  hcs12_get_operation_mode_name(next_mode));

	if (current_mode == HCS12_M_EMULATION_SINGLE_CHIP &&
	    next_mode == HCS12_M_EMULATION_EXPANDED) {
		ret = target_write_u8(target, HCS12_REG_MODE,
				      0b101 << HCS12_REG_MODE_MOD_SHIFT);
	} else {
		ret = target_write_u8(target, HCS12_REG_MODE,
				      next_mode << HCS12_REG_MODE_MOD_SHIFT);
	}

	if (ret != ERROR_OK)
		LOG_ERROR("Failed to write MODE register");

	return ret;
}

static int hcs12_step(struct target *target, int current, uint32_t address,
			int handle_breakpoints)
{
	struct hcs12 *hcs12 = target_to_hcs12(target);
	struct breakpoint *breakpoint = NULL;
	struct reg *pc = hcs12->pc;
	uint16_t next_step_pc;

	int ret;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = hcs12_switch_to_mode(target, HCS12_M_NORMAL_SINGLE_CHIP);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to swith chip to %s mode",
			  hcs12_get_operation_mode_name(HCS12_M_NORMAL_SINGLE_CHIP));
		return ret;
	}


	/* current = 1: continue on current pc, otherwise continue at <address> */
	if (!current)
		buf_set_u16(pc->value, 0, 16, address);

	uint16_t next_pc = buf_get_u16(pc->value, 0, 16);

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		breakpoint = breakpoint_find(target, next_pc);
		if (breakpoint)
			hcs12_unset_breakpoint(target, breakpoint);
	}

	/*
	  We can't really use TRACE1 BDM command on BGND instruction
	  because that would throw us into BDM frimware address
	  region, which is something we do not want, so go and look
	  for the first instruction other that BGND to continue execution
	 */
	ret = hcs12_find_next_non_bgnd_instr_after(target, next_pc);
	if (ret < 0) {
		LOG_ERROR("Failed to find any non BGND instructions after 0x08%" PRIx16, next_pc);
		return ERROR_FAIL;
	}
	next_step_pc = ret & 0xFFFF;

	if (next_step_pc != next_pc) {
		buf_set_u16(pc->value, 0, 16, next_step_pc);
		pc->dirty = true;
		pc->valid = true;
	}

	target->debug_reason = DBG_REASON_SINGLESTEP;

	target_call_event_callbacks(target, TARGET_EVENT_RESUMED);


	bool ccr_i_bit = buf_get_u16(hcs12->ccr->value, HCS12_CCR_I_BIT, 1);

	if (!ccr_i_bit) {
		/* Disable interrupts */
		buf_set_u16(hcs12->ccr->value, HCS12_CCR_I_BIT, 1, 1);
		hcs12->ccr->dirty = true;
		hcs12->ccr->valid = true;
	}

	hcs12_restore_context(target);

	ret = bdm_trace1(target);
	if (ret != ERROR_OK) {
		LOG_ERROR("Error issuing TRACE1 command");
		return ERROR_FAIL;
	}

	/* registers are now invalid */
	register_cache_invalidate(hcs12->core_cache);

	if (breakpoint)
		hcs12_set_breakpoint(target, breakpoint);

	ret = hcs12_debug_entry(target);
	if (ret != ERROR_OK)
		return ret;

	/* If we fixed I bit, we need to restore it
	 *
	 * Cases to consider when masking interrupts during step
	 *
	 * +--------+-----------+---------+-----------------------------------------------------+
	 * | Opcode | Initial I | Final I | Problem - action                                    |
	 * +--------+-----------+---------+-----------------------------------------------------+
	 * | ---    |     1     |    X    | None - no action (interrupts already masked)        |
	 * +--------+-----------+---------+-----------------------------------------------------+
	 * | CLI    |     0     |    ?    | It may be possible for an interrupt to occur,       |
	 * | WAIT   |           |         | setting I-flag which is then incorrectly cleared.   |
	 * | STOP   |           |         | (I don't think it applies to CLI but be safe.)      |
	 * | SWI    |           |         | - don't 'fix' CCR                                   |
	 * +--------+-----------+---------+-----------------------------------------------------+
	 * | RTI    |     0     |    1    | Contrived but possible situation. I flag            |
	 * |        |           |         | incorrectly cleared - don't 'fix' CCR               |
	 * +--------+-----------+---------+-----------------------------------------------------+
	 * | SEI    |     0     |    1    | The instruction may set I-flag which is then        |
	 * | TAP    |     0     |    1    | incorrectly cleared - don't 'fix' CCR               |
	 * +--------+-----------+---------+-----------------------------------------------------+
	 * | TPA    |     0     |    X    | The wrong value is transferred to A - fix A         |
	 * +--------+-----------+---------+-----------------------------------------------------+
	 * | ---    |     0     |    0    | CCR change - clear I-flag in new CCR                |
	 * +--------+-----------+---------+-----------------------------------------------------+
	 */
	if (!ccr_i_bit) {
		bool fix_ccr, fix_a;
		uint8_t opcode[3];

		for (size_t i = 0; i < ARRAY_SIZE(opcode); i++) {
			ret = target_read_u8(target, BDM_LOCAL_ADDR(next_step_pc + i), &opcode[i]);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to fetch instruction @ 0x%04" PRIx16, next_step_pc);
				return ret;
			}
		}

		fix_ccr = false;
		fix_a   = false;

		switch (opcode[0]) {
		case HCS12_OP_ANDCC:
			/* CLI instruction is translated to ANDCC 0xEF */
			if (opcode[1] != 0xEF)
				fix_ccr = true;
			break;
		case HCS12_OP_WAI:
			break;
		case HCS12_OP_SEI:
			if (opcode[1] != 0x10)
				fix_ccr = true;
			break;
		case HCS12_OP_STOP:
			if (opcode[1] != 0x3E)
				fix_ccr = true;
			break;
		case HCS12_OP_TFR:
			/* TAP instruction is translated to TFR A, CCR */
			if (opcode[1] != 0x02) {
				fix_ccr = true;
				fix_a   = true;
			}
			break;

		case HCS12_OP_RTI:
			break;
		case HCS12_OP_SWI:
			break;

		default:
			fix_ccr = true;
			break;
		}

		if (fix_ccr) {
			/* Don't 'fix' CCR as updated by instruction or int ack */
			LOG_DEBUG("Fixing CCR\n");

			buf_set_u16(hcs12->ccr->value, HCS12_CCR_I_BIT, 1, 0);
			hcs12->ccr->dirty = true;
			hcs12->ccr->valid = true;
		}

		if (fix_a) {
			struct reg *d = &hcs12->core_cache->reg_list[HCS12_D];
			/* Fix A  (clear I flag) */
			buf_set_u16(d->value, HCS12_CCR_I_BIT, 1, 0);
			d->dirty = true;
			d->valid = true;
		}
	}

	target_call_event_callbacks(target, TARGET_EVENT_HALTED);

	return ERROR_OK;
}

static int hcs12_disable_watchdog_in_active_bdm(struct target *target)
{
	int ret;

	ret = target_write_u8(target, HCS12_REG_COPCTL,
			      HCS12_REG_COPCTL_RSBCK | HCS12_REG_COPCTL_WRTMASK);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to disable watchdog");
		return ret;
	}

	return ret;
}

static int hcs12_resume(struct target *target, int current, uint32_t address,
			int handle_breakpoints, int debug_execution)
{
	int ret;
	struct hcs12 *hcs12 = target_to_hcs12(target);
	struct breakpoint *breakpoint = NULL;
	uint16_t resume_pc, next_pc;
	struct reg *r;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = hcs12_switch_to_mode(target, HCS12_M_NORMAL_SINGLE_CHIP);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to swith chip to %s mode",
			  hcs12_get_operation_mode_name(HCS12_M_NORMAL_SINGLE_CHIP));
		return ret;
	}

	if (!debug_execution) {
		target_free_all_working_areas(target);

		hcs12_enable_breakpoints(target);
		hcs12_enable_watchpoints(target);
	}

	if (debug_execution) {
		r = hcs12->ccr;

		/* Disable interrupts */
		/*
		 * REVISIT this clearly breaks non-debug execution, since the
		 * PRIMASK register state isn't saved/restored...  workaround
		 * by never resuming app code after debug execution.
		 */
		buf_set_u16(r->value, HCS12_CCR_I_BIT, 1, 1);
		r->dirty = true;
		r->valid = true;

		if (ret < 0)
			return ret;
	}

	/* current = 1: continue on current pc, otherwise continue at <address> */
	r = hcs12->pc;
	if (!current) {
		buf_set_u16(r->value, 0, 16, address);
		r->dirty = true;
		r->valid = true;
	}
	/*
	  Our current behaviour is to ignore all of the BGND
	  instructions in the code and resume execution at the point
	  after them
	 */
	next_pc = buf_get_u16(r->value, 0, 16);
	ret     = hcs12_find_next_non_bgnd_instr_after(target, next_pc);
	if (ret < 0) {
		LOG_ERROR("Failed to find any non BGND instructions after 0x08%" PRIx16, next_pc);
		return ERROR_FAIL;
	}
	resume_pc = ret & 0xFFFF;

	if (resume_pc != next_pc) {
		buf_set_u16(r->value, 0, 16, resume_pc);
		r->dirty = true;
		r->valid = true;
	}

	ret = hcs12_restore_context(target);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to restore register context");
		return ret;
	}

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		/* Single step past breakpoint at current address */
		breakpoint = breakpoint_find(target, resume_pc);
		if (breakpoint) {
			LOG_DEBUG("unset breakpoint at 0x%8.8" PRIx32 " (ID: %" PRIu32 ")",
				  breakpoint->address,
				  breakpoint->unique_id);
			hcs12_unset_breakpoint(target, breakpoint);
			ret = bdm_trace1(target);
			if (ret != ERROR_OK) {
				LOG_ERROR("Error issuing TRACE1 command");
				return ret;
			}
			hcs12_set_breakpoint(target, breakpoint);
		}
	}

	/* Restart core */
	ret = bdm_go(target);
	if (ret < 0) {
		LOG_ERROR("Failed to resume execution");
		return ret;
	}

	target->debug_reason = DBG_REASON_NOTHALTED;

	/* registers are now invalid */
	register_cache_invalidate(hcs12->core_cache);

	if (!debug_execution) {
		target->state = TARGET_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		LOG_DEBUG("target resumed at 0x%" PRIx32 "", resume_pc);
	} else {
		target->state = TARGET_DEBUG_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
		LOG_DEBUG("target debug resumed at 0x%" PRIx32 "", resume_pc);
	}

	return ERROR_OK;
}

static int hcs12_halt(struct target *target)
{
	int ret;

	LOG_DEBUG("target->state: %s",
		  target_state_name(target));

	if (target->state == TARGET_HALTED) {
		LOG_DEBUG("target was already halted");
		return ERROR_OK;
	}

	if (target->state == TARGET_UNKNOWN)
		LOG_WARNING("target was in unknown state when halt was requested");

	if (target->state == TARGET_RESET) {
		/* we came here in a reset_halt or reset_init sequence
		 * debug entry was already prepared in hcs12_assert_reset()
		 */
		target->debug_reason = DBG_REASON_DBGRQ;

		return ERROR_OK;
	}

	ret = hcs12_disable_watchdog_in_active_bdm(target);
	if (ret < 0)
		return ret;

	ret = bdm_read_bd_byte(target, BDM_REG_BDMSTS);
	if (ret < 0)
		return ret;

	const uint8_t bdmsts = (uint8_t)(ret & 0xFF);

	if (bdmsts & BDM_REG_BDMSTS_ENBDM) {
		if (bdmsts & BDM_REG_BDMSTS_BDMACT) {
			uint8_t crgflg;
			ret = target_read_u8(target, 0x37, &crgflg);
			if (ret != ERROR_OK)
				LOG_ERROR("Failed to read flags");

			LOG_WARNING("crgflg = %x", crgflg);

			LOG_WARNING("Device is already halted but its state does not reflect it");
			return ERROR_OK;
		}
	} else {
		ret = bdm_write_bd_byte(target, BDM_REG_BDMSTS,
					BDM_REG_BDMSTS_ENBDM | bdmsts);
		if (ret < 0)
			return ret;
	}

	ret = bdm_background(target);
	if (ret < 0)
		return ret;

	target->debug_reason = DBG_REASON_DBGRQ;

	return ERROR_OK;
}

static int hcs12_assert_reset(struct target *target)
{
	int ret;
	struct hcs12 *hcs12 = target_to_hcs12(target);

	LOG_DEBUG("target->state: %s",
		target_state_name(target));

	if (target_has_event_action(target, TARGET_EVENT_RESET_ASSERT)) {
		/* allow scripts to override the reset event */

		target_handle_event(target, TARGET_EVENT_RESET_ASSERT);
		register_cache_invalidate(hcs12->core_cache);
		target->state = TARGET_RESET;

		return ERROR_OK;
	}

	adapter_assert_reset();
	/*
	  FIXME: What is the actual delay needed for reset sequence?
	 */
	keep_alive();
	jtag_sleep(200 * 1000);

	if (target->reset_halt) {
		ret = bdm_assert_bknd(target);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to assert BKND pin");
			return ret;
		}
	}

	target->state = TARGET_RESET;
	register_cache_invalidate(hcs12->core_cache);

	return ERROR_OK;
}

static int hcs12_deassert_reset(struct target *target)
{
	int ret;

	adapter_deassert_reset();

	if (target->reset_halt) {
		keep_alive();
		jtag_sleep(200 * 1000);

		ret = bdm_deassert_bknd(target);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to deassert BKND pin");
			return ret;
		}

		ret = bdm_read_bd_byte(target, BDM_REG_BDMSTS);
		if (ret < 0)
			return ret;

		const uint8_t bdmsts = (uint8_t)(ret & 0xFF);

		/* Since we are ressetting into a special single chip mode
		 * BDM should be enable and active out of reset. If the is
		 * not the case something gone horribly wrong */
		if ((bdmsts & (BDM_REG_BDMSTS_ENBDM | BDM_REG_BDMSTS_BDMACT)) ==
		    (BDM_REG_BDMSTS_ENBDM | BDM_REG_BDMSTS_BDMACT))
			return ERROR_OK;
		else
			return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int hcs12_soft_reset_halt(struct target *target)
{
	return ERROR_FAIL;
}

static int hcs12_get_gdb_reg_list(struct target *target, struct reg **reg_list[],
				  int *reg_list_size, enum target_register_class reg_class)
{
	return ERROR_FAIL;
}

int32_t hcs12_ram_map_global_to_local(struct target *target, uint32_t address)
{
	assert(address <= 0x0FFFFF);
	/* FIXME: We probably should include the amount of RAM in our
	 * part specific information and use it for boundary check here */

	int ret;
	uint8_t  page   = address >> 12;
	uint16_t offset = address & 0xFFF;

	switch (page) {
	case 0xFF:
		return 0x3000 + offset;

	case 0xFE:
		return 0x2000 + offset;
	default:
		ret = target_write_u8(target, HCS12_REG_RPAGE, page);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to write to RPAGE register");
			return ret;
		}
		return 0x1000 + offset;
	}
}

static int hcs12_start_algorithm(struct target *target,
				 int num_mem_params, struct mem_param *mem_params,
				 int num_reg_params, struct reg_param *reg_params,
				 uint32_t entry_point, uint32_t exit_point,
				 void *arch_info)
{
	struct hcs12 *hcs12 = target_to_hcs12(target);
	struct hcs12_algorithm *hcs12_algorithm_info = arch_info;

	int ret, i;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* refresh core register cache
	 * Not needed if core register cache is always consistent with target process state */
	for (i = 0; i < HCS12_NUM_REGS; i++)
		hcs12_algorithm_info->context[i] = buf_get_u16(hcs12->core_cache->reg_list[i].value,
							       0, 16);


	for (i = 0; i < num_mem_params; i++) {
		if (mem_params[i].direction != PARAM_IN) {
			ret = target_write_buffer(target,
						  mem_params[i].address,
						  mem_params[i].size,
						  mem_params[i].value);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to write memory parameter @ 0x%" PRIx32,
					  mem_params[i].address);
				return ret;
			}
		}
	}

	for (i = 0; i < num_reg_params; i++) {
		struct reg *reg = register_get_by_name(hcs12->core_cache,
						       reg_params[i].reg_name,
						       false);
		assert(reg != NULL);
		assert(reg->size == reg_params[i].size);

		hcs12_set_core_reg(reg, reg_params[i].value);
	}

	ret = hcs12_ram_map_global_to_local(target, entry_point);
	if (ret < 0) {
		LOG_ERROR("Failed to map global resume point address to a local one");
		return ret;
	}

	const uint16_t pc = ret & 0xFFFF;

	ret = target_resume(target, 0, pc, 1, 1);
	if (ret != ERROR_OK)
		LOG_ERROR("Failed to resume the target");

	return ret;
}

static int hcs12_wait_algorithm(struct target *target,
				int num_mem_params, struct mem_param *mem_params,
				int num_reg_params, struct reg_param *reg_params,
				uint32_t exit_point, int timeout_ms,
				void *arch_info)
{
	struct hcs12 *hcs12 = target_to_hcs12(target);
	struct hcs12_algorithm *hcs12_algorithm_info = arch_info;

	int ret, i;
	uint16_t pc;

	/* NOTE:  hcs12_run_algorithm requires that each algorithm
	 * uses a BGND instruction at the exit point */

	ret = target_wait_state(target, TARGET_HALTED, timeout_ms);
	/* If the target fails to halt due to the breakpoint, force a halt */
	if (ret != ERROR_OK || target->state != TARGET_HALTED) {
		ret = target_halt(target);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to halt the target");
			return ret;
		}

		ret = target_wait_state(target, TARGET_HALTED, 500);
		if (ret != ERROR_OK) {
			LOG_ERROR("Timded out waiting for the target to halt");
			return ret;
		}

		return ERROR_TARGET_TIMEOUT;
	}

	if (exit_point) {
		ret = hcs12_get_core_reg(hcs12->pc);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to read the value of PC register");
			return ret;
		}

		pc = buf_get_u16(hcs12->pc, 0, 16);

		if (pc != exit_point) {
			LOG_ERROR("failed algorithm halted at 0x%" PRIx32 ", expected 0x%" PRIx32,
				  pc,
				  exit_point);

			return ERROR_FAIL;
		}
	}

	for (i = 0; i < num_mem_params; i++) {
		if (mem_params[i].direction != PARAM_OUT) {
			ret = target_read_buffer(target,
						 mem_params[i].address,
						 mem_params[i].size,
						 mem_params[i].value);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to read memory parameter @ 0x%" PRIx32,
					  mem_params[i].address);
				return ret;
			}
		}
	}

	for (i = 0; i < num_reg_params; i++) {
		if (reg_params[i].direction != PARAM_OUT) {
			struct reg *reg = register_get_by_name(hcs12->core_cache,
							       reg_params[i].reg_name,
							       0);
			assert(reg != NULL);
			assert(reg->size == reg_params[i].size);

			const uint16_t val = buf_get_u16(reg->value, 0, 16);
			buf_set_u16(reg_params[i].value, 0, 16, val);
		}
	}

	for (i = HCS12_NUM_REGS - 1; i >= 0; i--) {
		struct reg *reg = &hcs12->core_cache->reg_list[i];
		const uint16_t regvalue = buf_get_u16(reg->value, 0, 16);

		if (regvalue != hcs12_algorithm_info->context[i]) {
			LOG_DEBUG("restoring register %s with value 0x%8.8" PRIx32,
				  reg->name,
				  hcs12_algorithm_info->context[i]);

			buf_set_u16(reg->value, 0, 16, hcs12_algorithm_info->context[i]);

			reg->valid = 1;
			reg->dirty = 1;
		}
	}

	return ret;
}

static int hcs12_run_algorithm(struct target *target,
			       int num_mem_params, struct mem_param *mem_params,
			       int num_reg_params, struct reg_param *reg_params,
			       uint32_t entry_point, uint32_t exit_point,
			       int timeout_ms, void *arch_info)
{
	int ret;

	ret = hcs12_start_algorithm(target,
				    num_mem_params, mem_params,
				    num_reg_params, reg_params,
				    entry_point, exit_point,
				    arch_info);

	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to start algorightm");
		return ret;
	}

	ret = hcs12_wait_algorithm(target,
				   num_mem_params, mem_params,
				   num_reg_params, reg_params,
				   exit_point, timeout_ms,
				   arch_info);

	if (ret != ERROR_OK)
		LOG_ERROR("Failed to wait for algorightm");

	return ret;
}



static int hcs12_target_create(struct target *target, Jim_Interp *interp)
{
	struct hcs12 *hcs12;

	hcs12 = calloc(1, sizeof(*hcs12));
	if (!hcs12)
		return ERROR_FAIL;

	hcs12->target		= target;
	hcs12->read_core_reg	= hcs12_read_core_reg;
	hcs12->write_core_reg	= hcs12_write_core_reg;

	target->arch_info = hcs12;

	return ERROR_OK;
}

static void hcs12_build_reg_cache(struct target *target)
{
	struct hcs12 *hcs12 = target_to_hcs12(target);

	int i;
	struct reg_cache **cache_p;
	struct reg_cache *cache;
	struct reg *reg_list;
	struct reg_feature *feature;
	struct hcs12_register *arch_info;

	cache_p  = register_get_last_cache_p(&target->reg_cache);
	cache    = calloc(1, sizeof(*cache)),                 assert(cache    != NULL);
	reg_list = calloc(HCS12_NUM_REGS, sizeof(*reg_list)), assert(reg_list != NULL);

	cache->name	= "hcs12 registers";
	cache->next	= NULL;
	cache->reg_list = reg_list;
	cache->num_regs = HCS12_NUM_REGS;
	(*cache_p)	= cache;


	for (i = 0; i < HCS12_NUM_REGS; i++) {
		arch_info = calloc(1, sizeof(*arch_info)), assert(arch_info != NULL);

		arch_info->target = target;
		arch_info->write  = hcs12_registers_table[i].write;
		arch_info->read	  = hcs12_registers_table[i].read;

		reg_list[i].name	= hcs12_registers_table[i].name;
		reg_list[i].size	= 16;
		reg_list[i].value	= calloc(1, 2);
		reg_list[i].dirty	= 0;
		reg_list[i].valid	= 0;
		reg_list[i].type	= &hcs12_register_type;
		reg_list[i].arch_info	= arch_info;

		reg_list[i].group	= hcs12_registers_table[i].group;
		reg_list[i].number	= i;
		reg_list[i].exist	= true;
		reg_list[i].caller_save = true;	/* gdb defaults to true */
		feature = calloc(1, sizeof(*feature)), assert(feature != NULL);

		feature->name = hcs12_registers_table[i].feature;
		reg_list[i].feature = feature;

		reg_list[i].reg_data_type =
			calloc(1, sizeof(*reg_list[i].reg_data_type)), assert(reg_list[i].reg_data_type != NULL);

		reg_list[i].reg_data_type->type = hcs12_registers_table[i].type;
	}

	hcs12->sp  = reg_list + HCS12_SP;
	hcs12->pc  = reg_list + HCS12_PC;
	hcs12->ccr = reg_list + HCS12_CCR;
	hcs12->core_cache = cache;
}


static int hcs12_init_target(struct command_context *cmd_ctx, struct target *target)
{
	hcs12_build_reg_cache(target);
	return ERROR_OK;
}

static int hcs12_examine(struct target *target)
{
	if (!target_was_examined(target)) {
		target_set_examined(target);
		const int16_t family_id = bdm_read_bd_byte(target, BDM_REG_FAMILY_ID);

		if (family_id < 0) {
			LOG_ERROR("Failed to read MCU family ID");
			return ERROR_FAIL;
		} else if (family_id != HCS12_FAMILY_ID) {
			LOG_ERROR("Family ID is not what is expected for HCS12 (0x%02x)", family_id);
			return ERROR_FAIL;
		}

		/* FIXME: We need to handle secured part properly */

		int ret = hcs12_disable_watchdog_in_active_bdm(target);
		if (ret < 0)
			return ret;

	}

	return ERROR_OK;
}

struct target_type hcs12_target = {
	.name			= "hcs12",

	.poll			= hcs12_poll,
	.arch_state		= hcs12_arch_state,

	.target_request_data	= hcs12_target_request_data,

	.halt			= hcs12_halt,
	.resume			= hcs12_resume,
	.step			= hcs12_step,

	.assert_reset		= hcs12_assert_reset,
	.deassert_reset		= hcs12_deassert_reset,
	.soft_reset_halt	= hcs12_soft_reset_halt,

	.get_gdb_reg_list	= hcs12_get_gdb_reg_list,

	.read_memory		= bdm_read_memory,
	.write_memory		= bdm_write_memory,

	.add_breakpoint		= hcs12_add_breakpoint,
	.remove_breakpoint	= hcs12_remove_breakpoint,
	.add_watchpoint		= hcs12_add_watchpoint,
	.remove_watchpoint	= hcs12_remove_watchpoint,

	.target_create		= hcs12_target_create,
	.init_target		= hcs12_init_target,
	.examine		= hcs12_examine,

	.run_algorithm		= hcs12_run_algorithm,
	.start_algorithm	= hcs12_start_algorithm,
	.wait_algorithm		= hcs12_wait_algorithm,
};
