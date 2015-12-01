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
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/replacements.h>

#include "armv8.h"
#include "arm_disassembler.h"

#include "register.h"
#include <helper/binarybuffer.h>
#include <helper/command.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arm_opcodes.h"
#include "armv8_opcodes.h"
#include "target.h"
#include "target_type.h"


#define	_DEBUG_GDB_FUNC_ENTRY_	/* "<<<") entering; ">>>") leaving */


static const char * const armv8_state_strings[] = {
	"ARM", "Thumb", "Jazelle", "ThumbEE", "ARM64",
};

/* D7.2.15 CLIDR_EL1.Ctype<n>: 3-bit (0..7) for each cache level */
static const char * const armv8_cache_type[] = {
	"No cache",
	"Instruction cache only",
	"Data cache only",
	"Separate instruction and data caches",
	"Unified cache",
	"(reserved)",
	"(reserved)",
	"(reserved)"
};

/* D1.6.4 Saved program Status Registers (SPSRs), M[4:0] */
#if 0
static const struct {
	enum arm_mode value;
	const char *name;
} armv8_core_mode[] = {
#else
static const Jim_Nvp nvp_armv8_core_mode[] = {
#endif
	{ .value = ARMV8_MODE_EL0T,		.name = "EL0t" },
	{ .value = ARMV8_MODE_EL1T,		.name = "EL1t" },
	{ .value = ARMV8_MODE_EL1H,		.name = "EL1h" },
	{ .value = ARMV8_MODE_EL2T,		.name = "EL2t" },
	{ .value = ARMV8_MODE_EL2H,		.name = "EL2h" },
	{ .value = ARMV8_MODE_EL3T,		.name = "EL3t" },
	{ .value = ARMV8_MODE_EL3H,		.name = "EL3h" },
	{ .value = -1,					.name = NULL},
};


#if 0
static const struct {
	const char *name;
	unsigned psr;
	/* For user and system modes, these list indices for all registers.
	 * otherwise they're just indices for the shadow registers and SPSR.
	 */
	unsigned short n_indices;
	const uint8_t *indices;
} armv8_mode_data[] = {
	/* These special modes are currently only supported
	 * by ARMv6M and ARMv7M profiles */
	{
		.name = "EL0T",
		.psr = ARMV8_MODE_EL0T,
	},
	{
		.name = "EL1T",
		.psr = ARMV8_MODE_EL1T,
	},
	{
		.name = "EL1H",
		.psr = ARMV8_MODE_EL1H,
	},
	{
		.name = "EL2T",
		.psr = ARMV8_MODE_EL2T,
	},
	{
		.name = "EL2H",
		.psr = ARMV8_MODE_EL2H,
	},
	{
		.name = "EL3T",
		.psr = ARMV8_MODE_EL3T,
	},
	{
		.name = "EL3H",
		.psr = ARMV8_MODE_EL3H,
	},
};
#endif


static int armv8_full_context(struct target *target)
{
	LOG_ERROR("Implement this function");

	return ERROR_OK;
}

static int armv8_read_core_reg(struct target *target, struct reg *r,
	int num, enum arm_mode mode)
{
	uint64_t reg_value;
	int retval;
	struct arm_reg *armv8_core_reg;
	struct armv8_common *armv8 = target_to_armv8(target);

//	assert(false);		/* Debugging: How this function is called ? */
LOG_ERROR("load_core_reg_u64 = %p", armv8->load_core_reg_u64);
	assert(num < (int)armv8->arm.core_cache->num_regs);

	armv8_core_reg = armv8->arm.core_cache->reg_list[num].arch_info;
	retval = armv8->load_core_reg_u64(target,
			armv8_core_reg->num, &reg_value);
	if (retval != ERROR_OK)
		return retval;

	buf_set_u64(armv8->arm.core_cache->reg_list[num].value, 0, 64, reg_value);
	armv8->arm.core_cache->reg_list[num].valid = 1;
	armv8->arm.core_cache->reg_list[num].dirty = 0;

	return retval;
}

static int armv8_write_core_reg(struct target *target, struct reg *r,
	int num, enum arm_mode mode, uint8_t *value)
{
	uint64_t v64;
	int retval;
	struct arm_reg *armv8_core_reg;
	struct armv8_common *armv8 = target_to_armv8(target);

LOG_ERROR("store_core_reg_u64 = %p", armv8->store_core_reg_u64);

	assert(num < (int)armv8->arm.core_cache->num_regs);

	armv8_core_reg = armv8->arm.core_cache->reg_list[num].arch_info;
	v64 = buf_get_u64(value, 0, 64);
	retval = armv8->store_core_reg_u64(target,
					    armv8_core_reg->num,
					    v64);
	if (retval != ERROR_OK) {
		LOG_ERROR("JTAG failure");
		armv8->arm.core_cache->reg_list[num].dirty = armv8->arm.core_cache->reg_list[num].valid;
		return ERROR_JTAG_DEVICE_ERROR;
	}

	LOG_DEBUG("write core reg %i value 0x%" PRIx64 "", num, v64);
	armv8->arm.core_cache->reg_list[num].valid = 1;
	armv8->arm.core_cache->reg_list[num].dirty = 0;

	return ERROR_OK;
}

static void armv8_show_fault_registers(struct target *target)
{
	/* TODO */
}

static int armv8_read_ttbcr(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm_dpm *dpm = armv8->arm.dpm;
	uint32_t ttbcr;

	int retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto done;
	/*  MRC p15,0,<Rt>,c2,c0,2 ; Read CP15 Translation Table Base Control Register*/
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 2, 0, 2),
			&ttbcr);
	if (retval != ERROR_OK)
		goto done;
	armv8->armv8_mmu.ttbr1_used = ((ttbcr & 0x7) != 0) ? 1 : 0;
	armv8->armv8_mmu.ttbr0_mask  = 7 << (32 - ((ttbcr & 0x7)));
#if 0
	LOG_INFO("ttb1 %s ,ttb0_mask %x",
		armv8->armv8_mmu.ttbr1_used ? "used" : "not used",
		armv8->armv8_mmu.ttbr0_mask);
