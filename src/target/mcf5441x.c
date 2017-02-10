/***************************************************************************
 *                                                                         *                                          *
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
#include "bdm_cf26.h"
#include "mcf5441x.h"
#include "log.h"

static const struct {
	unsigned id;
	const char *name;
	unsigned bits;
	enum reg_type type;
	const char *group;
	const char *feature;
} mcf5441x_registers_table[] = {
	{ CF_D0, "d0", 32, REG_TYPE_INT, "general", "org.gnu.gdb.coldfire.core" },
	{ CF_D1, "d1", 32, REG_TYPE_INT, "general", "org.gnu.gdb.coldfire.core" },
	{ CF_D2, "d2", 32, REG_TYPE_INT, "general", "org.gnu.gdb.coldfire.core" },
	{ CF_D3, "d3", 32, REG_TYPE_INT, "general", "org.gnu.gdb.coldfire.core" },
	{ CF_D4, "d4", 32, REG_TYPE_INT, "general", "org.gnu.gdb.coldfire.core" },
	{ CF_D5, "d5", 32, REG_TYPE_INT, "general", "org.gnu.gdb.coldfire.core" },
	{ CF_D6, "d6", 32, REG_TYPE_INT, "general", "org.gnu.gdb.coldfire.core" },
	{ CF_D7, "d7", 32, REG_TYPE_INT, "general", "org.gnu.gdb.coldfire.core" },
	{ CF_A0, "a0", 32, REG_TYPE_DATA_PTR, "data_ptr", "org.gnu.gdb.coldfire.core" },
	{ CF_A1, "a1", 32, REG_TYPE_DATA_PTR, "data_ptr", "org.gnu.gdb.coldfire.core" },
	{ CF_A2, "a2", 32, REG_TYPE_DATA_PTR, "data_ptr", "org.gnu.gdb.coldfire.core" },
	{ CF_A3, "a3", 32, REG_TYPE_DATA_PTR, "data_ptr", "org.gnu.gdb.coldfire.core" },
	{ CF_A4, "a4", 32, REG_TYPE_DATA_PTR, "data_ptr", "org.gnu.gdb.coldfire.core" },
	{ CF_A5, "a5", 32, REG_TYPE_DATA_PTR, "data_ptr", "org.gnu.gdb.coldfire.core" },
	{ CF_FP, "fp", 32, REG_TYPE_DATA_PTR, "data_ptr", "org.gnu.gdb.coldfire.core" },
	{ CF_SP, "sp", 32, REG_TYPE_DATA_PTR, "data_ptr", "org.gnu.gdb.coldfire.core" },
	{ CF_PS, "ps", 32, REG_TYPE_INT, "general", "org.gnu.gdb.coldfire.core" },
	{ CF_PC, "pc", 32, REG_TYPE_CODE_PTR, "code_ptr", "org.gnu.gdb.coldfire.core" },
	{ CF_VBR, "vbr", 32, REG_TYPE_DATA_PTR, "data_ptr", "org.gnu.gdb.coldfire.core" },
};

#define CF_NUM_REGS	ARRAY_SIZE(mcf5441x_registers_table)

struct mcf5441x_reg {
	struct target *target;
};

static int mcf5441x_poll(struct target *target)
{
	enum target_state prev_target_state = target->state;
	uint32_t csr;

	csr = bdm_cf26_read_dm_reg(target, BDM_REG_CSR);

	if ((csr & CSR_BPKT) || (csr & CSR_HALT)) {

		target->state = TARGET_HALTED;

		if ((prev_target_state == TARGET_RUNNING) ||
			(prev_target_state == TARGET_RESET)) {

			target_call_event_callbacks(target, TARGET_EVENT_HALTED);

			if (mcf5441x_debug_entry(target) != ERROR_OK)
				LOG_WARNING("can't set debug entry");
		}
	}

	return ERROR_OK;
}

static int mcf5441x_read_core_reg(struct mcf5441x *mcf5441x, struct reg *reg)
{
	int32_t ret;
	struct target *target = mcf5441x->target;

	assert(reg->number < mcf5441x->core_cache->num_regs);

	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;

	/* special cases */
	if (reg->number == CF_PC)
		ret = bdm_cf26_read_pc(target);
	else
		ret = bdm_cf26_read_ad_reg(target, reg->number);

	buf_set_u32(reg->value, 0, 32, ret);
	reg->valid = 1;
	reg->dirty = 0;

	return ERROR_OK;
}

