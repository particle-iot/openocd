/***************************************************************************
 *   Copyright (C) 2009 by Duane Ellis                                     *
 *   openocd@duaneellis.com                                                *
 *                                                                         *
 *   Copyright (C) 2010 by Olaf Lüke (at91sam3s* support)                  *
 *   olaf@uni-paderborn.de                                                 *
 *                                                                         *
 *   Copyright (C) 2011 by Olivier Schonken, Jim Norris                    *
 *   (at91sam3x* & at91sam4 support)*                                      *
 *                                                                         *
 *   Copyright (C) 2015 Morgan Quigley
 *   (at91samv7x support)
 *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS for A PARTICULAR PURPOSE.  See the         *
 *   GNU General public License for more details.                          *
 *                                                                         *
 ***************************************************************************/

/* Some of the the lower level code was based on code supplied by
 * ATMEL under this copyright. */

/* BEGIN ATMEL COPYRIGHT */
/* ----------------------------------------------------------------------------
 *         ATMEL Microcontroller Software Support
 * ----------------------------------------------------------------------------
 * Copyright (c) 2009, Atmel Corporation
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the disclaimer below.
 *
 * Atmel's name may not be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * DISCLAIMER: THIS SOFTWARE IS PROVIDED BY ATMEL "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE
 * DISCLAIMED. IN NO EVENT SHALL ATMEL BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ----------------------------------------------------------------------------
 */
/* END ATMEL COPYRIGHT */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/time_support.h>

#define REG_NAME_WIDTH  (12)

/* samv7x / sams7x series only has one flash bank)*/
#define FLASH_BANK_BASE   0x00400000

#define AT91C_EFC_FCMD_GETD   (0x0)	/* (EFC) Get Flash Descriptor */
#define AT91C_EFC_FCMD_WP     (0x1)	/* (EFC) Write Page */
#define AT91C_EFC_FCMD_WPL    (0x2)	/* (EFC) Write Page and Lock */
#define AT91C_EFC_FCMD_EWP    (0x3)	/* (EFC) Erase Page and Write Page */
#define AT91C_EFC_FCMD_EWPL   (0x4)	/* (EFC) Erase Page, Write Page then Lock*/
#define AT91C_EFC_FCMD_EA     (0x5)	/* (EFC) Erase All */
/* cmd6 is not present in the SAM V7x or SAM Sx data sheets */
#define AT91C_EFC_FCMD_EPA    (0x7)     /* (EFC) Erase pages */
#define AT91C_EFC_FCMD_SLB    (0x8)	/* (EFC) Set Lock Bit */
#define AT91C_EFC_FCMD_CLB    (0x9)	/* (EFC) Clear Lock Bit */
#define AT91C_EFC_FCMD_GLB    (0xA)	/* (EFC) Get Lock Bit */
#define AT91C_EFC_FCMD_SFB    (0xB)	/* (EFC) Set Fuse Bit */
#define AT91C_EFC_FCMD_CFB    (0xC)	/* (EFC) Clear Fuse Bit */
#define AT91C_EFC_FCMD_GFB    (0xD)	/* (EFC) Get Fuse Bit */
#define AT91C_EFC_FCMD_STUI   (0xE)	/* (EFC) Start Read Unique ID */
#define AT91C_EFC_FCMD_SPUI   (0xF)	/* (EFC) Stop Read Unique ID */

#define OFFSET_EFC_FMR    0
#define OFFSET_EFC_FCR    4
#define OFFSET_EFC_FSR    8
#define OFFSET_EFC_FRR   12

extern struct flash_driver at91samv7_flash;

static float _tomhz(uint32_t freq_hz)
{
	float f;

	f = ((float)(freq_hz)) / 1000000.0;
	return f;
}

/* How the chip is configured. */
struct samv7_cfg {
	uint32_t unique_id[4];

	uint32_t slow_freq;
	uint32_t rc_freq;
	uint32_t mainosc_freq;
	uint32_t plla_freq;
	uint32_t mclk_freq;
	uint32_t cpu_freq;
	uint32_t fclk_freq;
	uint32_t pclk0_freq;
	uint32_t pclk1_freq;
	uint32_t pclk2_freq;

#define SAMV7_CHIPID_CIDR          (0x400E0940)
	uint32_t CHIPID_CIDR;
#define SAMV7_CHIPID_EXID          (0x400E0944)
	uint32_t CHIPID_EXID;

#define SAMV7_PMC_BASE             (0x400E0600)
#define SAMV7_PMC_SCSR             (SAMV7_PMC_BASE + 0x0008)
	uint32_t PMC_SCSR;
#define SAMV7_PMC_PCSR             (SAMV7_PMC_BASE + 0x0018)
	uint32_t PMC_PCSR;
#define SAMV7_CKGR_UCKR            (SAMV7_PMC_BASE + 0x001c)
	uint32_t CKGR_UCKR;
#define SAMV7_CKGR_MOR             (SAMV7_PMC_BASE + 0x0020)
	uint32_t CKGR_MOR;
#define SAMV7_CKGR_MCFR            (SAMV7_PMC_BASE + 0x0024)
	uint32_t CKGR_MCFR;
#define SAMV7_CKGR_PLLAR           (SAMV7_PMC_BASE + 0x0028)
	uint32_t CKGR_PLLAR;
#define SAMV7_PMC_MCKR             (SAMV7_PMC_BASE + 0x0030)
	uint32_t PMC_MCKR;
#define SAMV7_PMC_PCK0             (SAMV7_PMC_BASE + 0x0040)
	uint32_t PMC_PCK0;
#define SAMV7_PMC_PCK1             (SAMV7_PMC_BASE + 0x0044)
	uint32_t PMC_PCK1;
#define SAMV7_PMC_PCK2             (SAMV7_PMC_BASE + 0x0048)
	uint32_t PMC_PCK2;
#define SAMV7_PMC_SR               (SAMV7_PMC_BASE + 0x0068)
	uint32_t PMC_SR;
#define SAMV7_PMC_IMR              (SAMV7_PMC_BASE + 0x006c)
	uint32_t PMC_IMR;
#define SAMV7_PMC_FSMR             (SAMV7_PMC_BASE + 0x0070)
	uint32_t PMC_FSMR;
#define SAMV7_PMC_FSPR             (SAMV7_PMC_BASE + 0x0074)
	uint32_t PMC_FSPR;
};

struct samv7_bank_private {
	int probed;
	/* DANGER: THERE ARE DRAGONS HERE.. */
	/* NOTE: If you add more 'ghost' pointers */
	/* be aware that you must *manually* update */
	/* these pointers in the function samv7_GetDetails() */
	/* See the comment "Here there be dragons" */

	/* so we can find the chip we belong to */
	struct samv7_chip *chip;
	/* so we can find the original bank pointer */
	struct flash_bank *bank;
	uint32_t controller_address;
	uint32_t base_address;
	uint32_t flash_wait_states;
	bool present;
	unsigned size_bytes;
	unsigned nsectors;
	unsigned sector_size;
	unsigned page_size;
};

struct samv7_chip_details {
	/* THERE ARE DRAGONS HERE.. */
	/* note: If you add pointers here */
	/* be careful about them as they */
	/* may need to be updated inside */
	/* the function: "samv7_GetDetails() */
	/* which copy/overwrites the */
	/* 'runtime' copy of this structure */
	uint32_t chipid_cidr;
	const char *name;

	unsigned n_gpnvms;
#define SAMV7_N_NVM_BITS 9
	unsigned gpnvm[SAMV7_N_NVM_BITS];
	unsigned total_flash_size;
	unsigned total_sram_size;
	/* "initialized" from the global const data */
	struct samv7_bank_private bank;
};

struct samv7_chip {
	struct samv7_chip *next;
	int probed;

	/* this is "initialized" from the global const structure */
	struct samv7_chip_details details;
	struct target *target;
	struct samv7_cfg cfg;
};


struct samv7_reg_list {
	uint32_t address;  size_t struct_offset; const char *name;
	void (*explain_func)(struct samv7_chip *pInfo);
};

static struct samv7_chip *all_samv7_chips;