#endif
	if (armv8->armv8_mmu.ttbr1_used == 1) {
		LOG_INFO("SVC access above %" PRIx32,
			 (uint32_t)(0xffffffff & armv8->armv8_mmu.ttbr0_mask));
		armv8->armv8_mmu.os_border = 0xffffffff & armv8->armv8_mmu.ttbr0_mask;
	} else {
		/*  fix me , default is hard coded LINUX border  */
		armv8->armv8_mmu.os_border = 0xc0000000;
	}
done:
	dpm->finish(dpm);
	return retval;
}


/*  method adapted to cortex A : reused arm v4 v5 method*/
int armv8_mmu_translate_va(struct target *target,  uint32_t va, uint32_t *val)
{
	uint32_t first_lvl_descriptor = 0x0;
	uint32_t second_lvl_descriptor = 0x0;
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm_dpm *dpm = armv8->arm.dpm;
	uint32_t ttb = 0;	/*  default ttb0 */

	if (armv8->armv8_mmu.ttbr1_used == -1)
		armv8_read_ttbcr(target);
	if ((armv8->armv8_mmu.ttbr1_used) &&
		(va > (0xffffffff & armv8->armv8_mmu.ttbr0_mask))) {
		/*  select ttb 1 */
		ttb = 1;
	}
	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto done;

	/*  MRC p15,0,<Rt>,c2,c0,ttb */
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 2, 0, ttb),
			&ttb);
	if (retval != ERROR_OK)
		return retval;
	retval = armv8->armv8_mmu.read_physical_memory(target,
			(ttb & 0xffffc000) | ((va & 0xfff00000) >> 18),
			4, 1, (uint8_t *)&first_lvl_descriptor);
	if (retval != ERROR_OK)
		return retval;
	first_lvl_descriptor = target_buffer_get_u32(target, (uint8_t *)
			&first_lvl_descriptor);
	/*  reuse armv4_5 piece of code, specific armv8 changes may come later */
	LOG_DEBUG("1st lvl desc: %8.8" PRIx32 "", first_lvl_descriptor);

	if ((first_lvl_descriptor & 0x3) == 0) {
		LOG_ERROR("Address translation failure");
		return ERROR_TARGET_TRANSLATION_FAULT;
	}


	if ((first_lvl_descriptor & 0x3) == 2) {
		/* section descriptor */
		*val = (first_lvl_descriptor & 0xfff00000) | (va & 0x000fffff);
		return ERROR_OK;
	}

	if ((first_lvl_descriptor & 0x3) == 1) {
		/* coarse page table */
		retval = armv8->armv8_mmu.read_physical_memory(target,
				(first_lvl_descriptor & 0xfffffc00) | ((va & 0x000ff000) >> 10),
				4, 1, (uint8_t *)&second_lvl_descriptor);
		if (retval != ERROR_OK)
			return retval;
	} else if ((first_lvl_descriptor & 0x3) == 3)   {
		/* fine page table */
		retval = armv8->armv8_mmu.read_physical_memory(target,
				(first_lvl_descriptor & 0xfffff000) | ((va & 0x000ffc00) >> 8),
				4, 1, (uint8_t *)&second_lvl_descriptor);
		if (retval != ERROR_OK)
			return retval;
	}

	second_lvl_descriptor = target_buffer_get_u32(target, (uint8_t *)
			&second_lvl_descriptor);

	LOG_DEBUG("2nd lvl desc: %8.8" PRIx32 "", second_lvl_descriptor);

	if ((second_lvl_descriptor & 0x3) == 0) {
		LOG_ERROR("Address translation failure");
		return ERROR_TARGET_TRANSLATION_FAULT;
	}

	if ((second_lvl_descriptor & 0x3) == 1) {
		/* large page descriptor */
		*val = (second_lvl_descriptor & 0xffff0000) | (va & 0x0000ffff);
		return ERROR_OK;
	}

	if ((second_lvl_descriptor & 0x3) == 2) {
		/* small page descriptor */
		*val = (second_lvl_descriptor & 0xfffff000) | (va & 0x00000fff);
		return ERROR_OK;
	}

	if ((second_lvl_descriptor & 0x3) == 3) {
		*val = (second_lvl_descriptor & 0xfffffc00) | (va & 0x000003ff);
		return ERROR_OK;
	}

	/* should not happen */
	LOG_ERROR("Address translation failure");
	return ERROR_TARGET_TRANSLATION_FAULT;

done:
	return retval;
}

/*  V8 method VA TO PA  */
int armv8_mmu_translate_va_pa(struct target *target, uint64_t va,
	uint64_t *val, int meminfo)
{
	return ERROR_OK;
}

#if 0	/* Alamy: We might not need this later */
static int armv8_handle_inner_cache_info_command(struct command_context *cmd_ctx,
	struct armv8_cache_common *armv8_cache)
{
	if (armv8_cache->ctype == -1) {
		command_print(cmd_ctx, "cache not yet identified");
		return ERROR_OK;
	}

	command_print(cmd_ctx,
		"D-Cache: linelen %" PRIi32 ", associativity %" PRIi32 ", nsets %" PRIi32 ", cachesize %" PRId32 " KBytes",
		armv8_cache->d_u_size.linelen,
		armv8_cache->d_u_size.associativity,
		armv8_cache->d_u_size.nsets,
		armv8_cache->d_u_size.cachesize);

	command_print(cmd_ctx,
		"I-Cache: linelen %" PRIi32 ", associativity %" PRIi32 ", nsets %" PRIi32 ", cachesize %" PRId32 " KBytes",
		armv8_cache->i_size.linelen,
		armv8_cache->i_size.associativity,
		armv8_cache->i_size.nsets,
		armv8_cache->i_size.cachesize);

	return ERROR_OK;
}
#endif

#include <helper/time_support.h>
extern int aarch64_exec_opcode(struct target *target,
	uint32_t opcode, uint32_t *edscr_p);

