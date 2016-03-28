/***************************************************************************
 *   Copyright (C) 2015 by Tobias Diedrich                                 *
 *   <ranma+openwrt@tdiedrich.de>                                          *
 *                                                                         *
 *   based on the stmsmi code written by Antonio Borneo                    *
 *   <borneo.antonio@gmail.com>                                            *
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
 *   Free Software Foundation, Inc.                                        *
 *                                                                         *
 ***************************************************************************/
/* Driver for the Atheros AR7xxx/AR9xxx SPI flash interface.
 * Since no SPI mode register is present, presumably only
 * SPI "mode 3" (CPOL=1 and CPHA=1) is supported.
 * On boot, the first 4MiB of flash space are memory-mapped into the
 * area bf000000 - bfffffff (4 copies), so the MIPS bootstrap
 * vector bfc00000 is mapped to the beginning of the flash.
 * By writing a 1 to the REMAP_DISABLE bit in the SPI_CONTROL register,
 * the full area of 16MiB is mapped.
 * By writing a 0 to the SPI_FUNCTION_SELECT register (write-only dword
 * register @bf000000), memory mapping is disabled and the SPI registers
 * are exposed to the CPU instead:
 * bf000000 SPI_FUNCTION_SELECT
 * bf000004 SPI_CONTROL
 * bf000008 SPI_IO_CONTROL
 * bf00000c SPI_READ_DATA
 * When not memory-mapped, the SPI interface is essentially bitbanged
 * using SPI_CONTROL and SPI_IO_CONTROL with the only hardware-assistance
 * being the 32bit read-only shift-register SPI_READ_DATA. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include "spi.h"
#include <jtag/jtag.h>
#include <helper/time_support.h>
#include <target/mips32.h>
#include <target/mips32_pracc.h>

#define ATH79_REG_FS	 0
#define ATH79_REG_CLOCK  4
#define ATH79_REG_WRITE  8
#define ATH79_REG_DATA  12

#define AR7240_SPI_CS_DIS  0xf0000
#define AR7240_SPI_CE_LOW  0x60000
#define AR7240_SPI_CE_HIGH 0x60100

/* Timeout in ms */
#define ATH79_MAX_TIMEOUT  (3000)
/* Page buffer is 256 bytes */
#define ATH79_PAGE_BUFFER  0x100

struct ath79_flash_bank {
	int probed;
	uint8_t spi_page_buf[ATH79_PAGE_BUFFER];
	uint32_t io_base;
	uint32_t bank_num;
	const struct flash_device *dev;
};

struct ath79_target {
	char *name;
	uint32_t tap_idcode;
	uint32_t ath_base;
	uint32_t io_base;
};

static const struct ath79_target target_devices[] = {
	/* name,		  tap_idcode, ath_base,   io_base */
	{ "ATH79",		0x00000001, 0xbf000000, 0xbf000000 },
	{ NULL,		   0,		  0,		  0 }
};