static struct samv7_chip *get_current_samv7(struct command_context *cmd_ctx)
{
	struct target *t;
	static struct samv7_chip *p;

	t = get_current_target(cmd_ctx);
	if (!t) {
		command_print(cmd_ctx, "No current target?");
		return NULL;
	}

	p = all_samv7_chips;
	if (!p) {
		/* this should not happen */
		/* the command is not registered until the chip is created? */
		command_print(cmd_ctx, "No SAMV7 chips exist?");
		return NULL;
	}

	while (p) {
		if (p->target == t)
			return p;
		p = p->next;
	}
	command_print(cmd_ctx, "Cannot find SAMV7 chip?");
	return NULL;
}

/* The actual sector size of the SAMV7 flash memory is 128K bytes. 
 * 16 sectors for a 2048KB device. The lockregions are 16KB per lock 
 * region, with a 2048KB device having 128 lock regions.
 * For the best results, nsectors are thus set to the amount of lock 
 * regions, and the sector_size set to the lock region size. Page 
 * erases are used to erase 16KB sections when programming*/

/* these are used to *initialize* the "chip->details" structure. */
static const struct samv7_chip_details all_samv7_details[] = {
  /* Start atsamv71* series */
	/*atsamv71q21 - LQFP144/LFBGA144*/
	{
		.chipid_cidr    = 0xa1220e00,
		.name           = "atsamv71q21",
		.total_flash_size = 2048 * 1024,
		.total_sram_size  = 384 * 1024,
		.n_gpnvms       = 9,
		.bank = {
			.probed = 0,
			.chip  = NULL,
			.bank  = NULL,
			.base_address = FLASH_BANK_BASE,
			.controller_address = 0x400e0c00,
			.flash_wait_states = 6,
			.present = 1,
			.size_bytes =  2048 * 1024,
			.nsectors   =  128,
			.sector_size = 16384,
			.page_size   = 512,
		},
	},
	/* terminate the list of devices */
	{
		.chipid_cidr = 0,
		.name        = NULL,
	}
};

/* Globals above */
/***********************************************************************
 **********************************************************************
 **********************************************************************/
/* *ATMEL* style code - from the SAMV7x driver code */

/**
 * Get the current status of the EEFC and
 * the value of some status bits (LOCKE, PROGE).
 * @param bank - info about the bank
 * @param v    - result goes here
 */
static int samv7_efc_get_status(struct samv7_bank_private *bank, uint32_t *v)
{
	int r;
	r = target_read_u32(bank->chip->target,
			bank->controller_address + OFFSET_EFC_FSR, v);
	LOG_DEBUG("Status: 0x%08x (lockerror: %d, cmderror: %d, ready: %d)",
  		(unsigned int)(*v),
  		((unsigned int)((*v >> 2) & 1)),
  		((unsigned int)((*v >> 1) & 1)),
  		((unsigned int)((*v >> 0) & 1)));

	return r;
}

/**
 * Get the result of the last executed command.
 * @param bank - info about the bank
 * @param v    - result goes here
 */
static int samv7_efc_get_result(struct samv7_bank_private *bank, uint32_t *v)
{
	int r;
	uint32_t rv;
	r = target_read_u32(bank->chip->target,
			bank->controller_address + OFFSET_EFC_FRR,
			&rv);
	if (v)
		*v = rv;
	LOG_DEBUG("result: 0x%08x", ((unsigned int)(rv)));
	return r;
}

static int samv7_efc_start_command(struct samv7_bank_private *bank,
		unsigned command, unsigned argument)
{
	uint32_t n, v;
	int r;
	int retry;

	retry = 0;
do_retry:

	/* Check command & argument */
	switch (command) {
		case AT91C_EFC_FCMD_WP:
		case AT91C_EFC_FCMD_WPL:
		case AT91C_EFC_FCMD_EWP:
		case AT91C_EFC_FCMD_EWPL:
		/* case AT91C_EFC_FCMD_EPL: */
		case AT91C_EFC_FCMD_EPA:
		case AT91C_EFC_FCMD_SLB:
		case AT91C_EFC_FCMD_CLB:
			n = (bank->size_bytes / bank->page_size);
			if (argument >= n)
				LOG_ERROR("*BUG*: Embedded flash has only %u pages", (unsigned)(n));
			break;

		case AT91C_EFC_FCMD_SFB:
		case AT91C_EFC_FCMD_CFB:
			if (argument >= bank->chip->details.n_gpnvms) {
				LOG_ERROR("*BUG*: Embedded flash has only %d GPNVMs",
						bank->chip->details.n_gpnvms);
			}
			break;

		case AT91C_EFC_FCMD_GETD:
		case AT91C_EFC_FCMD_EA:
		case AT91C_EFC_FCMD_GLB:
		case AT91C_EFC_FCMD_GFB:
		case AT91C_EFC_FCMD_STUI:
		case AT91C_EFC_FCMD_SPUI:
			if (argument != 0)
				LOG_ERROR("Argument is meaningless for cmd: %d", command);
			break;
		default:
			LOG_ERROR("Unknown command %d", command);
			break;
	}

	if (command == AT91C_EFC_FCMD_SPUI) {
		/* this is a very special situation. */
		/* Situation (1) - error/retry - see below */
		/*      And we are being called recursively */
		/* Situation (2) - normal, finished reading unique id */
	} else {
		/* it should be "ready" */
		samv7_efc_get_status(bank, &v);
		if (v & 1) {
			/* then it is ready */
			/* we go on */
		} else {
			if (retry) {
				/* we have done this before */
				/* the controller is not responding. */
				LOG_ERROR("flash controller is not ready! Error");
				return ERROR_FAIL;
			} else {
				retry++;
				LOG_ERROR("Flash controller is not ready, attempting reset");
				/* we do that by issuing the *STOP* command */
				samv7_efc_start_command(bank, AT91C_EFC_FCMD_SPUI, 0);
				/* above is recursive, and further recursion is blocked by */
				/* if (command == AT91C_EFC_FCMD_SPUI) above */
				goto do_retry;
			}
		}
	}

	v = (0x5A << 24) | (argument << 8) | command;
	LOG_DEBUG("command: 0x%08x", ((unsigned int)(v)));
	r = target_write_u32(bank->bank->target,
			bank->controller_address + OFFSET_EFC_FCR, v);
	if (r != ERROR_OK)
		LOG_DEBUG("write failed");
	return r;
}

/**
 * Performs the given command and wait until its completion (or an error).
 * @param bank     - info about the bank
 * @param command  - Command to perform.
 * @param argument - Optional command argument.
 * @param status   - put command status bits here
 */
static int samv7_efc_perform_command(struct samv7_bank_private *bank,
		unsigned command, unsigned argument, uint32_t *status)
{
	int r;
	uint32_t v;
	long long ms_now, ms_end;

	if (status)
		*status = 0;

	r = samv7_efc_start_command(bank, command, argument);
	if (r != ERROR_OK)
		return r;

	ms_end = 10000 + timeval_ms();

	do {
		r = samv7_efc_get_status(bank, &v);
		if (r != ERROR_OK)
			return r;
		ms_now = timeval_ms();
		if (ms_now > ms_end) {
			/* error */
			LOG_ERROR("Command timeout");
			return ERROR_FAIL;
		}
	} while ((v & 1) == 0);

	/* error bits.. */
	if (status)
		*status = (v & 0x6);
	return ERROR_OK;
}

/**
 * Read the unique ID.
 * @param bank - info about the bank
 * The unique ID is stored in the 'bank' structure.
 */
static int samv7_read_unique_id(struct samv7_bank_private *bank)
{
	int r;
	uint32_t v;
	int x;
	bank->chip->cfg.unique_id[0] = 0;
	bank->chip->cfg.unique_id[1] = 0;
	bank->chip->cfg.unique_id[2] = 0;
	bank->chip->cfg.unique_id[3] = 0;

	r = samv7_efc_start_command(bank, AT91C_EFC_FCMD_STUI, 0);
	if (r < 0)
		return r;

	for (x = 0; x < 4; x++) {
		r = target_read_u32(bank->chip->target,
				bank->bank->base + (x * 4),
				&v);
		if (r < 0)
			return r;
		bank->chip->cfg.unique_id[x] = v;
	}

	r = samv7_efc_perform_command(bank, AT91C_EFC_FCMD_SPUI, 0, NULL);
	LOG_DEBUG("read unique id: result=%d, id = 0x%08x, 0x%08x, 0x%08x, 0x%08x",
  		r,
			(unsigned int)(bank->chip->cfg.unique_id[0]),
			(unsigned int)(bank->chip->cfg.unique_id[1]),
			(unsigned int)(bank->chip->cfg.unique_id[2]),
			(unsigned int)(bank->chip->cfg.unique_id[3]));
	return r;
}

