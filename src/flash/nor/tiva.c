/***************************************************************************
 *   Copyright (C) 2006 by Magnus Lundin                                   *
 *   lundin@mlu.mine.nu                                                    *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
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

/***************************************************************************
* Based on stellaris.c nor flash driver
* handle 16k pages of Tiva-snowflake devices
* TODO set/check flashcontroller timing (should already be done by software when setting sysclock)
*     implement flash protection
* 2014 by Heinz Schweiger
***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "jtag/interface.h"
#include "imp.h"
#include <target/algorithm.h>
#include <target/armv7m.h>

#define DID0_VER(did0) ((did0 >> 28)&0x07)

/* TIVA control registers */
#define SCB_BASE	0x400FE000
#define DID0		0x000
#define DID1		0x004
#define MEMTIM0		0x0C0


/* flash memory protection registers */
#define FMPRE0		0x200		/* PRE1 = PRE0 + 4, etc */
#define FMPPE0		0x400		/* PPE1 = PPE0 + 4, etc */


#define FLASH_CONTROL_BASE	0x400FD000
#define FLASH_FMA	(FLASH_CONTROL_BASE | 0x000)
#define FLASH_FMD	(FLASH_CONTROL_BASE | 0x004)
#define FLASH_FMC	(FLASH_CONTROL_BASE | 0x008)
#define FLASH_CRIS	(FLASH_CONTROL_BASE | 0x00C)
#define FLASH_CIM	(FLASH_CONTROL_BASE | 0x010)
#define FLASH_MISC	(FLASH_CONTROL_BASE | 0x014)
#define FLASH_FLASHPP  (FLASH_CONTROL_BASE | 0xFC0)
#define FLASH_SSIZE (FLASH_CONTROL_BASE | 0xFC4)

#define AMISC	1
#define PMISC	2

#define AMASK	1
#define PMASK	2

/* Flash Controller Command bits */
#define FMC_WRKEY	(0xA442 << 16)
#define FMC_COMT	(1 << 3)
#define FMC_MERASE	(1 << 2)
#define FMC_ERASE	(1 << 1)
#define FMC_WRITE	(1 << 0)

/* values to write in FMA to commit write-"once" values */
#define FLASH_FMA_PRE(x)	(2 * (x))	/* for FMPPREx */
#define FLASH_FMA_PPE(x)	(2 * (x) + 1)	/* for FMPPPEx */

static int tiva_mass_erase(struct flash_bank *bank);

struct tiva_flash_bank {
	/* chip id register */
	uint32_t did0;
	uint32_t did1;
	/* flash controller register */
	uint32_t flashpp;
	uint32_t ssize;

	const char *target_name;
	uint8_t target_class;

	uint32_t sramsiz;
	uint32_t flshsz;
	/* flash geometry */
	uint32_t num_pages;
	uint32_t pagesize;
	uint32_t pages_in_lockregion;

	/* nv memory bits */
	uint16_t num_lockbits;
};

static struct {
	uint8_t class;
	uint8_t partno;
	const char *partname;
} TivaParts[] = {
	{0x0A, 0x32, "TM4C129XNCZAD"},
	{0xFF, 0x00, "Unknown Part"}
};

static char *TivaClassname[11] = {
	"Sandstorm",
	"Fury",
	"Unknown",
	"DustDevil",
	"Tempest",
	"Blizzard",
	"Firestorm",
	"Unknown",
	"Unknown",
	"Unknown",
	"Tiva-Snowflake"
};

/***************************************************************************
*	openocd command interface                                              *
***************************************************************************/

/* flash_bank tiva <base> <size> 0 0 <target#>
 */
FLASH_BANK_COMMAND_HANDLER(tiva_flash_bank_command)
{
	struct tiva_flash_bank *tiva_info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	tiva_info = calloc(sizeof(struct tiva_flash_bank), 1);
	bank->base = 0x0;
	bank->driver_priv = tiva_info;

	tiva_info->target_name = "Unknown target";

	/* part wasn't probed for info yet */
	tiva_info->did1 = 0;

	/* TODO Specify the main crystal speed in kHz using an optional
	 * argument; ditto, the speed of an external oscillator used
	 * instead of a crystal.  Avoid programming flash using IOSC.
	 */
	return ERROR_OK;
}