static int mcf5441x_write_core_reg(struct mcf5441x *mcf5441x,
				   struct reg *reg, uint8_t *value)
{
	int ret, regval;
	struct target *target = mcf5441x->target;

	if (target->state != TARGET_HALTED)
		LOG_WARNING("target not halted");

	regval = buf_get_u32(value, 0, 32);

	assert(reg->number < mcf5441x->core_cache->num_regs);

	/* special cases */
	if (reg->number == CF_PC)
		ret = bdm_cf26_write_pc(target, regval);
	else
		ret = bdm_cf26_write_ad_reg(target, reg->number, regval);

	if (ret != ERROR_OK) {
		LOG_ERROR("BDM failure");
		reg->dirty = reg->valid;
		return ERROR_JTAG_DEVICE_ERROR;
	}

	reg->valid = 1;
	reg->dirty = 0;

	return ERROR_OK;
}

static int mcf5441x_get_core_reg(struct reg *reg)
{
	struct mcf5441x_reg *mcfreg;
	struct target *target;
	struct mcf5441x *cpu;

	mcfreg = reg->arch_info;
	target = mcfreg->target;
	cpu = target_to_mcf5441x(target);

	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;

	return cpu->read_core_reg(cpu, reg);
}

static int mcf5441x_set_core_reg(struct reg *reg, uint8_t *buf)
{
	struct mcf5441x_reg *mcfreg;
	struct target *target;
	uint32_t value;

	mcfreg = reg->arch_info;
	target = mcfreg->target;

	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;

	value = buf_get_u32(buf, 0, 32);
	buf_set_u32(reg->value, 0, 32, value);

	reg->dirty = 1;
	reg->valid = 1;

	return ERROR_OK;
}

static const struct reg_arch_type mcf5441x_reg_type = {
	.get = mcf5441x_get_core_reg,
	.set = mcf5441x_set_core_reg,
};

/**
 * Restores target context using the cache of core registers set up
 * by mcf5441x_build_reg_cache(), calling optional core-specific hooks.
 */
static int mcf5441x_restore_context(struct target *target)
{
	int i;
	struct mcf5441x *cpu = target_to_mcf5441x(target);
	struct reg_cache *cache = cpu->core_cache;

	for (i = cache->num_regs - 1; i >= 0; i--) {
		if (cache->reg_list[i].dirty) {
			cpu->write_core_reg(cpu,
				&cache->reg_list[i],
				(uint8_t *)cache->reg_list[i].value);
		}
	}

	return ERROR_OK;
}

static int mcf5441x_disable_breakpoints(struct target *target)
{
	int ret;
	struct breakpoint *breakpoint = target->breakpoints;

	/* set any pending breakpoints */
	while (breakpoint) {
		if (breakpoint->set) {
			ret = mcf5441x_unset_breakpoint(target, breakpoint);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to unset breakpoint");
				break;
			}
		}
		breakpoint = breakpoint->next;
	}

	return ERROR_OK;
}

static int mcf5441x_enable_breakpoints(struct target *target)
{
	int ret;
	struct breakpoint *breakpoint = target->breakpoints;

	/* set any pending breakpoints */
	while (breakpoint) {
		if (!breakpoint->set) {
			ret = mcf5441x_set_breakpoint(target, breakpoint);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to set breakpoint");
				break;
			}
		}
		breakpoint = breakpoint->next;
	}

	return ERROR_OK;
}