/**
 * Erases the entire flash.
 * @param pPrivate - the info about the bank.
 */
static int samv7_erase_bank(struct samv7_bank_private *bank)
{
	return samv7_efc_perform_command(bank, AT91C_EFC_FCMD_EA, 0, NULL);
}

/**
 * Erase specified flash pages
 * @param bank - the info about the bank.
 */
static int samv7_erase_pages(struct samv7_bank_private *bank,
		int first_page, int num_pages, uint32_t *status)
{
	uint8_t erase_pages;
	switch (num_pages) {
		case 4:  erase_pages = 0x00; break;
		case 8:  erase_pages = 0x01; break;
		case 16: erase_pages = 0x02; break;
		case 32: erase_pages = 0x03; break;
		default: erase_pages = 0x00; break;
	}

	/* AT91C_EFC_FCMD_EPA
	 * According to the datasheet FARG[15:2] defines the page from which
	 * the erase will start.This page must be modulo 4, 8, 16 or 32
	 * according to the number of pages to erase. FARG[1:0] defines the
	 * number of pages to be erased. Previously (firstpage << 2) was used
	 * to conform to this, seems it should not be shifted...
	 */
	return samv7_efc_perform_command(bank, AT91C_EFC_FCMD_EPA,
			first_page | erase_pages, status);
}

/**
 * Gets current GPNVM state.
 * @param pPrivate - info about the bank.
 * @param gpnvm    -  GPNVM bit index.
 * @param out      - result stored here, if non-NULL
 */
/* ------------------------------------------------------------------------------ */
static int samv7_get_gpnvm(struct samv7_bank_private *bank, 
		unsigned gpnvm, unsigned *out)
{
	uint32_t v;
	int r;

	if (gpnvm >= bank->chip->details.n_gpnvms) {
		LOG_ERROR("invalid gpnvm %d, max: %d, ignored",
			gpnvm, bank->chip->details.n_gpnvms);
		return ERROR_FAIL;
	}

	/* Get GPNVMs status */
	r = samv7_efc_perform_command(bank, AT91C_EFC_FCMD_GFB, 0, NULL);
	if (r != ERROR_OK) {
		LOG_ERROR("samv7_get_gpnvm failed");
		return r;
	}

	r = samv7_efc_get_result(bank, &v);

	if (out) {
		/* Check if GPNVM is set */
		/* get the bit and make it a 0/1 */
		*out = (v >> gpnvm) & 1;
	}

	return r;
}

/**
 * Clears the selected GPNVM bit.
 * @param pPrivate info about the bank
 * @param gpnvm GPNVM index.
 * @returns 0 if successful; otherwise returns an error code.
 */
static int samv7_clear_gpnvm(struct samv7_bank_private *bank, unsigned gpnvm)
{
	int r;
	unsigned v;

	if (gpnvm >= bank->chip->details.n_gpnvms) {
		LOG_ERROR("invalid gpnvm %d, max: %d, ignored",
				gpnvm, bank->chip->details.n_gpnvms);
		return ERROR_FAIL;
	}

	r = samv7_get_gpnvm(bank, gpnvm, &v);
	if (r != ERROR_OK) {
		LOG_DEBUG("get gpnvm failed: %d", r);
		return r;
	}
	r = samv7_efc_perform_command(bank, AT91C_EFC_FCMD_CFB, gpnvm, NULL);
	LOG_DEBUG("clear gpnvm result: %d", r);
	return r;
}

/**
 * Sets the selected GPNVM bit.
 * @param pPrivate info about the bank
 * @param gpnvm GPNVM index.
 */
static int samv7_set_gpnvm(struct samv7_bank_private *bank, unsigned gpnvm)
{
	int r;
	unsigned v;
	if (gpnvm >= bank->chip->details.n_gpnvms) {
		LOG_ERROR("invalid gpnvm %d, max: %d, ignored",
				gpnvm, bank->chip->details.n_gpnvms);
		return ERROR_FAIL;
	}

	r = samv7_get_gpnvm(bank, gpnvm, &v);
	if (r != ERROR_OK)
		return r;
	if (v) {
		r = ERROR_OK; /* the gpnvm bit is already set */
	} else {
		/* we need to set it */
		r = samv7_efc_perform_command(bank, AT91C_EFC_FCMD_SFB, gpnvm, NULL);
	}
	return r;
}

/**
 * Returns a bit field (at most 64) of locked regions within a page.
 * @param pPrivate info about the bank
 * @param v where to store locked bits
 */
static int samv7_get_lock_bits(struct samv7_bank_private *bank, uint32_t *v)
{
	int r;
	r = samv7_efc_perform_command(bank, AT91C_EFC_FCMD_GLB, 0, NULL);
	if (r == ERROR_OK)	{
		samv7_efc_get_result(bank, v);
		samv7_efc_get_result(bank, v);
		samv7_efc_get_result(bank, v);
		r = samv7_efc_get_result(bank, v);
	}
	LOG_DEBUG("samv7_get_lock_bits result: %d", r);
	return r;
}

/**
 * Unlocks all the regions in the given address range.
 * @param pPrivate info about the bank
 * @param start_sector first sector to unlock
 * @param end_sector last (inclusive) to unlock
 */
static int samv7_flash_unlock(struct samv7_bank_private *bank,
		unsigned start_sector, unsigned end_sector)
{
	int r;
	uint32_t status;
	uint32_t pg;
	uint32_t pages_per_sector;

	pages_per_sector = bank->sector_size / bank->page_size;
	while (start_sector <= end_sector) {
		pg = start_sector * pages_per_sector;
		r = samv7_efc_perform_command(bank, AT91C_EFC_FCMD_CLB, pg, &status);
		if (r != ERROR_OK)
			return r;
		start_sector++;
	}
	return ERROR_OK;
}

/**
 * Locks regions
 * @param pPrivate - info about the bank
 * @param start_sector - first sector to lock
 * @param end_sector   - last sector (inclusive) to lock
 */
static int samv7_flash_lock(struct samv7_bank_private *bank,
		unsigned start_sector, unsigned end_sector)
{
	uint32_t status;
	uint32_t pg;
	uint32_t pages_per_sector;
	int r;

	pages_per_sector = bank->sector_size / bank->page_size;
	while (start_sector <= end_sector) {
		pg = start_sector * pages_per_sector;

		r = samv7_efc_perform_command(bank, AT91C_EFC_FCMD_SLB, pg, &status);
		if (r != ERROR_OK)
			return r;
		start_sector++;
	}
	return ERROR_OK;
}

/****** END SAMV7 CODE ********/

/* begin helpful debug code */
/* print the fieldname, the field value, in dec & hex, and return field value */
#define SAMV7_REG_TO_STR_MAXLEN 256
static uint32_t samv7_extract_reg(struct samv7_chip *chip,
		const char *regname, uint32_t value, unsigned shift, unsigned width,
		char *out)
{
	uint32_t v;
	int hwidth, dwidth;

	/* extract the field */
	v = value >> shift;
	v = v & ((1 << width)-1);
	if (width <= 16) {
		hwidth = 4;
		dwidth = 5;
	} else {
		hwidth = 8;
		dwidth = 12;
	}

	if (out)
		snprintf(out, SAMV7_REG_TO_STR_MAXLEN,
				"\t%*s: %*" PRId32 " [0x%0*" PRIx32 "]",
				REG_NAME_WIDTH, regname, dwidth, v, hwidth, v);
	return v;
}