static int ath79_spi_bitbang_bytes(struct target *target, uint32_t io_base,
				   int pre_deselect, int post_deselect,
				   uint8_t *data, int len) {
	struct mips32_common *mips32 = target_to_mips32(target);
	struct mips_ejtag *ejtag_info = &mips32->ejtag_info;

	uint32_t pracc_out = 0;

	const int pracc_pre_post = 26;
	const int pracc_loop_byte = 8 * 2 + 2;

	struct pracc_queue_info ctx = {.max_code = pracc_pre_post + len *
					pracc_loop_byte};
	uint32_t *out = malloc(len + 3);

	memset(out, 0xa5, len + 3);

	pracc_queue_init(&ctx);
	if (ctx.retval != ERROR_OK)
		goto exit;

	LOG_DEBUG("ath79_spi_bitbang_bytes(%p, %08x, %d, %d, %p, %d)",
		  target, io_base, pre_deselect, post_deselect, data, len);

	/* Calculate largest len given the available instruction budget. */
	const int max_len = (PRACC_OUT_OFFSET / 4 - pracc_pre_post) /
				pracc_loop_byte;

	if (len > max_len) {
		LOG_INFO("len too big: %d > %d", len, max_len);
		ctx.retval = ERROR_BUF_TOO_SMALL;
		goto exit;
	} else {
		LOG_DEBUG("max len %d. len %d => max code %d", max_len, len,
			  ctx.max_code);
	}

	/* $15 = MIPS32_PRACC_BASE_ADDR */
	pracc_add(&ctx, 0, MIPS32_LUI(15, PRACC_UPPER_BASE_ADDR));
	/* $1 = io_base */
	pracc_add(&ctx, 0, MIPS32_LUI(1, UPPER16(io_base)));
	if (pre_deselect) {
		/* [$1 + FS] = 1  (enable flash io register access) */
		pracc_add(&ctx, 0, MIPS32_LUI(2, UPPER16(1)));
		pracc_add(&ctx, 0, MIPS32_ORI(2, 2, LOWER16(1)));
		pracc_add(&ctx, 0, MIPS32_SW(2, ATH79_REG_FS, 1));
		/* deselect flash just in case */
		/* $2 = SPI_CS_DIS */
		pracc_add(&ctx, 0, MIPS32_LUI(2, UPPER16(AR7240_SPI_CS_DIS)));
		/* $2 = SPI_CS_DIS */
		pracc_add(&ctx, 0,
			  MIPS32_ORI(2, 2, LOWER16(AR7240_SPI_CS_DIS)));
		/* [$1 + WRITE] = $2 */
		pracc_add(&ctx, 0, MIPS32_SW(2, ATH79_REG_WRITE, 1));
	}
	/* t0 = CLOCK_LOW + 0-bit */
	pracc_add(&ctx, 0, MIPS32_LUI(8, UPPER16((AR7240_SPI_CE_LOW + 0))));
	pracc_add(&ctx, 0, MIPS32_ORI(8, 8, LOWER16((AR7240_SPI_CE_LOW + 0))));
	/* t1 = CLOCK_LOW + 1-bit */
	pracc_add(&ctx, 0, MIPS32_LUI(9, UPPER16((AR7240_SPI_CE_LOW + 1))));
	pracc_add(&ctx, 0, MIPS32_ORI(9, 9, LOWER16((AR7240_SPI_CE_LOW + 1))));
	/* t2 = CLOCK_HIGH + 0-bit */
	pracc_add(&ctx, 0, MIPS32_LUI(10, UPPER16((AR7240_SPI_CE_HIGH + 0))));
	pracc_add(&ctx, 0,
		  MIPS32_ORI(10, 10, LOWER16((AR7240_SPI_CE_HIGH + 0))));
	/* t2 = CLOCK_HIGH + 1-bit */
	pracc_add(&ctx, 0, MIPS32_LUI(11, UPPER16((AR7240_SPI_CE_HIGH + 1))));
	pracc_add(&ctx, 0,
		  MIPS32_ORI(11, 11, LOWER16((AR7240_SPI_CE_HIGH + 1))));

	for (int i = 0; i < len; i++) {
		uint8_t x = data[i];

		LOG_DEBUG("%d: generating code for %02x", i, x);
		for (int j = 0; j < 8; j++) {
			int bit = ((x << j) & 0x80) == 0x80;

			LOG_DEBUG("  %d: generating code for bit %d", j, bit);
			if (bit) {
				/* [$1 + WRITE] = t1 */
				pracc_add(&ctx, 0,
					  MIPS32_SW(9, ATH79_REG_WRITE, 1));
				/* [$1 + WRITE] = t3 */
				pracc_add(&ctx, 0,
					  MIPS32_SW(11, ATH79_REG_WRITE, 1));
			} else {
				/* [$1 + WRITE] = t0 */
				pracc_add(&ctx, 0,
					  MIPS32_SW(8, ATH79_REG_WRITE, 1));
				/* [$1 + WRITE] = t2 */
				pracc_add(&ctx, 0,
					  MIPS32_SW(10, ATH79_REG_WRITE, 1));
			}
		}
		if (i % 4 == 3) {
			/* $3 = [$1 + DATA] */
			pracc_add(&ctx, 0, MIPS32_LW(3, ATH79_REG_DATA, 1));
			/* [OUTi] = $3 */
			pracc_add(&ctx, MIPS32_PRACC_PARAM_OUT + pracc_out,
				  MIPS32_SW(3, PRACC_OUT_OFFSET +
				 pracc_out, 15));
			pracc_out += 4;
		}
	}
	if (len & 3) { /* not a multiple of 4 bytes */
		/* $3 = [$1 + DATA] */
		pracc_add(&ctx, 0, MIPS32_LW(3, ATH79_REG_DATA, 1));
		/* [OUTi] = $3 */
		pracc_add(&ctx, MIPS32_PRACC_PARAM_OUT + pracc_out,
			  MIPS32_SW(3, PRACC_OUT_OFFSET + pracc_out, 15));
		pracc_out += 4;
	}

	if (post_deselect) {
		/* $2 = SPI_CS_DIS */
		pracc_add(&ctx, 0, MIPS32_LUI(2, UPPER16(AR7240_SPI_CS_DIS)));
		/* $2 = SPI_CS_DIS */
		pracc_add(&ctx, 0,
			  MIPS32_ORI(2, 2, LOWER16(AR7240_SPI_CS_DIS)));
		/* [$1 + WRITE] = $2 */
		pracc_add(&ctx, 0, MIPS32_SW(2, ATH79_REG_WRITE, 1));

		/* [$1 + FS] = 0  (disable flash io register access) */
		pracc_add(&ctx, 0, MIPS32_XORI(2, 2, 0));
		pracc_add(&ctx, 0, MIPS32_SW(2, ATH79_REG_FS, 1));
	}

	/* common pracc epilogue */
	/* jump to start */
	pracc_add(&ctx, 0, MIPS32_B(NEG16(ctx.code_count + 1)));
	/* restore $15 from DeSave */
	pracc_add(&ctx, 0, MIPS32_MFC0(15, 31, 0));

	LOG_DEBUG("Assembled %d instructions, %d stores:", ctx.code_count,
		  ctx.store_count);
	if (LOG_LEVEL_IS(LOG_LVL_DEBUG)) {
		for (int i = 0; i < ctx.code_count; i++)
			LOG_DEBUG("%08x", ctx.pracc_list[i]);
	}

	ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, out);
	if (ctx.retval != ERROR_OK)
		goto exit;

	int pracc_words = pracc_out / 4;

	if (len & 3) { /* not a multiple of 4 bytes */
		/* Need to realign last word. */
		out[pracc_words - 1] <<= 8 * (4 - len);
	}
	if (1234 != ntohl(1234)) {
		/* byteswap buffer */
		for (int i = 0; i < pracc_words; i++)
			out[i] = ntohl(out[i]);
	}
	if (LOG_LEVEL_IS(LOG_LVL_DEBUG)) {
		for (int i = 0; i < len; i++) {
			LOG_DEBUG("bitbang %02x => %02x",
				  data[i], ((uint8_t *)out)[i]);
		}
	}
	memcpy(data, out, len);

