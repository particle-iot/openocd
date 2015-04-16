/***************************************************************************
 *   Copyright (C) 2015 dmitry pervushin <dpervushin@gmail.com>            *
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
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include "spi.h"
#include "imp.h"

#define SFCTL_TIMEOUT	0x100
#define SFCTL_R_CTL0	0x400
#define SFCTL_R_CTL1	0x404
#define SFCTL_R_INTE	0x408
#define SFCTL_R_STS	0x40C
#define SFCTL_R_BUF0	0x500
#define SFCTL_R_BUF1	0x600

static int sfctl_probe(struct flash_bank *bank);
static int sfctl_autoprobe(struct flash_bank *bank);
static int sfctl_get_info(struct flash_bank *bank, char *buf, int buf_size);
static int sfctl_read(struct flash_bank *bank,
	uint8_t *buffer,
	uint32_t offset,
	uint32_t count);
static int sfctl_write(struct flash_bank *bank,
	const uint8_t *buffer,
	uint32_t offset,
	uint32_t count);
static int sfctl_erase(struct flash_bank *bank, int first, int last);
static int sfctl_protect_check(struct flash_bank *);
FLASH_BANK_COMMAND_HANDLER(sfctl_flash_bank_command);

struct flash_driver sfctl_flash = {
	.name = "sfctl",
	.flash_bank_command = sfctl_flash_bank_command,
	.erase = sfctl_erase,
	.protect = NULL,
	.write = sfctl_write,
	.read = sfctl_read,
	.probe = sfctl_probe,
	.auto_probe = sfctl_autoprobe,
	.erase_check = default_flash_blank_check,
	.protect_check = sfctl_protect_check,
	.info = sfctl_get_info,
};

struct sfctl_flash_bank {
	struct target *target;
	bool probed;
	const struct flash_device *dev;
	uint32_t base;
};

static int _sfctl_go_with_bufs(struct sfctl_flash_bank *sfb, int primary, int secondary);
static int _sfctl_wait(struct sfctl_flash_bank *sfb);
static int _sfctl_command(struct sfctl_flash_bank *sfb, uint8_t command, uint32_t addr);
static int _sfctl_readid(struct sfctl_flash_bank *sfb, uint32_t *id);
static int _sfctl_wrenable(struct sfctl_flash_bank *sfb);

static int sfctl_entry_check(struct flash_bank *bank, int first_probe)
{
	struct sfctl_flash_bank *sfb = bank->driver_priv;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (first_probe)
		return ERROR_OK;

	if (!sfb->probed)
		return ERROR_FAIL;
	return ERROR_OK;
}

static int sfctl_autoprobe(struct flash_bank *bank)
{
	int rc;

	rc = sfctl_entry_check(bank, false);
	if (rc != ERROR_OK)
		return sfctl_probe(bank);
	return rc;
}

static int sfctl_probe(struct flash_bank *bank)
{
	struct sfctl_flash_bank *sfb = bank->driver_priv;
	uint32_t id = 0xFFFFFFFF;
	struct flash_sector *sectors;
	int rc;

	rc = sfctl_entry_check(bank, true);
	if (rc != ERROR_OK)
		return rc;

	_sfctl_readid(sfb, &id);

	for (sfb->dev = flash_devices; sfb->dev->name; sfb->dev++) {
		if (sfb->dev->device_id == id)
			break;
	}
	if (!sfb->dev->name)
		return ERROR_FAIL;

	bank->size = sfb->dev->size_in_bytes;
	bank->num_sectors = bank->size / sfb->dev->sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (!sectors) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	rc = target_write_u32(sfb->target, sfb->base + 0x08,
			(0 << 0) |     /* SPI mode 0 */
			(1 << 4) |     /* fetch on 1st neg edge */
			(1 << 6) |     /* SF WIP */
			(0x1F << 8));  /* SFCSn deassertion time */
	if (rc != ERROR_OK)
		return rc;

	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset =
				sector * sfb->dev->sectorsize;
		sectors[sector].size = sfb->dev->sectorsize;
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 0;
	}

	bank->sectors = sectors;

	sfb->probed = true;

	return ERROR_OK;
}