static int get_tiva_info(struct flash_bank *bank, char *buf, int buf_size)
{
	int printed;
	struct tiva_flash_bank *tiva_info = bank->driver_priv;

	if (tiva_info->did1 == 0)
		return ERROR_FLASH_BANK_NOT_PROBED;

	printed = snprintf(buf,
			   buf_size,
			   "\nTI Tiva information: Chip is "
			   "class %i (%s) %s rev %c%i\n",
			   tiva_info->target_class,
			   TivaClassname[tiva_info->target_class],
			   tiva_info->target_name,
			   (int)('A' + ((tiva_info->did0 >> 8) & 0xFF)),
			   (int)((tiva_info->did0) & 0xFF));
	buf += printed;
	buf_size -= printed;

	printed = snprintf(buf,
			   buf_size,
			   "did1: 0x%8.8" PRIx32 ", arch: 0x%4.4" PRIx32
			   ", eproc: %s, ramsize: %ik, flashsize: %ik\n",
			   tiva_info->did1,
			   tiva_info->did1,
			   "ARMv7M",
			   tiva_info->sramsiz>>10,
			   tiva_info->flshsz>>10);
	buf += printed;
	buf_size -= printed;

	if (tiva_info->num_lockbits > 0) {
		snprintf(buf,
				buf_size,
				"pagesize: %" PRIu32 ", pages: %" PRIu32 ", "
				"lockbits: %" PRIu16 ", pages per lockbit: %" PRIu32 "\n",
				tiva_info->pagesize,
				tiva_info->num_pages,
				tiva_info->num_lockbits,
				tiva_info->pages_in_lockregion);
	}
	return ERROR_OK;
}

/***************************************************************************
*	chip identification and status                                         *
***************************************************************************/

/* Set the flash timimg register to match current clocking */
static void tiva_set_flash_timing(struct flash_bank *bank)
{
	/* struct tiva_flash_bank *tiva_info = bank->driver_priv; */
	struct target *target = bank->target;
	uint32_t memtim0;

	target_read_u32(target, SCB_BASE | MEMTIM0, &memtim0);

	LOG_WARNING("function tiva_set_flash_timing is not implemented, MEMTIM0 should be set here");
	LOG_DEBUG("current value of MEMTIM0 = 0x%" PRIx32 "", memtim0);
	return;
}