/* Refer to <kernel>/arch/arm64/mm/cache.S::__flush_dcache_all */
/* D7.2.20 CSSELR_El1, Cache Size Selection register
 * Select the current Cache Size ID Register, CCSIDR_EL1, by specifying the
 * required cache level and the cache type (either instruction or data cache)
 *
 * D7.2.14 CCSIDR_EL1, Current Cache Size ID register
 * Provides information about the architecture of the currently selected cache
 *
 * Porting notes:
 * - No need to make CSSELR and CCSIDR access atomic (in Debug State).
 *
 * Note: Kernel v4.2.1 kill flush_cache_all and related functions
 */
static int __armv8_flush_dcache_all(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm_dpm *dpm = armv8->arm.dpm;
#if 0
	struct armv8_cachesize *d_u_size =
		&(armv8->armv8_mmu.armv8_cache.d_u_size);
	int32_t c_way, c_index = d_u_size->index;
#endif
	uint32_t loc, level;
	uint32_t itr;		/* Instruction */
	int retval;
	uint32_t ctype;		/* Cache type */
	uint64_t csselr;	/* Current CSSELR_EL1 value */
	uint64_t ccsidr;
	int32_t ccsidr_way, ccsidr_set;		/* signed int, comparing with zero */
	uint32_t way_shift, set_shift;
	uint64_t dc_cisw;
	uint64_t time_start, time_finish;
	uint32_t edscr;

	if (target->state != TARGET_HALTED) {
		LOG_INFO("Flushing not performed: Target %s is not halted",
			target_name(target));
		return ERROR_TARGET_NOT_HALTED;
	}

	/*  check that cache data is on at target halt */
	if (!armv8->armv8_mmu.armv8_cache.d_u_cache_enabled) {
		LOG_INFO("flushed not performed: cache is not enabled at target halt");
		return ERROR_OK;
	}

	loc = ARMV8_CLIDR_LOC(armv8->armv8_mmu.armv8_cache.clidr);
	if (loc == 0) {
		LOG_INFO("flushed not performed: No cache implemented");
		return ERROR_OK;
	}

	LOG_INFO("Flushing %d-level cache on %s", loc, target_name(target));
	time_start = timeval_ms();

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)	goto dpm_done;

	/* MRS <Xt>, CSSELR_EL1 ; Read CSSELR_EL1 into Xt */
	itr = A64_OPCODE_MRS(0b11, 0b010, 0b0000, 0b0000, 0b000, AARCH64_X0);
	retval = dpm->instr_read_data_x0(dpm, itr, &csselr);
	if (retval != ERROR_OK)	goto dpm_done;

	/* Note: The code below is 0-based cache level, way, and set.
	 * UI will show n+1
	 */

	/* Start cleaning at cache level 0 */
	for (level = 0; level < loc; level++) {
		/* Skip if no cache, or just I-Cache */
		ctype = ARMV8_CLIDR_CTYPE(level+1, armv8->armv8_mmu.armv8_cache.clidr);
		if (ctype < 2) {
			LOG_INFO("cache level %d type is %d, skipped", level+1, ctype);
			continue;
		}

		/* Select current cache level in CSSELR_EL1
		 * Read cache information from CSSIDR_EL1 */

		/* MSR CSSELR_EL1, <Xt> ; Write Xt to CSSELR_EL1 */
		itr = A64_OPCODE_MSR(0b11, 0b010, 0b0000, 0b0000, 0b000, AARCH64_X0);
		retval = dpm->instr_write_data_x0(dpm, itr, level << 1);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to set cache level %d", level+1);
			continue;
		}
		/* isb to synchronize change of CSSELR : Not needed in Debug state */
		/* MRS <Xt>, CSSIDR_EL1; Read CSSIDR_EL1 into Xt */
		itr = A64_OPCODE_MRS(0b11, 0b001, 0b0000, 0b0000, 0b000, AARCH64_X0);
		retval = dpm->instr_read_data_x0(dpm, itr, &ccsidr);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to retrieve cache %"PRIu32 " information", level+1);
			continue;
		}

		ccsidr_way = ARMV8_CCSIDR_NUMWAYS(ccsidr);
		ccsidr_set = ARMV8_CCSIDR_NUMSETS(ccsidr);
		LOG_INFO("cache level %d has %d ways, %d sets",
			level+1, ccsidr_way+1, ccsidr_set+1);

		/* DC CISW Xt format */
		/* SetWay, bit [31:4]
		 *	Way, [31 :32-A], the number of the way
		 *	Set, [B-1:   L], the number of the set
		 *	Bits [L-1:   4], RES0
		 *	A = Log2(ASSOCIATIVITY), L=Log2(LineLen), B=(L+S), S=Log2(NUMSETS)
		 *	A,S: rounded up to the next integer.
		 *	Level, bit [3:1]
		 */
		set_shift = ARMV8_CCSIDR_LINESIZE(ccsidr) + 4;	// of cache line: log2(linelen)

		do {
			way_shift = __builtin_clz(ccsidr_way);
			do {
				dc_cisw = (ccsidr_way << way_shift) |
					(ccsidr_set << set_shift) |
					(level << 1);


				// We need aarch64_exec_instr() here
				itr = A64_OPCODE_DC_CISW(dc_cisw);
#if 0
				retval = dpm->instr_write_data_x0(dpm, itr, 0);
#else
				edscr = ARMV8_EDSCR_ITE;
				retval = aarch64_exec_opcode(target, itr, &edscr);
#endif
				if (retval != ERROR_OK) {
					LOG_ERROR("Failed to Clean/Invalidate cache %"PRIu32, level+1);
				}
			} while (--ccsidr_set >= 0);
		} while (--ccsidr_way >= 0);

		/* Ensure completion of previous cache maintenance operation */
		itr = A64_OPCODE_DSB_SY;
#if 0
		retval = dpm->instr_write_data_x0(dpm, itr, 0);
#else
		edscr = ARMV8_EDSCR_ITE;
		retval = aarch64_exec_opcode(target, itr, &edscr);
