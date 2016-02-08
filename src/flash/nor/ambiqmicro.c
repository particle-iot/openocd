/******************************************************************************
 *
 * @file ambiqmicro.c
 *
 * @brief Ambiq Micro flash driver.
 *
 *****************************************************************************/

/******************************************************************************
 * Copyright (c) 2015, David Racine <dracine at ambiqmicro.com>
 *
 * Copyright (c) 2016, Rick Foos <rfoos at solengtech.com>
 *
 * Copyright (c) 2016, Ambiq Micro, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "jtag/interface.h"
#include "imp.h"
#include "target/algorithm.h"
#include "target/armv7m.h"
#include "target/cortex_m.h"

/** Check error, log error, and return. */
#define CHECK_STATUS_ERROR(rc) { \
					if (rc != ERROR_OK) { \
						LOG_ERROR("%s:%d:%s(): status(0x%x)\n" \
						, __FILE__, __LINE__, __func__, rc); \
						return rc; } }

/** Read flash status from bootloader. */
#define CHECK_FLASH_STATUS(x, y) { \
				uint32_t retflash; \
				int rc = target_read_u32(x, y, &retflash);	\
				if (rc != ERROR_OK) {	\
					LOG_DEBUG("%s:%d:%s(): status(0x%x)\n" \
					, __FILE__, __LINE__, __func__, rc); return rc; }; \
				if (retflash != 0) { \
					LOG_ERROR("Flash not happy:%s:%d:%s(): status(0x%x)\n" \
					, __FILE__, __LINE__, __func__,	retflash); return -1; } }

/** Write to target. */
#define TARGET_WRITE_U32(x, y, z) { \
					int rc = target_write_u32(x, y, z); \
					if (rc != ERROR_OK) { \
						LOG_DEBUG("%s:%d:%s(): status(0x%x)\n" \
						, __FILE__, __LINE__, __func__, rc); return rc; } }

/*
 * Address and Key defines.
 */
#define PROGRAM_KEY      (0x12344321)
#define OTP_PROGRAM_KEY  (0x87655678)

#define FLASH_PROGRAM_MAIN_FROM_SRAM                0x0800005d
#define FLASH_PROGRAM_OTP_FROM_SRAM                 0x08000061
#define FLASH_ERASE_LIST_MAIN_PAGES_FROM_SRAM       0x08000065
#define FLASH_MASS_ERASE_MAIN_PAGES_FROM_SRAM       0x08000069


static const uint32_t apollo_flash_size[] = {
	1 << 15,
	1 << 16,
	1 << 17,
	1 << 18,
	1 << 19,
	1 << 20,
	1 << 21
};

static const uint32_t apollo_sram_size[] = {
	1 << 15,
	1 << 16,
	1 << 17,
	1 << 18,
	1 << 19,
	1 << 20,
	1 << 21
};

struct ambiqmicro_flash_bank {
	/* chip id register */

	uint32_t probed;

	const char *target_name;
	uint8_t target_class;

	uint32_t sramsiz;
	uint32_t flshsiz;

	/* flash geometry */
	uint32_t num_pages;
	uint32_t pagesize;
	uint32_t pages_in_lockregion;

	/* nv memory bits */
	uint16_t num_lockbits;

	/* main clock status */
	uint32_t rcc;
	uint32_t rcc2;
	uint8_t mck_valid;
	uint8_t xtal_mask;
	uint32_t iosc_freq;
	uint32_t mck_freq;
	const char *iosc_desc;
	const char *mck_desc;
};

static struct {
	uint8_t class;
	uint8_t partno;
	const char *partname;
} ambiqmicroParts[6] = {
	{0xFF, 0x00, "Unknown"},
	{0x01, 0x00, "Apollo"},
	{0x02, 0x00, "Apollo2"},
	{0x03, 0x00, "Unknown"},
	{0x04, 0x00, "Unknown"},
	{0x05, 0x00, "Apollo"},
};

static char *ambiqmicroClassname[6] = {
	"Unknown", "Apollo", "Apollo2", "Unknown", "Unknown", "Apollo"
};

/***************************************************************************
*	openocd command interface                                              *
***************************************************************************/

/* flash_bank ambiqmicro <base> <size> 0 0 <target#>
 */
