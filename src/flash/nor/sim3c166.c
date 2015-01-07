/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2011 by Erik Botö                                       *
 *   erik.boto@pelagicore.com                                              *
 *                                                                         *
 *   Copyright (C) 2015 by Andreas Bomholtz                                *
 *   andreas@erik.boto@pelagicore.com                                      *
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

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

/* sim3c166 register locations */

#define SIM3C166_FLASH_ACR	    0x40008000
#define SIM3C166_FLASH_KEYR	    0x4002E0C0
#define SIM3C166_FLASH_OPTKEYR	0x40008008
#define SIM3C166_FLASH_SR	    0x4002E000
#define SIM3C166_FLASH_CR		0x4002E0B0
#define SIM3C166_FLASH_AR		0x4002E0A0
#define SIM3C166_FLASH_OBR		0x4000801C
#define SIM3C166_FLASH_WRPR	    0x40008020

#define SIM3C166_FPEC_CLK		0x4002D020
/* option byte location */

#define SIM3C166_OB_RDP		    0x08040800
#define SIM3C166_OB_WRP0		0x08040808
#define SIM3C166_OB_WRP1		0x0804080A
#define SIM3C166_OB_WRP2		0x0804080C

/* FLASH_CR register bits */

#define FLASH_PG		(1 << 0)
#define FLASH_PER		(1 << 1)
#define FLASH_MER		(1 << 2)
#define FLASH_OPTPG		(1 << 4)
#define FLASH_OPTER		(1 << 5)
#define FLASH_STRT		(1 << 6)
#define FLASH_LOCK		(1 << 7)
#define FLASH_OPTWRE	(1 << 9)

#define FLASH_WRITE     0x00000720
#define FLASH_ERASE     0x00001234
#define FLASH_CLEAR     0x00000000

/* FLASH_SR register bits */

#define FLASH_BSY		0x00100000
#define FLASH_PGERR		(1 << 2)
#define FLASH_WRPRTERR	(1 << 4)
#define FLASH_EOP		(1 << 5)

/* SIM3C166_FLASH_OBR bit definitions (reading) */

#define OPT_ERROR		0
#define OPT_READOUT		1

/* register unlock keys */

#define KEY_UNLOCK      0x00040720
#define KEY1			0xA5
#define KEY2			0xF2

struct sim3c166_options {
	uint16_t RDP;
	uint16_t user_options;
	uint16_t protection[3];
};

struct sim3c166_flash_bank {
	struct sim3c166_options option_bytes;
	struct working_area *write_algorithm;
	int ppage_size;
	int probed;
};

/* flash bank sim3c166 <base> <size> 0 0 <target#>
 */
FLASH_BANK_COMMAND_HANDLER(sim3c166_flash_bank_command)
{
	struct sim3c166_flash_bank *sim3c166_info;

	if (CMD_ARGC < 6) {
		LOG_WARNING("incomplete flash_bank sim3c166 configuration");
		return ERROR_FLASH_BANK_INVALID;
	}

	sim3c166_info = malloc(sizeof(struct sim3c166_flash_bank));
	bank->driver_priv = sim3c166_info;

	sim3c166_info->write_algorithm = NULL;
	sim3c166_info->probed = 0;

	return ERROR_OK;
}

static inline int sim3c166_get_flash_status(struct flash_bank *bank, uint32_t *status)
{
	struct target *target = bank->target;
	return target_read_u32(target, SIM3C166_FLASH_SR, status);
}

static int sim3c166_wait_status_busy(struct flash_bank *bank, int timeout)
{
	uint32_t status;
	int retval = ERROR_OK;

	/* wait for busy to clear */
	for (;;) {
		retval = sim3c166_get_flash_status(bank, &status);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("status: 0x%" PRIx32 "", status);
		if ((status & FLASH_BSY) == 0)
			break;
		if (timeout-- <= 0) {
			LOG_ERROR("timed out waiting for flash");
			return ERROR_FAIL;
		}
		alive_sleep(1);
	}

	return retval;
}

