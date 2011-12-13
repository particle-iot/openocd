/***************************************************************************
 *   Copyright (C) 2011 by Mathias Kuester                                 *
 *   Mathias Kuester <kesmtp@freenet.de>                                   *
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

#include "jtag/jtag.h"
#include "jtag/stlink/stlink_interface.h"
#include "jtag/stlink/stlink_layout.h"
#include "register.h"
#include "algorithm.h"
#include "target.h"
#include "breakpoints.h"
#include "target_type.h"
#include "armv7m.h"

struct stm32_stlink_common {
	/** */
	struct reg_cache *core_cache;
	/** */
	struct armv7m_common armv7m;
};

static struct stm32_stlink_common *stm32_stlink;

static inline struct stlink_interface_s *target_to_stlink(struct target *target)
{
	return target->tap->priv;
}

static int stm32_stlink_load_core_reg_u32(struct target *target,
					  enum armv7m_regtype type,
					  uint32_t num, uint32_t *value)
{
	int retval;
	struct stlink_interface_s *stlink_if = target_to_stlink(target);

	LOG_DEBUG("%s", __func__);

	/* NOTE:  we "know" here that the register identifiers used
	 * in the v7m header match the Cortex-M3 Debug Core Register
	 * Selector values for R0..R15, xPSR, MSP, and PSP.
	 */
	switch (num) {
	case 0 ... 18:
		/* read a normal core register */
		retval =
		    stlink_if->layout->api->read_reg(stlink_if->fd, num, value);

		if (retval != ERROR_OK) {
			LOG_ERROR("JTAG failure %i", retval);
			return ERROR_JTAG_DEVICE_ERROR;
		}
		LOG_DEBUG("load from core reg %i  value 0x%" PRIx32 "",
			  (int)num, *value);
		break;

	case ARMV7M_PRIMASK:
	case ARMV7M_BASEPRI:
	case ARMV7M_FAULTMASK:
	case ARMV7M_CONTROL:
		/* Cortex-M3 packages these four registers as bitfields
		 * in one Debug Core register.  So say r0 and r2 docs;
		 * it was removed from r1 docs, but still works.
		 */
		retval =
		    stlink_if->layout->api->read_reg(stlink_if->fd, 20, value);

		switch (num) {
		case ARMV7M_PRIMASK:
			*value = buf_get_u32((uint8_t *) value, 0, 1);
			break;

		case ARMV7M_BASEPRI:
			*value = buf_get_u32((uint8_t *) value, 8, 8);
			break;

		case ARMV7M_FAULTMASK:
			*value = buf_get_u32((uint8_t *) value, 16, 1);
			break;

		case ARMV7M_CONTROL:
			*value = buf_get_u32((uint8_t *) value, 24, 2);
			break;
		}

		LOG_DEBUG("load from special reg %i value 0x%" PRIx32 "",
			  (int)num, *value);
		break;

	default:
		return ERROR_INVALID_ARGUMENTS;
	}

	return ERROR_OK;
}

