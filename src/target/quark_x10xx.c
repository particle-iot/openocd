/*
 * Copyright(c) 2013 Intel Corporation.
 *
 * Adrian Burns (adrian.burns@intel.com)
 * Thomas Faust (thomas.faust@intel.com)
 * Ivan De Cesaris (ivan.de.cesaris@intel.com)
 * Julien Carreno (julien.carreno@intel.com)
 * Jeffrey Maxwell (jeffrey.r.maxwell@intel.com)
 * Jessica Gomez (jessica.gomez.hernandez@intel.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Contact Information:
 * Intel Corporation
 */

/*
 * @file
 * Debugger for Intel Quark SoC X1000
 * Intel Quark X10xx is the first product in the Quark family of SoCs.
 * It is an IA-32 (Pentium x86 ISA) compatible SoC. The core CPU in the
 * X10xx is codenamed Lakemont. Lakemont version 1 (LMT1) is used in X10xx.
 * The CPU TAP (Lakemont TAP) is used for software debug and the CLTAP is
 * used for SoC level operations.
 * Useful docs are here: https://communities.intel.com/community/makers/documentation
 * Intel Quark SoC X1000 OpenOCD/GDB/Eclipse App Note (web search for doc num 330015)
 * Intel Quark SoC X1000 Debug Operations User Guide (web search for doc num 329866)
 * Intel Quark SoC X1000 Datasheet (web search for doc num 329676)
 *
 * This file implements any Quark SoC specific features, such as resetbreak.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/log.h>

#include "target.h"
#include "target_type.h"
#include "lakemont.h"
#include "x86_32_common.h"

int quark_x10xx_target_create(struct target *t, Jim_Interp *interp)
{
	struct x86_32_common *x86_32 = calloc(1, sizeof(struct x86_32_common));
	if (x86_32 == NULL) {
		LOG_ERROR("%s out of memory", __func__);
		return ERROR_FAIL;
	}
	x86_32_common_init_arch_info(t, x86_32);
	lakemont_init_arch_info(t, x86_32);
	return ERROR_OK;
}

int quark_x10xx_init_target(struct command_context *cmd_ctx, struct target *t)
{
	return lakemont_init_target(cmd_ctx, t);
}

/*
 * issue a system reset using the 0xcf9 I/O port and then break (this is the
 * closest we can get to a proper reset break without a connected srst pin).
*/
static int quark_x10xx_target_reset(struct target *t)
{
	LOG_DEBUG("issuing port 0xcf9 reset");
	struct x86_32_common *x86_32 = target_to_x86_32(t);
	int retval = ERROR_OK;

	/* we can't be running when issuing an I/O port write */
	if (t->state == TARGET_RUNNING) {
		retval = lakemont_halt(t);
		if (retval != ERROR_OK) {
			LOG_ERROR("%s could not halt target", __func__);
			return retval;
		}
	}

	/* save current tap so we can restore it later */
	struct jtag_tap *saved_tap = x86_32->curr_tap;

	/* prepare resetbreak setting the proper bits in CLTAPC_CPU_VPREQ */
	x86_32->curr_tap = jtag_tap_by_string("quark_x10xx.cltap");
	if (x86_32->curr_tap == NULL) {
		x86_32->curr_tap = saved_tap;
		LOG_ERROR("%s could not select quark_x10xx.cltap", __func__);
		return ERROR_FAIL;
	}

	static struct scan_blk scan;
	struct scan_field *fields = &scan.field;
	fields->in_value  = NULL;
	fields->num_bits  = 8;

	/* select CLTAPC_CPU_VPREQ instruction*/
	scan.out[0] = 0x51;
	fields->out_value = ((uint8_t *)scan.out);
	jtag_add_ir_scan(x86_32->curr_tap, fields, TAP_IDLE);
	retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		x86_32->curr_tap = saved_tap;
		LOG_ERROR("%s irscan failed to execute queue", __func__);
		return retval;
	}

	/* set enable_preq_on_reset & enable_preq_on_reset2 bits*/
	scan.out[0] = 0x06;
	fields->out_value  = ((uint8_t *)scan.out);
	jtag_add_dr_scan(x86_32->curr_tap, 1, fields, TAP_IDLE);
	retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("%s drscan failed to execute queue", __func__);
		x86_32->curr_tap = saved_tap;
		return retval;
	}

	/* restore current tap */
	x86_32->curr_tap = saved_tap;

	/* write 0x6 to I/O port 0xcf9 to cause the reset */
	const uint8_t cf9_reset_val[] = { 0x6 };

	retval = x86_32_common_write_io(t, (uint32_t)0xcf9, BYTE, cf9_reset_val);
	if (retval != ERROR_OK) {
		LOG_ERROR("%s could not write to port 0xcf9", __func__);
		return retval;
	}

	/* entered PM after reset, update the state */
	retval = lakemont_update_after_probemode_entry(t);
	if (retval != ERROR_OK) {
		LOG_ERROR("%s could not update state after probemode entry", __func__);
		return retval;
	}

	/* remove breakpoints and watchpoints */
	x86_32_common_reset_breakpoints_watchpoints(t);

	return ERROR_OK;

}

struct target_type quark_x10xx_target = {
	.name = "quark_x10xx",
	/* Quark X1000 SoC */
	.target_create    = quark_x10xx_target_create,
	.soft_reset_halt  = quark_x10xx_target_reset,
	.init_target      = quark_x10xx_init_target,
	/* lakemont probemode specific code */
	.poll = lakemont_poll,
	.arch_state = lakemont_arch_state,
	.halt = lakemont_halt,
	.resume = lakemont_resume,
	.step = lakemont_step,
	.assert_reset = lakemont_reset_assert,
	.deassert_reset = lakemont_reset_deassert,
	/* common x86 code */
	.commands = x86_32_command_handlers,
	.get_gdb_reg_list = x86_32_get_gdb_reg_list,
	.read_memory = x86_32_common_read_memory,
	.write_memory = x86_32_common_write_memory,
	.add_breakpoint = x86_32_common_add_breakpoint,
	.remove_breakpoint = x86_32_common_remove_breakpoint,
	.add_watchpoint = x86_32_common_add_watchpoint,
	.remove_watchpoint = x86_32_common_remove_watchpoint,
	.virt2phys = x86_32_common_virt2phys,
	.read_phys_memory = x86_32_common_read_phys_mem,
	.write_phys_memory = x86_32_common_write_phys_mem,
	.mmu = x86_32_common_mmu,
};
