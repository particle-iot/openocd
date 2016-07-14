/***************************************************************************
 *   Copyright (C) 2016 by Matthias Welwarsky                              *
 *   matthias.welwarsky@sysgo.com                                          *
 *                                                                         *
 *   Copyright (C) ST-Ericsson SA 2011 michel.jaouen@stericsson.com        *
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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/binarybuffer.h>
#include <helper/command.h>

#include "jtag/interface.h"
#include "arm.h"
#include "armv7a.h"
#include "armv7a_mmu.h"
#include "arm_opcodes.h"


/*  method adapted to Cortex-A : reused ARM v4 v5 method */
int armv7a_mmu_translate_va(struct target *target,  uint32_t va, uint32_t *val)
{
	uint32_t first_lvl_descriptor = 0x0;
	uint32_t second_lvl_descriptor = 0x0;
	int retval;
	struct armv7a_common *armv7a = target_to_armv7a(target);
	uint32_t ttbidx = 0;	/*  default to ttbr0 */
	uint32_t ttb_mask;
	uint32_t va_mask;
	uint32_t ttb;

	if (target->state != TARGET_HALTED)
		LOG_INFO("target not halted, using cached values for translation table!");

	/* if va is above the range handled by ttbr0, select ttbr1 */
	if (va > armv7a->armv7a_mmu.ttbr_range[0]) {
		/*  select ttb 1 */
		ttbidx = 1;
	}

	ttb = armv7a->armv7a_mmu.ttbr[ttbidx];
	ttb_mask = armv7a->armv7a_mmu.ttbr_mask[ttbidx];
	va_mask = 0xfff00000 & armv7a->armv7a_mmu.ttbr_range[ttbidx];

	LOG_DEBUG("ttb_mask %" PRIx32 " va_mask %" PRIx32 " ttbidx %i",
		  ttb_mask, va_mask, ttbidx);
	retval = armv7a->armv7a_mmu.read_physical_memory(target,
			(ttb & ttb_mask) | ((va & va_mask) >> 18),
			4, 1, (uint8_t *)&first_lvl_descriptor);
	if (retval != ERROR_OK)
		return retval;
	first_lvl_descriptor = target_buffer_get_u32(target, (uint8_t *)
			&first_lvl_descriptor);
	/*  reuse armv4_5 piece of code, specific armv7a changes may come later */
	LOG_DEBUG("1st lvl desc: %8.8" PRIx32 "", first_lvl_descriptor);

	if ((first_lvl_descriptor & 0x3) == 0) {
		LOG_ERROR("Address translation failure");
		return ERROR_TARGET_TRANSLATION_FAULT;
	}


	if ((first_lvl_descriptor & 0x40002) == 2) {
		/* section descriptor */
		*val = (first_lvl_descriptor & 0xfff00000) | (va & 0x000fffff);
		return ERROR_OK;
	} else if ((first_lvl_descriptor & 0x40002) == 0x40002) {
		/* supersection descriptor */
		if (first_lvl_descriptor & 0x00f001e0) {
			LOG_ERROR("Physical address does not fit into 32 bits");
			return ERROR_TARGET_TRANSLATION_FAULT;
		}
		*val = (first_lvl_descriptor & 0xff000000) | (va & 0x00ffffff);
		return ERROR_OK;
	}

	/* page table */
	retval = armv7a->armv7a_mmu.read_physical_memory(target,
			(first_lvl_descriptor & 0xfffffc00) | ((va & 0x000ff000) >> 10),
			4, 1, (uint8_t *)&second_lvl_descriptor);
	if (retval != ERROR_OK)
		return retval;

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
	} else {
		/* small page descriptor */
		*val = (second_lvl_descriptor & 0xfffff000) | (va & 0x00000fff);
	}

	return ERROR_OK;
}