exit:
	if (ctx.retval != ERROR_OK)
		memset(data, 0x5a, len);
	if (out)
		free(out);
	pracc_queue_free(&ctx);
	return ctx.retval;
}

FLASH_BANK_COMMAND_HANDLER(ath79_flash_bank_command)
{
	struct ath79_flash_bank *ath79_info;

	LOG_DEBUG("%s", __func__);

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	ath79_info = malloc(sizeof(struct ath79_flash_bank));
	if (!ath79_info) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	bank->driver_priv = ath79_info;
	ath79_info->probed = 0;

	return ERROR_OK;
}

/* Read the status register of the external SPI flash chip. */
static int read_status_reg(struct flash_bank *bank, uint32_t *status)
{
	struct target *target = bank->target;
	struct ath79_flash_bank *ath79_info = bank->driver_priv;
	uint32_t io_base = ath79_info->io_base;

	uint8_t spi_bytes[] = {SPIFLASH_READ_STATUS, 0};

	/* Send SPI command "read STATUS" */
	int retval = ath79_spi_bitbang_bytes(
		target, io_base, 1, 1, spi_bytes, sizeof(spi_bytes));

	*status = spi_bytes[1];

	return retval;
}

/* check for WIP (write in progress) bit in status register */
/* timeout in ms */
static int wait_till_ready(struct flash_bank *bank, int timeout)
{
	uint32_t status;
	int retval;
	long long endtime;

	endtime = timeval_ms() + timeout;
	do {
		/* read flash status register */
		retval = read_status_reg(bank, &status);
		if (retval != ERROR_OK)
			return retval;

		if ((status & SPIFLASH_BSY_BIT) == 0)
			return ERROR_OK;
		alive_sleep(1);
	} while (timeval_ms() < endtime);

	LOG_ERROR("timeout");
	return ERROR_FAIL;
}