static int stm32_stlink_store_core_reg_u32(struct target *target,
					   enum armv7m_regtype type,
					   uint32_t num, uint32_t value)
{
	int retval;
	uint32_t reg;
	struct armv7m_common *armv7m = target_to_armv7m(target);
	struct stlink_interface_s *stlink_if = target_to_stlink(target);

	LOG_DEBUG("%s", __func__);

#ifdef ARMV7_GDB_HACKS
	/* If the LR register is being modified, make sure it will put us
	 * in "thumb" mode, or an INVSTATE exception will occur. This is a
	 * hack to deal with the fact that gdb will sometimes "forge"
	 * return addresses, and doesn't set the LSB correctly (i.e., when
	 * printing expressions containing function calls, it sets LR = 0.)
	 * Valid exception return codes have bit 0 set too.
	 */
	if (num == ARMV7M_R14)
		value |= 0x01;
#endif

	/* NOTE:  we "know" here that the register identifiers used
	 * in the v7m header match the Cortex-M3 Debug Core Register
	 * Selector values for R0..R15, xPSR, MSP, and PSP.
	 */
	switch (num) {
	case 0 ... 18:
		retval =
		    stlink_if->layout->api->write_reg(stlink_if->fd, num,
						      value);

		if (retval != ERROR_OK) {
			struct reg *r;

			LOG_ERROR("JTAG failure");
			r = armv7m->core_cache->reg_list + num;
			r->dirty = r->valid;
			return ERROR_JTAG_DEVICE_ERROR;
		}
		LOG_DEBUG("write core reg %i value 0x%" PRIx32 "", (int)num,
			  value);
		break;

	case ARMV7M_PRIMASK:
	case ARMV7M_BASEPRI:
	case ARMV7M_FAULTMASK:
	case ARMV7M_CONTROL:
		/* Cortex-M3 packages these four registers as bitfields
		 * in one Debug Core register.  So say r0 and r2 docs;
		 * it was removed from r1 docs, but still works.
		 */
		/* cortexm3_dap_read_coreregister_u32(swjdp, &reg, 20); */

		switch (num) {
		case ARMV7M_PRIMASK:
			buf_set_u32((uint8_t *) &reg, 0, 1, value);
			break;

		case ARMV7M_BASEPRI:
			buf_set_u32((uint8_t *) &reg, 8, 8, value);
			break;

		case ARMV7M_FAULTMASK:
			buf_set_u32((uint8_t *) &reg, 16, 1, value);
			break;

		case ARMV7M_CONTROL:
			buf_set_u32((uint8_t *) &reg, 24, 2, value);
			break;
		}

		/* cortexm3_dap_write_coreregister_u32(swjdp, reg, 20); */

		LOG_DEBUG("write special reg %i value 0x%" PRIx32 " ", (int)num,
			  value);
		break;

	default:
		return ERROR_INVALID_ARGUMENTS;
	}

	return ERROR_OK;
}

static int stm32_stlink_init_arch_info(struct target *target,
				       struct stm32_stlink_common *stm32stlink,
				       struct jtag_tap *tap)
{
	struct armv7m_common *armv7m;

	LOG_DEBUG("%s", __func__);

	armv7m = &stm32stlink->armv7m;
	armv7m_init_arch_info(target, armv7m);

	armv7m->load_core_reg_u32 = stm32_stlink_load_core_reg_u32;
	armv7m->store_core_reg_u32 = stm32_stlink_store_core_reg_u32;

	return ERROR_OK;
}

static int stm32_stlink_init_target(struct command_context *cmd_ctx,
				    struct target *target)
{
	LOG_DEBUG("%s", __func__);

	armv7m_build_reg_cache(target);

	return ERROR_OK;
}

static int stm32_stlink_target_create(struct target *target,
				      Jim_Interp *interp)
{
	LOG_DEBUG("%s", __func__);

	stm32_stlink = calloc(1, sizeof(struct stm32_stlink_common));

	if (!stm32_stlink)
		return ERROR_INVALID_ARGUMENTS;

	target->arch_info = &stm32_stlink->armv7m;

	stm32_stlink_init_arch_info(target, stm32_stlink, target->tap);

	return ERROR_OK;
}

static int stm32_stlink_examine(struct target *target)
{
	LOG_DEBUG("%s", __func__);

	if (target->tap->hasidcode == false) {
		LOG_ERROR("no IDCODE present on device");

		return ERROR_INVALID_ARGUMENTS;
	}

	if (!target_was_examined(target)) {
		target_set_examined(target);

		LOG_INFO("IDCODE %x", target->tap->idcode);
	}

	return ERROR_OK;
}

static int stm32_stlink_load_context(struct target *target)
{
	struct armv7m_common *armv7m = &stm32_stlink->armv7m;

	for (unsigned i = 0; i < 23; i++) {
		if (!armv7m->core_cache->reg_list[i].valid)
			armv7m->read_core_reg(target, i);
	}

	return ERROR_OK;
}

