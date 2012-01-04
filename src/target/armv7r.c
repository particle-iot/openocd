/***************************************************************************
 *    Copyright (C) 2009 by David Brownell                                 *
 *                                                                         *
 *    Copyright (C) ST-Ericsson SA 2011 michel.jaouen@stericsson.com       *
 *                                                                         *
 *    Copyright (C) 2011 by Google Inc.                                    *
 *    aschultz@google.com - Copy from armv7a                               *
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

#include <helper/replacements.h>

#include "armv7r.h"
#include "arm_disassembler.h"

#include "register.h"
#include <helper/binarybuffer.h>
#include <helper/command.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arm_opcodes.h"
#include "target.h"
#include "target_type.h"

static void armv7r_show_fault_registers(struct target *target)
{
	uint32_t dfsr, ifsr, dfar, ifar;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm_dpm *dpm = armv7r->armv4_5_common.dpm;
	int retval;

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		return;

	/* ARMV4_5_MRC(cpnum, op1, r0, CRn, CRm, op2) */

	/* c5/c0 - {data, instruction} fault status registers */
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 5, 0, 0),
			&dfsr);
	if (retval != ERROR_OK)
		goto done;

	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 5, 0, 1),
			&ifsr);
	if (retval != ERROR_OK)
		goto done;

	/* c6/c0 - {data, instruction} fault address registers */
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 6, 0, 0),
			&dfar);
	if (retval != ERROR_OK)
		goto done;

	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 6, 0, 2),
			&ifar);
	if (retval != ERROR_OK)
		goto done;

	LOG_USER("Data fault registers        DFSR: %8.8" PRIx32
			", DFAR: %8.8" PRIx32, dfsr, dfar);
	LOG_USER("Instruction fault registers IFSR: %8.8" PRIx32
			", IFAR: %8.8" PRIx32, ifsr, ifar);

done:
	/* (void) */ dpm->finish(dpm);
}

static int armv7r_handle_inner_cache_info_command(struct command_context *cmd_ctx,
		struct armv7a_cache_common *armv7r_cache)
{
	if (armv7r_cache->ctype == -1) {
		command_print(cmd_ctx, "cache not yet identified");
		return ERROR_OK;
	}

	command_print(cmd_ctx,
			"D-Cache: linelen %i, associativity %i, nsets %i, cachesize %d KBytes",
			armv7r_cache->d_u_size.linelen,
			armv7r_cache->d_u_size.associativity,
			armv7r_cache->d_u_size.nsets,
			armv7r_cache->d_u_size.cachesize);

	command_print(cmd_ctx,
			"I-Cache: linelen %i, associativity %i, nsets %i, cachesize %d KBytes",
			armv7r_cache->i_size.linelen,
			armv7r_cache->i_size.associativity,
			armv7r_cache->i_size.nsets,
			armv7r_cache->i_size.cachesize);

	return ERROR_OK;
}

static int _armv7r_flush_all_data(struct target *target)
{
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm_dpm *dpm = armv7r->armv4_5_common.dpm;
	struct armv7a_cachesize *d_u_size =
		&(armv7r->armv7r_cache.d_u_size);
	int32_t c_way, c_index = d_u_size->index;
	int retval;
	/*  check that cache data is on at target halt */
	if (!armv7r->armv7r_cache.d_u_cache_enabled) {
		LOG_INFO("flushed not performed :cache not on at target halt");
		return ERROR_OK;
	}
	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto done;
	do {
		c_way = d_u_size->way;
		do {
			uint32_t value = (c_index << d_u_size->index_shift)
				| (c_way << d_u_size->way_shift);
			/*  DCCISW */
			/* LOG_INFO ("%d %d %x",c_way,c_index,value); */
			retval = dpm->instr_write_data_r0(dpm,
					ARMV4_5_MCR(15, 0, 0, 7, 14, 2),
					value);
			if (retval != ERROR_OK)
				goto done;
			c_way -= 1;
		} while (c_way >= 0);
		c_index -= 1;
	} while (c_index >= 0);
	return retval;
done:
	LOG_ERROR("flushed failed");
	dpm->finish(dpm);
	return retval;
}

