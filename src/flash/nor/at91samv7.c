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

/* samv7x / sams7x series has only one flash bank)*/
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

#define offset_EFC_FMR    0
#define offset_EFC_FCR    4
#define offset_EFC_FSR    8
#define offset_EFC_FRR   12

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
	struct samv7_chip *pChip;
	/* so we can find the original bank pointer */
	struct flash_bank *pBank;
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

/*The actual sector size of the SAMV7 flash memory is 128K bytes. 16 sectors for a 2048KB device*/
/*The lockregions are 16KB per lock region, with a 2048KB device having 128 lock regions. */
/*For the best results, nsectors are thus set to the amount of lock regions, and the sector_size*/
/*set to the lock region size.  Page erases are used to erase 16KB sections when programming*/

/* these are used to *initialize* the "pChip->details" structure. */
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
			.pChip  = NULL,
			.pBank  = NULL,
			.base_address = FLASH_BANK_BASE,
			.controller_address = 0x400e0c00,
			.flash_wait_states = 6,	/* workaround silicon bug */
			.present = 1,
			.size_bytes =  2024 * 1024,
			.nsectors   =  128,
			.sector_size = 16384,
			.page_size   = 512,
		},
	},

	/* terminate */
	{
		.chipid_cidr = 0,
		.name        = NULL,
	}
};

/* Globals above */
/***********************************************************************
 **********************************************************************
 **********************************************************************
 **********************************************************************
 **********************************************************************
 **********************************************************************/
/* *ATMEL* style code - from the SAMV7x driver code */

/**
 * Get the current status of the EEFC and
 * the value of some status bits (LOCKE, PROGE).
 * @param pPrivate - info about the bank
 * @param v        - result goes here
 */
static int EFC_GetStatus(struct samv7_bank_private *pPrivate, uint32_t *v)
{
	int r;
	r = target_read_u32(pPrivate->pChip->target,
			pPrivate->controller_address + offset_EFC_FSR,
			v);
	LOG_DEBUG("Status: 0x%08x (lockerror: %d, cmderror: %d, ready: %d)",
		(unsigned int)(*v),
		((unsigned int)((*v >> 2) & 1)),
		((unsigned int)((*v >> 1) & 1)),
		((unsigned int)((*v >> 0) & 1)));

	return r;
}

/**
 * Get the result of the last executed command.
 * @param pPrivate - info about the bank
 * @param v        - result goes here
 */
static int EFC_GetResult(struct samv7_bank_private *pPrivate, uint32_t *v)
{
	int r;
	uint32_t rv;
	r = target_read_u32(pPrivate->pChip->target,
			pPrivate->controller_address + offset_EFC_FRR,
			&rv);
	if (v)
		*v = rv;
	LOG_DEBUG("Result: 0x%08x", ((unsigned int)(rv)));
	return r;
}

static int EFC_StartCommand(struct samv7_bank_private *pPrivate,
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
			n = (pPrivate->size_bytes / pPrivate->page_size);
			if (argument >= n)
				LOG_ERROR("*BUG*: Embedded flash has only %u pages", (unsigned)(n));
			break;

		case AT91C_EFC_FCMD_SFB:
		case AT91C_EFC_FCMD_CFB:
			if (argument >= pPrivate->pChip->details.n_gpnvms) {
				LOG_ERROR("*BUG*: Embedded flash has only %d GPNVMs",
						pPrivate->pChip->details.n_gpnvms);
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
		EFC_GetStatus(pPrivate, &v);
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
				EFC_StartCommand(pPrivate, AT91C_EFC_FCMD_SPUI, 0);
				/* above is recursive, and further recursion is blocked by */
				/* if (command == AT91C_EFC_FCMD_SPUI) above */
				goto do_retry;
			}
		}
	}

	v = (0x5A << 24) | (argument << 8) | command;
	LOG_DEBUG("Command: 0x%08x", ((unsigned int)(v)));
	r = target_write_u32(pPrivate->pBank->target,
			pPrivate->controller_address + offset_EFC_FCR, v);
	if (r != ERROR_OK)
		LOG_DEBUG("Error Write failed");
	return r;
}

/**
 * Performs the given command and wait until its completion (or an error).
 * @param pPrivate - info about the bank
 * @param command  - Command to perform.
 * @param argument - Optional command argument.
 * @param status   - put command status bits here
 */