FLASH_BANK_COMMAND_HANDLER(ambiqmicro_flash_bank_command)
{
	struct ambiqmicro_flash_bank *ambiqmicro_info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	ambiqmicro_info = calloc(sizeof(struct ambiqmicro_flash_bank), 1);

	bank->driver_priv = ambiqmicro_info;

	ambiqmicro_info->target_name = "Unknown target";

	/* part wasn't probed yet */
	ambiqmicro_info->probed = 0;

	return ERROR_OK;
}

static int get_ambiqmicro_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct ambiqmicro_flash_bank *ambiqmicro_info = bank->driver_priv;
	int printed;
	char *classname;

	if (ambiqmicro_info->probed == 0) {
		LOG_ERROR("Target not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	/* Check class name in range. */
	if (ambiqmicro_info->target_class < sizeof(ambiqmicroClassname))
		classname = ambiqmicroClassname[ambiqmicro_info->target_class];
	else
		classname = ambiqmicroClassname[0];

	printed = snprintf(buf,
		buf_size,
		"\nAmbiq Micro information: Chip is "
		"class %d (%s) %s\n",
		ambiqmicro_info->target_class,
		classname,
		ambiqmicro_info->target_name);

	if ((printed < 0))
		return ERROR_BUF_TOO_SMALL;
	return ERROR_OK;
}

/***************************************************************************
*	chip identification and status                                         *
***************************************************************************/

/* Fill in driver info structure */
static int ambiqmicro_read_part_info(struct flash_bank *bank)
{
	struct ambiqmicro_flash_bank *ambiqmicro_info = bank->driver_priv;
	struct target *target = bank->target;
	uint32_t PartNum = 0;
	int retval;

	/*
	 * Read Part Number.
	 */
	retval = target_read_u32(target, 0x40020000, &PartNum);
	CHECK_STATUS_ERROR(retval);
	LOG_DEBUG("Part number: 0x%x", PartNum);

	/*
	 * Determine device class.
	 */
	ambiqmicro_info->target_class = (PartNum & 0xFF000000) >> 24;

	switch (ambiqmicro_info->target_class) {
		case 1:		/* 1 - Apollo */
		case 5:		/* 5 - Apollo Bootloader */
			bank->base = bank->bank_number * 0x40000;
			ambiqmicro_info->pagesize = 2048;
			ambiqmicro_info->flshsiz =
			apollo_flash_size[(PartNum & 0x00F00000) >> 20];
			ambiqmicro_info->sramsiz =
			apollo_sram_size[(PartNum & 0x000F0000) >> 16];
			ambiqmicro_info->num_pages = ambiqmicro_info->flshsiz /
			ambiqmicro_info->pagesize;
			if (ambiqmicro_info->num_pages > 128) {
				ambiqmicro_info->num_pages = 128;
				ambiqmicro_info->flshsiz = 1024 * 256;
			}
			break;

		default:
			LOG_INFO("Unknown Class. Using Apollo-64 as default.");

			bank->base = bank->bank_number * 0x40000;
			ambiqmicro_info->pagesize = 2048;
			ambiqmicro_info->flshsiz = apollo_flash_size[1];
			ambiqmicro_info->sramsiz = apollo_sram_size[0];
			ambiqmicro_info->num_pages = ambiqmicro_info->flshsiz /
			ambiqmicro_info->pagesize;
			if (ambiqmicro_info->num_pages > 128) {
				ambiqmicro_info->num_pages = 128;
				ambiqmicro_info->flshsiz = 1024 * 256;
			}
			break;

	}

	if (ambiqmicro_info->target_class <
		(sizeof(ambiqmicroParts)/sizeof(ambiqmicroParts[0])))
		ambiqmicro_info->target_name =
			ambiqmicroParts[ambiqmicro_info->target_class].partname;
	else
		ambiqmicro_info->target_name =
			ambiqmicroParts[0].partname;

	LOG_DEBUG("num_pages: %d, pagesize: %d, flash: %d, sram: %d",
		ambiqmicro_info->num_pages,
		ambiqmicro_info->pagesize,
		ambiqmicro_info->flshsiz,
		ambiqmicro_info->sramsiz);

	return ERROR_OK;
}

/***************************************************************************
*	flash operations                                                       *
***************************************************************************/

static int ambiqmicro_protect_check(struct flash_bank *bank)
{
	struct ambiqmicro_flash_bank *ambiqmicro = bank->driver_priv;
	int status = ERROR_OK;
	uint32_t i;


	if (ambiqmicro->probed == 0) {
		LOG_ERROR("Target not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	for (i = 0; i < (unsigned) bank->num_sectors; i++)
		bank->sectors[i].is_protected = -1;

	return status;
}

static int ambiqmicro_exec_command(struct target *target,
	uint32_t command,
	uint32_t flash_return_address)
{
	int retval = target_resume(
		target,
		false,
		command,
		true,
		true);
#if 0
	if (retval != ERROR_OK)
		LOG_ERROR("error executing ambiqmicro command");
#endif

	/*
	 * Wait for halt.
	 */
	for (;; ) {
		target_poll(target);
		if (target->state == TARGET_HALTED)
			break;
		else if (target->state == TARGET_RUNNING ||
			target->state == TARGET_DEBUG_RUNNING) {
			/*
			 * Keep polling until target halts.
			 */
			target_poll(target);
			alive_sleep(100);
			LOG_DEBUG("state = %d", target->state);
		} else {
			LOG_ERROR("Target not halted or running %d", target->state);
			break;
		}
	}

	/*
	 * Read return value.
	 */
	CHECK_FLASH_STATUS(target, flash_return_address);

	/* Return code from target_resume. */
	return retval;
}

static int ambiqmicro_mass_erase(struct flash_bank *bank)
{
	struct target *target = NULL;
	struct ambiqmicro_flash_bank *ambiqmicro_info = NULL;
	int retval = ERROR_OK;

	ambiqmicro_info = bank->driver_priv;
	target = bank->target;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (ambiqmicro_info->probed == 0) {
		LOG_ERROR("Target not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	/*
	 * Clear Bootloader bit.
	 */
	TARGET_WRITE_U32(target, 0x400201a0, 0x0);

	/*
	 * Set up the SRAM.
	 */

	/*
	 * Bank.
	 */
	TARGET_WRITE_U32(target, 0x10000000, bank->bank_number);

	/*
	 * Write Key.
	 */
	TARGET_WRITE_U32(target, 0x10000004, PROGRAM_KEY);

	/*
	 * Breakpoint.
	 */
	TARGET_WRITE_U32(target, 0x10000008, 0xfffffffe);

	/*
	 * Erase the main array.
	 */
	LOG_INFO("Mass erase on bank %d", bank->bank_number);

	/*
	 * passed pc, addr = ROM function, handle breakpoints, not debugging.
	 */
	retval = ambiqmicro_exec_command(target, FLASH_MASS_ERASE_MAIN_PAGES_FROM_SRAM, 0x10000008);
	if (retval != ERROR_OK)
		LOG_ERROR("error executing ambiqmicro flash mass erase algorithm");

	/*
	 * Set Bootloader bit, regardless of command execution.
	 */
	TARGET_WRITE_U32(target, 0x400201a0, 0x1);

	return retval;
}


static int ambiqmicro_erase(struct flash_bank *bank, int first, int last)
{
	struct ambiqmicro_flash_bank *ambiqmicro_info = bank->driver_priv;
	struct target *target = bank->target;
	uint32_t retval = ERROR_OK;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (ambiqmicro_info->probed == 0) {
		LOG_ERROR("Target not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	/*
	 * Check pages.
	 * Fix num_pages for the device.
	 */
	if ((first < 0) || (last < first) || (last >= (int)ambiqmicro_info->num_pages))
		return ERROR_FLASH_SECTOR_INVALID;

	/*
	 * Just Mass Erase if all pages are given.
	 * TODO: Fix num_pages for the device
	 */
	if ((first == 0) && (last == ((int)ambiqmicro_info->num_pages-1)))
		return ambiqmicro_mass_erase(bank);

	/*
	 * Clear Bootloader bit.
	 */
	TARGET_WRITE_U32(target, 0x400201a0, 0x0);

	/*
	 * Set up the SRAM.
	 */

	/*
	 * Bank.
	 */
	TARGET_WRITE_U32(target, 0x10000000, bank->bank_number);

	/*
	 * Number of pages to erase.
	 */
	TARGET_WRITE_U32(target, 0x10000004, 1 + (last-first));

	/*
	 * Write Key.
	 */
	TARGET_WRITE_U32(target, 0x10000008, PROGRAM_KEY);

	/*
	 * Breakpoint.
	 */
	TARGET_WRITE_U32(target, 0x1000000c, 0xfffffffe);

	/*
	 * Pointer to flash address.
	 */
	TARGET_WRITE_U32(target, 0x10000010, first);

	/*
	 * Erase the pages.
	 */
	LOG_INFO("Erasing pages %d to %d on bank %d", first, last, bank->bank_number);

	/*
	 * passed pc, addr = ROM function, handle breakpoints, not debugging.
	 */
	retval = ambiqmicro_exec_command(target, FLASH_ERASE_LIST_MAIN_PAGES_FROM_SRAM, 0x1000000C);
	if (retval != ERROR_OK)
		LOG_ERROR("error executing ambiqmicro flash mass erase algorithm");
#if 0
	retval = target_resume(
		target,
		false,
		FLASH_ERASE_LIST_MAIN_PAGES_FROM_SRAM,
		true,
		true);

	if (retval != ERROR_OK)
		LOG_ERROR("error executing ambiqmicro flash mass erase algorithm");

	/*
	 * Wait for halt.
	 */
	for (;; ) {
		target_poll(target);
		if (target->state == TARGET_HALTED)
			break;
		else if (target->state == TARGET_RUNNING ||
			target->state == TARGET_DEBUG_RUNNING) {
			/*
			 * Keep polling until target halts.
			 */
			target_poll(target);
			alive_sleep(100);
			LOG_DEBUG("state = %d", target->state);
		} else {
			LOG_ERROR("Target not halted or running %d", target->state);
			retval = ERROR_OK;
			break;
		}
	}

	/*
	 * Read flash return value.
	 */
	CHECK_FLASH_STATUS(target, 0x1000000c);
#endif

	LOG_INFO("%d pages erased!", 1+(last-first));

	if (first == 0) {
		/*
		 * Set Bootloader bit.
		 */
		TARGET_WRITE_U32(target, 0x400201a0, 0x1);
	}

	return retval;
}

static int ambiqmicro_protect(struct flash_bank *bank, int set, int first, int last)
{
	/* struct ambiqmicro_flash_bank *ambiqmicro_info = bank->driver_priv;
	 * struct target *target = bank->target; */

	/*
	 * TODO
	 */
	LOG_INFO("Not yet implemented");

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	return ERROR_OK;
}

static int ambiqmicro_write_block(struct flash_bank *bank,
	const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	/* struct ambiqmicro_flash_bank *ambiqmicro_info = bank->driver_priv; */
	struct target *target = bank->target;
	uint32_t address = bank->base + offset;
	uint32_t buffer_pointer = 0x10000010;
	uint32_t maxbuffer;
	uint32_t thisrun_count;
	int retval = ERROR_OK;

	if (((count%4) != 0) || ((offset%4) != 0)) {
		LOG_ERROR("write block must be multiple of 4 bytes in offset & length");
		return ERROR_FAIL;
	}

	/*
	 * Max buffer size for this device.
	 * Hard code 6kB for the buffer.
	 */
	maxbuffer = 0x1800;

	LOG_INFO("Flashing main array");

	while (count > 0) {
		if (count > maxbuffer)
			thisrun_count = maxbuffer;
		else
			thisrun_count = count;

		/*
		 * Set up the SRAM.
		 */

		/*
		 * Pointer to flash.
		 */
		TARGET_WRITE_U32(target, 0x10000000, address);

		/*
		 * Number of 32-bit words to program.
		 */
		TARGET_WRITE_U32(target, 0x10000004, thisrun_count/4);

		/*
		 * Write Key.
		 */
		TARGET_WRITE_U32(target, 0x10000008, PROGRAM_KEY);

		/*
		 * Breakpoint.
		 */
		TARGET_WRITE_U32(target, 0x1000000c, 0xfffffffe);

		/*
		 * Write Buffer.
		 */
		retval = target_write_buffer(target, buffer_pointer, thisrun_count, buffer);

		if (retval != ERROR_OK)
			break;

		LOG_DEBUG("address = 0x%08x", address);

		retval = ambiqmicro_exec_command(target, FLASH_PROGRAM_MAIN_FROM_SRAM, 0x1000000c);
		if (retval != ERROR_OK) {
			LOG_ERROR("error executing ambiqmicro flash write algorithm");
			break;
		}
#if 0
		retval = target_resume(
			target,
			false,
			FLASH_PROGRAM_MAIN_FROM_SRAM,
			true,
			true);

		if (retval != ERROR_OK) {
			LOG_ERROR("error executing ambiqmicro flash write algorithm");
			break;
		}

		/*
		 * Wait for halt.
		 */
		for (;; ) {
			target_poll(target);
			if (target->state == TARGET_HALTED)
				break;
			else if (target->state == TARGET_RUNNING ||
				target->state == TARGET_DEBUG_RUNNING) {
				/*
				 * Keep polling until target halts.
				 */
				target_poll(target);
				alive_sleep(100);
				LOG_DEBUG("state = %d", target->state);
			} else {
				LOG_ERROR("Target not halted or running %d", target->state);
				retval = ERROR_OK;
				break;
			}
		}

		/*
		 * Read return value.
		 */
		CHECK_FLASH_STATUS(target, 0x1000000c);
#endif
		buffer += thisrun_count;
		address += thisrun_count;
		count -= thisrun_count;
	}


	LOG_INFO("Main array flashed");

	/*
	 * Clear Bootloader bit.
	 */
	TARGET_WRITE_U32(target, 0x400201a0, 0x0);

	return retval;
}

static int ambiqmicro_write(struct flash_bank *bank, const uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	uint32_t retval;

	/* try using a block write */
	retval = ambiqmicro_write_block(bank, buffer, offset, count);
	if (retval != ERROR_OK)
		LOG_ERROR("write failed");

	return retval;
}

static int ambiqmicro_probe(struct flash_bank *bank)
{
	struct ambiqmicro_flash_bank *ambiqmicro_info = bank->driver_priv;
	uint32_t retval;

	/* If this is a ambiqmicro chip, it has flash; probe() is just
	 * to figure out how much is present.  Only do it once.
	 */
	if (ambiqmicro_info->probed == 1) {
		LOG_INFO("Target already probed");
		return ERROR_OK;
	}

	/* ambiqmicro_read_part_info() already handled error checking and
	 * reporting.  Note that it doesn't write, so we don't care about
	 * whether the target is halted or not.
	 */
	retval = ambiqmicro_read_part_info(bank);
	if (retval != ERROR_OK)
		return retval;

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}

	/* provide this for the benefit of the NOR flash framework */
	bank->size = ambiqmicro_info->pagesize * ambiqmicro_info->num_pages;
	bank->num_sectors = ambiqmicro_info->num_pages;
	bank->sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	for (int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].offset = i * ambiqmicro_info->pagesize;
		bank->sectors[i].size = ambiqmicro_info->pagesize;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = -1;
	}

	/*
	 * Part has been probed.
	 */
	ambiqmicro_info->probed = 1;

	return retval;
}

static int ambiqmicro_otp_program(struct flash_bank *bank,
	uint32_t offset, uint32_t count)
{
	struct target *target = NULL;
	struct ambiqmicro_flash_bank *ambiqmicro_info = NULL;
	uint32_t retval = ERROR_OK;

	ambiqmicro_info = bank->driver_priv;
	target = bank->target;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (ambiqmicro_info->probed == 0) {
		LOG_ERROR("Target not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	if (count > 256) {
		LOG_ERROR("Count must be < 256");
		return -1;
	}

	/*
	 * Clear Bootloader bit.
	 */
	TARGET_WRITE_U32(target, 0x400201a0, 0x0);

	/*
	 * Set up the SRAM.
	 */

	/*
	 * Bank.
	 */
	TARGET_WRITE_U32(target, 0x10000000, offset);

	/*
	 * Num of words to program.
	 */
	TARGET_WRITE_U32(target, 0x10000004, count);

	/*
	 * Write Key.
	 */
	TARGET_WRITE_U32(target, 0x10000008, OTP_PROGRAM_KEY);

	/*
	 * Breakpoint.
	 */
	TARGET_WRITE_U32(target, 0x1000000c, 0xfffffffe);

	/*
	 * Program OTP.
	 */
	LOG_INFO("Programming OTP offset 0x%08x", offset);

	/*
	 * passed pc, addr = ROM function, handle breakpoints, not debugging.
	 */
	retval = ambiqmicro_exec_command(target, FLASH_PROGRAM_OTP_FROM_SRAM, 0x1000000C);
	if (retval != ERROR_OK)
		LOG_ERROR("error executing ambiqmicro otp program algorithm");
#if 0
	retval = target_resume(
		target,
		false,
		FLASH_PROGRAM_OTP_FROM_SRAM,
		true,
		true);

	if (retval != ERROR_OK)
		LOG_ERROR("error executing ambiqmicro otp program algorithm");

	/*
	 * Wait for halt.
	 */
	for (;; ) {
		target_poll(target);
		if (target->state == TARGET_HALTED)
			break;
		else if (target->state == TARGET_RUNNING ||
			target->state == TARGET_DEBUG_RUNNING) {
			/*
			 * Keep polling until target halts.
			 */
			target_poll(target);
			alive_sleep(100);
			LOG_DEBUG("state = %d", target->state);
		} else {
			LOG_ERROR("Target not halted or running %d", target->state);
			retval = ERROR_OK;
			break;
		}
	}

	/*
	 * Read return value.
	 */
	CHECK_FLASH_STATUS(target, 0x1000000c);
#endif

	LOG_INFO("Programming OTP finished.");

	return retval;
}

#if 0
static int ambiqmicro_auto_probe(struct flash_bank *bank)
{
	struct ambiqmicro_flash_bank *ambiqmicro_info = bank->driver_priv;
	if (ambiqmicro_info->probed)
		return ERROR_OK;
	return ambiqmicro_probe(bank);
}
#endif


COMMAND_HANDLER(ambiqmicro_handle_mass_erase_command)
{
	int i;

	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *bank;
	uint32_t retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	if (ambiqmicro_mass_erase(bank) == ERROR_OK) {
		/* set all sectors as erased */
		for (i = 0; i < bank->num_sectors; i++)
			bank->sectors[i].is_erased = 1;

		command_print(CMD_CTX, "ambiqmicro mass erase complete");
	} else
		command_print(CMD_CTX, "ambiqmicro mass erase failed");

	return ERROR_OK;
}

COMMAND_HANDLER(ambiqmicro_handle_page_erase_command)
{
	struct flash_bank *bank;
	uint32_t first, last;
	uint32_t retval;

	if (CMD_ARGC < 3)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], first);
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], last);

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	if (ambiqmicro_erase(bank, first, last) == ERROR_OK)
		command_print(CMD_CTX, "ambiqmicro page erase complete");
	else
		command_print(CMD_CTX, "ambiqmicro page erase failed");

	return ERROR_OK;
}


/**
 * Enable/Disable fault regs.
 */
COMMAND_HANDLER(ambiqmicro_handle_fault_capture_command)
{
	struct flash_bank *bank;
	uint32_t enable;
	uint32_t retval;

	if (CMD_ARGC < 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], enable);
	enable = enable >= 1;

	command_print(CMD_CTX, "enable=%d", enable);

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	/*
	 * TODO
	 */
	LOG_INFO("Not yet implemented");

	return ERROR_OK;
}

/**
 * Program the otp block.
 */
COMMAND_HANDLER(ambiqmicro_handle_program_otp_command)
{
	struct flash_bank *bank;
	uint32_t offset, count;
	uint32_t retval;

	if (CMD_ARGC < 3)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], offset);
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], count);

	command_print(CMD_CTX, "offset=0x%08x count=%d", offset, count);

	CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);

	retval = ambiqmicro_otp_program(bank, offset, count);

	if (retval != ERROR_OK)
		LOG_ERROR("error check log");

	return ERROR_OK;
}



