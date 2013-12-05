/*
 * Copyright(c) 2013 Intel Corporation. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/log.h>

#include "target.h"
#include "target_type.h"
#include "lakemont.h"
#include "x86_32_common.h"

/* The Intel Quark SoC X1000 Core is codenamed lakemont.
 * Lakemont version 1 (LMT1) was used in Intel Quark SoC X1000.
 * This file implements any SoC specific features such as resetbreak (TODO)
 * Lakemont probemode is done in lakemont.c
 * Generic x86 32 bit memory and breakpoint operations are in x86_32_common.c
 *
 * See Quark Debug Operations User Guide (on the web) Doc number 329866
 */
int quark_x10xx_target_create(struct target *t, Jim_Interp *interp)
{
	LOG_DEBUG("%s", __func__);
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
	LOG_DEBUG("%s", __func__);
	return lakemont_init_target(cmd_ctx, t);
}

struct target_type quark_x10xx_target = {
	.name = "quark_x10xx",
	/* Quark X1000 SoC */
	.target_create = quark_x10xx_target_create,
	.init_target = quark_x10xx_init_target,
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