static int mcf5441x_step(struct target *target, int current, uint32_t address,
				int handle_breakpoints)
{
	struct mcf5441x *cpu = target_to_mcf5441x(target);
	uint32_t value;
	int ret;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* current = 1: continue on current pc,
	 * otherwise continue at <address>
	 */
	if (!current) {
		LOG_INFO("mcf5441x_step() not current");
		buf_set_u32(cpu->pc->value, 0, 32, address);
		cpu->pc->dirty = 1;
	}

	target->debug_reason = DBG_REASON_SINGLESTEP;

	/*
	 * restoring context now, before go
	 */
	mcf5441x_restore_context(target);

	target_call_event_callbacks(target, TARGET_EVENT_RESUMED);

	/*
	 * getting CRS and re-setting for step
	 */
	value = bdm_cf26_read_dm_reg(target, BDM_REG_CSR);
	/* setting in step, no interrupt */
	value |= (CSR_SSM | CSR_IPI);
	/* emulator mode */
	value |= CSR_EMULATION;
	bdm_cf26_write_dm_reg(target, BDM_REG_CSR, value);

	/*
	 * ColdFire is magic, is magic, oh-oh-oh, the summer is magic
	 *
	 * With BDM in STEP + EMULATION mode, cpu caches somewhere a group
	 * of program instructions just after each go-step (go performs a step).
	 * We need to remove all the breakpoints so, or it will remember
	 * of them even if they are not there anymore.
	 */
	mcf5441x_disable_breakpoints(target);

	/* ok, go now acts as single step */
	ret = bdm_cf26_go(target);
	if (ret < 0) {
		LOG_ERROR("Failed to resume execution");
		return ret;
	}

	if (mcf5441x_debug_entry(target) != ERROR_OK)
		LOG_WARNING("can't set debug entry");

	/* now we should be halted with new reg read */
	target_call_event_callbacks(target, TARGET_EVENT_HALTED);

	return ERROR_OK;
}

static int mcf5441x_set_breakpoint(struct target *target,
				 struct breakpoint *breakpoint)
{
	int retval;

	if (breakpoint->set) {
		LOG_WARNING("breakpoint (BPID: %" PRIu32 ") already set",
			    breakpoint->unique_id);
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		/* TODO: gdb / kdevelop actually asks for SW breakpoints
		 * only */
	} else if (breakpoint->type == BKPT_SOFT) {

		uint8_t code[2];

		/*
		 * technique is to replace current memory location
		 * with an HALT.
		 */
		retval = target_read_memory(target,
				breakpoint->address,
				breakpoint->length, 1,
				breakpoint->orig_instr);

		if (retval != ERROR_OK)
			return retval;

		/* swap to LE is done inside bdm_cf26_write_mem_word
		 */
		h_u16_to_be(code, CF_OP_HALT);
		retval = target_write_memory(target,
				breakpoint->address,
				breakpoint->length, 1,
				code);

		if (retval != ERROR_OK)
			return retval;

		breakpoint->set = true;
	}

	LOG_DEBUG("BPID: %" PRIu32 ", Type: %d, Address: 0x%08" PRIx32
		" Length: %d (set=%d)",
		breakpoint->unique_id,
		(int)(breakpoint->type),
		breakpoint->address,
		breakpoint->length,
		breakpoint->set);

	return ERROR_OK;
}

static int mcf5441x_unset_breakpoint(struct target *target,
				 struct breakpoint *breakpoint)
{
	int retval;

	if (!breakpoint->set) {
		LOG_WARNING("breakpoint not set");
		return ERROR_OK;
	}

	LOG_DEBUG("BPID: %" PRIu32 ", Type: %d, Address: 0x%08" PRIx32
		" Length: %d (set=%d)",
		breakpoint->unique_id,
		(int)(breakpoint->type),
		breakpoint->address,
		breakpoint->length,
		breakpoint->set);

	if (breakpoint->type == BKPT_HARD) {
		/* TODO */
	} else {
		/* restore original instruction (kept in target endianness) */
		retval = target_write_memory(target,
				breakpoint->address,
				breakpoint->length, 1,
				breakpoint->orig_instr);

		if (retval != ERROR_OK)
			return retval;
	}
	breakpoint->set = false;