static int stm32_stlink_poll(struct target *target)
{
	enum target_state state;
	struct stlink_interface_s *stlink_if = target_to_stlink(target);

	state = stlink_if->layout->api->state(stlink_if->fd);

	if (state == TARGET_UNKNOWN) {
		LOG_ERROR
		    ("jtag status contains invalid mode value - communication failure");
		return ERROR_TARGET_FAILURE;
	}

	if (target->state == state)
		return ERROR_OK;

	if (state == TARGET_HALTED) {
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_HALTED);
		target->state = state;

		stm32_stlink_load_context(target);
	}

	return ERROR_OK;
}

static int stm32_stlink_arch_state(struct target *target)
{
	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static int stm32_stlink_assert_reset(struct target *target)
{
	int res;
	struct stlink_interface_s *stlink_if = target_to_stlink(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);

	LOG_DEBUG("%s", __func__);

	res = stlink_if->layout->api->reset(stlink_if->fd);

	if (res != ERROR_OK)
		return res;

	/* virtual assert reset, we need it for the internal
	 * jtag state machine
	 */
	jtag_add_reset(1, 1);

	/* registers are now invalid */
	register_cache_invalidate(armv7m->core_cache);

	stm32_stlink_load_context(target);

	target->state = TARGET_HALTED;

	return ERROR_OK;
}

static int stm32_stlink_deassert_reset(struct target *target)
{
	int res;

	LOG_DEBUG("%s", __func__);

	/* virtual deassert reset, we need it for the internal
	 * jtag state machine
	 */
	jtag_add_reset(0, 0);

	if (!target->reset_halt) {
		res = target_resume(target, 1, 0, 0, 0);

		if (res != ERROR_OK)
			return res;
	}

	return ERROR_OK;
}

static int stm32_stlink_soft_reset_halt(struct target *target)
{
	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static int stm32_stlink_halt(struct target *target)
{
	int res;
	struct stlink_interface_s *stlink_if = target_to_stlink(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);

	LOG_DEBUG("%s", __func__);

	if (target->state == TARGET_HALTED) {
		LOG_DEBUG("target was already halted");
		return ERROR_OK;
	}

	if (target->state == TARGET_UNKNOWN) {
		LOG_WARNING
		    ("target was in unknown state when halt was requested");
	}

	res = stlink_if->layout->api->halt(stlink_if->fd);

	if (res != ERROR_OK)
		return res;

	target->debug_reason = DBG_REASON_DBGRQ;

	stm32_stlink_load_context(target);

	LOG_INFO("halted: PC: 0x%x", buf_get_u32(armv7m->arm.pc->value, 0, 32));

	return ERROR_OK;
}

static int stm32_stlink_resume(struct target *target, int current,
			       uint32_t address, int handle_breakpoints,
			       int debug_execution)
{
	int res;
	struct stlink_interface_s *stlink_if = target_to_stlink(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	struct reg *r;

	LOG_DEBUG("%s %d %x %d %d", __func__, current, address,
		  handle_breakpoints, debug_execution);

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	r = armv7m->arm.pc;
	if (!current) {
		buf_set_u32(r->value, 0, 32, address);
		r->dirty = true;
		r->valid = true;
	}

	armv7m_restore_context(target);

	/* registers are now invalid */
	register_cache_invalidate(armv7m->core_cache);

	res = stlink_if->layout->api->run(stlink_if->fd);

	if (res != ERROR_OK)
		return res;

	target->state = TARGET_RUNNING;

	target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);

	return ERROR_OK;
}

static int stm32_stlink_step(struct target *target, int current,
			     uint32_t address, int handle_breakpoints)
{
	int res;
	struct stlink_interface_s *stlink_if = target_to_stlink(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);

	LOG_DEBUG("%s", __func__);

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	armv7m_restore_context(target);

	target_call_event_callbacks(target, TARGET_EVENT_RESUMED);

	res = stlink_if->layout->api->step(stlink_if->fd);

	if (res != ERROR_OK)
		return res;

	/* registers are now invalid */
	register_cache_invalidate(armv7m->core_cache);

	target->debug_reason = DBG_REASON_SINGLESTEP;
	target_call_event_callbacks(target, TARGET_EVENT_HALTED);

	stm32_stlink_load_context(target);

	LOG_INFO("halted: PC: 0x%x", buf_get_u32(armv7m->arm.pc->value, 0, 32));

	return ERROR_OK;
}

static int stm32_stlink_read_memory(struct target *target, uint32_t address,
				    uint32_t size, uint32_t count,
				    uint8_t *buffer)
{
	int res;
	uint32_t *dst = (uint32_t *) buffer;
	uint32_t c;
	struct stlink_interface_s *stlink_if = target_to_stlink(target);

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!count || !buffer)
		return ERROR_INVALID_ARGUMENTS;
	if (size != 4) {
		LOG_DEBUG("%s %x %d %d", __func__, address, size, count);
		return ERROR_INVALID_ARGUMENTS;
	}

	while (count) {
		if (count > 128)
			c = 128;
		else
			c = count;

		res =
		    stlink_if->layout->api->read_mem32(stlink_if->fd, address,
						       c, dst);

		if (res != ERROR_OK)
			return res;
		dst += c;
		address += (c * 4);
		count -= c;
	}

	return ERROR_OK;
}

static int stm32_stlink_write_memory(struct target *target, uint32_t address,
				     uint32_t size, uint32_t count,
				     const uint8_t *buffer)
{
	int res;
	uint32_t *dst = (uint32_t *) buffer;
	uint32_t c;
	struct stlink_interface_s *stlink_if = target_to_stlink(target);

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!count || !buffer)
		return ERROR_INVALID_ARGUMENTS;
	if (size != 4) {
		LOG_DEBUG("%s %x %d %d", __func__, address, size, count);
		return ERROR_INVALID_ARGUMENTS;
	}

	while (count) {
		if (count > 128)
			c = 128;
		else
			c = count;

		res =
		    stlink_if->layout->api->write_mem32(stlink_if->fd, address,
							c, dst);

		if (res != ERROR_OK)
			return res;
		dst += c;
		address += (c * 4);
		count -= c;
	}

	return ERROR_OK;
}