static int sim3c166_read_options(struct flash_bank *bank)
{
	uint32_t optiondata;
	struct sim3c166_flash_bank *sim3c166_info = NULL;
	struct target *target = bank->target;

	sim3c166_info = bank->driver_priv;

	/* read current option bytes */
	int retval = target_read_u32(target, SIM3C166_FLASH_OBR, &optiondata);
	if (retval != ERROR_OK)
		return retval;

	sim3c166_info->option_bytes.user_options = (uint16_t)0xFFFC | ((optiondata >> 2) & 0x03);
	sim3c166_info->option_bytes.RDP = (optiondata & (1 << OPT_READOUT)) ? 0xFFFF : 0x5AA5;

	if (optiondata & (1 << OPT_READOUT))
		LOG_INFO("Device Security Bit Set");

	/* each bit refers to a 4bank protection */
	retval = target_read_u32(target, SIM3C166_FLASH_WRPR, &optiondata);
	if (retval != ERROR_OK)
		return retval;

	sim3c166_info->option_bytes.protection[0] = (uint16_t)optiondata;
	sim3c166_info->option_bytes.protection[1] = (uint16_t)(optiondata >> 8);
	sim3c166_info->option_bytes.protection[2] = (uint16_t)(optiondata >> 16);

	return ERROR_OK;
}

static int sim3c166_erase_options(struct flash_bank *bank)
{
	struct sim3c166_flash_bank *sim3c166_info = NULL;
	struct target *target = bank->target;

	sim3c166_info = bank->driver_priv;

	/* read current options */
	sim3c166_read_options(bank);

	/* unlock flash registers */
	int retval = target_write_u8(target, SIM3C166_FLASH_KEYR, KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SIM3C166_FLASH_KEYR, KEY2);
	if (retval != ERROR_OK)
		return retval;

	/* unlock option flash registers */
	retval = target_write_u8(target, SIM3C166_FLASH_OPTKEYR, KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SIM3C166_FLASH_OPTKEYR, KEY2);
	if (retval != ERROR_OK)
		return retval;

	/* erase option bytes */
	retval = target_write_u32(target, SIM3C166_FLASH_CR, FLASH_OPTER | FLASH_OPTWRE);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, SIM3C166_FLASH_CR, FLASH_OPTER | FLASH_STRT | FLASH_OPTWRE);
	if (retval != ERROR_OK)
		return retval;

	retval = sim3c166_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* clear readout protection and complementary option bytes
	 * this will also force a device unlock if set */
	sim3c166_info->option_bytes.RDP = 0x5AA5;

	return ERROR_OK;
}

static int sim3c166_write_options(struct flash_bank *bank)
{
	struct sim3c166_flash_bank *sim3c166_info = NULL;
	struct target *target = bank->target;

	sim3c166_info = bank->driver_priv;

	/* unlock flash registers */
	int retval = target_write_u8(target, SIM3C166_FLASH_KEYR, KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SIM3C166_FLASH_KEYR, KEY2);
	if (retval != ERROR_OK)
		return retval;

	/* unlock option flash registers */
	retval = target_write_u8(target, SIM3C166_FLASH_OPTKEYR, KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SIM3C166_FLASH_OPTKEYR, KEY2);
	if (retval != ERROR_OK)
		return retval;

	/* program option bytes */
	retval = target_write_u32(target, SIM3C166_FLASH_CR, FLASH_OPTPG | FLASH_OPTWRE);
	if (retval != ERROR_OK)
		return retval;

	retval = sim3c166_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* write protection byte 1 */
	retval = target_write_u16(target, SIM3C166_OB_WRP0, sim3c166_info->option_bytes.protection[0]);
	if (retval != ERROR_OK)
		return retval;

	retval = sim3c166_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* write protection byte 2 */
	retval = target_write_u16(target, SIM3C166_OB_WRP1, sim3c166_info->option_bytes.protection[1]);
	if (retval != ERROR_OK)
		return retval;

	retval = sim3c166_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* write protection byte 3 */
	retval = target_write_u16(target, SIM3C166_OB_WRP2, sim3c166_info->option_bytes.protection[2]);
	if (retval != ERROR_OK)
		return retval;

	retval = sim3c166_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* write readout protection bit */
	retval = target_write_u16(target, SIM3C166_OB_RDP, sim3c166_info->option_bytes.RDP);
	if (retval != ERROR_OK)
		return retval;

	retval = sim3c166_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, SIM3C166_FLASH_CR, FLASH_LOCK);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int sim3c166_protect_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct sim3c166_flash_bank *sim3c166_info = bank->driver_priv;

	uint32_t protection;
	int i, s;
	int num_bits;
	int set;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* each bit refers to a 4bank protection (bit 0-23) */
	int retval = target_read_u32(target, SIM3C166_FLASH_WRPR, &protection);
	if (retval != ERROR_OK)
		return retval;

	/* each protection bit is for 4 * 2K pages */
	num_bits = (bank->num_sectors / sim3c166_info->ppage_size);

	for (i = 0; i < num_bits; i++) {
		set = 1;
		if (protection & (1 << i))
			set = 0;

		for (s = 0; s < sim3c166_info->ppage_size; s++)
			bank->sectors[(i * sim3c166_info->ppage_size) + s].is_protected = set;
	}

	return ERROR_OK;
}