/* Read device id register, flash controller register and fill in driver info structure */
static int tiva_read_part_info(struct flash_bank *bank)
{
	struct tiva_flash_bank *tiva_info = bank->driver_priv;
	struct target *target = bank->target;
	uint32_t did0, did1, ver, fam;
	int i;

	/* Read and parse chip identification register */
	target_read_u32(target, SCB_BASE | DID0, &did0);
	target_read_u32(target, SCB_BASE | DID1, &did1);
	target_read_u32(target, FLASH_FLASHPP, &tiva_info->flashpp);
	target_read_u32(target, FLASH_SSIZE, &tiva_info->ssize);

	LOG_DEBUG("did0 0x%" PRIx32 ", did1 0x%" PRIx32 ", flashpp 0x%" PRIx32 ", ssize 0x%" PRIx32 "",
	did0, did1, tiva_info->flashpp, tiva_info->ssize);

	ver = did0 >> 28;
	if ((ver != 0) && (ver != 1)) {
		LOG_WARNING("Unknown did0 version, cannot identify target");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	if (did1 == 0) {
		LOG_WARNING("Cannot identify target as a Tiva");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	ver = did1 >> 28;
	fam = (did1 >> 24) & 0xF;
	if (((ver != 0) && (ver != 1)) || (fam != 0)) {
		LOG_WARNING("Unknown did1 version/family.");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	/* get device class */
	if (DID0_VER(did0) > 0) {
		tiva_info->target_class = (did0 >> 16) & 0xFF;
	} else {
		/* Sandstorm class */
		tiva_info->target_class = 0;
	}

	switch (tiva_info->target_class) {
		case 10:			/* Tiva SnowFlake */
		    break;
		default:
			LOG_WARNING("Unknown did0 class");
	}

	for (i = 0; TivaParts[i].partno; i++) {
		if ((TivaParts[i].partno == ((did1 >> 16) & 0xFF)) &&
				(TivaParts[i].class == tiva_info->target_class))
			break;
	}

	tiva_info->target_name = TivaParts[i].partname;

	tiva_info->did0 = did0;
	tiva_info->did1 = did1;

	tiva_info->num_pages = (1 + (tiva_info->flashpp & 0xFFFF)) / 8;
	tiva_info->pagesize = 16*1024; /* 16k page */
	tiva_info->pages_in_lockregion = 1;
	tiva_info->num_lockbits = tiva_info->num_pages;  /* 8 bits in FMPPEx protect 1 page, 1bit=2k, 8bit = 1page */
	tiva_info->flshsz = tiva_info->pagesize * tiva_info->num_pages;

	tiva_info->sramsiz = 256 * (1 + (tiva_info->ssize & 0xFFFF));


	/* REVISIT for at least Tempest parts, read NVMSTAT.FWB too.
	 * That exposes a 32-word Flash Write Buffer ... enabling
	 * writes of more than one word at a time.
	 */

	return ERROR_OK;
}

/***************************************************************************
*	flash operations                                                       *
***************************************************************************/

static int tiva_protect_check(struct flash_bank *bank)
{
	struct tiva_flash_bank *tiva = bank->driver_priv;
	int status = ERROR_OK;
	unsigned i;
	unsigned page;

	if (tiva->did1 == 0)
		return ERROR_FLASH_BANK_NOT_PROBED;

	for (page = 0; page < (unsigned) bank->num_sectors; page++)
		bank->sectors[page].is_protected = -1;

	/* Read each Flash Memory Protection Program Enable (FMPPE) register
	 * to report any pages that we can't write.  Ignore the Read Enable
	 * register (FMPRE).
	 */
	for (page = 0; page < (unsigned) bank->num_sectors; page++) {
	    uint32_t lockbits;

		i = page/4; /* FMPPE register index*/
		status = target_read_u32(bank->target,
				SCB_BASE + (FMPPE0 + 4 * i),
				&lockbits);
		LOG_DEBUG("FMPPE%d = %#8.8x (status %d)", i,
				(unsigned) lockbits, status);
		if (status != ERROR_OK)
			goto done;
		bank->sectors[page].is_protected = !(lockbits & (1 << 8*(page%4)));
	}

done:
	return status;
}

static int tiva_erase(struct flash_bank *bank, int first, int last)
{
	int banknr;
	uint32_t flash_fmc, flash_cris;
	struct tiva_flash_bank *tiva_info = bank->driver_priv;
	struct target *target = bank->target;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (tiva_info->did1 == 0)
		return ERROR_FLASH_BANK_NOT_PROBED;

	if ((first < 0) || (last < first) || (last >= (int)tiva_info->num_pages))
		return ERROR_FLASH_SECTOR_INVALID;

	if ((first == 0) && (last == ((int)tiva_info->num_pages-1)))
		return tiva_mass_erase(bank);

	/* Refresh flash controller timing */
	tiva_set_flash_timing(bank);

	/* Clear and disable flash programming interrupts */
	target_write_u32(target, FLASH_CIM, 0);
	target_write_u32(target, FLASH_MISC, PMISC | AMISC);

	/* REVISIT this clobbers state set by any halted firmware ...
	 * it might want to process those IRQs.
	 */

	for (banknr = first; banknr <= last; banknr++) {
		/* Address is first word in page */
		target_write_u32(target, FLASH_FMA, banknr * tiva_info->pagesize);
		/* Write erase command */
		target_write_u32(target, FLASH_FMC, FMC_WRKEY | FMC_ERASE);
		/* Wait until erase complete */
		do {
			target_read_u32(target, FLASH_FMC, &flash_fmc);
		} while (flash_fmc & FMC_ERASE);

		/* Check acess violations */
		target_read_u32(target, FLASH_CRIS, &flash_cris);
		if (flash_cris & (AMASK)) {
			LOG_WARNING("Error erasing flash page %i,  flash_cris 0x%" PRIx32 "",
					banknr, flash_cris);
			target_write_u32(target, FLASH_CRIS, 0);
			return ERROR_FLASH_OPERATION_FAILED;
		}

		bank->sectors[banknr].is_erased = 1;
	}

	return ERROR_OK;
}

static int tiva_protect(struct flash_bank *bank, int set, int first, int last)
{
	uint32_t fmppe, flash_fmc, flash_cris;
	int lockregion;

	struct tiva_flash_bank *tiva_info = bank->driver_priv;
	struct target *target = bank->target;

	LOG_ERROR("No support yet for protection");
		return ERROR_FLASH_OPERATION_FAILED;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!set) {
		LOG_ERROR("Hardware doesn't support page-level unprotect. "
			"Try the 'recover' command.");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (tiva_info->did1 == 0)
		return ERROR_FLASH_BANK_NOT_PROBED;

	/* lockregions are 2 pages ... must protect [even..odd] */
	if ((first < 0) || (first & 1)
			|| (last < first) || !(last & 1)
			|| (last >= 2 * tiva_info->num_lockbits)) {
		LOG_ERROR("Can't protect unaligned or out-of-range pages.");
		return ERROR_FLASH_SECTOR_INVALID;
	}

	/* Refresh flash controller timing */
	tiva_set_flash_timing(bank);

	/* convert from pages to lockregions */
	first /= 2;
	last /= 2;

	/* FIXME this assumes single FMPPE, for a max of 64K of flash!!
	 * Current parts can be much bigger.
	 */
	if (last >= 32) {
		LOG_ERROR("No support yet for protection > 64K");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	/* target_read_u32(target, SCB_BASE | FMPPE, &fmppe); */

	for (lockregion = first; lockregion <= last; lockregion++)
		fmppe &= ~(1 << lockregion);

	/* Clear and disable flash programming interrupts */
	target_write_u32(target, FLASH_CIM, 0);
	target_write_u32(target, FLASH_MISC, PMISC | AMISC);

	/* REVISIT this clobbers state set by any halted firmware ...
	 * it might want to process those IRQs.
	 */

	LOG_DEBUG("fmppe 0x%" PRIx32 "", fmppe);
	/* target_write_u32(target, SCB_BASE | FMPPE, fmppe); */

	/* Commit FMPPE */
	target_write_u32(target, FLASH_FMA, 1);

	/* Write commit command */
	/* REVISIT safety check, since this cannot be undone
	 * except by the "Recover a locked device" procedure.
	 * REVISIT DustDevil-A0 parts have an erratum making FMPPE commits
	 * inadvisable ... it makes future mass erase operations fail.
	 */
	LOG_WARNING("Flash protection cannot be removed once committed, commit is NOT executed !");
	/* target_write_u32(target, FLASH_FMC, FMC_WRKEY | FMC_COMT); */

	/* Wait until erase complete */
	do {
		target_read_u32(target, FLASH_FMC, &flash_fmc);
	} while (flash_fmc & FMC_COMT);

	/* Check acess violations */
	target_read_u32(target, FLASH_CRIS, &flash_cris);
	if (flash_cris & (AMASK)) {
		LOG_WARNING("Error setting flash page protection,  flash_cris 0x%" PRIx32 "", flash_cris);
		target_write_u32(target, FLASH_CRIS, 0);
		return ERROR_FLASH_OPERATION_FAILED;
	}

	return ERROR_OK;
}

/* see contib/loaders/flash/stellaris.s for src */

static const uint8_t tiva_write_code[] = {
								/* write: */
	0xDF, 0xF8, 0x40, 0x40,		/* ldr		r4, pFLASH_CTRL_BASE */
	0xDF, 0xF8, 0x40, 0x50,		/* ldr		r5, FLASHWRITECMD */
								/* wait_fifo: */
	0xD0, 0xF8, 0x00, 0x80,		/* ldr		r8, [r0, #0] */
	0xB8, 0xF1, 0x00, 0x0F,		/* cmp		r8, #0 */
	0x17, 0xD0,					/* beq		exit */
	0x47, 0x68,					/* ldr		r7, [r0, #4] */
	0x47, 0x45,					/* cmp		r7, r8 */
	0xF7, 0xD0,					/* beq		wait_fifo */
								/* mainloop: */
	0x22, 0x60,					/* str		r2, [r4, #0] */
	0x02, 0xF1, 0x04, 0x02,		/* add		r2, r2, #4 */
	0x57, 0xF8, 0x04, 0x8B,		/* ldr		r8, [r7], #4 */
	0xC4, 0xF8, 0x04, 0x80,		/* str		r8, [r4, #4] */
	0xA5, 0x60,					/* str		r5, [r4, #8] */
								/* busy: */
	0xD4, 0xF8, 0x08, 0x80,		/* ldr		r8, [r4, #8] */
	0x18, 0xF0, 0x01, 0x0F,		/* tst		r8, #1 */
	0xFA, 0xD1,					/* bne		busy */
	0x8F, 0x42,					/* cmp		r7, r1 */
	0x28, 0xBF,					/* it		cs */
	0x00, 0xF1, 0x08, 0x07,		/* addcs	r7, r0, #8 */
	0x47, 0x60,					/* str		r7, [r0, #4] */
	0x01, 0x3B,					/* subs		r3, r3, #1 */
	0x03, 0xB1,					/* cbz		r3, exit */
	0xE2, 0xE7,					/* b		wait_fifo */
								/* exit: */
	0x00, 0xBE,					/* bkpt		#0 */

	/* pFLASH_CTRL_BASE: */
	0x00, 0xD0, 0x0F, 0x40,	/* .word	0x400FD000 */
	/* FLASHWRITECMD: */
	0x01, 0x00, 0x42, 0xA4	/* .word	0xA4420001 */
};
static int tiva_write_block(struct flash_bank *bank,
		const uint8_t *buffer, uint32_t offset, uint32_t wcount)
{
	struct target *target = bank->target;
	uint32_t buffer_size = 16384;
	struct working_area *source;
	struct working_area *write_algorithm;
	uint32_t address = bank->base + offset;
	struct reg_param reg_params[4];
	struct armv7m_algorithm armv7m_info;
	int retval = ERROR_OK;

	/* power of two, and multiple of word size */
	static const unsigned buf_min = 128;

	/* for small buffers it's faster not to download an algorithm */
	if (wcount * 4 < buf_min)
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;

	LOG_DEBUG("(bank=%p buffer=%p offset=%08" PRIx32 " wcount=%08" PRIx32 "",
			bank, buffer, offset, wcount);

	/* flash write code */
	if (target_alloc_working_area(target, sizeof(tiva_write_code),
			&write_algorithm) != ERROR_OK) {
		LOG_DEBUG("no working area for block memory writes");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	};

	/* plus a buffer big enough for this data */
	if (wcount * 4 < buffer_size)
		buffer_size = wcount * 4;

	/* memory buffer */
	while (target_alloc_working_area_try(target, buffer_size, &source) != ERROR_OK) {
		buffer_size /= 2;
		if (buffer_size <= buf_min) {
			target_free_working_area(target, write_algorithm);
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
		LOG_DEBUG("retry target_alloc_working_area(%s, size=%u)",
				target_name(target), (unsigned) buffer_size);
	};

	target_write_buffer(target, write_algorithm->address,
			sizeof(tiva_write_code),
			tiva_write_code);

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);

	buf_set_u32(reg_params[0].value, 0, 32, source->address);
	buf_set_u32(reg_params[1].value, 0, 32, source->address + source->size);
	buf_set_u32(reg_params[2].value, 0, 32, address);
	buf_set_u32(reg_params[3].value, 0, 32, wcount);

	retval = target_run_flash_async_algorithm(target, buffer, wcount, 4,
			0, NULL,
			4, reg_params,
			source->address, source->size,
			write_algorithm->address, 0,
			&armv7m_info);

	if (retval == ERROR_FLASH_OPERATION_FAILED)
		LOG_ERROR("error %d executing tiva flash write algorithm", retval);

	target_free_working_area(target, write_algorithm);
	target_free_working_area(target, source);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);

	return retval;
}

static int tiva_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct tiva_flash_bank *tiva_info = bank->driver_priv;
	struct target *target = bank->target;
	uint32_t address = offset;
	uint32_t flash_cris, flash_fmc;
	uint32_t words_remaining = (count / 4);
	uint32_t bytes_remaining = (count & 0x00000003);
	uint32_t bytes_written = 0;
	int retval;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_DEBUG("(bank=%p buffer=%p offset=%08" PRIx32 " count=%08" PRIx32 "",
			bank, buffer, offset, count);

	if (tiva_info->did1 == 0)
		return ERROR_FLASH_BANK_NOT_PROBED;

	if (offset & 0x3) {
		LOG_WARNING("offset size must be word aligned");
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	if (offset + count > bank->size)
		return ERROR_FLASH_DST_OUT_OF_BANK;

	/* Refresh flash controller timing */
	tiva_set_flash_timing(bank);

	/* Clear and disable flash programming interrupts */
	target_write_u32(target, FLASH_CIM, 0);
	target_write_u32(target, FLASH_MISC, PMISC | AMISC);

	/* REVISIT this clobbers state set by any halted firmware ...
	 * it might want to process those IRQs.
	 */

	/* multiple words to be programmed? */
	if (words_remaining > 0) {
		/* try using a block write */
		retval = tiva_write_block(bank, buffer, offset,
				words_remaining);
		if (retval != ERROR_OK) {
			if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE) {
				LOG_DEBUG("writing flash word-at-a-time");
			} else if (retval == ERROR_FLASH_OPERATION_FAILED) {
				/* if an error occured, we examine the reason, and quit */
				target_read_u32(target, FLASH_CRIS, &flash_cris);

				LOG_ERROR("flash writing failed with CRIS: 0x%" PRIx32 "", flash_cris);
				return ERROR_FLASH_OPERATION_FAILED;
			}
		} else {
			buffer += words_remaining * 4;
			address += words_remaining * 4;
			words_remaining = 0;
		}
	}

	while (words_remaining > 0) {
		if (!(address & 0xff))
			LOG_DEBUG("0x%" PRIx32 "", address);

		/* Program one word */
		target_write_u32(target, FLASH_FMA, address);
		target_write_memory(target, FLASH_FMD, 4, 1, buffer);
		target_write_u32(target, FLASH_FMC, FMC_WRKEY | FMC_WRITE);
		/* LOG_DEBUG("0x%x 0x%x 0x%x",address,buf_get_u32(buffer, 0, 32),FMC_WRKEY | FMC_WRITE); */
		/* Wait until write complete */
		do {
			target_read_u32(target, FLASH_FMC, &flash_fmc);
		} while (flash_fmc & FMC_WRITE);

		buffer += 4;
		address += 4;
		words_remaining--;
	}

	if (bytes_remaining) {
		uint8_t last_word[4] = {0xff, 0xff, 0xff, 0xff};

		/* copy the last remaining bytes into the write buffer */
		memcpy(last_word, buffer+bytes_written, bytes_remaining);

		if (!(address & 0xff))
			LOG_DEBUG("0x%" PRIx32 "", address);

		/* Program one word */
		target_write_u32(target, FLASH_FMA, address);
		target_write_memory(target, FLASH_FMD, 4, 1, last_word);
		target_write_u32(target, FLASH_FMC, FMC_WRKEY | FMC_WRITE);
		/* LOG_DEBUG("0x%x 0x%x 0x%x",address,buf_get_u32(buffer, 0, 32),FMC_WRKEY | FMC_WRITE); */
		/* Wait until write complete */
		do {
			target_read_u32(target, FLASH_FMC, &flash_fmc);
		} while (flash_fmc & FMC_WRITE);
	}

	/* Check access violations */
	target_read_u32(target, FLASH_CRIS, &flash_cris);
	if (flash_cris & (AMASK)) {
		LOG_DEBUG("flash_cris 0x%" PRIx32 "", flash_cris);
		return ERROR_FLASH_OPERATION_FAILED;
	}
	return ERROR_OK;
}

static int tiva_probe(struct flash_bank *bank)
{
	struct tiva_flash_bank *tiva_info = bank->driver_priv;
	int retval;

	/* If this is a tiva chip, it has flash; probe() is just
	 * to figure out how much is present.  Only do it once.
	 */
	if (tiva_info->did1 != 0)
		return ERROR_OK;

	/* tiva_read_part_info() already handled error checking and
	 * reporting.  Note that it doesn't write, so we don't care about
	 * whether the target is halted or not.
	 */
	retval = tiva_read_part_info(bank);
	if (retval != ERROR_OK)
		return retval;

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}

	/* provide this for the benefit of the NOR flash framework */
	bank->size = 16*1024 * tiva_info->num_pages;
	bank->num_sectors = tiva_info->num_pages;
	bank->sectors = calloc(bank->num_sectors, sizeof(struct flash_sector));
	for (int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].offset = i * tiva_info->pagesize;
		bank->sectors[i].size = tiva_info->pagesize;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = -1;
	}

	return retval;
}