static int  armv7r_flush_all_data(struct target *target)
{
	int retval = ERROR_FAIL;
	/*  check that armv7r_cache is correctly identify */
	struct armv7r_common *armv7r = target_to_armv7r(target);
	if (armv7r->armv7r_cache.ctype == -1) {
		LOG_ERROR("trying to flush un-identified cache");
		return retval;
	}

	if (target->smp) {
		/*  look if all the other target have been flushed in order to flush level
		 *  2 */
		struct target_list *head;
		struct target *curr;
		head = target->head;
		while (head != (struct target_list *)NULL) {
			curr = head->target;
			if ((curr->state == TARGET_HALTED)) {
				LOG_INFO("Wait flushing data l1 on core %d", curr->coreid);
				retval = _armv7r_flush_all_data(curr);
			}
			head = head->next;
		}
	} else {
		retval = _armv7r_flush_all_data(target);
	}
	return retval;
}


/* L2 is not specific to armv7r a specific file is needed */
static int armv7r_l2x_flush_all_data(struct target *target)
{

#define L2X0_CLEAN_INV_WAY		0x7FC
	int retval = ERROR_FAIL;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct armv7a_l2x_cache *l2x_cache = (struct armv7a_l2x_cache *)
		(armv7r->armv7r_cache.l2_cache);
	uint32_t base = l2x_cache->base;
	uint32_t l2_way = l2x_cache->way;
	uint32_t l2_way_val = (1 << l2_way) - 1;
	retval = armv7r_flush_all_data(target);
	if (retval != ERROR_OK)
		return retval;
	retval = target->type->write_phys_memory(target,
			(uint32_t)(base + (uint32_t)L2X0_CLEAN_INV_WAY),
			(uint32_t)4,
			(uint32_t)1,
			(uint8_t *)&l2_way_val);
	return retval;
}

static int armv7r_handle_l2x_cache_info_command(struct command_context *cmd_ctx,
		struct armv7a_cache_common *armv7r_cache)
{

	struct armv7a_l2x_cache *l2x_cache = (struct armv7a_l2x_cache *)
			(armv7r_cache->l2_cache);

	if (armv7r_cache->ctype == -1) {
		command_print(cmd_ctx, "cache not yet identified");
		return ERROR_OK;
	}

	command_print(cmd_ctx,
			"L1 D-Cache: linelen %i, associativity %i, nsets %i, cachesize %d KBytes",
		armv7r_cache->d_u_size.linelen,
		armv7r_cache->d_u_size.associativity,
		armv7r_cache->d_u_size.nsets,
		armv7r_cache->d_u_size.cachesize);

	command_print(cmd_ctx,
			"L1 I-Cache: linelen %i, associativity %i, nsets %i, cachesize %d KBytes",
		armv7r_cache->i_size.linelen,
		armv7r_cache->i_size.associativity,
		armv7r_cache->i_size.nsets,
		armv7r_cache->i_size.cachesize);
	command_print(cmd_ctx, "L2 unified cache Base Address 0x%x, %d ways",
			l2x_cache->base, l2x_cache->way);


	return ERROR_OK;
}