static int sim3c166_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	int i;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* unlock flash registers */
	int retval = target_write_u32(target, SIM3C166_FLASH_SR, KEY_UNLOCK);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SIM3C166_FLASH_KEYR, KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SIM3C166_FLASH_KEYR, KEY2);
	if (retval != ERROR_OK)
		return retval;

	for (i = first; i <= last; i++) {
		retval = target_write_u32(target, SIM3C166_FLASH_AR, bank->base + bank->sectors[i].offset);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target, SIM3C166_FLASH_CR, FLASH_ERASE);
		if (retval != ERROR_OK)
			return retval;

		retval = sim3c166_wait_status_busy(bank, 100);
		if (retval != ERROR_OK)
			return retval;

		bank->sectors[i].is_erased = 1;
	}

	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int sim3c166_protect(struct flash_bank *bank, int set, int first, int last)
{
	struct sim3c166_flash_bank *sim3c166_info = NULL;
	struct target *target = bank->target;
	uint16_t prot_reg[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
	int i, reg, bit;
	int status;
	uint32_t protection;

	sim3c166_info = bank->driver_priv;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((first % sim3c166_info->ppage_size) != 0) {
		LOG_WARNING("aligned start protect sector to a %d sector boundary",
				sim3c166_info->ppage_size);
		first = first - (first % sim3c166_info->ppage_size);
	}
	if (((last + 1) % sim3c166_info->ppage_size) != 0) {
		LOG_WARNING("aligned end protect sector to a %d sector boundary",
				sim3c166_info->ppage_size);
		last++;
		last = last - (last % sim3c166_info->ppage_size);
		last--;
	}

	/* each bit refers to a 4bank protection */
	int retval = target_read_u32(target, SIM3C166_FLASH_WRPR, &protection);
	if (retval != ERROR_OK)
		return retval;

	prot_reg[0] = (uint16_t)protection;
	prot_reg[1] = (uint16_t)(protection >> 8);
	prot_reg[2] = (uint16_t)(protection >> 16);

	for (i = first; i <= last; i++) {
		reg = (i / sim3c166_info->ppage_size) / 8;
		bit = (i / sim3c166_info->ppage_size) - (reg * 8);

		LOG_WARNING("reg, bit: %d, %d", reg, bit);
		if (set)
			prot_reg[reg] &= ~(1 << bit);
		else
			prot_reg[reg] |= (1 << bit);
	}

	if ((status = sim3c166_erase_options(bank)) != ERROR_OK)
		return status;

	sim3c166_info->option_bytes.protection[0] = prot_reg[0];
	sim3c166_info->option_bytes.protection[1] = prot_reg[1];
	sim3c166_info->option_bytes.protection[2] = prot_reg[2];

	return sim3c166_write_options(bank);
}

static int sim3c166_write(struct flash_bank *bank, const uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	uint32_t words_remaining = (count / 2);
	uint32_t bytes_remaining = (count & 0x00000001);
	uint32_t address = bank->base + offset;
	uint32_t bytes_written = 0;
	int retval;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & 0x1) {
		LOG_WARNING("offset 0x%" PRIx32 " breaks required 2-byte alignment", offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	/* unlock flash registers */
	retval = target_write_u32(target, SIM3C166_FLASH_AR, FLASH_CLEAR);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, SIM3C166_FLASH_SR, FLASH_WRITE);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SIM3C166_FLASH_KEYR, KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SIM3C166_FLASH_KEYR, KEY2);
	if (retval != ERROR_OK)
		return retval;

	while (words_remaining > 0) {
		uint16_t value;
		memcpy(&value, buffer + bytes_written, sizeof(uint16_t));

		retval = target_write_u16(target, SIM3C166_FLASH_CR, value);
		if (retval != ERROR_OK)
		  return retval;

		bytes_written += 2;
		words_remaining--;
		address += 2;
	}

	if (bytes_remaining) {
		uint16_t value = 0xffff;
		memcpy(&value, buffer + bytes_written, bytes_remaining);

		retval = target_write_u16(target, SIM3C166_FLASH_CR, value);
		if (retval != ERROR_OK)
		  return retval;
	}

	return retval;
}