static const char _unknown[] = "unknown";
static const char *const eproc_names[] = {
	"cortex-m7",  /* 0 */
	"arm946es",   /* 1 */
	"arm7tdmi",   /* 2 */
	"cortex-m3",  /* 3 */
	"arm920t",    /* 4 */
	"arm926ejs",  /* 5 */
	"cortex-a5",  /* 6 */
	"cortex-m4",  /* 7 */
	_unknown,     /* 8 */
	_unknown,     /* 9 */
	_unknown,     /* 10 */
	_unknown,     /* 11 */
	_unknown,     /* 12 */
	_unknown,     /* 13 */
	_unknown,     /* 14 */
	_unknown,     /* 15 */
};

#define nvpsize2 nvpsize		/* these two tables are identical */
static const char *const nvpsize[] = {
	"none",        /*  0 */
	"8K bytes",    /*  1 */
	"16K bytes",   /*  2 */
	"32K bytes",   /*  3 */
	_unknown,      /*  4 */
	"64K bytes",   /*  5 */
	_unknown,      /*  6 */
	"128K bytes",  /*  7 */
	_unknown,      /*  8 */
	"256K bytes",  /*  9 */
	"512K bytes",  /* 10 */
	_unknown,      /* 11 */
	"1024K bytes", /* 12 */
	_unknown,      /* 13 */
	"2048K bytes", /* 14 */
	_unknown,      /* 15 */
};

static const char *const sramsize[] = {
	"48K Bytes",   /*  0 */
	"192K Bytes",  /*  1 */
	"384K Bytes",  /*  2 */
	"6K Bytes",    /*  3 */
	"24K Bytes",   /*  4 */
	"4K Bytes",    /*  5 */
	"80K Bytes",   /*  6 */
	"160K Bytes",  /*  7 */
	"8K Bytes",    /*  8 */
	"16K Bytes",   /*  9 */
	"32K Bytes",   /* 10 */
	"64K Bytes",   /* 11 */
	"128K Bytes",  /* 12 */
	"256K Bytes",  /* 13 */
	"96K Bytes",   /* 14 */
	"512K Bytes",  /* 15 */
};

static const struct archnames { unsigned value; const char *name; } archnames[] = {
	{ 0x10, "SAM E70 series"                               },
	{ 0x11, "SAM S70 series"                               },
	{ 0x12, "SAM V71 series"                               },
	{ 0x13, "SAM V70 series"                               },
	{ 0x19, "AT91SAM9xx Series"                            },
	{ 0x29, "AT91SAM9XExx Series"                          },
	{ 0x34, "AT91x34 Series"                               },
	{ 0x37, "CAP7 Series"                                  },
	{ 0x39, "CAP9 Series"                                  },
	{ 0x3B, "CAP11 Series"                                 },
	{ 0x3C, "ATSAM4E"                                      },
	{ 0x40, "AT91x40 Series"                               },
	{ 0x42, "AT91x42 Series"                               },
	{ 0x43, "SAMG51 Series"                                },
	{ 0x47, "SAMG53 Series"                                },
	{ 0x55, "AT91x55 Series"                               },
	{ 0x60, "AT91SAM7Axx Series"                           },
	{ 0x61, "AT91SAM7AQxx Series"                          },
	{ 0x63, "AT91x63 Series"                               },
	{ 0x70, "AT91SAM7Sxx Series"                           },
	{ 0x71, "AT91SAM7XCxx Series"                          },
	{ 0x72, "AT91SAM7SExx Series"                          },
	{ 0x73, "AT91SAM7Lxx Series"                           },
	{ 0x75, "AT91SAM7Xxx Series"                           },
	{ 0x76, "AT91SAM7SLxx Series"                          },
	{ 0x80, "ATSAM3UxC Series (100-pin version)"           },
	{ 0x81, "ATSAM3UxE Series (144-pin version)"           },
	{ 0x83, "ATSAM3A/SAM4A xC Series (100-pin version)"    },
	{ 0x84, "ATSAM3X/SAM4X xC Series (100-pin version)"    },
	{ 0x85, "ATSAM3X/SAM4X xE Series (144-pin version)"    },
	{ 0x86, "ATSAM3X/SAM4X xG Series (208/217-pin version)"},
	{ 0x88, "ATSAM3S/SAM4S xA Series (48-pin version)"	   },
	{ 0x89, "ATSAM3S/SAM4S xB Series (64-pin version)"	   },
	{ 0x8A, "ATSAM3S/SAM4S xC Series (100-pin version)"    },
	{ 0x92, "AT91x92 Series"                               },
	{ 0x93, "ATSAM3NxA Series (48-pin version)"            },
	{ 0x94, "ATSAM3NxB Series (64-pin version)"            },
	{ 0x95, "ATSAM3NxC Series (100-pin version)"           },
	{ 0x98, "ATSAM3SDxA Series (48-pin version)"           },
	{ 0x99, "ATSAM3SDxB Series (64-pin version)"           },
	{ 0x9A, "ATSAM3SDxC Series (100-pin version)"          },
	{ 0xA5, "ATSAM5A"                                      },
	{ 0xF0, "AT75Cxx Series"                               },
	{   -1, NULL                                           },
};

static const char *const nvptype[] = {
	"rom",	                                  /* 0 */
	"romless or onchip flash",	              /* 1 */
	"embedded flash memory",                  /* 2 */
	"rom(nvpsiz) + embedded flash (nvpsiz2)",	/* 3 */
	"sram emulating flash",                   /* 4 */
	_unknown,	                                /* 5 */
	_unknown,	                                /* 6 */
	_unknown,	                                /* 7 */
};

static const char *_yes_or_no(uint32_t v)
{
	if (v)
		return "YES";
	else
		return "NO";
}

static const char *const _rc_freq[] = {
	"4 MHz", "8 MHz", "12 MHz", "reserved"
};

static void samv7_explain_ckgr_mor(struct samv7_chip *chip)
{
	uint32_t v;
	uint32_t rcen;
	char str[SAMV7_REG_TO_STR_MAXLEN];

	v = samv7_extract_reg(chip, "MOSCXTEN", chip->cfg.CKGR_MOR, 0, 1, str);
	LOG_INFO("%s (main xtal enabled: %s)", str, _yes_or_no(v));
	v = samv7_extract_reg(chip, "MOSCXTBY", chip->cfg.CKGR_MOR, 1, 1, str);
	LOG_INFO("%s (main osc bypass: %s)", str, _yes_or_no(v));
	rcen = samv7_extract_reg(chip, "MOSCRCEN", chip->cfg.CKGR_MOR, 3, 1, str);
	LOG_INFO("%s (onchip RC-OSC enabled: %s)", str, _yes_or_no(rcen));
	v = samv7_extract_reg(chip, "MOSCRCF", chip->cfg.CKGR_MOR, 4, 3, str);
	LOG_INFO("%s (onchip RC-OSC freq: %s)", str, _rc_freq[v]);
	chip->cfg.rc_freq = 0;
	if (rcen) {
		switch (v) {
			default: chip->cfg.rc_freq =  0; break;
			case 0:  chip->cfg.rc_freq =  4 * 1000 * 1000; break;
			case 1:  chip->cfg.rc_freq =  8 * 1000 * 1000; break;
			case 2:  chip->cfg.rc_freq = 12 * 1000 * 1000; break;
		}
	}
	v = samv7_extract_reg(chip, "MOSCXTST", chip->cfg.CKGR_MOR, 8, 8, str);
	LOG_INFO("%s (startup clks, time= %f uSecs)",
			str, ((float)(v * 1000000)) / ((float)(chip->cfg.slow_freq)));
	v = samv7_extract_reg(chip, "MOSCSEL", chip->cfg.CKGR_MOR, 24, 1, str);
	LOG_INFO("%s (mainosc source: %s)", str, v ? "crystal" : "internal RC");
	v = samv7_extract_reg(chip, "CFDEN", chip->cfg.CKGR_MOR, 25, 1, str);
	LOG_INFO("%s (clock failure enabled: %s)", str, _yes_or_no(v));
}