static int armv7r_l2x_cache_init(struct target *target, uint32_t base, uint32_t way)
{
	struct armv7a_l2x_cache *l2x_cache;
	struct target_list *head = target->head;
	struct target *curr;

	struct armv7r_common *armv7r = target_to_armv7r(target);
	l2x_cache = calloc(1, sizeof(struct armv7a_l2x_cache));
	l2x_cache->base = base;
	l2x_cache->way = way;
	/*LOG_INFO("cache l2 initialized base %x  way %d",
	l2x_cache->base,l2x_cache->way);*/
	if (armv7r->armv7r_cache.l2_cache)
		LOG_INFO("cache l2 already initialized\n");
	armv7r->armv7r_cache.l2_cache = (void *) l2x_cache;
	/*  initialize l1 / l2x cache function  */
	armv7r->armv7r_cache.flush_all_data_cache
		= armv7r_l2x_flush_all_data;
	armv7r->armv7r_cache.display_cache_info =
		armv7r_handle_l2x_cache_info_command;
	/*  initialize all target in this cluster (smp target)*/
	/*  l2 cache must be configured after smp declaration */
	while (head != (struct target_list *)NULL) {
		curr = head->target;
		if (curr != target) {
			armv7r = target_to_armv7r(curr);
			if (armv7r->armv7r_cache.l2_cache)
				LOG_ERROR("smp target : cache l2 already initialized\n");
			armv7r->armv7r_cache.l2_cache = (void *) l2x_cache;
			armv7r->armv7r_cache.flush_all_data_cache =
				armv7r_l2x_flush_all_data;
			armv7r->armv7r_cache.display_cache_info =
				armv7r_handle_l2x_cache_info_command;
		}
		head = head->next;
	}
	return JIM_OK;
}

COMMAND_HANDLER(handle_cache_l2x)
{
	struct target *target = get_current_target(CMD_CTX);
	uint32_t base, way;
	switch (CMD_ARGC) {
	case 0:
		return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	case 2:
		/* command_print(CMD_CTX, "%s %s", CMD_ARGV[0], CMD_ARGV[1]); */

		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], base);
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], way);

		/* AP address is in bits 31:24 of DP_SELECT */
		 armv7r_l2x_cache_init(target, base, way);
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}
	return ERROR_OK;
}


int armv7r_handle_cache_info_command(struct command_context *cmd_ctx,
		struct armv7a_cache_common *armv7r_cache)
{
	if (armv7r_cache->ctype == -1) {
		command_print(cmd_ctx, "cache not yet identified");
		return ERROR_OK;
	}

	if (armv7r_cache->display_cache_info)
		armv7r_cache->display_cache_info(cmd_ctx, armv7r_cache);
	return ERROR_OK;
}


/*  retrieve core id cluster id  */
static int armv7r_read_mpidr(struct target *target)
{
	int retval = ERROR_FAIL;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm_dpm *dpm = armv7r->armv4_5_common.dpm;
	uint32_t mpidr;
	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto done;
	/* MRC p15,0,<Rd>,c0,c0,5; read Multiprocessor ID register*/

	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 0, 0, 5),
			&mpidr);
	if (retval != ERROR_OK)
		goto done;
	if (mpidr & 1<<31) {
		armv7r->multi_processor_system = (mpidr >> 30) & 1;
		armv7r->cluster_id = (mpidr >> 8) & 0xf;
		armv7r->cpu_id = mpidr & 0x3;
		LOG_INFO("%s cluster %x core %x %s", target->cmd_name,
				armv7r->cluster_id,
				armv7r->cpu_id,
				armv7r->multi_processor_system == 0 ? "multi core" : "mono core");
	} else {
		LOG_ERROR("mpdir not in multiprocessor format");
	}

done:
	dpm->finish(dpm);
	return retval;
}