static const struct command_registration ambiqmicro_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.usage = "<bank>",
		.handler = ambiqmicro_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.help = "Erase entire device",
	},
	{
		.name = "page_erase",
		.usage = "<bank> <first> <last>",
		.handler = ambiqmicro_handle_page_erase_command,
		.mode = COMMAND_EXEC,
		.help = "Erase device pages",
	},
	{
		.name = "fault_capture",
		.usage = "<bank> <enable>",
		.handler = ambiqmicro_handle_fault_capture_command,
		.mode = COMMAND_EXEC,
		.help = "Enable/disable the fault capture registers",
	},
	{
		.name = "program_otp",
		.handler = ambiqmicro_handle_program_otp_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> <offset> <count>",
		.help =
			"Program OTP (assumes you have already written array starting at 0x10000010)",
	},
	COMMAND_REGISTRATION_DONE
};
static const struct command_registration ambiqmicro_command_handlers[] = {
	{
		.name = "ambiqmicro",
		.mode = COMMAND_EXEC,
		.help = "ambiqmicro flash command group",
		.usage = "Support for Ambiq Micro parts.",
		.chain = ambiqmicro_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver ambiqmicro_flash = {
	.name = "ambiqmicro",
	.commands = ambiqmicro_command_handlers,
	.flash_bank_command = ambiqmicro_flash_bank_command,
	.erase = ambiqmicro_erase,
	.protect = ambiqmicro_protect,
	.write = ambiqmicro_write,
	.read = default_flash_read,
	.probe = ambiqmicro_probe,
	.auto_probe = ambiqmicro_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = ambiqmicro_protect_check,
	.info = get_ambiqmicro_info,
};