static int tiva_mass_erase(struct flash_bank *bank)
{
	struct target *target = NULL;
	struct tiva_flash_bank *tiva_info = NULL;
	uint32_t flash_fmc;

	tiva_info = bank->driver_priv;
	target = bank->target;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (tiva_info->did1 == 0)
		return ERROR_FLASH_BANK_NOT_PROBED;

	/* Refresh flash controller timing */
	tiva_set_flash_timing(bank);

	/* Clear and disable flash programming interrupts */
	target_write_u32(target, FLASH_CIM, 0);
	target_write_u32(target, FLASH_MISC, PMISC | AMISC);

	/* REVISIT this clobbers state set by any halted firmware ...
	 * it might want to process those IRQs.
	 */

	target_write_u32(target, FLASH_FMA, 0);
	target_write_u32(target, FLASH_FMC, FMC_WRKEY | FMC_MERASE);
	/* Wait until erase complete */
	do {
		target_read_u32(target, FLASH_FMC, &flash_fmc);
	} while (flash_fmc & FMC_MERASE);

	/* if device has > 128k, then second erase cycle is needed
	 * this is only valid for older devices, but will not hurt */
	if (tiva_info->num_pages * tiva_info->pagesize > 0x20000) {
		target_write_u32(target, FLASH_FMA, 0x20000);
		target_write_u32(target, FLASH_FMC, FMC_WRKEY | FMC_MERASE);
		/* Wait until erase complete */
		do {
			target_read_u32(target, FLASH_FMC, &flash_fmc);
		} while (flash_fmc & FMC_MERASE);
	}

	return ERROR_OK;
}