#endif
	} /* End of for(level) */

	/* Switch back to current cache level */
	itr = A64_OPCODE_MSR(0b11, 0b010, 0b0000, 0b0000, 0b000, AARCH64_X0);
	retval = dpm->instr_write_data_x0(dpm, itr, csselr);
	if (retval != ERROR_OK)
		LOG_ERROR("Failed to restore cache level after flushing");

	time_finish = timeval_ms();
	LOG_INFO("Time %"PRIu64 " - %"PRIu64 " = %"PRIu64,
		time_start, time_finish, time_finish-time_start);

dpm_done:
	/* void */ dpm->finish(dpm);
	return retval;
}

/* This function is enabled by armv8_cache.identified flag
 * after cache is identified.
 */
static int armv8_flush_dcache_all(struct target *target)
{
	int retval = ERROR_FAIL;

	/* check that armv8_cache is correctly identify */
	struct armv8_common *armv8 = target_to_armv8(target);
	if (!armv8->armv8_mmu.armv8_cache.identified) {
		LOG_ERROR("trying to flush un-identified cache");
		return retval;
	}

	if (target->smp) {
		/* Walk through all cores */
		struct target_list *head;
		struct target *curr;
		head = target->head;
		while (head != (struct target_list *)NULL) {
			curr = head->target;
			if (curr->state == TARGET_HALTED) {
				retval = __armv8_flush_dcache_all(curr);
			}
			head = head->next;
		}
	} else
		retval = __armv8_flush_dcache_all(target);
	return retval;
}

int armv8_invalidate_icache(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm_dpm *dpm = armv8->arm.dpm;
	int retval = ERROR_FAIL;
	uint32_t itr;		/* Instruction */
	uint32_t edscr;

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)	goto dpm_done;

	itr = A64_OPCODE_IC_IALLUIS;	/* ic    ialluis // I+BTB cache invalidate */
	edscr = 0;
	retval = aarch64_exec_opcode(target, itr, &edscr);

dpm_done:
	if (retval != ERROR_OK)
		LOG_ERROR("Failed to invalidate instruction cache");
	/* void */ dpm->finish(dpm);

	return retval;
}

int armv8_flush_cache_all(struct target *target)
{
	int rc_dcache, rc_icache;

	rc_dcache = armv8_flush_dcache_all(target);
	rc_icache = armv8_invalidate_icache(target);

	/* report if any error happened */
	return (rc_dcache == ERROR_OK) ? rc_icache : rc_dcache;
}

int armv8_handle_cache_info_command(struct command_context *cmd_ctx,
	struct armv8_cache_common *armv8_cache)
{
	if (armv8_cache->identified == false) {
		command_print(cmd_ctx, "cache not yet identified");
		return ERROR_OK;
	}

	if (armv8_cache->display_cache_info)
		armv8_cache->display_cache_info(cmd_ctx, armv8_cache);
	return ERROR_OK;
}

#if 0	/* Alamy: MPIDR_EL1 bit definitions are different from ARMv7 */
/*  retrieve core id cluster id  */
static int armv8_read_mpidr(struct target *target)
{
	int retval = ERROR_FAIL;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm_dpm *dpm = armv8->arm.dpm;
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
		armv8->multi_processor_system = (mpidr >> 30) & 1;
		armv8->cluster_id = (mpidr >> 8) & 0xf;
		armv8->cpu_id = mpidr & 0x3;
		LOG_INFO("%s cluster %x core %x %s", target_name(target),
			armv8->cluster_id,
			armv8->cpu_id,
			armv8->multi_processor_system == 0 ? "multi core" : "mono core");

	} else
		LOG_ERROR("mpdir not in multiprocessor format");

done:
	dpm->finish(dpm);
	return retval;
}
#endif

/*
 * We will access the three registers to help us to identify caches
 *	CLIDR_EL1
 *	CSSELR_EL1
 *	CCSIDR_EL1
 *
 * D7.2.15 CLIDR_EL1, Cache Level ID Register
 * Identifies the type of cache, or caches, implemented at each level,
 * up to maximum of seven levels. Also identifies the Level of Coherence (LoC)
 * and Level of Unification (LoU) for the cache hierarchy.
 * NOTE: When HCR_EL2.TID2==1, Non-Secure EL1 read to this register are
 *	trapped to EL2.
 *
 */