	return ERROR_OK;
}

static int mcf5441x_add_breakpoint(struct target *target,
				   struct breakpoint *breakpoint)
{
	return mcf5441x_set_breakpoint (target, breakpoint);
}

static int mcf5441x_remove_breakpoint(struct target *target,
				   struct breakpoint *breakpoint)
{
	if (breakpoint->set)
		mcf5441x_unset_breakpoint(target, breakpoint);

	return ERROR_OK;
}

static int mcf5441x_resume(struct target *target, int current, uint32_t address,
			int handle_breakpoints, int debug_execution)
{
	struct mcf5441x *cpu = target_to_mcf5441x(target);
	uint32_t value;
	int ret;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!debug_execution)
		mcf5441x_enable_breakpoints(target);

	ret = mcf5441x_restore_context(target);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to restore register context");
		return ret;
	}

	/* the front-end may request us not to handle breakpoints */

	/* removing any emulation mode from CSR */
	value = bdm_cf26_read_dm_reg(target, BDM_REG_CSR);
	/* setting in step, no interrupt */
	value &= ~(CSR_SSM | CSR_IPI);
	/* emulator mode */
	value &= ~CSR_EMULATION;
	bdm_cf26_write_dm_reg(target, BDM_REG_CSR, value);

	/* Restart core */
	ret = bdm_cf26_go(target);
	if (ret < 0) {
		LOG_ERROR("Failed to resume execution");
		return ret;
	}

	target->debug_reason = DBG_REASON_NOTHALTED;

	/* registers are now invalid */
	register_cache_invalidate(cpu->core_cache);

	if (!debug_execution) {
		target->state = TARGET_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		LOG_DEBUG("target resumed");
	} else {
		target->state = TARGET_DEBUG_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
		LOG_DEBUG("target debug resumed");
	}

	return ERROR_OK;
}

static int mcf5441x_assert_reset(struct target *target)
{
	bdm_cf26_assert_reset(target);

	target->state = TARGET_RESET;

	return ERROR_OK;
}

static int mcf5441x_deassert_reset(struct target *target)
{
	bdm_cf26_deassert_reset(target);

	return ERROR_OK;
}

static int mcf5441x_setup_all_cpu_regs(struct target *target, uint8_t *reg_buf)
{
	int i;
	uint32_t value;
	struct mcf5441x *cpu = target_to_mcf5441x(target);
	struct reg_cache *cache = cpu->core_cache;

	for (i = CF_SP; i >= 0; i--) {
		value = be_to_h_u32(reg_buf);
		buf_set_u32(cache->reg_list[i].value, 0, 32, value);
		cache->reg_list[i].valid = 1;
		reg_buf += sizeof(uint32_t) + sizeof(uint16_t);
	}

	/* ok, misaligned rest now (different order in pemu buffer) */
	value = be_to_h_u32(reg_buf);
	buf_set_u32(cache->reg_list[CF_PC].value, 0, 32, value);
	cache->reg_list[CF_PC].valid = 1;
	reg_buf += sizeof(uint32_t) + sizeof(uint16_t);
	value = be_to_h_u32(reg_buf);
	buf_set_u32(cache->reg_list[CF_VBR].value, 0, 32, value);
	cache->reg_list[CF_VBR].valid = 1;
	reg_buf += sizeof(uint32_t) + sizeof(uint16_t);
	value = be_to_h_u32(reg_buf);
	buf_set_u32(cache->reg_list[CF_PS].value, 0, 32, value);
	cache->reg_list[CF_PS].valid = 1;

	return ERROR_OK;
}