static void samv7_explain_chipid_cidr(struct samv7_chip *chip)
{
	int x;
	uint32_t v;
	const char *cp;
	char str[SAMV7_REG_TO_STR_MAXLEN];

	/* print the requested register fields for the user */
	samv7_extract_reg(chip, "Version", chip->cfg.CHIPID_CIDR, 0, 5, str);
	LOG_INFO("%s", str);

	v = samv7_extract_reg(chip, "EPROC", chip->cfg.CHIPID_CIDR, 5, 3, str);
	LOG_INFO("%s %s", str, eproc_names[v]);

	v = samv7_extract_reg(chip, "NVPSIZE", chip->cfg.CHIPID_CIDR, 8, 4, str);
	LOG_INFO("%s %s", str, nvpsize[v]);

	v = samv7_extract_reg(chip, "NVPSIZE2", chip->cfg.CHIPID_CIDR, 12, 4, str);
	LOG_INFO("%s %s", str, nvpsize2[v]);

	v = samv7_extract_reg(chip, "SRAMSIZE", chip->cfg.CHIPID_CIDR, 16, 4, str);
	LOG_INFO("%s %s", str, sramsize[v]);

	v = samv7_extract_reg(chip, "ARCH", chip->cfg.CHIPID_CIDR, 20, 8, str);
	cp = _unknown;
	for (x = 0; archnames[x].name; x++) {
		if (v == archnames[x].value) {
			cp = archnames[x].name;
			break;
		}
	}

	LOG_INFO("%s %s", str, cp);

	v = samv7_extract_reg(chip, "NVPTYP", chip->cfg.CHIPID_CIDR, 28, 3, str);
	LOG_INFO("%s %s", str, nvptype[v]);

	v = samv7_extract_reg(chip, "EXTID", chip->cfg.CHIPID_CIDR, 31, 1, str);
	LOG_INFO("%s (exists: %s)", str, _yes_or_no(v));
}

static void samv7_explain_ckgr_mcfr(struct samv7_chip *chip)
{
	uint32_t v;
	char str[SAMV7_REG_TO_STR_MAXLEN];

	/* print the requested register fields for the user */
	v = samv7_extract_reg(chip, "MAINFRDY", chip->cfg.CKGR_MCFR, 16, 1, str);
	LOG_INFO("%s (main ready: %s)", str, _yes_or_no(v));

	v = samv7_extract_reg(chip, "MAINF", chip->cfg.CKGR_MCFR, 0, 16, str);

	v = (v * chip->cfg.slow_freq) / 16;
	chip->cfg.mainosc_freq = v;

	LOG_INFO("%s (%3.03f Mhz (%" PRIu32 ".%03" PRIu32 "khz slowclk)",
			str,
			_tomhz(v),
			(uint32_t)(chip->cfg.slow_freq / 1000),
			(uint32_t)(chip->cfg.slow_freq % 1000));
}

static void samv7_explain_ckgr_plla(struct samv7_chip *chip)
{
	uint32_t mula, diva;
	char str[SAMV7_REG_TO_STR_MAXLEN];

	/* print the requested register fields for the user */
	diva = samv7_extract_reg(chip, "DIVA", chip->cfg.CKGR_PLLAR, 0, 8, str);
	LOG_INFO("%s", str);
	mula = samv7_extract_reg(chip, "MULA", chip->cfg.CKGR_PLLAR, 16, 11, str);
	LOG_INFO("%s", str);
	chip->cfg.plla_freq = 0;
	if (mula == 0)
		LOG_INFO("\tPLLA Freq: (Disabled,mula = 0)");
	else if (diva == 0)
		LOG_INFO("\tPLLA Freq: (Disabled,diva = 0)");
	else if (diva >= 1) {
		chip->cfg.plla_freq = (chip->cfg.mainosc_freq * (mula + 1) / diva);
		LOG_INFO("\tPLLA Freq: %3.03f MHz",
			_tomhz(chip->cfg.plla_freq));
	}
}

static void samv7_explain_mckr(struct samv7_chip *chip)
{
	uint32_t css, pres, fin = 0;
	int pdiv = 0;
	const char *cp = NULL;
	char str[SAMV7_REG_TO_STR_MAXLEN];

	css = samv7_extract_reg(chip, "CSS", chip->cfg.PMC_MCKR, 0, 2, str);
	switch (css & 3) {
		case 0:
			fin = chip->cfg.slow_freq;
			cp = "slowclk";
			break;
		case 1:
			fin = chip->cfg.mainosc_freq;
			cp  = "mainosc";
			break;
		case 2:
			fin = chip->cfg.plla_freq;
			cp  = "plla";
			break;
		case 3:
			if (chip->cfg.CKGR_UCKR & (1 << 16)) {
				fin = 480 * 1000 * 1000;
				cp = "upll";
			} else {
				fin = 0;
				cp  = "upll (*ERROR* UPLL is disabled)";
			}
			break;
		default:
			assert(0);
			break;
	}

	LOG_INFO("%s %s (%3.03f Mhz)", str, cp, _tomhz(fin));
	pres = samv7_extract_reg(chip, "PRES", chip->cfg.PMC_MCKR, 4, 3, str);
	switch (pres & 0x07) {
		case 0:
			pdiv = 1;
			cp = "selected clock";
			break;
		case 1:
			pdiv = 2;
			cp = "clock/2";
			break;
		case 2:
			pdiv = 4;
			cp = "clock/4";
			break;
		case 3:
			pdiv = 8;
			cp = "clock/8";
			break;
		case 4:
			pdiv = 16;
			cp = "clock/16";
			break;
		case 5:
			pdiv = 32;
			cp = "clock/32";
			break;
		case 6:
			pdiv = 64;
			cp = "clock/64";
			break;
		case 7:
			pdiv = 6;
			cp = "clock/6";
			break;
		default:
			assert(0);
			break;
	}
	LOG_INFO("%s (%s)", str, cp);
	fin = fin / pdiv;
  /* TODO: revisit this logic; the SAMV7 clock tree is more complex. We  *
   * need to account for the MCLK divider downstream of the CPU clock    */
	chip->cfg.cpu_freq = fin;
	chip->cfg.mclk_freq = fin;
	chip->cfg.fclk_freq = fin;
	LOG_INFO("\t\tResulting CPU Freq: %3.03f", _tomhz(fin));
}

static uint32_t *samv7_get_reg_ptr(struct samv7_cfg *cfg,
		const struct samv7_reg_list *reg_list)
{
	/* this function exists to help keep funky offsetof() errors
	 * and casting from causing bugs. By using prototypes, we can detect 
   * what would be casting errors.                                     */
	return (uint32_t *)(void *)(((char *)(cfg)) + reg_list->struct_offset);
}

#define SAMV7_ENTRY(NAME, FUNC)  { .address = SAMV7_ ## NAME, .struct_offset = offsetof( \
						  struct samv7_cfg, \
						  NAME), # NAME, FUNC }
static const struct samv7_reg_list samv7_all_regs[] = {
	SAMV7_ENTRY(CKGR_MOR, samv7_explain_ckgr_mor),
	SAMV7_ENTRY(CKGR_MCFR, samv7_explain_ckgr_mcfr),
	SAMV7_ENTRY(CKGR_PLLAR, samv7_explain_ckgr_plla),
	SAMV7_ENTRY(CKGR_UCKR, NULL),
	SAMV7_ENTRY(PMC_FSMR, NULL),
	SAMV7_ENTRY(PMC_FSPR, NULL),
	SAMV7_ENTRY(PMC_IMR, NULL),
	SAMV7_ENTRY(PMC_MCKR, samv7_explain_mckr),
	SAMV7_ENTRY(PMC_PCK0, NULL),
	SAMV7_ENTRY(PMC_PCK1, NULL),
	SAMV7_ENTRY(PMC_PCK2, NULL),
	SAMV7_ENTRY(PMC_PCSR, NULL),
	SAMV7_ENTRY(PMC_SCSR, NULL),
	SAMV7_ENTRY(PMC_SR, NULL),
	SAMV7_ENTRY(CHIPID_CIDR, samv7_explain_chipid_cidr),
	SAMV7_ENTRY(CHIPID_EXID, NULL),
	/* TERMINATE THE LIST */
	{ .name = NULL }
};
#undef SAMV7_ENTRY

static struct samv7_bank_private *get_samv7_bank_private(struct flash_bank *bank)
{
	return bank->driver_priv;
}