static int EFC_PerformCommand(struct samv7_bank_private *pPrivate,
	unsigned command,
	unsigned argument,
	uint32_t *status)
{

	int r;
	uint32_t v;
	long long ms_now, ms_end;

	/* default */
	if (status)
		*status = 0;

	r = EFC_StartCommand(pPrivate, command, argument);
	if (r != ERROR_OK)
		return r;

	ms_end = 10000 + timeval_ms();

	do {
		r = EFC_GetStatus(pPrivate, &v);
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
 * @param pPrivate - info about the bank
 * The unique ID is stored in the 'pPrivate' structure.
 */
static int FLASHD_ReadUniqueID(struct samv7_bank_private *pPrivate)
{
	int r;
	uint32_t v;
	int x;
	/* assume 0 */
	pPrivate->pChip->cfg.unique_id[0] = 0;
	pPrivate->pChip->cfg.unique_id[1] = 0;
	pPrivate->pChip->cfg.unique_id[2] = 0;
	pPrivate->pChip->cfg.unique_id[3] = 0;

	LOG_DEBUG("Begin");
	r = EFC_StartCommand(pPrivate, AT91C_EFC_FCMD_STUI, 0);
	if (r < 0)
		return r;

	for (x = 0; x < 4; x++) {
		r = target_read_u32(pPrivate->pChip->target,
				pPrivate->pBank->base + (x * 4),
				&v);
		if (r < 0)
			return r;
		pPrivate->pChip->cfg.unique_id[x] = v;
	}

	r = EFC_PerformCommand(pPrivate, AT91C_EFC_FCMD_SPUI, 0, NULL);
	LOG_DEBUG("End: R=%d, id = 0x%08x, 0x%08x, 0x%08x, 0x%08x",
		r,
		(unsigned int)(pPrivate->pChip->cfg.unique_id[0]),
		(unsigned int)(pPrivate->pChip->cfg.unique_id[1]),
		(unsigned int)(pPrivate->pChip->cfg.unique_id[2]),
		(unsigned int)(pPrivate->pChip->cfg.unique_id[3]));
	return r;

}

/**
 * Erases the entire flash.
 * @param pPrivate - the info about the bank.
 */
static int FLASHD_EraseEntireBank(struct samv7_bank_private *pPrivate)
{
	LOG_DEBUG("Here");
	return EFC_PerformCommand(pPrivate, AT91C_EFC_FCMD_EA, 0, NULL);
}

/**
 * Erases the entire flash.
 * @param pPrivate - the info about the bank.
 */
static int FLASHD_ErasePages(struct samv7_bank_private *pPrivate,
							 int firstPage,
							 int numPages,
							 uint32_t *status)
{
	LOG_DEBUG("Here");
	uint8_t erasePages;
	switch (numPages)	{
		case 4:
			erasePages = 0x00;
			break;
		case 8:
			erasePages = 0x01;
			break;
		case 16:
			erasePages = 0x02;
			break;
		case 32:
			erasePages = 0x03;
			break;
		default:
			erasePages = 0x00;
			break;
	}

	/* AT91C_EFC_FCMD_EPA
	 * According to the datasheet FARG[15:2] defines the page from which
	 * the erase will start.This page must be modulo 4, 8, 16 or 32
	 * according to the number of pages to erase. FARG[1:0] defines the
	 * number of pages to be erased. Previously (firstpage << 2) was used
	 * to conform to this, seems it should not be shifted...
	 */
	return EFC_PerformCommand(pPrivate,
		/* send Erase Page */
		AT91C_EFC_FCMD_EPA,
		(firstPage) | erasePages,
		status);
}

/**
 * Gets current GPNVM state.
 * @param pPrivate - info about the bank.
 * @param gpnvm    -  GPNVM bit index.
 * @param puthere  - result stored here.
 */
/* ------------------------------------------------------------------------------ */
static int FLASHD_GetGPNVM(struct samv7_bank_private *pPrivate, unsigned gpnvm, unsigned *puthere)
{
	uint32_t v;
	int r;

	LOG_DEBUG("Here");
	if (gpnvm >= pPrivate->pChip->details.n_gpnvms) {
		LOG_ERROR("Invalid GPNVM %d, max: %d, ignored",
			gpnvm, pPrivate->pChip->details.n_gpnvms);
		return ERROR_FAIL;
	}

	/* Get GPNVMs status */
	r = EFC_PerformCommand(pPrivate, AT91C_EFC_FCMD_GFB, 0, NULL);
	if (r != ERROR_OK) {
		LOG_ERROR("Failed");
		return r;
	}

	r = EFC_GetResult(pPrivate, &v);

	if (puthere) {
		/* Check if GPNVM is set */
		/* get the bit and make it a 0/1 */
		*puthere = (v >> gpnvm) & 1;
	}

	return r;
}

/**
 * Clears the selected GPNVM bit.
 * @param pPrivate info about the bank
 * @param gpnvm GPNVM index.
 * @returns 0 if successful; otherwise returns an error code.
 */
static int FLASHD_ClrGPNVM(struct samv7_bank_private *pPrivate, unsigned gpnvm)
{
	int r;
	unsigned v;

	LOG_DEBUG("Here");
	if (gpnvm >= pPrivate->pChip->details.n_gpnvms) {
		LOG_ERROR("Invalid GPNVM %d, max: %d, ignored",
			gpnvm, pPrivate->pChip->details.n_gpnvms);
		return ERROR_FAIL;
	}

	r = FLASHD_GetGPNVM(pPrivate, gpnvm, &v);
	if (r != ERROR_OK) {
		LOG_DEBUG("Failed: %d", r);
		return r;
	}
	r = EFC_PerformCommand(pPrivate, AT91C_EFC_FCMD_CFB, gpnvm, NULL);
	LOG_DEBUG("End: %d", r);
	return r;
}

/**
 * Sets the selected GPNVM bit.
 * @param pPrivate info about the bank
 * @param gpnvm GPNVM index.
 */
static int FLASHD_SetGPNVM(struct samv7_bank_private *pPrivate, unsigned gpnvm)
{
	int r;
	unsigned v;
	if (gpnvm >= pPrivate->pChip->details.n_gpnvms) {
		LOG_ERROR("Invalid GPNVM %d, max: %d, ignored",
			gpnvm, pPrivate->pChip->details.n_gpnvms);
		return ERROR_FAIL;
	}

	r = FLASHD_GetGPNVM(pPrivate, gpnvm, &v);
	if (r != ERROR_OK)
		return r;
	if (v) {
		/* already set */
		r = ERROR_OK;
	} else {
		/* set it */
		r = EFC_PerformCommand(pPrivate, AT91C_EFC_FCMD_SFB, gpnvm, NULL);
	}
	return r;
}

/**
 * Returns a bit field (at most 64) of locked regions within a page.
 * @param pPrivate info about the bank
 * @param v where to store locked bits
 */
static int FLASHD_GetLockBits(struct samv7_bank_private *pPrivate, uint32_t *v)
{
	int r;
	LOG_DEBUG("Here");
	r = EFC_PerformCommand(pPrivate, AT91C_EFC_FCMD_GLB, 0, NULL);
	if (r == ERROR_OK)	{
		EFC_GetResult(pPrivate, v);
		EFC_GetResult(pPrivate, v);
		EFC_GetResult(pPrivate, v);
		r = EFC_GetResult(pPrivate, v);
	}
	LOG_DEBUG("End: %d", r);
	return r;
}

/**
 * Unlocks all the regions in the given address range.
 * @param pPrivate info about the bank
 * @param start_sector first sector to unlock
 * @param end_sector last (inclusive) to unlock
 */

static int FLASHD_Unlock(struct samv7_bank_private *pPrivate,
	unsigned start_sector,
	unsigned end_sector)
{
	int r;
	uint32_t status;
	uint32_t pg;
	uint32_t pages_per_sector;

	pages_per_sector = pPrivate->sector_size / pPrivate->page_size;

	/* Unlock all pages */
	while (start_sector <= end_sector) {
		pg = start_sector * pages_per_sector;

		r = EFC_PerformCommand(pPrivate, AT91C_EFC_FCMD_CLB, pg, &status);
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
static int FLASHD_Lock(struct samv7_bank_private *pPrivate,
	unsigned start_sector,
	unsigned end_sector)
{
	uint32_t status;
	uint32_t pg;
	uint32_t pages_per_sector;
	int r;

	pages_per_sector = pPrivate->sector_size / pPrivate->page_size;

	/* Lock all pages */
	while (start_sector <= end_sector) {
		pg = start_sector * pages_per_sector;

		r = EFC_PerformCommand(pPrivate, AT91C_EFC_FCMD_SLB, pg, &status);
		if (r != ERROR_OK)
			return r;
		start_sector++;
	}
	return ERROR_OK;
}

/****** END SAMV7 CODE ********/

/* begin helpful debug code */
/* print the fieldname, the field value, in dec & hex, and return field value */
static uint32_t samv7_reg_fieldname(struct samv7_chip *pChip,
	const char *regname,
	uint32_t value,
	unsigned shift,
	unsigned width)
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

	/* show the basics */
	LOG_USER_N("\t%*s: %*" PRId32 " [0x%0*" PRIx32 "] ",
		REG_NAME_WIDTH, regname,
		dwidth, v,
		hwidth, v);
	return v;
}

static const char _unknown[] = "unknown";
static const char *const eproc_names[] = {
	"cortex-m7",				/* 0 */
	"arm946es",					/* 1 */
	"arm7tdmi",					/* 2 */
	"cortex-m3",				/* 3 */
	"arm920t",					/* 4 */
	"arm926ejs",				/* 5 */
	"cortex-a5",				/* 6 */
	"cortex-m4",				/* 7 */
	_unknown,					/* 8 */
	_unknown,					/* 9 */
	_unknown,					/* 10 */
	_unknown,					/* 11 */
	_unknown,					/* 12 */
	_unknown,					/* 13 */
	_unknown,					/* 14 */
	_unknown,					/* 15 */
};

#define nvpsize2 nvpsize		/* these two tables are identical */
static const char *const nvpsize[] = {
	"none",						/*  0 */
	"8K bytes",					/*  1 */
	"16K bytes",				/*  2 */
	"32K bytes",				/*  3 */
	_unknown,					/*  4 */
	"64K bytes",				/*  5 */
	_unknown,					/*  6 */
	"128K bytes",				/*  7 */
	_unknown,					/*  8 */
	"256K bytes",				/*  9 */
	"512K bytes",				/* 10 */
	_unknown,					/* 11 */
	"1024K bytes",				/* 12 */
	_unknown,					/* 13 */
	"2048K bytes",				/* 14 */
	_unknown,					/* 15 */
};

static const char *const sramsize[] = {
	"48K Bytes",				/*  0 */
	"192K Bytes",					/*  1 */
	"384K Bytes",					/*  2 */
	"6K Bytes",					/*  3 */
	"24K Bytes",				/*  4 */
	"4K Bytes",					/*  5 */
	"80K Bytes",				/*  6 */
	"160K Bytes",				/*  7 */
	"8K Bytes",					/*  8 */
	"16K Bytes",				/*  9 */
	"32K Bytes",				/* 10 */
	"64K Bytes",				/* 11 */
	"128K Bytes",				/* 12 */
	"256K Bytes",				/* 13 */
	"96K Bytes",				/* 14 */
	"512K Bytes",				/* 15 */
};

static const struct archnames { unsigned value; const char *name; } archnames[] = {
	{ 0x10,  "SAM E70 series" },
	{ 0x11,  "SAM S70 series" },
	{ 0x12,  "SAM V71 series" },
	{ 0x13,  "SAM V70 series" },
	{ 0x19,  "AT91SAM9xx Series"                                            },
	{ 0x29,  "AT91SAM9XExx Series"                                          },
	{ 0x34,  "AT91x34 Series"                                                       },
	{ 0x37,  "CAP7 Series"                                                          },
	{ 0x39,  "CAP9 Series"                                                          },
	{ 0x3B,  "CAP11 Series"                                                         },
	{ 0x3C, "ATSAM4E"                                                               },
	{ 0x40,  "AT91x40 Series"                                                       },
	{ 0x42,  "AT91x42 Series"                                                       },
	{ 0x43,  "SAMG51 Series"
	},
	{ 0x47,  "SAMG53 Series"
	},
	{ 0x55,  "AT91x55 Series"                                                       },
	{ 0x60,  "AT91SAM7Axx Series"                                           },
	{ 0x61,  "AT91SAM7AQxx Series"                                          },
	{ 0x63,  "AT91x63 Series"                                                       },
	{ 0x70,  "AT91SAM7Sxx Series"                                           },
	{ 0x71,  "AT91SAM7XCxx Series"                                          },
	{ 0x72,  "AT91SAM7SExx Series"                                          },
	{ 0x73,  "AT91SAM7Lxx Series"                                           },
	{ 0x75,  "AT91SAM7Xxx Series"                                           },
	{ 0x76,  "AT91SAM7SLxx Series"                                          },
	{ 0x80,  "ATSAM3UxC Series (100-pin version)"           },
	{ 0x81,  "ATSAM3UxE Series (144-pin version)"           },
	{ 0x83,  "ATSAM3A/SAM4A xC Series (100-pin version)"},
	{ 0x84,  "ATSAM3X/SAM4X xC Series (100-pin version)"},
	{ 0x85,  "ATSAM3X/SAM4X xE Series (144-pin version)"},
	{ 0x86,  "ATSAM3X/SAM4X xG Series (208/217-pin version)"	},
	{ 0x88,  "ATSAM3S/SAM4S xA Series (48-pin version)"	},
	{ 0x89,  "ATSAM3S/SAM4S xB Series (64-pin version)"	},
	{ 0x8A,  "ATSAM3S/SAM4S xC Series (100-pin version)"},
	{ 0x92,  "AT91x92 Series"                                                       },
	{ 0x93,  "ATSAM3NxA Series (48-pin version)"            },
	{ 0x94,  "ATSAM3NxB Series (64-pin version)"            },
	{ 0x95,  "ATSAM3NxC Series (100-pin version)"           },
	{ 0x98,  "ATSAM3SDxA Series (48-pin version)"           },
	{ 0x99,  "ATSAM3SDxB Series (64-pin version)"           },
	{ 0x9A,  "ATSAM3SDxC Series (100-pin version)"          },
	{ 0xA5,  "ATSAM5A"                                                              },
	{ 0xF0,  "AT75Cxx Series"                                                       },
	{ -1, NULL },
};

static const char *const nvptype[] = {
	"rom",	/* 0 */
	"romless or onchip flash",	/* 1 */
	"embedded flash memory",/* 2 */
	"rom(nvpsiz) + embedded flash (nvpsiz2)",	/* 3 */
	"sram emulating flash",	/* 4 */
	_unknown,	/* 5 */
	_unknown,	/* 6 */
	_unknown,	/* 7 */
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

static void samv7_explain_ckgr_mor(struct samv7_chip *pChip)
{
	uint32_t v;
	uint32_t rcen;

	v = samv7_reg_fieldname(pChip, "MOSCXTEN", pChip->cfg.CKGR_MOR, 0, 1);
	LOG_USER("(main xtal enabled: %s)", _yes_or_no(v));
	v = samv7_reg_fieldname(pChip, "MOSCXTBY", pChip->cfg.CKGR_MOR, 1, 1);
	LOG_USER("(main osc bypass: %s)", _yes_or_no(v));
	rcen = samv7_reg_fieldname(pChip, "MOSCRCEN", pChip->cfg.CKGR_MOR, 3, 1);
	LOG_USER("(onchip RC-OSC enabled: %s)", _yes_or_no(rcen));
	v = samv7_reg_fieldname(pChip, "MOSCRCF", pChip->cfg.CKGR_MOR, 4, 3);
	LOG_USER("(onchip RC-OSC freq: %s)", _rc_freq[v]);

	pChip->cfg.rc_freq = 0;
	if (rcen) {
		switch (v) {
			default:
				pChip->cfg.rc_freq = 0;
				break;
			case 0:
				pChip->cfg.rc_freq = 4 * 1000 * 1000;
				break;
			case 1:
				pChip->cfg.rc_freq = 8 * 1000 * 1000;
				break;
			case 2:
				pChip->cfg.rc_freq = 12 * 1000 * 1000;
				break;
		}
	}

	v = samv7_reg_fieldname(pChip, "MOSCXTST", pChip->cfg.CKGR_MOR, 8, 8);
	LOG_USER("(startup clks, time= %f uSecs)",
		((float)(v * 1000000)) / ((float)(pChip->cfg.slow_freq)));
	v = samv7_reg_fieldname(pChip, "MOSCSEL", pChip->cfg.CKGR_MOR, 24, 1);
	LOG_USER("(mainosc source: %s)",
		v ? "external xtal" : "internal RC");

	v = samv7_reg_fieldname(pChip, "CFDEN", pChip->cfg.CKGR_MOR, 25, 1);
	LOG_USER("(clock failure enabled: %s)",
		_yes_or_no(v));
}

static void samv7_explain_chipid_cidr(struct samv7_chip *pChip)
{
	int x;
	uint32_t v;
	const char *cp;

	samv7_reg_fieldname(pChip, "Version", pChip->cfg.CHIPID_CIDR, 0, 5);
	LOG_USER_N("\n");

	v = samv7_reg_fieldname(pChip, "EPROC", pChip->cfg.CHIPID_CIDR, 5, 3);
	LOG_USER("%s", eproc_names[v]);

	v = samv7_reg_fieldname(pChip, "NVPSIZE", pChip->cfg.CHIPID_CIDR, 8, 4);
	LOG_USER("%s", nvpsize[v]);

	v = samv7_reg_fieldname(pChip, "NVPSIZE2", pChip->cfg.CHIPID_CIDR, 12, 4);
	LOG_USER("%s", nvpsize2[v]);

	v = samv7_reg_fieldname(pChip, "SRAMSIZE", pChip->cfg.CHIPID_CIDR, 16, 4);
	LOG_USER("%s", sramsize[v]);

	v = samv7_reg_fieldname(pChip, "ARCH", pChip->cfg.CHIPID_CIDR, 20, 8);
	cp = _unknown;
	for (x = 0; archnames[x].name; x++) {
		if (v == archnames[x].value) {
			cp = archnames[x].name;
			break;
		}
	}

	LOG_USER("%s", cp);

	v = samv7_reg_fieldname(pChip, "NVPTYP", pChip->cfg.CHIPID_CIDR, 28, 3);
	LOG_USER("%s", nvptype[v]);

	v = samv7_reg_fieldname(pChip, "EXTID", pChip->cfg.CHIPID_CIDR, 31, 1);
	LOG_USER("(exists: %s)", _yes_or_no(v));
}

static void samv7_explain_ckgr_mcfr(struct samv7_chip *pChip)
{
	uint32_t v;

	v = samv7_reg_fieldname(pChip, "MAINFRDY", pChip->cfg.CKGR_MCFR, 16, 1);
	LOG_USER("(main ready: %s)", _yes_or_no(v));

	v = samv7_reg_fieldname(pChip, "MAINF", pChip->cfg.CKGR_MCFR, 0, 16);

	v = (v * pChip->cfg.slow_freq) / 16;
	pChip->cfg.mainosc_freq = v;

	LOG_USER("(%3.03f Mhz (%" PRIu32 ".%03" PRIu32 "khz slowclk)",
		_tomhz(v),
		(uint32_t)(pChip->cfg.slow_freq / 1000),
		(uint32_t)(pChip->cfg.slow_freq % 1000));
}

static void samv7_explain_ckgr_plla(struct samv7_chip *pChip)
{
	uint32_t mula, diva;

	diva = samv7_reg_fieldname(pChip, "DIVA", pChip->cfg.CKGR_PLLAR, 0, 8);
	LOG_USER_N("\n");
	mula = samv7_reg_fieldname(pChip, "MULA", pChip->cfg.CKGR_PLLAR, 16, 11);
	LOG_USER_N("\n");
	pChip->cfg.plla_freq = 0;
	if (mula == 0)
		LOG_USER("\tPLLA Freq: (Disabled,mula = 0)");
	else if (diva == 0)
		LOG_USER("\tPLLA Freq: (Disabled,diva = 0)");
	else if (diva >= 1) {
		pChip->cfg.plla_freq = (pChip->cfg.mainosc_freq * (mula + 1) / diva);
		LOG_USER("\tPLLA Freq: %3.03f MHz",
			_tomhz(pChip->cfg.plla_freq));
	}
}

static void samv7_explain_mckr(struct samv7_chip *pChip)
{
	uint32_t css, pres, fin = 0;
	int pdiv = 0;
	const char *cp = NULL;

	css = samv7_reg_fieldname(pChip, "CSS", pChip->cfg.PMC_MCKR, 0, 2);
	switch (css & 3) {
		case 0:
			fin = pChip->cfg.slow_freq;
			cp = "slowclk";
			break;
		case 1:
			fin = pChip->cfg.mainosc_freq;
			cp  = "mainosc";
			break;
		case 2:
			fin = pChip->cfg.plla_freq;
			cp  = "plla";
			break;
		case 3:
			if (pChip->cfg.CKGR_UCKR & (1 << 16)) {
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

	LOG_USER("%s (%3.03f Mhz)",
		cp,
		_tomhz(fin));
	pres = samv7_reg_fieldname(pChip, "PRES", pChip->cfg.PMC_MCKR, 4, 3);
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
	LOG_USER("(%s)", cp);
	fin = fin / pdiv;
	/* samv7 has a *SINGLE* clock - */
	/* other at91 series parts have divisors for these. */
	pChip->cfg.cpu_freq = fin;
	pChip->cfg.mclk_freq = fin;
	pChip->cfg.fclk_freq = fin;
	LOG_USER("\t\tResult CPU Freq: %3.03f",
		_tomhz(fin));
}

static uint32_t *samv7_get_reg_ptr(struct samv7_cfg *pCfg, const struct samv7_reg_list *pList)
{
	/* this function exists to help */
	/* keep funky offsetof() errors */
	/* and casting from causing bugs */

	/* By using prototypes - we can detect what would */
	/* be casting errors. */

	return (uint32_t *)(void *)(((char *)(pCfg)) + pList->struct_offset);
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
static const struct samv7_reg_list *samv7_GetReg(struct samv7_chip *pChip, uint32_t *goes_here)
{
	const struct samv7_reg_list *pReg;

	pReg = &(samv7_all_regs[0]);
	while (pReg->name) {
		uint32_t *pPossible;

		/* calculate where this one go.. */
		/* it is "possibly" this register. */

		pPossible = ((uint32_t *)(void *)(((char *)(&(pChip->cfg))) + pReg->struct_offset));

		/* well? Is it this register */
		if (pPossible == goes_here) {
			/* Jump for joy! */
			return pReg;
		}

		/* next... */
		pReg++;
	}
	/* This is *TOTAL*PANIC* - we are totally screwed. */
	LOG_ERROR("INVALID SAMV7 REGISTER");
	return NULL;
}

static int samv7_ReadThisReg(struct samv7_chip *pChip, uint32_t *goes_here)
{
	const struct samv7_reg_list *pReg;
	int r;

	pReg = samv7_GetReg(pChip, goes_here);
	if (!pReg)
		return ERROR_FAIL;

	r = target_read_u32(pChip->target, pReg->address, goes_here);
	if (r != ERROR_OK) {
		LOG_ERROR("Cannot read SAMV7 register: %s @ 0x%08x, Err: %d",
			pReg->name, (unsigned)(pReg->address), r);
	}
	return r;
}

static int samv7_ReadAllRegs(struct samv7_chip *pChip)
{
	int r;
	const struct samv7_reg_list *pReg;

	pReg = &(samv7_all_regs[0]);
	while (pReg->name) {
		r = samv7_ReadThisReg(pChip,
				samv7_get_reg_ptr(&(pChip->cfg), pReg));
		if (r != ERROR_OK) {
			LOG_ERROR("Cannot read SAMv7 register: %s @ 0x%08x, Error: %d",
				pReg->name, ((unsigned)(pReg->address)), r);
			return r;
		}
		pReg++;
	}

	return ERROR_OK;
}

static int samv7_GetInfo(struct samv7_chip *pChip)
{
	const struct samv7_reg_list *pReg;
	uint32_t regval;

	pReg = &(samv7_all_regs[0]);
	while (pReg->name) {
		/* display all regs */
		LOG_DEBUG("Start: %s", pReg->name);
		regval = *samv7_get_reg_ptr(&(pChip->cfg), pReg);
		LOG_USER("%*s: [0x%08" PRIx32 "] -> 0x%08" PRIx32,
			REG_NAME_WIDTH,
			pReg->name,
			pReg->address,
			regval);
		if (pReg->explain_func)
			(*(pReg->explain_func))(pChip);
		LOG_DEBUG("End: %s", pReg->name);
		pReg++;
	}
	LOG_USER("   rc-osc: %3.03f MHz", _tomhz(pChip->cfg.rc_freq));
	LOG_USER("  mainosc: %3.03f MHz", _tomhz(pChip->cfg.mainosc_freq));
	LOG_USER("     plla: %3.03f MHz", _tomhz(pChip->cfg.plla_freq));
	LOG_USER(" cpu-freq: %3.03f MHz", _tomhz(pChip->cfg.cpu_freq));
	LOG_USER("mclk-freq: %3.03f MHz", _tomhz(pChip->cfg.mclk_freq));

	LOG_USER(" UniqueId: 0x%08" PRIx32 " 0x%08" PRIx32 " 0x%08" PRIx32 " 0x%08"PRIx32,
		pChip->cfg.unique_id[0],
		pChip->cfg.unique_id[1],
		pChip->cfg.unique_id[2],
		pChip->cfg.unique_id[3]);

	return ERROR_OK;
}

static int samv7_protect_check(struct flash_bank *bank)
{
	int r;
	uint32_t v[4] = {0};
	unsigned x;
	struct samv7_bank_private *pPrivate;

	LOG_DEBUG("Begin");
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	pPrivate = get_samv7_bank_private(bank);
	if (!pPrivate) {
		LOG_ERROR("no private for this bank?");
		return ERROR_FAIL;
	}
	if (!(pPrivate->probed))
		return ERROR_FLASH_BANK_NOT_PROBED;

	r = FLASHD_GetLockBits(pPrivate, v);
	if (r != ERROR_OK) {
		LOG_DEBUG("Failed: %d", r);
		return r;
	}

	for (x = 0; x < pPrivate->nsectors; x++)
		bank->sectors[x].is_protected = (!!(v[x >> 5] & (1 << (x % 32))));
	LOG_DEBUG("Done");
	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(samv7_flash_bank_command)
{
	struct samv7_chip *pChip;

	pChip = all_samv7_chips;

	/* is this an existing chip? */
	while (pChip) {
		if (pChip->target == bank->target)
			break;
		pChip = pChip->next;
	}

	if (!pChip) {
		/* this is a *NEW* chip */
		pChip = calloc(1, sizeof(struct samv7_chip));
		if (!pChip) {
			LOG_ERROR("NO RAM!");
			return ERROR_FAIL;
		}
		pChip->target = bank->target;
		/* insert at head */
		pChip->next = all_samv7_chips;
		all_samv7_chips = pChip;
		pChip->target = bank->target;
		/* assumption is this runs at 32khz */
		pChip->cfg.slow_freq = 32768;
		pChip->probed = 0;
	}

	switch (bank->base) {
		default:
			LOG_ERROR("Address 0x%08x invalid bank address (try 0x%08x"
				"[at91samv7 series] )",
				((unsigned int)(bank->base)),
				((unsigned int)(FLASH_BANK_BASE)));
			return ERROR_FAIL;
			break;

		/* at91samv7 series only has bank 0*/
		case FLASH_BANK_BASE:
			bank->driver_priv = &(pChip->details.bank);
			pChip->details.bank.pChip = pChip;
			pChip->details.bank.pBank = bank;
			break;
	}

	/* we initialize after probing. */
	return ERROR_OK;
}

static int samv7_GetDetails(struct samv7_bank_private *pPrivate)
{
	const struct samv7_chip_details *pDetails;
	struct samv7_chip *pChip;
	struct flash_bank *saved_bank;

	LOG_DEBUG("Begin");
	pDetails = all_samv7_details;
	while (pDetails->name) {
		/* Compare cidr without version bits */
		if (pDetails->chipid_cidr == (pPrivate->pChip->cfg.CHIPID_CIDR & 0xFFFFFFE0))
			break;
		else
			pDetails++;
	}
	if (pDetails->name == NULL) {
		LOG_ERROR("SAMV7 ChipID 0x%08x not found in table (perhaps you can ID this chip?)",
			(unsigned int)(pPrivate->pChip->cfg.CHIPID_CIDR));
		/* Help the victim, print details about the chip */
		LOG_INFO("SAMV7 CHIPID_CIDR: 0x%08" PRIx32 " decodes as follows",
			pPrivate->pChip->cfg.CHIPID_CIDR);
		samv7_explain_chipid_cidr(pPrivate->pChip);
		return ERROR_FAIL;
	}

	/* DANGER: THERE ARE DRAGONS HERE */

	/* get our pChip - it is going */
	/* to be over-written shortly */
	pChip = pPrivate->pChip;

	/* Note that, in reality: */
	/*  */
	/*     pPrivate = &(pChip->details.bank[0]) */
	/* or  pPrivate = &(pChip->details.bank[1]) */
	/*  */

	/* save the "bank" pointers */
	saved_bank = pChip->details.bank.pBank;

	/* Overwrite the "details" structure. */
	memcpy(&(pPrivate->pChip->details),
		pDetails,
		sizeof(pPrivate->pChip->details));

	/* now fix the ghosted pointers */
	pChip->details.bank.pChip = pChip;
	pChip->details.bank.pBank = saved_bank;

	/* update the *BANK*SIZE* */

	LOG_DEBUG("End");
	return ERROR_OK;
}

static int _samv7_probe(struct flash_bank *bank, int noise)
{
	unsigned x;
	int r;
	struct samv7_bank_private *pPrivate;


	LOG_DEBUG("Begin: Noise: %d", noise);
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	pPrivate = get_samv7_bank_private(bank);
	if (!pPrivate) {
		LOG_ERROR("Invalid/unknown bank number");
		return ERROR_FAIL;
	}

	r = samv7_ReadAllRegs(pPrivate->pChip);
	if (r != ERROR_OK)
		return r;

	LOG_DEBUG("Here");
	if (pPrivate->pChip->probed)
		r = samv7_GetInfo(pPrivate->pChip);
	else
		r = samv7_GetDetails(pPrivate);
	if (r != ERROR_OK)
		return r;

	/* update the flash bank size */
	if (bank->base == pPrivate->pChip->details.bank.base_address)
		bank->size = pPrivate->pChip->details.bank.size_bytes;

	if (bank->sectors == NULL) {
		bank->sectors = calloc(pPrivate->nsectors, (sizeof((bank->sectors)[0])));
		if (bank->sectors == NULL) {
			LOG_ERROR("No memory!");
			return ERROR_FAIL;
		}
		bank->num_sectors = pPrivate->nsectors;

		for (x = 0; ((int)(x)) < bank->num_sectors; x++) {
			bank->sectors[x].size = pPrivate->sector_size;
			bank->sectors[x].offset = x * (pPrivate->sector_size);
			/* mark as unknown */
			bank->sectors[x].is_erased = -1;
			bank->sectors[x].is_protected = -1;
		}
	}

	pPrivate->probed = 1;

	r = samv7_protect_check(bank);
	if (r != ERROR_OK)
		return r;

	/* read unique id, */
	FLASHD_ReadUniqueID(pPrivate);

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
	struct samv7_bank_private *pPrivate;
	int r;
	int i;
	int pageCount;
	/*16 pages equals 8KB - Same size as a lock region*/
	pageCount = 16;
	uint32_t status;

	LOG_DEBUG("Here");
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	r = samv7_auto_probe(bank);
	if (r != ERROR_OK) {
		LOG_DEBUG("Here,r=%d", r);
		return r;
	}

	pPrivate = get_samv7_bank_private(bank);
	if (!(pPrivate->probed))
		return ERROR_FLASH_BANK_NOT_PROBED;

	if ((first == 0) && ((last + 1) == ((int)(pPrivate->nsectors)))) {
		/* whole chip */
		LOG_DEBUG("Here");
		return FLASHD_EraseEntireBank(pPrivate);
	}
	LOG_INFO("samv7 does not auto-erase while programming (Erasing relevant sectors)");
	LOG_INFO("samv7 First: 0x%08x Last: 0x%08x", (unsigned int)(first), (unsigned int)(last));
	for (i = first; i <= last; i++) {
		/*16 pages equals 8KB - Same size as a lock region*/
		r = FLASHD_ErasePages(pPrivate, (i * pageCount), pageCount, &status);
		LOG_INFO("Erasing sector: 0x%08x", (unsigned int)(i));
		if (r != ERROR_OK)
			LOG_ERROR("SAMV7: Error performing Erase page @ lock region number %d",
				(unsigned int)(i));
		if (status & (1 << 2)) {
			LOG_ERROR("SAMV7: Lock Region %d is locked", (unsigned int)(i));
			return ERROR_FAIL;
		}
		if (status & (1 << 1)) {
			LOG_ERROR("SAMV7: Flash Command error @lock region %d", (unsigned int)(i));
			return ERROR_FAIL;
		}
	}

	return ERROR_OK;
}

static int samv7_protect(struct flash_bank *bank, int set, int first, int last)
{
	struct samv7_bank_private *pPrivate;
	int r;

	LOG_DEBUG("Here");
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	pPrivate = get_samv7_bank_private(bank);
	if (!(pPrivate->probed))
		return ERROR_FLASH_BANK_NOT_PROBED;

	if (set)
		r = FLASHD_Lock(pPrivate, (unsigned)(first), (unsigned)(last));
	else
		r = FLASHD_Unlock(pPrivate, (unsigned)(first), (unsigned)(last));
	LOG_DEBUG("End: r=%d", r);

	return r;

}

static int samv7_page_read(struct samv7_bank_private *pPrivate, unsigned pagenum, uint8_t *buf)
{
	uint32_t adr;
	int r;

	adr = pagenum * pPrivate->page_size;
	adr = adr + pPrivate->base_address;

	r = target_read_memory(pPrivate->pChip->target,
			adr,
			4,					/* THIS*MUST*BE* in 32bit values */
			pPrivate->page_size / 4,
			buf);
	if (r != ERROR_OK)
		LOG_ERROR("SAMV7: Flash program failed to read page phys address: 0x%08x",
			(unsigned int)(adr));
	return r;
}

static int samv7_page_write(struct samv7_bank_private *pPrivate, unsigned pagenum, const uint8_t *buf)
{
	uint32_t adr;
	uint32_t status;
	uint32_t fmr;	/* EEFC Flash Mode Register */
	int r;

	adr = pagenum * pPrivate->page_size;
	adr = (adr + pPrivate->base_address);

	/* Get flash mode register value */
	r = target_read_u32(pPrivate->pChip->target, pPrivate->controller_address, &fmr);
	if (r != ERROR_OK)
		LOG_DEBUG("Error Read failed: read flash mode register");

	/* Clear flash wait state field */
	fmr &= 0xfffff0ff;

	/* set FWS (flash wait states) field in the FMR (flash mode register) */
	fmr |= (pPrivate->flash_wait_states << 8);

	LOG_DEBUG("Flash Mode: 0x%08x", ((unsigned int)(fmr)));
	r = target_write_u32(pPrivate->pBank->target, pPrivate->controller_address, fmr);
	if (r != ERROR_OK)
		LOG_DEBUG("Error Write failed: set flash mode register");

	/* 1st sector 8kBytes - page 0 - 15*/
	/* 2nd sector 8kBytes - page 16 - 30*/
	/* 3rd sector 48kBytes - page 31 - 127*/
	LOG_DEBUG("Wr Page %u @ phys address: 0x%08x", pagenum, (unsigned int)(adr));
	r = target_write_memory(pPrivate->pChip->target,
			adr,
			4,					/* THIS*MUST*BE* in 32bit values */
			pPrivate->page_size / 4,
			buf);
	if (r != ERROR_OK) {
		LOG_ERROR("SAMV7: Failed to write (buffer) page at phys address 0x%08x",
			(unsigned int)(adr));
		return r;
	}

	r = EFC_PerformCommand(pPrivate,
			/* send Erase & Write Page */
			AT91C_EFC_FCMD_WP,	/*AT91C_EFC_FCMD_EWP only works on first two 8kb sectors*/
			pagenum,
			&status);

	if (r != ERROR_OK)
		LOG_ERROR("SAMV7: Error performing Write page @ phys address 0x%08x",
			(unsigned int)(adr));
	if (status & (1 << 2)) {
		LOG_ERROR("SAMV7: Page @ Phys address 0x%08x is locked", (unsigned int)(adr));
		return ERROR_FAIL;
	}
	if (status & (1 << 1)) {
		LOG_ERROR("SAMV7: Flash Command error @phys address 0x%08x", (unsigned int)(adr));
		return ERROR_FAIL;
	}
	return ERROR_OK;
}

static int samv7_write(struct flash_bank *bank,
	const uint8_t *buffer,
	uint32_t offset,
	uint32_t count)
{
	int n;
	unsigned page_cur;
	unsigned page_end;
	int r;
	unsigned page_offset;
	struct samv7_bank_private *pPrivate;
	uint8_t *pagebuffer;

	/* incase we bail further below, set this to null */
	pagebuffer = NULL;

	/* ignore dumb requests */
	if (count == 0) {
		r = ERROR_OK;
		goto done;
	}

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		r = ERROR_TARGET_NOT_HALTED;
		goto done;
	}

	pPrivate = get_samv7_bank_private(bank);
	if (!(pPrivate->probed)) {
		r = ERROR_FLASH_BANK_NOT_PROBED;
		goto done;
	}

	if ((offset + count) > pPrivate->size_bytes) {
		LOG_ERROR("Flash write error - past end of bank");
		LOG_ERROR(" offset: 0x%08x, count 0x%08x, BankEnd: 0x%08x",
			(unsigned int)(offset),
			(unsigned int)(count),
			(unsigned int)(pPrivate->size_bytes));
		r = ERROR_FAIL;
		goto done;
	}

	pagebuffer = malloc(pPrivate->page_size);
	if (!pagebuffer) {
		LOG_ERROR("No memory for %d Byte page buffer", (int)(pPrivate->page_size));
		r = ERROR_FAIL;
		goto done;
	}

	/* what page do we start & end in? */
	page_cur = offset / pPrivate->page_size;
	page_end = (offset + count - 1) / pPrivate->page_size;

	LOG_DEBUG("Offset: 0x%08x, Count: 0x%08x", (unsigned int)(offset), (unsigned int)(count));
	LOG_DEBUG("Page start: %d, Page End: %d", (int)(page_cur), (int)(page_end));

	/* Special case: all one page */
	/*  */
	/* Otherwise: */
	/*    (1) non-aligned start */
	/*    (2) body pages */
	/*    (3) non-aligned end. */

	/* Handle special case - all one page. */
	if (page_cur == page_end) {
		LOG_DEBUG("Special case, all in one page");
		r = samv7_page_read(pPrivate, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;

		page_offset = (offset & (pPrivate->page_size-1));
		memcpy(pagebuffer + page_offset,
			buffer,
			count);

		r = samv7_page_write(pPrivate, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;
		r = ERROR_OK;
		goto done;
	}

	/* non-aligned start */
	page_offset = offset & (pPrivate->page_size - 1);
	if (page_offset) {
		LOG_DEBUG("Not-Aligned start");
		/* read the partial */
		r = samv7_page_read(pPrivate, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;

		/* over-write with new data */
		n = (pPrivate->page_size - page_offset);
		memcpy(pagebuffer + page_offset,
			buffer,
			n);

		r = samv7_page_write(pPrivate, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;

		count  -= n;
		offset += n;
		buffer += n;
		page_cur++;
	}

	/* By checking that offset is correct here, we also
	fix a clang warning */
	assert(offset % pPrivate->page_size == 0);

	/* intermediate large pages */
	/* also - the final *terminal* */
	/* if that terminal page is a full page */
	LOG_DEBUG("Full Page Loop: cur=%d, end=%d, count = 0x%08x",
		(int)page_cur, (int)page_end, (unsigned int)(count));

	while ((page_cur < page_end) &&
			(count >= pPrivate->page_size)) {
		r = samv7_page_write(pPrivate, page_cur, buffer);
		if (r != ERROR_OK)
			goto done;
		count -= pPrivate->page_size;
		buffer += pPrivate->page_size;
		page_cur += 1;
	}

	/* terminal partial page? */
	if (count) {
		LOG_DEBUG("Terminal partial page, count = 0x%08x", (unsigned int)(count));
		/* we have a partial page */
		r = samv7_page_read(pPrivate, page_cur, pagebuffer);
		if (r != ERROR_OK)
			goto done;
					/* data goes at start */
		memcpy(pagebuffer, buffer, count);
		r = samv7_page_write(pPrivate, page_cur, pagebuffer);
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
	struct samv7_chip *pChip;
	pChip = get_current_samv7(CMD_CTX);
	if (!pChip)
		return ERROR_OK;

	unsigned x;
	int r;

	/* bank must exist before we can do anything */
	if (pChip->details.bank.pBank == NULL) {
		x = 0;
		command_print(CMD_CTX,
			"Please define bank %d via command: flash bank %s ... ",
			x,
			at91samv7_flash.name);
		return ERROR_FAIL;
	}

	/* if bank is not probed, then probe it */
	if (!(pChip->details.bank.probed)) {
		r = samv7_auto_probe(pChip->details.bank.pBank);
		if (r != ERROR_OK)
			return ERROR_FAIL;
	}
	/* above guarantees the "chip details" structure is valid */
	/* and thus, bank private areas are valid */
	/* and we have a SAMV7 chip, what a concept! */

	r = samv7_GetInfo(pChip);
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
	struct samv7_chip *pChip;

	pChip = get_current_samv7(CMD_CTX);
	if (!pChip)
		return ERROR_OK;

	if (pChip->target->state != TARGET_HALTED) {
		LOG_ERROR("samv7 - target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (pChip->details.bank.pBank == NULL) {
		command_print(CMD_CTX, "Bank must be defined first via: flash bank %s ...",
			at91samv7_flash.name);
		return ERROR_FAIL;
	}
	if (!pChip->details.bank.probed) {
		r = samv7_auto_probe(pChip->details.bank.pBank);
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
			if ((0 == strcmp(CMD_ARGV[0], "show")) && (0 == strcmp(CMD_ARGV[1], "all")))
				who = -1;
			else {
				uint32_t v32;
				COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], v32);
				who = v32;
			}
			break;
	}

	if (0 == strcmp("show", CMD_ARGV[0])) {
		if (who == -1) {
showall:
			r = ERROR_OK;
			for (x = 0; x < pChip->details.n_gpnvms; x++) {
				r = FLASHD_GetGPNVM(&(pChip->details.bank), x, &v);
				if (r != ERROR_OK)
					break;
				command_print(CMD_CTX, "samv7-gpnvm%u: %u", x, v);
			}
			return r;
		}
		if ((who >= 0) && (((unsigned)(who)) < pChip->details.n_gpnvms)) {
			r = FLASHD_GetGPNVM(&(pChip->details.bank), who, &v);
			command_print(CMD_CTX, "samv7-gpnvm%u: %u", who, v);
			return r;
		} else {
			command_print(CMD_CTX, "samv7-gpnvm invalid GPNVM: %u", who);
			return ERROR_COMMAND_SYNTAX_ERROR;
		}
	}

	if (who == -1) {
		command_print(CMD_CTX, "Missing GPNVM number");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (0 == strcmp("set", CMD_ARGV[0]))
		r = FLASHD_SetGPNVM(&(pChip->details.bank), who);
	else if ((0 == strcmp("clr", CMD_ARGV[0])) ||
		 (0 == strcmp("clear", CMD_ARGV[0])))			/* quietly accept both */
		r = FLASHD_ClrGPNVM(&(pChip->details.bank), who);
	else {
		command_print(CMD_CTX, "Unknown command: %s", CMD_ARGV[0]);
		r = ERROR_COMMAND_SYNTAX_ERROR;
	}
	return r;
}

COMMAND_HANDLER(samv7_handle_slowclk_command)
{
	struct samv7_chip *pChip;

	pChip = get_current_samv7(CMD_CTX);
	if (!pChip)
		return ERROR_OK;

	switch (CMD_ARGC) {
		case 0:
			/* show */
			break;
		case 1:
		{
			/* set */
			uint32_t v;
			COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], v);
			if (v > 200000) {
				/* absurd slow clock of 200Khz? */
				command_print(CMD_CTX, "Absurd/illegal slow clock freq: %d\n", (int)(v));
				return ERROR_COMMAND_SYNTAX_ERROR;
			}
			pChip->cfg.slow_freq = v;
			break;
		}
		default:
			/* error */
			command_print(CMD_CTX, "Too many parameters");
			return ERROR_COMMAND_SYNTAX_ERROR;
			break;
	}
	command_print(CMD_CTX, "Slowclk freq: %d.%03dkhz",
		(int)(pChip->cfg.slow_freq / 1000),
		(int)(pChip->cfg.slow_freq % 1000));
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