static int mcf5441x_debug_entry(struct target *target)
{
	struct mcf5441x *cpu = target_to_mcf5441x(target);
	struct reg_cache *cache = cpu->core_cache;
	struct breakpoint *breakpoint;
	uint8_t *reg_buf;

	bdm_cf26_get_all_cpu_regs(target, &reg_buf);
	mcf5441x_setup_all_cpu_regs(target, reg_buf);

	uint32_t pc_value = buf_get_u32(cpu->pc->value, 0, 32);

	breakpoint = breakpoint_find(target, pc_value - 2);
	if (breakpoint) {
		mcf5441x_unset_breakpoint(target, breakpoint);

		buf_set_u32(cpu->pc->value, 0, 32, pc_value - 2);

		cpu->write_core_reg(cpu,
				&cache->reg_list[CF_PC],
				(uint8_t *)cache->reg_list[CF_PC].value);
	}

	return ERROR_OK;
}

static int mcf5441x_halt(struct target *target)
{
	if (target->state == TARGET_HALTED) {
		LOG_DEBUG("target was already halted");
		return ERROR_OK;
	}

	if (target->state == TARGET_UNKNOWN)
		LOG_WARNING(
			"target was in unknown state when halt was requested");

	/*
	 * need expert help here: seems that gdb, if not informed
	 * here immediately, cannot wait poll to reply and times-out
	 * closing conneciton.
	 * Sp gdb as default seems to have a quite short timeout, so,
	 * arranging to reply to halted asap here
	 */
	if (target->state != TARGET_HALTED) {
		int timeout = 0;
		uint32_t csr;

		target->state = TARGET_HALTED;
		target_call_event_callbacks(target, TARGET_EVENT_HALTED);

		bdm_cf26_halt(target);
		alive_sleep(1);

		while (timeout++ < 100) {
			csr = bdm_cf26_read_dm_reg(target, BDM_REG_CSR);

			if (csr & CSR_BPKT)
				break;

			alive_sleep(1);
		}

		if (mcf5441x_debug_entry(target) != ERROR_OK)
			LOG_WARNING("can't set debug entry");
	}

	return ERROR_OK;
}

static void mcf5441x_build_reg_cache(struct target *target)
{
	struct mcf5441x *mcf5441x = target_to_mcf5441x(target);
	struct reg_feature *feature;
	struct reg_cache **cache_p;
	struct reg_cache *cache;
	struct reg *reg_list;
	struct mcf5441x_reg *arch_info;
	int num_regs = CF_NUM_REGS;
	int i;

	arch_info = calloc(CF_NUM_REGS, sizeof(struct mcf5441x_reg));
	reg_list = calloc(CF_NUM_REGS, sizeof(struct reg));
	cache = malloc(sizeof(struct reg_cache));
	cache_p = register_get_last_cache_p(&target->reg_cache);

	cache->name = "coldfire registers";
	cache->next = NULL;
	cache->reg_list = reg_list;
	cache->num_regs = num_regs;
	(*cache_p) = cache;

	for (i = 0; i < num_regs; i++) {
		arch_info[i].target = target;
		reg_list[i].name = mcf5441x_registers_table[i].name;
		reg_list[i].size = mcf5441x_registers_table[i].bits;
		reg_list[i].value = calloc(1, 4);
		reg_list[i].dirty = 0;
		reg_list[i].valid = 0;
		reg_list[i].type = &mcf5441x_reg_type;
		reg_list[i].arch_info = &arch_info[i];
		reg_list[i].group = mcf5441x_registers_table[i].group;
		reg_list[i].number = i;
		reg_list[i].exist = true;
		reg_list[i].caller_save = true;	/* gdb defaults to true */

		feature = calloc(1, sizeof(*feature));
		if (feature) {
			feature->name = mcf5441x_registers_table[i].feature;
			reg_list[i].feature = feature;
		}
		reg_list[i].reg_data_type =
			calloc(1, sizeof(struct reg_data_type));
		if (reg_list[i].reg_data_type)
			reg_list[i].reg_data_type->type =
				mcf5441x_registers_table[i].type;
	}

	mcf5441x->core_cache = cache;

	mcf5441x->pc = &cache->reg_list[CF_PC];
	mcf5441x->sp = &cache->reg_list[CF_SP];
}