int armv8_identify_cache(struct target *target)
{
	/*  read cache descriptor */
	int retval = ERROR_FAIL;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm_dpm *dpm = armv8->arm.dpm;
	uint32_t itr;
	uint64_t clidr;
//	uint64_t cache_selected;
//	uint32_t cache_i_reg, cache_d_reg;
	struct armv8_cache_common *cache = &(armv8->armv8_mmu.armv8_cache);

#if 0
	armv8_read_ttbcr(target);	/* Alamy: Do we need this ? */
#endif

	/* MRS <Xt>, CLIDR_EL1 ; Read CLIDR_EL1 into Xt */
	itr = A64_OPCODE_MRS(0b11, 0b001, 0b0000, 0b0000, 0b001, AARCH64_X0);
	retval = armv8->arm.mrs(target, itr, &clidr);

if (retval == ERROR_OK) {
	LOG_INFO("L%"PRIu64 " cache is the highest inner level", ARMV8_CLIDR_ICB(clidr));
	LOG_INFO("LoUU (Level of Unification Uniprocessor) = %"PRIu64,
		ARMV8_CLIDR_LOUU(clidr));
	LOG_INFO("LoC (Level of Coherence) = %"PRIu64, ARMV8_CLIDR_LOC(clidr));
	LOG_INFO("LoUIS (Level of Unification Inner Shareable) = %"PRIu64,
		ARMV8_CLIDR_LOUIS(clidr));
	LOG_INFO("Cache type (7,6,5,4,3,2,1) = "
		"%s, %s, %s, %s, %s, %s, %s",
		armv8_cache_type[ ARMV8_CLIDR_CTYPE7(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE6(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE5(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE4(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE3(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE2(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE1(clidr) ]
		);


}

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)	goto dpm_done;

	/* MRS <Xt>, CLIDR_EL1 ; Read CLIDR_EL1 into Xt */
	itr = A64_OPCODE_MRS(0b11, 0b001, 0b0000, 0b0000, 0b001, AARCH64_X0);
	retval = dpm->instr_read_data_x0(dpm, itr, &clidr);
	if (retval != ERROR_OK)	goto dpm_done;

	LOG_INFO("L%"PRIu64 " cache is the highest inner level", ARMV8_CLIDR_ICB(clidr));
	LOG_INFO("LoUU (Level of Unification Uniprocessor) = %"PRIu64,
		ARMV8_CLIDR_LOUU(clidr));
	LOG_INFO("LoC (Level of Coherence) = %"PRIu64, ARMV8_CLIDR_LOC(clidr));
	LOG_INFO("LoUIS (Level of Unification Inner Shareable) = %"PRIu64,
		ARMV8_CLIDR_LOUIS(clidr));
	LOG_INFO("Cache type (7,6,5,4,3,2,1) = "
		"%s, %s, %s, %s, %s, %s, %s",
		armv8_cache_type[ ARMV8_CLIDR_CTYPE7(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE6(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE5(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE4(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE3(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE2(clidr) ],
		armv8_cache_type[ ARMV8_CLIDR_CTYPE1(clidr) ]
		);

//	/* void */ dpm->finish(dpm);	// Alamy: Redudent


#if 0
	/* Save selected cache, restore it later */
	/* MRS <Xt>, CSSELR_EL1 ; Read CSSELR_EL1 into Xt */
	itr = A64_OPCODE_MRS(0b11, 0b010, 0b0000, 0b0000, 0b000, AARCH64_X0);
	retval = dpm->instr_read_data_x0(dpm, itr, &csselr_);
	if (retval != ERROR_OK)	goto dpm_done;

	/*  retrieve selected cache
	 *  MRC p15, 2,<Rd>, c0, c0, 0; Read CSSELR */
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 2, 0, 0, 0, 0),
			&cache_selected);
	if (retval != ERROR_OK)
		goto done;

	retval = armv8->arm.mrc(target, 15,
			2, 0,	/* op1, op2 */
			0, 0,	/* CRn, CRm */
			&cache_selected);
	if (retval != ERROR_OK)
		goto done;
	/* select instruction cache
	 *  MCR p15, 2,<Rd>, c0, c0, 0; Write CSSELR
	 *  [0]  : 1 instruction cache selection , 0 data cache selection */
	retval = dpm->instr_write_data_r0(dpm,
			ARMV4_5_MRC(15, 2, 0, 0, 0, 0),
			1);
	if (retval != ERROR_OK)
		goto done;

	/* read CCSIDR
	 * MRC P15,1,<RT>,C0, C0,0 ;on cortex A9 read CCSIDR
	 * [2:0] line size  001 eight word per line
	 * [27:13] NumSet 0x7f 16KB, 0xff 32Kbytes, 0x1ff 64Kbytes */
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
#endif


#if 0	/* Alamy: do it in __flush_dcache_all() */
	/* put fake type */
	cache->d_u_size.linelen = 16 << (cache_d_reg & 0x7);
	cache->d_u_size.cachesize = (((cache_d_reg >> 13) & 0x7fff)+1)/8;
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
		cache->d_u_size.way_shift = 32-i;
	}
#endif

#if 0
	LOG_INFO("data cache index %d << %d, way %d << %d",
			cache->d_u_size.index, cache->d_u_size.index_shift,
			cache->d_u_size.way,
			cache->d_u_size.way_shift);

	LOG_INFO("data cache %d bytes %d KBytes asso %d ways",
			cache->d_u_size.linelen,
			cache->d_u_size.cachesize,
			cache->d_u_size.associativity);
#endif

#if 0	/* Alamy: Do it in __flush_dcache_all() */
	cache->i_size.linelen = 16 << (cache_i_reg & 0x7);
	cache->i_size.associativity = ((cache_i_reg >> 3) & 0x3ff) + 1;
	cache->i_size.nsets = (cache_i_reg >> 13) & 0x7fff;
	cache->i_size.cachesize = (((cache_i_reg >> 13) & 0x7fff)+1)/8;
	/*  compute info for set way operation on cache */
	cache->i_size.index_shift = (cache_i_reg & 0x7) + 4;
	cache->i_size.index = (cache_i_reg >> 13) & 0x7fff;
	cache->i_size.way = ((cache_i_reg >> 3) & 0x3ff);
	cache->i_size.way_shift = cache->i_size.way + 1;
	{
		int i = 0;
		while (((cache->i_size.way_shift >> i) & 1) != 1)
			i++;
		cache->i_size.way_shift = 32-i;
	}
#endif

#if 0
	LOG_INFO("instruction cache index %d << %d, way %d << %d",
			cache->i_size.index, cache->i_size.index_shift,
			cache->i_size.way, cache->i_size.way_shift);

	LOG_INFO("instruction cache %d bytes %d KBytes asso %d ways",
			cache->i_size.linelen,
			cache->i_size.cachesize,
			cache->i_size.associativity);
#endif

#if 0	/* Alamy: Temporary disable it */
	/*  if no l2 cache initialize l1 data cache flush function function */
	if (armv8->armv8_mmu.armv8_cache.flush_all_data_cache == NULL) {
		armv8->armv8_mmu.armv8_cache.display_cache_info =
			armv8_handle_inner_cache_info_command;
		armv8->armv8_mmu.armv8_cache.flush_all_data_cache =
			armv8_flush_all_data;
	}
	armv8->armv8_mmu.armv8_cache.ctype = 0;
#endif
	if (retval == ERROR_OK) {
		cache->clidr = clidr;
		cache->flush_dcache_all = armv8_flush_dcache_all;
		cache->identified = true;	/* Says that we have data ready (for flushing) */
	}

dpm_done:
	/* void */ dpm->finish(dpm);

//	armv8_read_mpidr(target);	/* Alamy: We don't seem to need this, just info */
	return retval;
}

int armv8_init_arch_info(struct target *target, struct armv8_common *armv8)
{
	struct arm *arm = &armv8->arm;
	arm->arch_info = armv8;
	target->arch_info = &armv8->arm;
	/*  target is useful in all function arm v4 5 compatible */
	armv8->arm.target = target;
	armv8->arm.common_magic = ARM_COMMON_MAGIC;
	armv8->common_magic = ARMV8_COMMON_MAGIC;

	/* register access setup, must be after arm_dpm_setup() */
	arm->full_context = armv8_full_context;
	arm->read_core_reg = armv8_read_core_reg;
	arm->write_core_reg = armv8_write_core_reg;

	armv8->armv8_mmu.armv8_cache.identified = false;
	armv8->armv8_mmu.armv8_cache.flush_dcache_all = NULL;
//	armv8->armv8_mmu.armv8_cache.l2_cache = NULL;
//	armv8->armv8_mmu.armv8_cache.flush_all_data_cache = NULL;
	armv8->armv8_mmu.armv8_cache.display_cache_info = NULL;
	return ERROR_OK;
}

const char *armv8_core_mode_name(enum arm_mode mode)
{
	const char *cp;

	cp = Jim_Nvp_value2name_simple(nvp_armv8_core_mode, mode)->name;
	if (!cp) {
		LOG_ERROR("Invalid core mode: %d", (int)(mode));
		cp = "(*BUG*unknown*BUG*)";
	}
	return cp;
}

void log_pstate_aarch64(uint32_t pstate)
{
	LOG_USER("\tPSTATE:0x%.8"PRIx32 " (%c%c%c%c%c%c%c%c %s %s %s)",
		pstate,
		(pstate & ARMV8_PSTATE_N)	? 'N' : 'n',
		(pstate & ARMV8_PSTATE_Z)	? 'Z' : 'z',
		(pstate & ARMV8_PSTATE_C)	? 'C' : 'c',
		(pstate & ARMV8_PSTATE_V)	? 'V' : 'v',
		(pstate & ARMV8_PSTATE_D)	? 'D' : 'd',
		(pstate & ARMV8_PSTATE_A)	? 'A' : 'a',
		(pstate & ARMV8_PSTATE_I)	? 'I' : 'i',
		(pstate & ARMV8_PSTATE_F)	? 'F' : 'f',

		(pstate & ARMV8_PSTATE_SS)	? "SS": "ss",
		(pstate & ARMV8_PSTATE_IL)	? "IL": "il",
		armv8_core_mode_name(ARMV8_PSTATE_MODE(pstate))
	);
}

void log_pstate_aarch32(uint32_t pstate)
{
	LOG_USER("AArch32 PSTATE --- (TBD)");
}

void log_pstate_detail(uint32_t pstate)
{
	if (pstate & ARMV8_PSTATE_nRW)
		log_pstate_aarch32(pstate);
	else
		log_pstate_aarch64(pstate);
}

/* Printing information as:
 * target a57.cpu0 halted in ARM64 state due to breakpoint
 *	SP:0xffffffc00078fd20 X30:0xffffffc00076fbdc PC:0xffffffc000096f70
 *	PSTATE:0x600002c5 (nZCvDaIF ss il EL1h)
 */
int armv8_aarch64_state(struct target *target)
{
	struct arm *arm = target_to_arm(target);

	if (arm->common_magic != ARM_COMMON_MAGIC) {
		LOG_ERROR("BUG: called for a non-ARM target");
		return ERROR_FAIL;
	}

#if 1	/* Alamy: just for debugging, restore later */
	struct reg *reg_x30 = armv8_get_reg_by_num(arm, AARCH64_X30);
#else
	struct reg *reg_x30 = armv8_get_reg_by_num(arm, AARCH64_X0);
#endif
	assert(reg_x30 != NULL);
	uint32_t pstate = buf_get_u32(arm->spsr->value, 0, 32);

	LOG_USER("target %s halted in %s state due to %s%s\n\t"
		"SP:0x%.16"PRIx64 " X30:0x%.16"PRIx64 " PC:0x%.16"PRIx64,
		target_name(target),
		armv8_state_strings[arm->core_state],
		debug_reason_name(target),
/*		armv8_mode_name(arm->core_mode), ***** Alamy: debug this: ELx[t/h] */
		arm->is_semihosting ? ", semihosting" : "",
		buf_get_u64(arm->cpsr->value, 0, 64),	/* use as SP */
		buf_get_u64(reg_x30->value, 0, 64),
		buf_get_u64(arm->pc->value, 0, 64) );
	log_pstate_detail(pstate);

	return ERROR_OK;
}

int armv8_arch_state(struct target *target)
{
	static const char * const state[] = {
		"disabled", "enabled"
	};

	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;

	if (armv8->common_magic != ARMV8_COMMON_MAGIC) {
		LOG_ERROR("BUG: called for a non-Armv8 target");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (arm->core_state == ARM_STATE_AARCH64)
		armv8_aarch64_state(target);
	else
		arm_arch_state(target);

	LOG_USER("MMU: %s, D-Cache: %s, I-Cache: %s",
		state[armv8->armv8_mmu.mmu_enabled],
		state[armv8->armv8_mmu.armv8_cache.d_u_cache_enabled],
		state[armv8->armv8_mmu.armv8_cache.i_cache_enabled]);

	if (arm->core_mode == ARM_MODE_ABT)
		armv8_show_fault_registers(target);
	if (target->debug_reason == DBG_REASON_WATCHPOINT)
		LOG_USER("Watchpoint triggered at PC %#08x",
			(unsigned) armv8->dpm.wp_pc);

	return ERROR_OK;
}

#if 0
static const struct {
	unsigned id;
	const char *name;
	unsigned bits;
	enum reg_type type;
	const char *group;
	const char *feature;
} armv8_regs[] = {
	{ ARMV8_R0,  "x0",  64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R1,  "x1",  64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R2,  "x2",  64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R3,  "x3",  64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R4,  "x4",  64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R5,  "x5",  64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R6,  "x6",  64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R7,  "x7",  64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R8,  "x8",  64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R9,  "x9",  64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R10, "x10", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R11, "x11", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R12, "x12", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R13, "x13", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R14, "x14", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R15, "x15", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R16, "x16", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R17, "x17", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R18, "x18", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R19, "x19", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R20, "x20", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R21, "x21", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R22, "x22", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R23, "x23", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R24, "x24", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R25, "x25", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R26, "x26", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R27, "x27", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R28, "x28", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R29, "x29", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R30, "x30", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },

	{ ARMV8_R31, "sp", 64, REG_TYPE_DATA_PTR, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_PC,  "pc", 64, REG_TYPE_CODE_PTR, "general", "org.gnu.gdb.aarch64.core" },

	{ ARMV8_xPSR, "CPSR", 64, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
};
#endif

static const struct {
	unsigned id;
	const char *name;
//	unsigned bits;
	enum reg_type type;
	const char *group;
	const char *feature;
} armv8_regs[] = {
	/* ARMv8 has AArch64 & AArch32 modes
	 * Here we only define AArch64 registers, must find a way for AArch32 registers
	 */

	/* -------------------- org.gnu.gdb.aarch64.core -------------------- */
	/* 64 * 31 + 64 + 64 + 32 = 2144 bits = 268 bytes */
	{ AARCH64_X0,   "x0", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X1,   "x1", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X2,   "x2", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X3,   "x3", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X4,   "x4", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X5,   "x5", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X6,   "x6", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X7,   "x7", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X8,   "x8", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X9,   "x9", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X10, "x10", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X11, "x11", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X12, "x12", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X13, "x13", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X14, "x14", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X15, "x15", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X16, "x16", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X17, "x17", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X18, "x18", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X19, "x19", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X20, "x20", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X21, "x21", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X22, "x22", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X23, "x23", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X24, "x24", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X25, "x25", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X26, "x26", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X27, "x27", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X28, "x28", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X29, "x29", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_X30, "x30", REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },

	/* Special registers */
	/*
		Zero register:			XZR(64-bit) / WZR(32-bit)
		Program counter:		PC
		Stack pointer:			SP_EL0		SP_EL1		SP_EL2		SP_EL3
		Program Status register:			SPSR_EL1	SPSR_EL2	SPSR_EL3
		Exception Link register				ELR_EL1		ELR_EL2		ELR_EL3
	*/

	{ AARCH64_SP,  "sp", REG_TYPE_DATA_PTR, "general", "org.gnu.gdb.aarch64.core" },
	{ AARCH64_PC,  "pc", REG_TYPE_CODE_PTR, "general", "org.gnu.gdb.aarch64.core" },

	{ AARCH64_PSTATE, "SPSR", REG_TYPE_UINT32, "general", "org.gnu.gdb.aarch64.core" },

	/* -------------------- org.gnu.gdb.aarch64.fpu -------------------- */
	/* 128 * 32 + 32 + 32 = 4160 bits = 520 bytes */
	{ AARCH64_V0,   "v0", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V1,   "v1", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V2,   "v2", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V3,   "v3", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V4,   "v4", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V5,   "v5", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V6,   "v6", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V7,   "v7", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V8,   "v8", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V9,   "v9", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V10, "v10", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V11, "v11", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V12, "v12", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V13, "v13", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V14, "v14", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V15, "v15", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V16, "v16", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V17, "v17", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V18, "v18", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V19, "v19", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V20, "v20", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V21, "v21", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V22, "v22", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V23, "v23", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V24, "v24", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V25, "v25", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V26, "v26", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V27, "v27", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V28, "v28", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V29, "v29", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V30, "v30", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_V31, "v31", REG_TYPE_UINT128, "fpu", "org.gnu.gdb.aarch64.fpu" },

	{ AARCH64_FPCR, "fpcr", REG_TYPE_UINT32, "fpu", "org.gnu.gdb.aarch64.fpu" },
	{ AARCH64_FPSR, "fpsr", REG_TYPE_UINT32, "fpu", "org.gnu.gdb.aarch64.fpu" },
};

#define ARMV8_NUM_REGS	ARRAY_SIZE(armv8_regs)


static int armv8_get_core_reg(struct reg *reg)
{
	int retval;
	struct arm_reg *armv8_reg = reg->arch_info;
	struct target *target = armv8_reg->target;
	struct arm *arm = target_to_arm(target);

LOG_DEBUG("regnum=%d, target->state = %d", armv8_reg->num, target->state);
	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;

	/* Alamy: Looks like it will never come to this point */
	/* target.c::handle_reg_command::reg->type_get() would be called when
	 * reg is not valid (not in halted state).
	 * And it got returned when it's not in halted state above
	 * Reason
	 *   When PE enters Debug state (halted), it poll() registers.
	 *   Thus, all the registers are valid in Debug state (halted).
	 */
//	retval = arm->read_core_reg(target, reg, armv8_reg->num, arm->core_mode);
	retval = armv8_read_core_reg(target, reg, armv8_reg->num, arm->core_mode);
	if (retval == ERROR_OK) {
		reg->valid = 1;
		reg->dirty = 0;
	}

	return retval;
}

static int armv8_set_core_reg(struct reg *reg, uint8_t *buf)
{
	struct arm_reg *armv8_reg = reg->arch_info;
	struct target *target = armv8_reg->target;
//	uint64_t value = buf_get_u64(buf, 0, 64);

	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;

	LOG_DEBUG("%s", reg->name);

//	buf_set_u64(reg->value, 0, 64, value);
	buf_cpy(buf, reg->value, reg->size);	/* buf_cpy(from,to,size) */
	reg->dirty = 1;
	reg->valid = 1;

	return ERROR_OK;
}

static const struct reg_arch_type armv8_reg_type = {
	.get = armv8_get_core_reg,
	.set = armv8_set_core_reg,
};

/** Builds cache of architecturally defined registers.  */
struct reg_cache *armv8_build_reg_cache(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;
//	int num_regs = ARMV8_NUM_REGS;
	struct reg_cache **cache_p = register_get_last_cache_p(&target->reg_cache);
	struct reg_cache *cache = malloc(sizeof(struct reg_cache));
	struct reg *reg_list = calloc(ARMV8_NUM_REGS, sizeof(struct reg));
	struct arm_reg *arch_info = calloc(ARMV8_NUM_REGS, sizeof(struct arm_reg));
	struct reg_feature *feature;
	uint i;


	if (!cache || !reg_list || !arch_info) {
		free(cache);
		free(reg_list);
		free(arch_info);
		return NULL;
	}

	/* Build the process context cache */
	cache->name = "ARMv8 registers";
	cache->next = NULL;
	cache->reg_list = reg_list;
	cache->num_regs = ARMV8_NUM_REGS;
//	cache->num_regs = 34;
	(*cache_p) = cache;

	for (i = 0; i < ARMV8_NUM_REGS; i++) {
		uint32_t bitsize;

		/* CAUTION: Do NOT use arch_info[i].value which has only 4 bytes */
		arch_info[i].num = armv8_regs[i].id;
		arch_info[i].target = target;
		arch_info[i].arm = arm;

		switch (armv8_regs[i].type) {
		case REG_TYPE_UINT32:	bitsize = 32;		break;
		case REG_TYPE_UINT128:	bitsize = 0;		break;	/* Alamy: Hacking */
		case REG_TYPE_DATA_PTR:
		case REG_TYPE_CODE_PTR:
		case REG_TYPE_UINT64:
		default:		bitsize = 64;		break;
		}

		reg_list[i].name = armv8_regs[i].name;
//		reg_list[i].size = armv8_regs[i].bits;
		/* .size is used in many places in arm_dpm to differ 32/64-bit operations */
		reg_list[i].size = bitsize;
		reg_list[i].value = calloc(1, (bitsize >> 3));	/* 64-bit = 8 bytes */
		reg_list[i].dirty = 0;
		reg_list[i].valid = 0;
		reg_list[i].type = &armv8_reg_type;
		reg_list[i].arch_info = &arch_info[i];

		reg_list[i].group = armv8_regs[i].group;
		reg_list[i].number = i;
		reg_list[i].exist = true;
		reg_list[i].caller_save = true;	/* gdb defaults to true */

		if (reg_list[i].value == NULL) {
			LOG_ERROR("unable to allocate reg value");
		}

		feature = calloc(1, sizeof(struct reg_feature));
		if (feature) {
			feature->name = armv8_regs[i].feature;
			reg_list[i].feature = feature;
		} else
			LOG_ERROR("unable to allocate feature list");

		reg_list[i].reg_data_type = calloc(1, sizeof(struct reg_data_type));
		if (reg_list[i].reg_data_type)
			reg_list[i].reg_data_type->type = armv8_regs[i].type;
		else
			LOG_ERROR("unable to allocate reg type list");
	}

	arm->cpsr = reg_list + AARCH64_SP;
	arm->pc   = reg_list + AARCH64_PC;
	arm->spsr = reg_list + AARCH64_PSTATE,
	arm->core_cache = cache;

	return cache;
}

/*
 * Returns handle to the register currently mapped to a given number.
 *
 * \param arm This core's state and registers are used.
 * \param regnum From <AARCH64_X0..AARCH64_REG_NUM) corresponding to
 *   X0..X30: General Registers
 *   SP     : Stack pointer
 *   PC     : Program counter
 *   PSTATE : Processor state (SPSR)
 *
 * WARNING: This function does NOT consider AArch32 state ... yet.
 */
struct reg *armv8_get_reg_by_num(struct arm *arm, unsigned regnum)
{
	struct reg *r;

	/* reg_list is allocated in armv8_build_reg_cache()
	 * make sure we don't have a violation accessing to the array
	 */
#if 1
	if (regnum >= arm->core_cache->num_regs)
		return NULL;

	r = arm->core_cache->reg_list + regnum;

	return r;
#else
	for (listnum = 0; listnum < arm->core_cache->num_regs; listnum++)
	{
		r = arm->core_cache->reg_list + listnum;
		if (r->archinfo.num == regnum)
			return r;
	}
	return NULL;
#endif
}

COMMAND_HANDLER(handle_armv8_disassemble_command)
{
	int retval = ERROR_OK;
	struct target *target = get_current_target(CMD_CTX);

	if (target == NULL) {
		LOG_ERROR("No target selected");
		return ERROR_FAIL;
	}

	return retval;
}
const struct command_registration armv8_command_handlers[] = {
	{
		.name = "disassemble",
		.handler = handle_armv8_disassemble_command,
		.mode = COMMAND_EXEC,
		.usage = "address [count]",
		.help = "disassemble ARMv8 instructions",
	},
	{
		.chain = dap_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};


int armv8_get_gdb_reg_list(struct target *target,
	struct reg **reg_list[], int *reg_list_size,
	enum target_register_class reg_class)
{
	struct arm *arm = target_to_arm(target);
	int rc = ERROR_FAIL;
	int i;

#ifdef	_DEBUG_GDB_FUNC_ENTRY_
	LOG_DEBUG("<<< %s: class: %d, num_regs=%d", target_name(target),
		reg_class, arm->core_cache->num_regs);
#endif

	switch (reg_class) {
	case REG_CLASS_GENERAL:
	case REG_CLASS_ALL:
//		*reg_list_size = ARMV8_NUM_REGS;	/* or arm->core_cache->num_regs */
		*reg_list_size = arm->core_cache->num_regs;	/* or ARMV8_NUM_REGS */
		*reg_list = malloc(sizeof(struct reg *) * (*reg_list_size));
		if (*reg_list == NULL) {
			rc = ERROR_FAIL;
			goto done;
		}

		for (i = 0; i < *reg_list_size; i++)
				(*reg_list)[i] = armv8_get_reg_by_num(arm, i);

		rc = ERROR_OK;
		break;

	default:
		LOG_ERROR("not a valid register class type in query.");
		rc = ERROR_FAIL;
		break;
	}

done:
#ifdef	_DEBUG_GDB_FUNC_ENTRY_
	LOG_DEBUG(">>> rc = %d", rc);
#endif
	return rc;
}