COMMAND_HANDLER(tiva_handle_mass_erase_command)
{
	int i;

	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	if (tiva_mass_erase(bank) == ERROR_OK) {
		/* set all sectors as erased */
		for (i = 0; i < bank->num_sectors; i++)
			bank->sectors[i].is_erased = 1;

		command_print(CMD_CTX, "tiva mass erase complete");
	} else
		command_print(CMD_CTX, "tiva mass erase failed");

	return ERROR_OK;
}

/**
 * Perform the Stellaris "Recovering a 'Locked' Device procedure.
 * This performs a mass erase and then restores all nonvolatile registers
 * (including USER_* registers and flash lock bits) to their defaults.
 * Accordingly, flash can be reprogrammed, and JTAG can be used.
 *
 * NOTE that DustDevil parts (at least rev A0 silicon) have errata which
 * can affect this operation if flash protection has been enabled.
 */
COMMAND_HANDLER(tiva_handle_recover_command)
{
	struct flash_bank *bank;
	int retval;

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (retval != ERROR_OK)
		return retval;

	/* REVISIT ... it may be worth sanity checking that the AP is
	 * inactive before we start.  ARM documents that switching a DP's
	 * mode while it's active can cause fault modes that need a power
	 * cycle to recover.
	 */

	/* assert SRST */
	if (!(jtag_get_reset_config() & RESET_HAS_SRST)) {
		LOG_ERROR("Can't recover Tiva flash without SRST");
		return ERROR_FAIL;
	}
	adapter_assert_reset();

	for (int i = 0; i < 5; i++) {
		retval = dap_to_swd(bank->target);
		if (retval != ERROR_OK)
			goto done;

		retval = dap_to_jtag(bank->target);
		if (retval != ERROR_OK)
			goto done;
	}

	/* de-assert SRST */
	adapter_deassert_reset();
	retval = jtag_execute_queue();

	/* wait 400+ msec ... OK, "1+ second" is simpler */
	usleep(1000);

	/* USER INTERVENTION required for the power cycle
	 * Restarting OpenOCD is likely needed because of mode switching.
	 */
	LOG_INFO("USER ACTION:  "
		"power cycle Tiva chip, then restart OpenOCD.");

done:
	return retval;
}

static const struct command_registration tiva_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.usage = "<bank>",
		.handler = tiva_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.help = "erase entire device",
	},
	{
		.name = "recover",
		.handler = tiva_handle_recover_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "recover (and erase) locked device",
	},
	COMMAND_REGISTRATION_DONE
};
static const struct command_registration tiva_command_handlers[] = {
	{
		.name = "tiva",
		.mode = COMMAND_EXEC,
		.help = "Tiva flash command group",
		.usage = "",
		.chain = tiva_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver tiva_flash = {
	.name = "tiva",
	.commands = tiva_command_handlers,
	.flash_bank_command = tiva_flash_bank_command,
	.erase = tiva_erase,
	.protect = tiva_protect,
	.write = tiva_write,
	.read = default_flash_read,
	.probe = tiva_probe,
	.auto_probe = tiva_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = tiva_protect_check,
	.info = get_tiva_info,
};