static int mcf5441x_check_hw(uint32_t d0, uint32_t d1)
{
	if ((d0 & 0x0f) != 0xf)
		return ERROR_FAIL;

	if (((d0 & 0xf0) >> 4) != 0x02)
		return ERROR_FAIL;

	if (!(d0 & (1 << 11)))
		return ERROR_FAIL;

	if (((d0 & 0xf00000) >> 20) != 0x4)
		return ERROR_FAIL;

	LOG_INFO("mcf5441x detected, D+PSTB, MMU, ISA_C");

	return ERROR_OK;
}

static int mcf5441x_examine(struct target *target)
{
	if (!target_was_examined(target)) {

		target_set_examined(target);

		const uint32_t csr =
			bdm_cf26_read_dm_reg(target, BDM_REG_CSR);

		LOG_INFO("examining cpu ...");

		if (csr == 0xffffffff) {
			LOG_ERROR("device not connected or not powered");
			return ERROR_FAIL;
		} else {
			/*
			 * ColdFire processors load hardware configuration
			 * information into the D0 and D1
			 */
			uint32_t d0, d1;

			d0 = bdm_cf26_read_ad_reg(target, CF_D0);
			d1 = bdm_cf26_read_ad_reg(target, CF_D1);

			if (d0 == 0xffffffff || d1 == 0xffffffff)
				return ERROR_FAIL;

			if (mcf5441x_check_hw(d0, d1) == ERROR_FAIL)
				return ERROR_FAIL;

			/*
			 * set an initial known state
			 */
			target->state = TARGET_RESET;

			mcf5441x_halt(target);
			mcf5441x_debug_entry(target);

			target->debug_reason = DBG_REASON_DBGRQ;
		}
	}

	return ERROR_OK;
}

static int mcf5441x_init_target(struct command_context *cmd_ctx,
				struct target *target)
{
	mcf5441x_build_reg_cache(target);

	return ERROR_OK;
}

static int mcf5441x_target_create(struct target *target, Jim_Interp *interp)
{
	struct mcf5441x *mcf5441x;

	mcf5441x = calloc(1, sizeof(*mcf5441x));
	if (!mcf5441x)
		return ERROR_FAIL;

	mcf5441x->target = target;
	mcf5441x->read_core_reg = mcf5441x_read_core_reg;
	mcf5441x->write_core_reg = mcf5441x_write_core_reg;

	target->arch_info = mcf5441x;

	return ERROR_OK;
}

int mcf5441x_get_gdb_reg_list(struct target *target, struct reg **reg_list[],
		int *reg_list_size, enum target_register_class reg_class)
{
	struct mcf5441x *cpu = target_to_mcf5441x(target);
	int i;

	if (reg_class == REG_CLASS_ALL)
		*reg_list_size = cpu->core_cache->num_regs;
	else
		*reg_list_size = 8;

	*reg_list = malloc(sizeof(struct reg *) * (*reg_list_size));
	if (*reg_list == NULL)
		return ERROR_FAIL;

	for (i = 0; i < *reg_list_size; i++)
		(*reg_list)[i] = &cpu->core_cache->reg_list[i];

	return ERROR_OK;
}

struct target_type mcf5441x_target = {
	.name = "mcf5441x",
	.target_create = mcf5441x_target_create,
	.init_target = mcf5441x_init_target,
	.examine = mcf5441x_examine,
	.get_gdb_reg_list =  mcf5441x_get_gdb_reg_list,
	.halt = mcf5441x_halt,
	.resume = mcf5441x_resume,
	.poll = mcf5441x_poll,
	.step = mcf5441x_step,
	.assert_reset = mcf5441x_assert_reset,
	.deassert_reset = mcf5441x_deassert_reset,
	.add_breakpoint = mcf5441x_add_breakpoint,
	.remove_breakpoint = mcf5441x_remove_breakpoint,
	.read_memory = bdm_cf26_read_memory,
	.write_memory = bdm_cf26_write_memory,
};