static int sfctl_get_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct sfctl_flash_bank *sfb = bank->driver_priv;
	int rc;

	rc = sfctl_entry_check(bank, false);
	if (rc != ERROR_OK)
		return rc;
	if (sfb && sfb->dev)
		snprintf(buf, buf_size, "FSCTL flash info\n\tdevice '%s'\n\tid 0x%08x\n",
			sfb->dev->name, sfb->dev->device_id);
	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(sfctl_flash_bank_command)
{
	struct sfctl_flash_bank *sfb;

	sfb = malloc(sizeof(*sfb));
	if (!sfb) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	bank->driver_priv = sfb;
	sfb->probed = false;
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], sfb->base);
	bank->base = 0x10000000;
	sfb->dev = NULL;
	sfb->target = bank->target;

	return ERROR_OK;
}

static int _sfctl_wait(struct sfctl_flash_bank *sfb)
{
	int rc;
	uint32_t sts;
	uint32_t c;

	for (c = 0;; c++) {
		rc = target_read_u32(sfb->target, sfb->base + SFCTL_R_STS, &sts);
		if (rc != ERROR_OK)
			return rc;
		if ((sts & (1 << 1)) == 0)
			break;
		if (c > SFCTL_TIMEOUT)
			return ERROR_TARGET_TIMEOUT;
		alive_sleep(1);
	}
	return ERROR_OK;
}

static int _sfctl_go_with_bufs(struct sfctl_flash_bank *sfb, int primary, int secondary)
{
	int rc;
	uint32_t v;

	rc = target_read_u32(sfb->target, sfb->base + SFCTL_R_CTL1, &v);
	if (rc != ERROR_OK)
		return rc;

	/* secondary size and enable */
	v &= ~(0xFF << 24);
	v &= ~(1 << 5);
	/* primary size and enable */
	v &= ~(0x7 << 16);
	v &= ~(1 << 4);

	if (primary > 0) {
		v |= (1 << 4);
		v |= ((primary - 1) << 16);
	}

	if (secondary > 0) {
		v |= (1 << 5);
		v |= ((secondary - 1) << 24);
	}

	v |= 1;

	rc = target_write_u32(sfb->target, sfb->base + SFCTL_R_CTL1, v);
	if (rc != ERROR_OK)
		return rc;
	rc = _sfctl_wait(sfb);
	if (rc != ERROR_OK)
		return rc;
	return rc;
}

static int _sfctl_readid(struct sfctl_flash_bank *sfb, uint32_t *id)
{
	int rc = ERROR_OK;
	uint8_t v[4];

	rc = _sfctl_wait(sfb);
	if (rc != ERROR_OK)
		return rc;
	rc = _sfctl_command(sfb, SPIFLASH_READ_ID, 0);
	if (rc != ERROR_OK)
		return rc;
	rc = _sfctl_go_with_bufs(sfb, 5, 0);
	if (rc != ERROR_OK)
		return rc;
	rc = _sfctl_wait(sfb);
	if (rc != ERROR_OK)
		return rc;
	rc = target_read_u32(sfb->target, sfb->base + SFCTL_R_BUF0, (uint32_t *)v);
	if (rc != ERROR_OK)
		return rc;
	*id = be_to_h_u32(v);
	return rc;
}

static int _sfctl_wrenable(struct sfctl_flash_bank *sfb)
{
	int rc;

	rc = _sfctl_wait(sfb);
	if (rc == ERROR_OK)
		rc = _sfctl_command(sfb, SPIFLASH_WRITE_ENABLE, 0);
	if (rc == ERROR_OK)
		rc = _sfctl_go_with_bufs(sfb, 1, 0);
	return _sfctl_wait(sfb);
}

static int sfctl_protect_check(struct flash_bank *bank)
{
	return ERROR_OK;
}