/*  V7 method VA TO PA  */
int armv7a_mmu_translate_va_pa(struct target *target, uint32_t va,
	uint32_t *val, int meminfo)
{
	int retval = ERROR_FAIL;
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct arm_dpm *dpm = armv7a->arm.dpm;
	uint32_t virt = va & ~0xfff;
	uint32_t NOS, NS, INNER, OUTER;
	*val = 0xdeadbeef;
	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto done;
	/*  mmu must be enable in order to get a correct translation
	 *  use VA to PA CP15 register for conversion */
	retval = dpm->instr_write_data_r0(dpm,
			ARMV4_5_MCR(15, 0, 0, 7, 8, 0),
			virt);
	if (retval != ERROR_OK)
		goto done;
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 7, 4, 0),
			val);
	/* decode memory attribute */
	NOS = (*val >> 10) & 1;	/*  Not Outer shareable */
	NS = (*val >> 9) & 1;	/* Non secure */
	INNER = (*val >> 4) &  0x7;
	OUTER = (*val >> 2) & 0x3;

	if (retval != ERROR_OK)
		goto done;
	*val = (*val & ~0xfff)  +  (va & 0xfff);
	if (*val == va)
		LOG_WARNING("virt = phys  : MMU disable !!");
	if (meminfo) {
		LOG_INFO("%" PRIx32 " : %" PRIx32 " %s outer shareable %s secured",
			va, *val,
			NOS == 1 ? "not" : " ",
			NS == 1 ? "not" : "");
		switch (OUTER) {
			case 0:
				LOG_INFO("outer: Non-Cacheable");
				break;
			case 1:
				LOG_INFO("outer: Write-Back, Write-Allocate");
				break;
			case 2:
				LOG_INFO("outer: Write-Through, No Write-Allocate");
				break;
			case 3:
				LOG_INFO("outer: Write-Back, no Write-Allocate");
				break;
		}
		switch (INNER) {
			case 0:
				LOG_INFO("inner: Non-Cacheable");
				break;
			case 1:
				LOG_INFO("inner: Strongly-ordered");
				break;
			case 3:
				LOG_INFO("inner: Device");
				break;
			case 5:
				LOG_INFO("inner: Write-Back, Write-Allocate");
				break;
			case 6:
				LOG_INFO("inner:  Write-Through");
				break;
			case 7:
				LOG_INFO("inner: Write-Back, no Write-Allocate");
				break;
			default:
				LOG_INFO("inner: %" PRIx32 " ???", INNER);
		}
	}

done:
	dpm->finish(dpm);

	return retval;
}

