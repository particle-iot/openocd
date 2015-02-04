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
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 *                                                                         *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>
#include "spi.h"
#include "imp.h"

#define CHECK_ERROR_RETURN(code) do { rc = code; if (rc != ERROR_OK) return rc; } while (0)
#define SFCTL_TIMEOUT 	0x100
#define SFCTL_R_CTL0 	0x400
#define SFCTL_R_CTL1	0x404
#define SFCTL_R_INTE	0x408
#define SFCTL_R_STS	0x40C
#define SFCTL_R_BUF0	0x500
#define SFCTL_R_BUF1	0x600

static int sfctl_probe(struct flash_bank *bank);
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
static int sfctl_erase_check(struct flash_bank *);
FLASH_BANK_COMMAND_HANDLER(sfctl_flash_bank_command);

struct flash_driver sfctl_flash = {
	.name = "sfctl",
	.flash_bank_command = sfctl_flash_bank_command,
	.erase = sfctl_erase,
	.protect = NULL,
	.write = sfctl_write,
	.read = sfctl_read,
	.probe = sfctl_probe,
	.auto_probe = sfctl_probe,
	.erase_check = sfctl_erase_check,
	.protect_check = sfctl_protect_check,
	.info = sfctl_get_info,
};

struct sfctl_flash_bank {
	struct target *target;
	bool probed;
	const struct flash_device *dev;
	uint32_t base;
};

static int _sfctl_command(struct sfctl_flash_bank *sfb, uint8_t command, uint32_t addr);
static int _sfctl_readid(struct sfctl_flash_bank *sfb, uint32_t *id);
static int _sfctl_wrenable(struct sfctl_flash_bank *sfb);

#define ENTRY_CHECK(bank, __probed) \
    do {								\
	if (((struct target*)bank->target)->state != TARGET_HALTED) {	\
		LOG_ERROR("Target not halted");				\
		return ERROR_TARGET_NOT_HALTED;				\
	}								\
	if (((struct sfctl_flash_bank*)bank->driver_priv)->probed) {	\
		if (__probed)						\
			return ERROR_OK;				\
	} else {							\
		if (!__probed)						\
			return ERROR_OK;				\
	}								\
    } while (false)

static int sfctl_probe(struct flash_bank *bank)
{
	struct sfctl_flash_bank *sfb = bank->driver_priv;
	uint32_t id = 0xFFFFFFFF;
	struct flash_sector *sectors;

	ENTRY_CHECK(bank, true);

	_sfctl_readid(sfb, &id);

	for (sfb->dev = flash_devices; sfb->dev->name; sfb->dev ++) {
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

	ENTRY_CHECK(bank, false);
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
	sfb->base = 0x40006000;
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
		CHECK_ERROR_RETURN(target_read_u32(sfb->target, sfb->base + SFCTL_R_STS, &sts));
		if ((sts & (1 << 2)) == 0)
			break;
		if (c > SFCTL_TIMEOUT)
			return ERROR_TARGET_TIMEOUT;
		alive_sleep(1);
	}
	return ERROR_OK;
}

static int _sfctl_go(struct sfctl_flash_bank *sfb)
{
	int rc;
	uint32_t v;
	
	CHECK_ERROR_RETURN(target_read_u32(sfb->target,sfb->base + SFCTL_R_CTL1, &v));
	v |= 1;
	CHECK_ERROR_RETURN(target_write_u32(sfb->target, sfb->base + SFCTL_R_CTL1, v));
	return rc;
}

static int _sfctl_buffers(struct sfctl_flash_bank *sfb, int primary, int secondary)
{
	int rc;
	uint32_t v;
	
	CHECK_ERROR_RETURN(target_read_u32(sfb->target,sfb->base + SFCTL_R_CTL1, &v));

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
		v |= ((secondary -1) << 24);
	}
	CHECK_ERROR_RETURN(target_write_u32(sfb->target, sfb->base + SFCTL_R_CTL1, v));
	return rc;

}

static uint32_t _swap(uint32_t v)
{
	v = ((v >> 8) & 0x00FF00FF) | ((v & 0x00FF00FF) << 8);
	v = ((v >> 16) & 0x0000FFFF) | ((v & 0x0000FFFF) << 16);
	return v;
}

static int _sfctl_readid(struct sfctl_flash_bank *sfb, uint32_t *id)
{
	int rc;
	uint32_t v;

	CHECK_ERROR_RETURN(_sfctl_wait(sfb));
	CHECK_ERROR_RETURN(_sfctl_command(sfb, SPIFLASH_READ_ID, 0));
	CHECK_ERROR_RETURN(_sfctl_buffers(sfb, 5, 0));
	CHECK_ERROR_RETURN(_sfctl_go(sfb));
	CHECK_ERROR_RETURN(target_read_u32(sfb->target, sfb->base + SFCTL_R_BUF0, &v));
	*id = _swap(v);
	return rc;
}