/* Send "write enable" command to SPI flash chip. */
static int ath79_write_enable(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct ath79_flash_bank *ath79_info = bank->driver_priv;
	uint32_t io_base = ath79_info->io_base;
	uint32_t status;
	int retval;

	uint8_t spi_bytes[] = {SPIFLASH_WRITE_ENABLE};

	/* Send SPI command "write enable" */
	retval = ath79_spi_bitbang_bytes(
		target, io_base, 1, 1, spi_bytes, sizeof(spi_bytes));
	if (retval != ERROR_OK)
		return retval;

	/* read flash status register */
	retval = read_status_reg(bank, &status);
	if (retval != ERROR_OK)
		return retval;

	/* Check write enabled */
	if ((status & SPIFLASH_WE_BIT) == 0) {
		LOG_ERROR("Cannot enable write to flash. Status=0x%08" PRIx32,
			  status);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int erase_command(struct flash_bank *bank, int sector)
{
	struct ath79_flash_bank *ath79_info = bank->driver_priv;
	struct target *target = bank->target;
	uint32_t io_base = ath79_info->io_base;
	uint32_t offset = bank->sectors[sector].offset;

	uint8_t spi_bytes[] = {
		ath79_info->dev->erase_cmd,
		offset >> 16,
		offset >> 8,
		offset
	};

	/* bitbang command */
	return ath79_spi_bitbang_bytes(
		target, io_base, 1, 1, spi_bytes, sizeof(spi_bytes));
}

static int ath79_erase_sector(struct flash_bank *bank, int sector)
{
	int retval;

	retval = ath79_write_enable(bank);
	if (retval != ERROR_OK)
		return retval;

	/* send SPI command "block erase" */
	retval = erase_command(bank, sector);
	if (retval != ERROR_OK)
		return retval;

	/* poll WIP for end of self timed Sector Erase cycle */
	retval = wait_till_ready(bank, ATH79_MAX_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int ath79_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	struct ath79_flash_bank *ath79_info = bank->driver_priv;
	int retval = ERROR_OK;
	int sector;

	LOG_DEBUG("%s: from sector %d to sector %d", __func__, first, last);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((first < 0) || (last < first) || (last >= bank->num_sectors)) {
		LOG_ERROR("Flash sector invalid");
		return ERROR_FLASH_SECTOR_INVALID;
	}

	if (!ath79_info->probed) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	for (sector = first; sector <= last; sector++) {
		if (bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	for (sector = first; sector <= last; sector++) {
		retval = ath79_erase_sector(bank, sector);
		if (retval != ERROR_OK)
			break;
		keep_alive();
	}

	return retval;
}

static int ath79_protect(struct flash_bank *bank, int set,
			 int first, int last)
{
	int sector;

	for (sector = first; sector <= last; sector++)
		bank->sectors[sector].is_protected = set;
	return ERROR_OK;
}

static int ath79_write_page(struct flash_bank *bank, const uint8_t *buffer,
			    uint32_t address, uint32_t len)
{
	struct ath79_flash_bank *ath79_info = bank->driver_priv;
	struct target *target = bank->target;
	uint32_t io_base = ath79_info->io_base;
	uint32_t written = 0;
	uint8_t spi_cmd[] = {
		SPIFLASH_PAGE_PROGRAM,
		address >> 16,
		address >> 8,
		address,
	};
	int retval;

	if (address & 0xff) {
		LOG_ERROR("ath79_write_page: unaligned write address: %08x\n",
			  address);
		return ERROR_FAIL;
	}
	if (len > sizeof(ath79_info->spi_page_buf)) {
		LOG_ERROR("ath79_write_page: length bigger than page size %d: %d\n",
			  (int)sizeof(ath79_info->spi_page_buf), len);
		return ERROR_FAIL;
	}

	uint32_t i;

	for (i = 0; i < len; i++) {
		if (buffer[i] != 0xff)
			break;
	}
	if (i == len)  /* all 0xff, no need to program. */
		return ERROR_OK;

	address -= ath79_info->io_base;

	LOG_INFO("writing %d bytes to flash page @0x%08x", len, address);

	memcpy(ath79_info->spi_page_buf, buffer, len);

	/* unlock writes */
	retval = ath79_write_enable(bank);
	if (retval != ERROR_OK)
		return retval;

	/* bitbang command */
	retval = ath79_spi_bitbang_bytes(
		target, io_base, 1, 0, spi_cmd, sizeof(spi_cmd));
	if (retval != ERROR_OK)
		return retval;

	/* Length limit derived from pracc code size limit */
	const uint32_t spi_max_len = 112;

	while (len > spi_max_len) {
		/* write blocks with len limited by pracc code size */
		retval = ath79_spi_bitbang_bytes(target, io_base, 0, 0,
						 &ath79_info->spi_page_buf[written],
						 spi_max_len);
		if (retval != ERROR_OK)
			return retval;

		written += spi_max_len;
		len -= spi_max_len;
	}

	/* write final block of data */
	return ath79_spi_bitbang_bytes(
		target, io_base, 0, 1, &ath79_info->spi_page_buf[written], len);
}

static int ath79_write_buffer(struct flash_bank *bank, const uint8_t *buffer,
			      uint32_t address, uint32_t len)
{
	int retval;

	LOG_DEBUG("%s: address=0x%08" PRIx32 " len=0x%08" PRIx32,
		  __func__, address, len);

	while (len > 0) {
		const uint32_t page_size = 0x100;

		/* Page size is 256 bytes */
		int page_len = len > 0x100 ? 0x100 : len;

		retval = ath79_write_page(
			bank, buffer, address, page_len);
		if (retval != ERROR_OK)
			return retval;

		buffer += page_size;
		address += page_size;
		len -= page_len;
	}

	return ERROR_OK;
}

static int ath79_write(struct flash_bank *bank, const uint8_t *buffer,
		       uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct ath79_flash_bank *ath79_info = bank->driver_priv;
	int sector;

	LOG_DEBUG("%s: offset=0x%08" PRIx32 " count=0x%08" PRIx32,
		  __func__, offset, count);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset + count > ath79_info->dev->size_in_bytes) {
		LOG_WARNING("Write pasts end of flash. Extra data discarded.");
		count = ath79_info->dev->size_in_bytes - offset;
	}

	/* Check sector protection */
	for (sector = 0; sector < bank->num_sectors; sector++) {
		/* Start offset in or before this sector? */
		/* End offset in or behind this sector? */
		struct flash_sector *bs = &bank->sectors[sector];

		if ((offset < ((*bs).offset + (*bs).size)) &&
			((offset + count - 1) >= (*bs).offset) &&
			(*bs).is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	return ath79_write_buffer(bank, buffer, bank->base + offset, count);
}

/* Return ID of flash device */
/* On exit, SW mode is kept */
static int read_flash_id(struct flash_bank *bank, uint32_t *id)
{
	struct target *target = bank->target;
	struct ath79_flash_bank *ath79_info = bank->driver_priv;
	uint32_t io_base = ath79_info->io_base;
	int retval;
	uint8_t spi_bytes[] = {SPIFLASH_READ_ID, 0, 0, 0, 0, 0, 0, 0};

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Send SPI command "read ID" */
	retval = ath79_spi_bitbang_bytes(target, io_base, 1, 1,
					 spi_bytes, sizeof(spi_bytes));
	if (retval != ERROR_OK)
		return retval;

	*id = (spi_bytes[1] << 0)
		| (spi_bytes[2] << 8)
		| (spi_bytes[3] << 16);
	LOG_ERROR("read_flash_id: %06x", *id);

	return ERROR_OK;
}

static int ath79_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct ath79_flash_bank *ath79_info = bank->driver_priv;
	struct flash_sector *sectors;
	uint32_t id = 0; /* silence uninitialized warning */
	const struct ath79_target *target_device;
	int retval;

	if (ath79_info->probed)
		free(bank->sectors);
	ath79_info->probed = 0;

	for (target_device = target_devices; target_device->name;
		++target_device)
		if (target_device->tap_idcode == target->tap->idcode)
			break;
	if (!target_device->name) {
		LOG_ERROR("Device ID 0x%" PRIx32 " is not known as SMI capable",
			  target->tap->idcode);
		return ERROR_FAIL;
	}

	ath79_info->io_base = target_device->io_base;

	LOG_DEBUG("Valid SMI on device %s at address 0x%" PRIx32,
		  target_device->name, bank->base);

	/* read and decode flash ID; returns in SW mode */
	retval = read_flash_id(bank, &id);
	if (retval != ERROR_OK)
		return retval;

	ath79_info->dev = NULL;
	for (const struct flash_device *p = flash_devices; p->name; p++)
		if (p->device_id == id) {
			ath79_info->dev = p;
			break;
		}

	if (!ath79_info->dev) {
		LOG_ERROR("Unknown flash device (ID 0x%08" PRIx32 ")", id);
		return ERROR_FAIL;
	}

	LOG_INFO("Found flash device \'%s\' (ID 0x%08" PRIx32 ")",
		 ath79_info->dev->name, ath79_info->dev->device_id);

	/* Set correct size value */
	bank->size = ath79_info->dev->size_in_bytes;

	/* create and fill sectors array */
	bank->num_sectors =
		ath79_info->dev->size_in_bytes / ath79_info->dev->sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (!sectors) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset = sector * ath79_info->dev->sectorsize;
		sectors[sector].size = ath79_info->dev->sectorsize;
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 1;
	}

	bank->sectors = sectors;
	ath79_info->probed = 1;
	return ERROR_OK;
}

static int ath79_auto_probe(struct flash_bank *bank)
{
	struct ath79_flash_bank *ath79_info = bank->driver_priv;

	if (ath79_info->probed)
		return ERROR_OK;
	return ath79_probe(bank);
}

static int ath79_protect_check(struct flash_bank *bank)
{
	/* Nothing to do. Protection is only handled in SW. */
	return ERROR_OK;
}

static int get_ath79_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct ath79_flash_bank *ath79_info = bank->driver_priv;

	if (!ath79_info->probed) {
		snprintf(buf, buf_size,
			 "\nSMI flash bank not probed yet\n");
		return ERROR_OK;
	}

	snprintf(buf, buf_size, "\nSMI flash information:\n"
		"  Device \'%s\' (ID 0x%08" PRIx32 ")\n",
		ath79_info->dev->name, ath79_info->dev->device_id);

	return ERROR_OK;
}

struct flash_driver ath79_flash = {
	.name = "ath79",
	.flash_bank_command = ath79_flash_bank_command,
	.erase = ath79_erase,
	.protect = ath79_protect,
	.write = ath79_write,
	.read = default_flash_read,
	.probe = ath79_probe,
	.auto_probe = ath79_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = ath79_protect_check,
	.info = get_ath79_info,
};