static int stm32_stlink_bulk_write_memory(struct target *target,
					  uint32_t address, uint32_t count,
					  const uint8_t *buffer)
{
	return stm32_stlink_write_memory(target, address, 4, count, buffer);
}

struct target_type stm32_stlink_target = {
	.name = "stm32_stlink",

	.init_target = stm32_stlink_init_target,
	.target_create = stm32_stlink_target_create,
	.examine = stm32_stlink_examine,

	.poll = stm32_stlink_poll,
	.arch_state = stm32_stlink_arch_state,

	.assert_reset = stm32_stlink_assert_reset,
	.deassert_reset = stm32_stlink_deassert_reset,
	.soft_reset_halt = stm32_stlink_soft_reset_halt,

	.halt = stm32_stlink_halt,
	.resume = stm32_stlink_resume,
	.step = stm32_stlink_step,

	.get_gdb_reg_list = NULL,

	.read_memory = stm32_stlink_read_memory,
	.write_memory = stm32_stlink_write_memory,
	.bulk_write_memory = stm32_stlink_bulk_write_memory,

	.run_algorithm = armv7m_run_algorithm,
	.start_algorithm = armv7m_start_algorithm,
	.wait_algorithm = armv7m_wait_algorithm,

	.add_breakpoint = NULL,
	.remove_breakpoint = NULL,
	.add_watchpoint = NULL,
	.remove_watchpoint = NULL,
};