COMMAND_HANDLER(armv7a_mmu_dump_table)
{
	struct target *target = get_current_target(CMD_CTX);
	struct armv7a_common *armv7a = target_to_armv7a(target);
	struct armv7a_mmu_common *mmu = &armv7a->armv7a_mmu;
	struct armv7a_cache_common *cache = &mmu->armv7a_cache;

	int ttbidx = 0;
	int pt_idx;
	uint32_t *first_lvl_ptbl;
	target_addr_t ttb;

	if (mmu->cached == -1) {
		LOG_ERROR("TTB not cached!\n");
		return ERROR_FAIL;
	}

	if (CMD_ARGC < 1)
		return ERROR_COMMAND_ARGUMENT_INVALID;

	if (!strcmp(CMD_ARGV[0], "addr")) {
		if (CMD_ARGC < 2)
			return ERROR_COMMAND_ARGUMENT_INVALID;

		COMMAND_PARSE_NUMBER(target_addr, CMD_ARGV[1], ttb);
	} else {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], ttbidx);
		if (ttbidx > 1)
			return ERROR_COMMAND_ARGUMENT_INVALID;

		ttb = mmu->ttbr[ttbidx] & mmu->ttbr_mask[ttbidx];
	}

	LOG_USER("Page Directory at (phys): %8.8" TARGET_PRIxADDR, ttb);

	first_lvl_ptbl = malloc(sizeof(uint32_t)*4096);
	if (first_lvl_ptbl == NULL)
		return ERROR_FAIL;

	/*
	 * this may or may not be necessary depending on whether
	 * the table walker is configured to use the cache or not.
	 */
	cache->flush_all_data_cache(target);

	mmu->read_physical_memory(target, ttb, 4, 4096, (uint8_t *)first_lvl_ptbl);

	for (pt_idx = 0; pt_idx < 4096;) {
		uint32_t first_lvl_descriptor = target_buffer_get_u32(target,
						(uint8_t *)&first_lvl_ptbl[pt_idx]);

		LOG_DEBUG("L1 desc[%8.8"PRIx32"]: %8.8"PRIx32, pt_idx << 20, first_lvl_descriptor);

		/* skip empty entries in the first level table */
		if ((first_lvl_descriptor & 3) == 0) {
			pt_idx++;
		} else
		if ((first_lvl_descriptor & 0x40002) == 2) {
			/* section descriptor */
			uint32_t va_range = 1024*1024; /* 1MB range */
			uint32_t va_start = pt_idx << 20;
			uint32_t va_end = va_start + va_range;

			uint32_t pa_start = (first_lvl_descriptor & 0xfff00000);
			uint32_t pa_end = pa_start + va_range;

			LOG_USER("VA[%8.8"PRIx32" -- %8.8"PRIx32"]: PA[%8.8"PRIx32" -- %8.8"PRIx32"]",
				va_start, va_end, pa_start, pa_end);
			pt_idx++;
		} else
		if ((first_lvl_descriptor & 0x40002) == 0x40002) {
			/* supersection descriptor */
			uint32_t va_range = 16*1024*1024; /* 1MB range */
			uint32_t va_start = pt_idx << 20;
			uint32_t va_end = va_start + va_range;

			uint32_t pa_start = (first_lvl_descriptor & 0xff000000);
			uint32_t pa_end = pa_start + va_range;

			LOG_USER("VA[%8.8"PRIx32" -- %8.8"PRIx32"]: PA[%8.8"PRIx32" -- %8.8"PRIx32"]",
				va_start, va_end, pa_start, pa_end);

			/* skip next 15 entries, they're duplicating the first entry */
			pt_idx += 16;
		} else {
			uint32_t second_lvl_descriptor;
			uint32_t *pt2;
			int pt2_idx;

			/* page table, always 1KB long */
			pt2 = malloc(1024);
			mmu->read_physical_memory(target, (first_lvl_descriptor & 0xfffffc00),
						  4, 256, (uint8_t *)pt2);

			for (pt2_idx = 0; pt2_idx < 256; ) {
				second_lvl_descriptor = target_buffer_get_u32(target,
						(uint8_t *)&pt2[pt2_idx]);

				if ((second_lvl_descriptor & 3) == 0) {
					/* skip entry */
					pt2_idx++;
				} else
				if ((second_lvl_descriptor & 3) == 1) {
					/* large page */
					uint32_t va_range = 64*1024; /* 64KB range */
					uint32_t va_start = pt_idx << 20;
					uint32_t va_end = va_start + va_range;

					uint32_t pa_start = (second_lvl_descriptor & 0xffff0000);
					uint32_t pa_end = pa_start + va_range;

					LOG_USER("VA[%8.8"PRIx32" -- %8.8"PRIx32"]: PA[%8.8"PRIx32" -- %8.8"PRIx32"]",
						va_start, va_end, pa_start, pa_end);

					pt2_idx += 16;
				} else {
					/* small page */
					uint32_t va_range = 4*1024; /* 4KB range */
					uint32_t va_start = pt_idx << 20;
					uint32_t va_end = va_start + va_range;

					uint32_t pa_start = (second_lvl_descriptor & 0xfffff000);
					uint32_t pa_end = pa_start + va_range;

					LOG_USER("VA[%8.8"PRIx32" -- %8.8"PRIx32"]: PA[%8.8"PRIx32" -- %8.8"PRIx32"]",
						va_start, va_end, pa_start, pa_end);

					pt2_idx++;
				}
			}
			free(pt2);
		}
	}

	free(first_lvl_ptbl);
	return ERROR_OK;
}

static const struct command_registration armv7a_mmu_group_handlers[] = {
	{
		.name = "dump",
		.handler = armv7a_mmu_dump_table,
		.mode = COMMAND_ANY,
		.help = "dump translation table 0, 1 or from <address>",
		.usage = "(0|1|addr <address>)",
	},
	COMMAND_REGISTRATION_DONE
};

const struct command_registration armv7a_mmu_command_handlers[] = {
	{
		.name = "mmu",
		.mode = COMMAND_ANY,
		.help = "mmu command group",
		.usage = "",
		.chain = armv7a_mmu_group_handlers,
	},
	COMMAND_REGISTRATION_DONE
};