/**
 * Given a pointer to where it goes in the structure,
 * determine the register name, address from the all registers table.
 */
static const struct samv7_reg_list *samv7_get_reg(struct samv7_chip *chip, 
		uint32_t *reg_ptr)
{
	const struct samv7_reg_list *reg_list;
	uint32_t *p_possible;

	reg_list = &(samv7_all_regs[0]);
	while (reg_list->name) {
		/* calculate where this one go.. it is "possibly" this register. */
		p_possible = (uint32_t *)((char *)&chip->cfg + reg_list->struct_offset);
		if (p_possible == reg_ptr) /* well? Is it this register */
			return reg_list; /* Jump for joy! */
		reg_list++; /* wasn't this one. move on to the next... */
	}
	LOG_ERROR("invalid samv7 register");
	return NULL;
}

static int samv7_read_reg(struct samv7_chip *chip, uint32_t *out)
{
	const struct samv7_reg_list *reg;
	int r;

	reg = samv7_get_reg(chip, out);
	if (!reg)
		return ERROR_FAIL;

	r = target_read_u32(chip->target, reg->address, out);
	if (r != ERROR_OK) {
		LOG_ERROR("cannot read samv7 register: %s @ 0x%08x, error %d",
  			reg->name, (unsigned)(reg->address), r);
	}
	return r;
}

static int samv7_read_all_regs(struct samv7_chip *chip)
{
	int r;
	const struct samv7_reg_list *reg;

	reg = &(samv7_all_regs[0]);
	while (reg->name) {
		r = samv7_read_reg(chip, samv7_get_reg_ptr(&(chip->cfg), reg));
		if (r != ERROR_OK) {
			LOG_ERROR("Cannot read samv7 register: %s @ 0x%08x, Error: %d",
				reg->name, ((unsigned)(reg->address)), r);
			return r;
		}
		reg++;
	}
	return ERROR_OK;
}

static int samv7_get_info(struct samv7_chip *chip)
{
	const struct samv7_reg_list *reg;
	uint32_t regval;

	reg = &(samv7_all_regs[0]);
	while (reg->name) {
		/* display all regs */
		regval = *samv7_get_reg_ptr(&(chip->cfg), reg);
		LOG_DEBUG("%*s: [0x%08" PRIx32 "] -> 0x%08" PRIx32,
				REG_NAME_WIDTH, reg->name, reg->address, regval);
		if (reg->explain_func)
			(*(reg->explain_func))(chip);
		reg++;
	}
	LOG_DEBUG("   rc-osc: %3.03f MHz", _tomhz(chip->cfg.rc_freq));
	LOG_DEBUG("  mainosc: %3.03f MHz", _tomhz(chip->cfg.mainosc_freq));
	LOG_DEBUG("     plla: %3.03f MHz", _tomhz(chip->cfg.plla_freq));
	LOG_USER(" cpu-freq: %3.03f MHz", _tomhz(chip->cfg.cpu_freq));
	LOG_USER("mclk-freq: %3.03f MHz", _tomhz(chip->cfg.mclk_freq));

	LOG_USER(" UniqueId (hex): 0x%08" PRIx32 "_%08" PRIx32 "_%08" PRIx32 "_%08" PRIx32,
			chip->cfg.unique_id[0], chip->cfg.unique_id[1],
			chip->cfg.unique_id[2], chip->cfg.unique_id[3]);

	return ERROR_OK;
}