int armv7r_identify_cache(struct target *target)
{
	/*  read cache descriptor */
	int retval = ERROR_FAIL;
	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm_dpm *dpm = armv7r->armv4_5_common.dpm;
	uint32_t cache_selected, clidr;
	uint32_t cache_i_reg, cache_d_reg;
	struct armv7a_cache_common *cache = &(armv7r->armv7r_cache);
	retval = dpm->prepare(dpm);

	if (retval != ERROR_OK)
		goto done;
	/*  retrieve CLIDR */
	/*  mrc	p15, 1, r0, c0, c0, 1		@ read clidr */
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 1, 0, 0, 0, 1),
			&clidr);
	if (retval != ERROR_OK)
		goto done;
	clidr = (clidr & 0x7000000) >> 23;
	LOG_INFO("number of cache level %d", clidr / 2);
	if ((clidr / 2) > 1) {
		// FIXME not supported present in cortex A8 and later
		//  in cortex A7, A15
		LOG_ERROR("cache l2 present :not supported");
	}
	/*  retrieve selected cache  */
	/*  MRC p15, 2,<Rd>, c0, c0, 0; Read CSSELR */
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 2, 0, 0, 0, 0),
			&cache_selected);
	if (retval != ERROR_OK)
		goto done;

	retval = armv7r->armv4_5_common.mrc(target, 15,
			2, 0,	/* op1, op2 */
			0, 0,	/* CRn, CRm */
			&cache_selected);
	if (retval != ERROR_OK)
		goto done;
	/* select instruction cache*/
	/*  MCR p15, 2,<Rd>, c0, c0, 0; Write CSSELR */
	/*  [0]  : 1 instruction cache selection , 0 data cache selection */
	retval = dpm->instr_write_data_r0(dpm,
			ARMV4_5_MRC(15, 2, 0, 0, 0, 0),
			1);
	if (retval != ERROR_OK)
		goto done;

	/* read CCSIDR*/
	/* MRC P15,1,<RT>,C0, C0,0 ;on cortex A9 read CCSIDR */
	/* [2:0] line size  001 eight word per line */
	/* [27:13] NumSet 0x7f 16KB, 0xff 32Kbytes, 0x1ff 64Kbytes */
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 1, 0, 0, 0, 0),
			&cache_i_reg);
	if (retval != ERROR_OK)
		goto done;

	/*  select data cache*/
	retval = dpm->instr_write_data_r0(dpm,
			ARMV4_5_MRC(15, 2, 0, 0, 0, 0),
			0);
	if (retval != ERROR_OK)
		goto done;

	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 1, 0, 0, 0, 0),
			&cache_d_reg);
	if (retval != ERROR_OK)
		goto done;

	/*  restore selected cache  */
	dpm->instr_write_data_r0(dpm,
			ARMV4_5_MRC(15, 2, 0, 0, 0, 0),
			cache_selected);

	if (retval != ERROR_OK)
		goto done;
	dpm->finish(dpm);

	// put fake type
	cache->d_u_size.linelen = 16 << (cache_d_reg & 0x7);
	cache->d_u_size.cachesize = (((cache_d_reg >> 13) & 0x7fff) + 1) / 8;
	cache->d_u_size.nsets = (cache_d_reg >> 13) & 0x7fff;
	cache->d_u_size.associativity = ((cache_d_reg >> 3) & 0x3ff) + 1;
	/*  compute info for set way operation on cache */
	cache->d_u_size.index_shift = (cache_d_reg & 0x7) + 4;
	cache->d_u_size.index = (cache_d_reg >> 13) & 0x7fff;
	cache->d_u_size.way = ((cache_d_reg >> 3) & 0x3ff);
	cache->d_u_size.way_shift = cache->d_u_size.way + 1;
	{
		int i = 0;
		while (((cache->d_u_size.way_shift >> i) & 1) != 1)
			i++;
		cache->d_u_size.way_shift = 32 - i;
	}
	/*
	LOG_INFO("data cache index %d << %d, way %d << %d",
			cache->d_u_size.index, cache->d_u_size.index_shift,
	         cache->d_u_size.way, 	cache->d_u_size.way_shift);

	LOG_INFO("data cache %d bytes %d KBytes asso %d ways",
			cache->d_u_size.linelen,
			cache->d_u_size.cachesize,
			cache->d_u_size.associativity
			);
	*/
	cache->i_size.linelen = 16 << (cache_i_reg & 0x7);
	cache->i_size.associativity = ((cache_i_reg >> 3) & 0x3ff) + 1;
	cache->i_size.nsets = (cache_i_reg >> 13) & 0x7fff;
	cache->i_size.cachesize = (((cache_i_reg >> 13) & 0x7fff) + 1) / 8;
	/*  compute info for set way operation on cache */
	cache->i_size.index_shift = (cache_i_reg & 0x7) + 4;
	cache->i_size.index = (cache_i_reg >> 13) & 0x7fff;
	cache->i_size.way = ((cache_i_reg >> 3) & 0x3ff);
	cache->i_size.way_shift = cache->i_size.way + 1;
	{
		int i = 0;
		while (((cache->i_size.way_shift >> i) & 1) != 1)
			i++;
		cache->i_size.way_shift = 32 - i;
	}
	/*
	LOG_INFO("instruction cache index %d << %d, way %d << %d",
			cache->i_size.index, cache->i_size.index_shift,
	         cache->i_size.way, cache->i_size.way_shift);

	LOG_INFO("instruction cache %d bytes %d KBytes asso %d ways",
			cache->i_size.linelen,
			cache->i_size.cachesize,
			cache->i_size.associativity
			);
	*/

	/*  if no l2 cache initialize l1 data cache flush function function */
	if (armv7r->armv7r_cache.flush_all_data_cache == NULL) {
		armv7r->armv7r_cache.display_cache_info =
			armv7r_handle_inner_cache_info_command;
		armv7r->armv7r_cache.flush_all_data_cache =
			armv7r_flush_all_data;
	}
	armv7r->armv7r_cache.ctype = 0;