static int sfctl_write(struct flash_bank *bank,
	const uint8_t *buffer,
	uint32_t offset,
	uint32_t count)
{
	struct sfctl_flash_bank *sfb = bank->driver_priv;
	int rc;
	int i, cnt;

	rc = sfctl_entry_check(bank, false);
	if (rc != ERROR_OK)
		return rc;
	rc = _sfctl_wait(sfb);
	if (rc != ERROR_OK)
		return rc;
	while (count > 0) {

		cnt = count;
		if (cnt > 256)
			cnt = 256;

		rc = _sfctl_wrenable(sfb);
		if (rc != ERROR_OK)
			return rc;

		for (i = 0; rc == ERROR_OK && i < cnt; i++) {
			rc = target_write_u8(sfb->target, sfb->base + SFCTL_R_BUF1 + i, buffer[i]);
			if (rc != ERROR_OK)
				return rc;
		}

		rc = _sfctl_command(sfb, SPIFLASH_PAGE_PROGRAM, offset);
		if (rc != ERROR_OK)
			return rc;

		rc = _sfctl_go_with_bufs(sfb, 4, 256);
		if (rc != ERROR_OK)
			return rc;

		rc = _sfctl_wait(sfb);
		if (rc != ERROR_OK)
			return rc;

		buffer = buffer + cnt;
		count = count - cnt;
		offset = offset + cnt;
	}
	return rc;
}


static int sfctl_read(struct flash_bank *bank,
	uint8_t *buffer,
	uint32_t offset,
	uint32_t count)
{
	struct sfctl_flash_bank *sfb = bank->driver_priv;
	int rc;
	int i, cnt;

	rc = sfctl_entry_check(bank, false);
	if (rc != ERROR_OK)
		return rc;
	rc = _sfctl_wait(sfb);
	if (rc != ERROR_OK)
		return rc;

	memset(buffer, 0xEE, count);
	while (count > 0) {

		cnt = count;
		if (cnt > 256)
			cnt = 256;

		for (i = 0; rc == ERROR_OK && i < cnt; i++)
			rc = target_write_u8(sfb->target, sfb->base + SFCTL_R_BUF1 + i, 0xAA);
		if (rc == ERROR_OK)
			rc = _sfctl_command(sfb, SPIFLASH_FAST_READ, offset);
		if (rc == ERROR_OK)
			rc = _sfctl_go_with_bufs(sfb, 5, 256);
		for (i = 0; rc == ERROR_OK && i < cnt; i++)
			rc = target_read_u8(sfb->target, sfb->base + SFCTL_R_BUF1 + i, buffer + i);
		if (rc != ERROR_OK)
			break;

		buffer = buffer + cnt;
		count = count - cnt;
		offset = offset + cnt;
	}
	return rc;
}

static int _sfctl_erase_chip(struct sfctl_flash_bank *sfb)
{
	int rc;

	rc = _sfctl_wrenable(sfb);
	if (rc != ERROR_OK)
		return rc;
	rc = _sfctl_command(sfb, 0xC7, 0);
	if (rc != ERROR_OK)
		return rc;
	rc = _sfctl_go_with_bufs(sfb, 1, 0);
	return rc;
}

static int sfctl_erase(struct flash_bank *bank, int first, int last)
{
	struct sfctl_flash_bank *sfb = bank->driver_priv;
	int rc = ERROR_OK;
	int sector;

	rc = sfctl_entry_check(bank, false);
	if (rc != ERROR_OK)
		return rc;
	if ((first < 0) || (last < first) || (last >= bank->num_sectors)) {
		LOG_ERROR("Flash sector invalid");
		return ERROR_FLASH_SECTOR_INVALID;
	}
	for (sector = first; sector <= last; sector++) {
		if (bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	rc = _sfctl_wait(sfb);

	if (first == 0 && last == bank->num_sectors - 1) {
		LOG_INFO("%s: erasing whole chip", __func__);
		return _sfctl_erase_chip(sfb);
	}

	for (sector = first; rc == ERROR_OK && sector <= last; sector++) {
		rc = _sfctl_wrenable(sfb);
		if (rc == ERROR_OK)
			rc = _sfctl_command(sfb, sfb->dev->erase_cmd,
					sector * sfb->dev->sectorsize);
		if (rc == ERROR_OK)
			rc = _sfctl_go_with_bufs(sfb, 4, 0);
	}
	return rc;
}

static int _sfctl_command(struct sfctl_flash_bank *sfb, uint8_t command, uint32_t addr)
{
	return target_write_u32(sfb->target, sfb->base + SFCTL_R_BUF0,
				command | (addr << 8));
}