static int sim3c166_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct sim3c166_flash_bank *sim3c166_info = bank->driver_priv;
	int i;
	uint16_t num_pages;
	int page_size;
	uint32_t base_address = 0x00000000;

	sim3c166_info->probed = 0;

	/* Enable FPEC CLK */
	int retval = target_write_u32(target, SIM3C166_FPEC_CLK, 0x40000002);
	if (retval != ERROR_OK)
		return retval;

	page_size = 1024;
	sim3c166_info->ppage_size = 4;
	num_pages = 256;

	LOG_INFO("flash size = %dkbytes", num_pages*page_size/1024);

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}

	bank->base = base_address;
	bank->size = (num_pages * page_size);
	bank->num_sectors = num_pages;
	bank->sectors = malloc(sizeof(struct flash_sector) * num_pages);

	for (i = 0; i < num_pages; i++) {
		bank->sectors[i].offset = i * page_size;
		bank->sectors[i].size = page_size;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 1;
	}

	sim3c166_info->probed = 1;

	return ERROR_OK;
}

static int sim3c166_auto_probe(struct flash_bank *bank)
{
	struct sim3c166_flash_bank *sim3c166_info = bank->driver_priv;
	if (sim3c166_info->probed)
		return ERROR_OK;
	return sim3c166_probe(bank);
}

COMMAND_HANDLER(sim3c166_handle_lock_command)
{
	struct target *target = NULL;
	struct sim3c166_flash_bank *sim3c166_info = NULL;

	if (CMD_ARGC < 1) {
		command_print(CMD_CTX, "sim3c166 lock <bank>");
		return ERROR_OK;
	}

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	sim3c166_info = bank->driver_priv;

	target = bank->target;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (sim3c166_erase_options(bank) != ERROR_OK) {
		command_print(CMD_CTX, "sim3c166 failed to erase options");
		return ERROR_OK;
	}

	/* set readout protection */
	sim3c166_info->option_bytes.RDP = 0;

	if (sim3c166_write_options(bank) != ERROR_OK) {
		command_print(CMD_CTX, "sim3c166 failed to lock device");
		return ERROR_OK;
	}

	command_print(CMD_CTX, "sim3c166 locked");

	return ERROR_OK;
}

COMMAND_HANDLER(sim3c166_handle_unlock_command)
{
	struct target *target = NULL;

	if (CMD_ARGC < 1) {
		command_print(CMD_CTX, "sim3c166 unlock <bank>");
		return ERROR_OK;
	}

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	target = bank->target;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (sim3c166_erase_options(bank) != ERROR_OK) {
		command_print(CMD_CTX, "sim3c166 failed to unlock device");
		return ERROR_OK;
	}

	if (sim3c166_write_options(bank) != ERROR_OK) {
		command_print(CMD_CTX, "sim3c166 failed to lock device");
		return ERROR_OK;
	}

	command_print(CMD_CTX, "sim3c166 unlocked.\n"
			"INFO: a reset or power cycle is required "
			"for the new settings to take effect.");

	return ERROR_OK;
}

static const struct command_registration sim3c166_exec_command_handlers[] = {
	{
		.name = "lock",
		.handler = sim3c166_handle_lock_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Lock entire flash device.",
	},
	{
		.name = "unlock",
		.handler = sim3c166_handle_unlock_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Unlock entire protected flash device.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration sim3c166_command_handlers[] = {
	{
		.name = "sim3c166",
		.mode = COMMAND_ANY,
		.help = "sim3c166 flash command group",
		.chain = sim3c166_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver sim3c166_flash = {
	.name = "sim3c166",
	.commands = sim3c166_command_handlers,
	.flash_bank_command = sim3c166_flash_bank_command,
	.erase = sim3c166_erase,
	.protect = sim3c166_protect,
	.write = sim3c166_write,
	.read = default_flash_read,
	.probe = sim3c166_probe,
	.auto_probe = sim3c166_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = sim3c166_protect_check,
};