static int samv7_protect_check(struct flash_bank *bank)
{
	int r;
	uint32_t v[4] = {0};
	unsigned x;
	struct samv7_bank_private *bank_private;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	bank_private = get_samv7_bank_private(bank);
	if (!bank_private) {
		LOG_ERROR("no private for this bank?");
		return ERROR_FAIL;
	}
	if (!(bank_private->probed))
		return ERROR_FLASH_BANK_NOT_PROBED;

	r = samv7_get_lock_bits(bank_private, v);
	if (r != ERROR_OK) {
		LOG_DEBUG("Failed: %d", r);
		return r;
	}

	for (x = 0; x < bank_private->nsectors; x++)
		bank->sectors[x].is_protected = (!!(v[x >> 5] & (1 << (x % 32))));
	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(samv7_flash_bank_command)
{
	struct samv7_chip *chip;

	chip = all_samv7_chips;

	/* is this an existing chip? */
	while (chip) {
		if (chip->target == bank->target)
			break;
		chip = chip->next;
	}

	if (!chip) {
		/* this is a *NEW* chip */
		chip = calloc(1, sizeof(struct samv7_chip));
		if (!chip) {
			LOG_ERROR("NO RAM!");
			return ERROR_FAIL;
		}
		chip->target = bank->target;
		/* insert at head */
		chip->next = all_samv7_chips;
		all_samv7_chips = chip;
		chip->target = bank->target;
		/* assumption is this runs at 32khz */
		chip->cfg.slow_freq = 32768;
		chip->probed = 0;
	}

	switch (bank->base) {
		default:
			LOG_ERROR("Address 0x%08x invalid bank address (try 0x%08x"
					"[at91samv7 series] )",
					(unsigned int)(bank->base),
					(unsigned int)(FLASH_BANK_BASE));
			return ERROR_FAIL;
			break;

		/* at91samv7 series only has bank 0*/
		case FLASH_BANK_BASE:
			bank->driver_priv = &(chip->details.bank);
			chip->details.bank.chip = chip;
			chip->details.bank.bank = bank;
			break;
	}

	/* we initialize after probing. */
	return ERROR_OK;
}

static int samv7_get_details(struct samv7_bank_private *bank)
{
	const struct samv7_chip_details *details;
	struct samv7_chip *chip;
	struct flash_bank *saved_bank;

	details = all_samv7_details;
	while (details->name) {
		/* Compare cidr without version bits */
		if (details->chipid_cidr == (bank->chip->cfg.CHIPID_CIDR & 0xFFFFFFE0))
			break;
		else
			details++;
	}
	if (details->name == NULL) {
		LOG_ERROR("SAMV7 ChipID 0x%08x not found in table (perhaps you can ID this chip?)",
				(unsigned int)(bank->chip->cfg.CHIPID_CIDR));
		/* Help the victim, print details about the chip */
		LOG_INFO("SAMV7 CHIPID_CIDR: 0x%08" PRIx32 " decodes as follows",
				bank->chip->cfg.CHIPID_CIDR);
		samv7_explain_chipid_cidr(bank->chip);
		return ERROR_FAIL;
	}

	/* DANGER: THERE ARE DRAGONS HERE */
	/* get our chip - it is going to be over-written shortly */
	chip = bank->chip;

	/* Note that, in reality: */
	/*  */
	/*     pPrivate = &(chip->details.bank[0]) */
	/* or  pPrivate = &(chip->details.bank[1]) */
	/*  */

	/* save the "bank" pointers */
	saved_bank = chip->details.bank.bank;

	/* Overwrite the "details" structure. */
	memcpy(&(bank->chip->details), details, sizeof(bank->chip->details));

	/* now fix the ghosted pointers */
	chip->details.bank.chip = chip;
	chip->details.bank.bank = saved_bank;

	/* update the *BANK*SIZE* */
	return ERROR_OK;
}

static int _samv7_probe(struct flash_bank *bank, int noise)
{
	unsigned x;
	int r;
	struct samv7_bank_private *bank_private;

	LOG_DEBUG("Begin: Noise: %d", noise);
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	bank_private = get_samv7_bank_private(bank);
	if (!bank_private) {
		LOG_ERROR("Invalid/unknown bank number");
		return ERROR_FAIL;
	}

	r = samv7_read_all_regs(bank_private->chip);
	if (r != ERROR_OK)
		return r;

	LOG_DEBUG("Here");
	if (bank_private->chip->probed)
		r = samv7_get_info(bank_private->chip);
	else
		r = samv7_get_details(bank_private);
	if (r != ERROR_OK)
		return r;

	/* update the flash bank size */
	if (bank->base == bank_private->chip->details.bank.base_address)
		bank->size = bank_private->chip->details.bank.size_bytes;

	if (bank->sectors == NULL) {
		bank->sectors = calloc(bank_private->nsectors, 
				(sizeof((bank->sectors)[0])));
		if (bank->sectors == NULL) {
			LOG_ERROR("No memory!");
			return ERROR_FAIL;
		}
		bank->num_sectors = bank_private->nsectors;

		for (x = 0; ((int)(x)) < bank->num_sectors; x++) {
			bank->sectors[x].size = bank_private->sector_size;
			bank->sectors[x].offset = x * (bank_private->sector_size);
			/* mark as unknown */
			bank->sectors[x].is_erased = -1;
			bank->sectors[x].is_protected = -1;
		}
	}

	bank_private->probed = 1;

	r = samv7_protect_check(bank);
	if (r != ERROR_OK)
		return r;

	samv7_read_unique_id(bank_private);

	return r;
}

static int samv7_probe(struct flash_bank *bank)
{
	return _samv7_probe(bank, 1);
}

static int samv7_auto_probe(struct flash_bank *bank)
{
	return _samv7_probe(bank, 0);
}

static int samv7_erase(struct flash_bank *bank, int first, int last)
{
	struct samv7_bank_private *bank_private;
	int r;
	int i;
	int page_count;
	/*16 pages equals 8KB - Same size as a lock region*/
	page_count = 16;
	uint32_t status;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	r = samv7_auto_probe(bank);
	if (r != ERROR_OK) {
		return r;
	}

	bank_private = get_samv7_bank_private(bank);
	if (!(bank_private->probed))
		return ERROR_FLASH_BANK_NOT_PROBED;

	if ((first == 0) && ((last + 1) == (int)(bank_private->nsectors))) {
		/* whole chip */
		return samv7_erase_bank(bank_private);
	}
	LOG_INFO("samv7 does not auto-erase; erasing relevant sectors...");
	LOG_INFO("samv7 first: 0x%08x last: 0x%08x", 
			(unsigned int)(first), (unsigned int)(last));
	for (i = first; i <= last; i++) {
		/*16 pages equals 8KB - Same size as a lock region*/
		r = samv7_erase_pages(bank_private, (i * page_count), page_count, &status);
		LOG_INFO("erasing sector: 0x%08x", (unsigned int)(i));
		if (r != ERROR_OK)
			LOG_ERROR("error performing erase page @ lock region number %d",
					(unsigned int)(i));
		if (status & (1 << 2)) {
			LOG_ERROR("lock region %d is locked", (unsigned int)(i));
			return ERROR_FAIL;
		}
		if (status & (1 << 1)) {
			LOG_ERROR("flash command error @lock region %d", (unsigned int)(i));
			return ERROR_FAIL;
		}
	}
	return ERROR_OK;
}

static int samv7_protect(struct flash_bank *bank, int set, int first, int last)
{
	struct samv7_bank_private *bank_private;
	int r;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	bank_private = get_samv7_bank_private(bank);
	if (!(bank_private->probed))
		return ERROR_FLASH_BANK_NOT_PROBED;

	if (set)
		r = samv7_flash_lock(bank_private, (unsigned)(first), (unsigned)(last));
	else
		r = samv7_flash_unlock(bank_private, (unsigned)(first), (unsigned)(last));

	return r;
}

static int samv7_page_read(struct samv7_bank_private *bank_private, 
		unsigned pagenum, uint8_t *buf)
{
	uint32_t adr;
	int r;

	adr = pagenum * bank_private->page_size;
	adr = adr + bank_private->base_address;

	r = target_read_memory(bank_private->chip->target,
			adr,
			4,					/* THIS*MUST*BE* in 32bit values */
			bank_private->page_size / 4,
			buf);
	if (r != ERROR_OK)
		LOG_ERROR("flash program failed to read page phys address: 0x%08x",
				(unsigned int)(adr));
	return r;
}

static int samv7_page_write(struct samv7_bank_private *bank, 
		unsigned pagenum, const uint8_t *buf)
{
	uint32_t adr;
	uint32_t status;
	uint32_t fmr;	/* EEFC Flash Mode Register */
	int r;

	adr = pagenum * bank->page_size;
	adr = (adr + bank->base_address);

	/* Get flash mode register value */
	r = target_read_u32(bank->chip->target, bank->controller_address, &fmr);
	if (r != ERROR_OK)
		LOG_DEBUG("Error Read failed: read flash mode register");

	/* Clear flash wait state field */
	fmr &= 0xfffff0ff;

	/* set FWS (flash wait states) field in the FMR (flash mode register) */
	fmr |= (bank->flash_wait_states << 8);

	LOG_DEBUG("flash mode: 0x%08x", ((unsigned int)(fmr)));
	r = target_write_u32(bank->bank->target, bank->controller_address, fmr);
	if (r != ERROR_OK)
		LOG_DEBUG("write failed: set flash mode register");

	/* 1st sector 8kBytes - page 0 - 15*/
	/* 2nd sector 8kBytes - page 16 - 30*/
	/* 3rd sector 48kBytes - page 31 - 127*/
	LOG_DEBUG("write page %u at address 0x%08x", pagenum, (unsigned int)(adr));
	r = target_write_memory(bank->chip->target,
			adr,
			4,					/* must be in 32bit values */
			bank->page_size / 4,
			buf);
	if (r != ERROR_OK) {
		LOG_ERROR("failed to write (buffer) page at 0x%08x",
			(unsigned int)(adr));
		return r;
	}

	/* send Erase & Write Page */
	r = samv7_efc_perform_command(bank,
			AT91C_EFC_FCMD_WP,	/*AT91C_EFC_FCMD_EWP only works on first two 8kb sectors*/
			pagenum,
			&status);

	if (r != ERROR_OK)
		LOG_ERROR("error performing write page at 0x%08x",
			(unsigned int)(adr));
	if (status & (1 << 2)) {
		LOG_ERROR("page at 0x%08x is locked", (unsigned int)(adr));
		return ERROR_FAIL;
	}
	if (status & (1 << 1)) {
		LOG_ERROR("SAMV7: Flash Command error @phys address 0x%08x", (unsigned int)(adr));
		return ERROR_FAIL;
	}
	return ERROR_OK;
}

static int samv7_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	int n;
	unsigned page_cur;
	unsigned page_end;
	int r;
	unsigned page_offset;
	struct samv7_bank_private *bank_private;
	uint8_t *pagebuffer;

	/* incase we bail further below, set this to null */
	pagebuffer = NULL;

	/* ignore dumb requests */
	if (count == 0) {
		r = ERROR_OK;
		goto done;
	}

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("target not halted");
		r = ERROR_TARGET_NOT_HALTED;
		goto done;
	}

	bank_private = get_samv7_bank_private(bank);
	if (!(bank_private->probed)) {
		r = ERROR_FLASH_BANK_NOT_PROBED;
		goto done;
	}

	if ((offset + count) > bank_private->size_bytes) {
		LOG_ERROR("flash write error - past end of bank");
		LOG_ERROR(" offset: 0x%08x, count 0x%08x, bank end: 0x%08x",
				(unsigned int)(offset),
				(unsigned int)(count),
				(unsigned int)(bank_private->size_bytes));
		r = ERROR_FAIL;
		goto done;
	}

	pagebuffer = malloc(bank_private->page_size);
	if (!pagebuffer) {
		LOG_ERROR("no memory for %d-byte page buffer", 
				(int)(bank_private->page_size));
		r = ERROR_FAIL;
		goto done;
	}

	page_cur = offset / bank_private->page_size;
	page_end = (offset + count - 1) / bank_private->page_size;

	LOG_DEBUG("offset: 0x%08x, count: 0x%08x", 
			(unsigned int)(offset), (unsigned int)(count));
	LOG_DEBUG("page start: %d, page end: %d", (int)(page_cur), (int)(page_end));

	/* Special case: all one page */
	/* Otherwise:                 */
	/*    (1) non-aligned start   */
	/*    (2) body pages          */
	/*    (3) non-aligned end.    */

	/* handle special case - all one page. */
	if (page_cur == page_end) {
		LOG_DEBUG("special case, all in one page");
		r = samv7_page_read(bank_private, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;

		page_offset = (offset & (bank_private->page_size-1));
		memcpy(pagebuffer + page_offset, buffer, count);

		r = samv7_page_write(bank_private, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;
		r = ERROR_OK;
		goto done;
	}

	/* step 1) handle the non-aligned starting address */
	page_offset = offset & (bank_private->page_size - 1);
	if (page_offset) {
		LOG_DEBUG("non-aligned start");
		/* read the partial page */
		r = samv7_page_read(bank_private, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;

		/* over-write with new data */
		n = (bank_private->page_size - page_offset);
		memcpy(pagebuffer + page_offset, buffer, n);

		r = samv7_page_write(bank_private, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;

		count  -= n;
		offset += n;
		buffer += n;
		page_cur++;
	}

	/* By checking that offset is correct here, we also fix a clang warning */
	assert(offset % bank_private->page_size == 0);

	/* step 2) handle the intermediate large pages, and the final page, * 
	 *         if that page is a full page                              */
	LOG_DEBUG("full page loop: cur=%d, end=%d, count = 0x%08x",
			(int)page_cur, (int)page_end, (unsigned int)(count));

	while ((page_cur < page_end) &&
			(count >= bank_private->page_size)) {
		r = samv7_page_write(bank_private, page_cur, buffer);
		if (r != ERROR_OK)
			goto done;
		count -= bank_private->page_size;
		buffer += bank_private->page_size;
		page_cur += 1;
	}

	/* step 3) write final page, if it's partial (otherwise it's already done) */
	if (count) {
		LOG_DEBUG("final partial page, count = 0x%08x", (unsigned int)(count));
		/* we have a partial page */
		r = samv7_page_read(bank_private, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;
		memcpy(pagebuffer, buffer, count); /* data goes at start of page */
		r = samv7_page_write(bank_private, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;
	}
	LOG_DEBUG("Done!");
	r = ERROR_OK;
done:
	if (pagebuffer)
		free(pagebuffer);
	return r;
}

COMMAND_HANDLER(samv7_handle_info_command)
{
	struct samv7_chip *chip;
	chip = get_current_samv7(CMD_CTX);
	if (!chip)
		return ERROR_OK;

	unsigned x;
	int r;

	/* bank must exist before we can do anything */
	if (chip->details.bank.bank == NULL) {
		x = 0;
		command_print(CMD_CTX,
				"Please define bank %d via command: flash bank %s ... ",
				x, at91samv7_flash.name);
		return ERROR_FAIL;
	}

	/* if bank is not probed, then probe it */
	if (!(chip->details.bank.probed)) {
		r = samv7_auto_probe(chip->details.bank.bank);
		if (r != ERROR_OK)
			return ERROR_FAIL;
	}
	/* above guarantees the "chip details" structure is valid */
	/* and thus, bank private areas are valid */
	/* and we have a SAMV7 chip, what a concept! */

	r = samv7_get_info(chip);
	if (r != ERROR_OK) {
		LOG_DEBUG("Samv7Info, Failed %d", r);
		return r;
	}

	return ERROR_OK;
}

COMMAND_HANDLER(samv7_handle_gpnvm_command)
{
	unsigned x, v;
	int r, who;
	struct samv7_chip *chip;

	chip = get_current_samv7(CMD_CTX);
	if (!chip)
		return ERROR_OK;

	if (chip->target->state != TARGET_HALTED) {
		LOG_ERROR("samv7 - target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (chip->details.bank.bank == NULL) {
		command_print(CMD_CTX, "Bank must be defined first via: flash bank %s ...",
				at91samv7_flash.name);
		return ERROR_FAIL;
	}
	if (!chip->details.bank.probed) {
		r = samv7_auto_probe(chip->details.bank.bank);
		if (r != ERROR_OK)
			return r;
	}

	switch (CMD_ARGC) {
		default:
			return ERROR_COMMAND_SYNTAX_ERROR;
			break;
		case 0:
			goto showall;
			break;
		case 1:
			who = -1;
			break;
		case 2:
			if ((!strcmp(CMD_ARGV[0], "show")) && (!strcmp(CMD_ARGV[1], "all" )))
				who = -1;
			else {
				uint32_t v32;
				COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], v32);
				who = v32;
			}
			break;
	}

	if (!strcmp("show", CMD_ARGV[0])) {
		if (who == -1) {
showall:
			r = ERROR_OK;
			for (x = 0; x < chip->details.n_gpnvms; x++) {
				r = samv7_get_gpnvm(&(chip->details.bank), x, &v);
				if (r != ERROR_OK)
					break;
				command_print(CMD_CTX, "samv7-gpnvm%u: %u", x, v);
			}
			return r;
		}
		if ((who >= 0) && (((unsigned)(who)) < chip->details.n_gpnvms)) {
			r = samv7_get_gpnvm(&(chip->details.bank), who, &v);
			command_print(CMD_CTX, "samv7-gpnvm%u: %u", who, v);
			return r;
		} else {
			command_print(CMD_CTX, "invalid gpnvm: %u", who);
			return ERROR_COMMAND_SYNTAX_ERROR;
		}
	}

	if (who == -1) {
		command_print(CMD_CTX, "missing gpnvm number");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (!strcmp("set", CMD_ARGV[0]))
		r = samv7_set_gpnvm(&(chip->details.bank), who);
	else if (!strcmp("clr", CMD_ARGV[0]) || !strcmp("clear", CMD_ARGV[0]))
		r = samv7_clear_gpnvm(&(chip->details.bank), who);
	else {
		command_print(CMD_CTX, "unknown command: %s", CMD_ARGV[0]);
		r = ERROR_COMMAND_SYNTAX_ERROR;
	}
	return r;
}

COMMAND_HANDLER(samv7_handle_slowclk_command)
{
	struct samv7_chip *chip;

	chip = get_current_samv7(CMD_CTX);
	if (!chip)
		return ERROR_OK;

	switch (CMD_ARGC) {
		case 0: /* show */
			break;
		case 1: /* set */
		{
			uint32_t v;
			COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], v);
			if (v > 200000) {
				command_print(CMD_CTX, "illegal slow clock freq: %d\n", (int)(v));
				return ERROR_COMMAND_SYNTAX_ERROR;
			}
			chip->cfg.slow_freq = v;
			break;
		}
		default:
			/* error */
			command_print(CMD_CTX, "too many parameters");
			return ERROR_COMMAND_SYNTAX_ERROR;
			break;
	}
	command_print(CMD_CTX, "slowclk freq: %d.%03dkhz",
			(int)(chip->cfg.slow_freq / 1000),
			(int)(chip->cfg.slow_freq % 1000));
	return ERROR_OK;
}

static const struct command_registration at91samv7_exec_command_handlers[] = {
	{
		.name = "gpnvm",
		.handler = samv7_handle_gpnvm_command,
		.mode = COMMAND_EXEC,
		.usage = "[('clr'|'set'|'show') bitnum]",
		.help = "Without arguments, shows all bits in the gpnvm "
			"register.  Otherwise, clears, sets, or shows one "
			"General Purpose Non-Volatile Memory (gpnvm) bit.",
	},
	{
		.name = "info",
		.handler = samv7_handle_info_command,
		.mode = COMMAND_EXEC,
		.help = "Print information about the current at91samv7 chip"
			"and its flash configuration.",
	},
	{
		.name = "slowclk",
		.handler = samv7_handle_slowclk_command,
		.mode = COMMAND_EXEC,
		.usage = "[clock_hz]",
		.help = "Display or set the slowclock frequency "
			"(default 32768 Hz).",
	},
	COMMAND_REGISTRATION_DONE
};
static const struct command_registration at91samv7_command_handlers[] = {
	{
		.name = "at91samv7",
		.mode = COMMAND_ANY,
		.help = "at91samv7 flash command group",
		.usage = "",
		.chain = at91samv7_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver at91samv7_flash = {
	.name = "at91samv7",
	.commands = at91samv7_command_handlers,
	.flash_bank_command = samv7_flash_bank_command,
	.erase = samv7_erase,
	.protect = samv7_protect,
	.write = samv7_write,
	.read = default_flash_read,
	.probe = samv7_probe,
	.auto_probe = samv7_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = samv7_protect_check,
};