static int _sfctl_wrenable(struct sfctl_flash_bank *sfb)
{
	int rc;

	CHECK_ERROR_RETURN(_sfctl_wait(sfb));
	CHECK_ERROR_RETURN(_sfctl_command(sfb, SPIFLASH_WRITE_ENABLE, 0));
	CHECK_ERROR_RETURN(_sfctl_buffers(sfb, 1, 0));
	CHECK_ERROR_RETURN(_sfctl_go(sfb));
	return rc;
}

static int _sfctl_status(struct sfctl_flash_bank *sfb, uint32_t *result)
{
	int rc;

	CHECK_ERROR_RETURN(_sfctl_wait(sfb));
	CHECK_ERROR_RETURN(_sfctl_command(sfb, SPIFLASH_READ_STATUS, 0));
	CHECK_ERROR_RETURN(_sfctl_buffers(sfb, 2, 0));
	CHECK_ERROR_RETURN(_sfctl_go(sfb));
	CHECK_ERROR_RETURN(target_read_u32(sfb->target, sfb->base + SFCTL_R_BUF0, result));
	return rc;
}

static int sfctl_protect_check(struct flash_bank *bank)
{
	return ERROR_OK;
}

static int sfctl_erase_check(struct flash_bank *bank)
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
	uint32_t v;

	ENTRY_CHECK(bank, false);
	CHECK_ERROR_RETURN(_sfctl_wait(sfb));
	while (count > 0) {

		if ( (cnt = count) > 256)
			cnt = 256;

		_sfctl_wrenable(sfb);
		for (i = 0; i < cnt; i ++)
			CHECK_ERROR_RETURN(target_write_u8(sfb->target, sfb->base + SFCTL_R_BUF1 + i, buffer[i]));
		CHECK_ERROR_RETURN(_sfctl_command(sfb, SPIFLASH_PAGE_PROGRAM, offset));
		CHECK_ERROR_RETURN(_sfctl_buffers(sfb, 4, 256));
		CHECK_ERROR_RETURN(_sfctl_go(sfb));
		CHECK_ERROR_RETURN(_sfctl_wait(sfb));
		do {
		   CHECK_ERROR_RETURN(_sfctl_status(sfb, &v));
		}  while (v != 0);
		buffer = buffer + cnt;
		count = count - cnt;
		offset = offset + cnt;
	}
	return ERROR_OK;
}


static int sfctl_read(struct flash_bank *bank,
	uint8_t *buffer,
	uint32_t offset,
	uint32_t count)
{
	struct sfctl_flash_bank *sfb = bank->driver_priv;
	int rc;
	int i, cnt;

	ENTRY_CHECK(bank, false);
	CHECK_ERROR_RETURN(_sfctl_wait(sfb));
	while (count > 0) {

		if ( (cnt = count) > 256)
			cnt = 256;

		CHECK_ERROR_RETURN(_sfctl_command(sfb, SPIFLASH_FAST_READ, offset));
		CHECK_ERROR_RETURN(_sfctl_buffers(sfb, 5, 256));
		CHECK_ERROR_RETURN(_sfctl_go(sfb));
		CHECK_ERROR_RETURN(_sfctl_wait(sfb));
		for (i = 0; i < cnt; i ++)
			CHECK_ERROR_RETURN(target_read_u8(sfb->target, sfb->base + SFCTL_R_BUF1 + i, buffer + i));
		buffer = buffer + cnt;
		count = count - cnt;
		offset = offset + cnt;
	}
	return ERROR_OK;
}

static int sfctl_erase(struct flash_bank *bank, int first, int last)
{
	struct sfctl_flash_bank *sfb = bank->driver_priv;
	int rc = ERROR_OK;
	int sector;

	ENTRY_CHECK(bank, false);
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
	
	CHECK_ERROR_RETURN(_sfctl_wait(sfb));
	for (sector = first; sector <= last; sector++) {
		_sfctl_wrenable(sfb);
		CHECK_ERROR_RETURN(_sfctl_command(sfb, sfb->dev->erase_cmd, sector * sfb->dev->sectorsize));
		CHECK_ERROR_RETURN(_sfctl_buffers(sfb, 4, 0));
		CHECK_ERROR_RETURN(_sfctl_go(sfb));
		CHECK_ERROR_RETURN(_sfctl_wait(sfb));
	}
	return ERROR_OK;
}

static int _sfctl_command(struct sfctl_flash_bank *sfb, uint8_t command, uint32_t addr)
{
	return target_write_u32(sfb->target, sfb->base + SFCTL_R_BUF0,
			  	command | _swap(addr));
}