done:
	dpm->finish(dpm);
	armv7r_read_mpidr(target);
	return retval;

}



int armv7r_init_arch_info(struct target *target, struct armv7r_common *armv7r)
{
	struct arm *armv4_5 = &armv7r->armv4_5_common;
	armv4_5->arch_info = armv7r;
	target->arch_info = &armv7r->armv4_5_common;
	/*  target is useful in all function arm v4 5 compatible */
	armv7r->armv4_5_common.target = target;
	armv7r->armv4_5_common.common_magic =  ARM_COMMON_MAGIC;
	armv7r->common_magic = ARMV7_COMMON_MAGIC;
	armv7r->armv7r_cache.l2_cache = NULL;
	armv7r->armv7r_cache.ctype = -1;
	armv7r->armv7r_cache.flush_all_data_cache = NULL;
	armv7r->armv7r_cache.display_cache_info = NULL;
	return ERROR_OK;
}

int armv7r_arch_state(struct target *target)
{
	static const char *state[] = {
		"disabled", "enabled"
	};

	struct armv7r_common *armv7r = target_to_armv7r(target);
	struct arm *armv4_5 = &armv7r->armv4_5_common;

	if (armv7r->common_magic != ARMV7_COMMON_MAGIC) {
		LOG_ERROR("BUG: called for a non-ARMv7R target");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	arm_arch_state(target);

	/* TODO: print state of PMU */
	LOG_USER("D-Cache: %s, I-Cache: %s",
		 state[armv7r->armv7r_cache.d_u_cache_enabled],
		 state[armv7r->armv7r_cache.i_cache_enabled]);

	if (armv4_5->core_mode == ARM_MODE_ABT)
		armv7r_show_fault_registers(target);
	if (target->debug_reason == DBG_REASON_WATCHPOINT)
		LOG_USER("Watchpoint triggered at PC %#08x",
				(unsigned) armv7r->dpm.wp_pc);

	return ERROR_OK;
}

static const struct command_registration l2_cache_commands[] = {
	{
		.name = "l2x",
		.handler = handle_cache_l2x,
		.mode = COMMAND_EXEC,
		.help = "configure l2x cache "
			"",
		.usage = "[base_addr] [number_of_way]",
	},
	COMMAND_REGISTRATION_DONE
};

const struct command_registration l2x_cache_command_handlers_r[] = {
	{
		.name = "cache_config",
		.mode = COMMAND_EXEC,
		.help = "cache configuation for a target",
		.chain = l2_cache_commands,
	},
	COMMAND_REGISTRATION_DONE
};


const struct command_registration armv7r_command_handlers[] = {
	{
		.chain = dap_command_handlers,
	},
	{
		.chain = l2x_cache_command_handlers_r,
	},
	COMMAND_REGISTRATION_DONE
};
