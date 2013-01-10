/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2011 by Andreas Fritiofson                              *
 *   andreas.fritiofson@gmail.com                                          *
 *                                                                         *
 *   Copyright (C) 2013 by Roman Dmitrienko                                *
 *   me@iamroman.org                                                       *
 *
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


/* keep family IDs in decimal */
#define EFM_FAMILY_ID_GECKO             71
#define EFM_FAMILY_ID_GIANT_GECKO       72
#define EFM_FAMILY_ID_TINY_GECKO        73
#define EFM_FAMILY_ID_LEOPARD_GECKO     74

#define EFM32_FLASH_ERASE_TMO           100
#define EFM32_FLASH_WDATAREADY_TMO      100
#define EFM32_FLASH_WRITE_TMO           100

/* size in bytes, not words; must fit all Gecko devices */
#define LOCKBITS_PAGE_SZ                512

#define EFM32_MSC_INFO_BASE             0x0fe00000

#define EFM32_MSC_USER_DATA             EFM32_MSC_INFO_BASE
#define EFM32_MSC_LOCK_BITS             (EFM32_MSC_INFO_BASE+0x4000)
#define EFM32_MSC_DEV_INFO              (EFM32_MSC_INFO_BASE+0x8000)

/* PAGE_SIZE is only present in Leopard and Giant Gecko MCUs */
#define EFM32_MSC_DI_PAGE_SIZE          (EFM32_MSC_DEV_INFO+0x1e7)
#define EFM32_MSC_DI_FLASH_SZ           (EFM32_MSC_DEV_INFO+0x1f8)
#define EFM32_MSC_DI_RAM_SZ             (EFM32_MSC_DEV_INFO+0x1fa)
#define EFM32_MSC_DI_PART_NUM           (EFM32_MSC_DEV_INFO+0x1fc)
#define EFM32_MSC_DI_PART_FAMILY        (EFM32_MSC_DEV_INFO+0x1fe)
#define EFM32_MSC_DI_PROD_REV           (EFM32_MSC_DEV_INFO+0x1ff)

#define EFM32_MSC_REGBASE               0x400c0000
#define EFM32_MSC_WRITECTRL             (EFM32_MSC_REGBASE+0x008)
#define EFM32_MSC_WRITECTRL_WREN_MASK   0x1
#define EFM32_MSC_WRITECMD              (EFM32_MSC_REGBASE+0x00c)
#define EFM32_MSC_WRITECMD_LADDRIM_MASK 0x1
#define EFM32_MSC_WRITECMD_ERASEPAGE_MASK 0x2
#define EFM32_MSC_WRITECMD_WRITEONCE_MASK 0x8
#define EFM32_MSC_ADDRB                 (EFM32_MSC_REGBASE+0x010)
#define EFM32_MSC_WDATA                 (EFM32_MSC_REGBASE+0x018)
#define EFM32_MSC_STATUS                (EFM32_MSC_REGBASE+0x01c)
#define EFM32_MSC_STATUS_BUSY_MASK      0x1
#define EFM32_MSC_STATUS_LOCKED_MASK    0x2
#define EFM32_MSC_STATUS_INVADDR_MASK   0x4
#define EFM32_MSC_STATUS_WDATAREADY_MASK 0x8
#define EFM32_MSC_STATUS_WORDTIMEOUT_MASK 0x10
#define EFM32_MSC_STATUS_ERASEABORTED_MASK 0x20


struct efm32x_flash_bank {
	int probed;
	uint8_t lb_page[LOCKBITS_PAGE_SZ];
};

struct efm32_info {
	uint16_t flash_sz_kib;
	uint16_t ram_sz_kib;
	uint16_t part_num;
	uint8_t part_family;
	uint8_t prod_rev;
	uint16_t page_size;
};


static int efm32x_write(struct flash_bank *bank, uint8_t *buffer,
	uint32_t offset, uint32_t count);


static int efm32x_get_flash_size(struct flash_bank *bank, uint16_t *flash_sz)
{
	return target_read_u16(bank->target, EFM32_MSC_DI_FLASH_SZ, flash_sz);
}

static int efm32x_get_ram_size(struct flash_bank *bank, uint16_t *ram_sz)
{
	return target_read_u16(bank->target, EFM32_MSC_DI_RAM_SZ, ram_sz);
}

static int efm32x_get_part_num(struct flash_bank *bank, uint16_t *pnum)
{
	return target_read_u16(bank->target, EFM32_MSC_DI_PART_NUM, pnum);
}

static int efm32x_get_part_family(struct flash_bank *bank, uint8_t *pfamily)
{
	return target_read_u8(bank->target, EFM32_MSC_DI_PART_FAMILY, pfamily);
}

static int efm32x_get_prod_rev(struct flash_bank *bank, uint8_t *prev)
{
	return target_read_u8(bank->target, EFM32_MSC_DI_PROD_REV, prev);
}

static int efm32x_read_info(struct flash_bank *bank,
	struct efm32_info *efm32_info)
{
	int ret;
	uint32_t cpuid = 0;

	memset(efm32_info, 0, sizeof(struct efm32_info));

	ret = target_read_u32(bank->target, 0xe000ed00, &cpuid);
	if (ERROR_OK != ret)
		return ret;

	if (((cpuid >> 4) & 0xfff) == 0xc23) {
		/* Cortex M3 device */
	} else {
		LOG_ERROR("Target is not CortexM3");
		return ERROR_FAIL;
	}

	ret = efm32x_get_flash_size(bank, &(efm32_info->flash_sz_kib));
	if (ERROR_OK != ret)
		return ret;

	ret = efm32x_get_ram_size(bank, &(efm32_info->ram_sz_kib));
	if (ERROR_OK != ret)
		return ret;

	ret = efm32x_get_part_num(bank, &(efm32_info->part_num));
	if (ERROR_OK != ret)
		return ret;

	ret = efm32x_get_part_family(bank, &(efm32_info->part_family));
	if (ERROR_OK != ret)
		return ret;

	ret = efm32x_get_prod_rev(bank, &(efm32_info->prod_rev));
	if (ERROR_OK != ret)
		return ret;

	if (EFM_FAMILY_ID_GECKO == efm32_info->part_family ||
			EFM_FAMILY_ID_TINY_GECKO == efm32_info->part_family)
		efm32_info->page_size = 512;
	else if (EFM_FAMILY_ID_GIANT_GECKO == efm32_info->part_family ||
			EFM_FAMILY_ID_LEOPARD_GECKO == efm32_info->part_family) {
		uint8_t pg_size = 0;

		ret = target_read_u8(bank->target, EFM32_MSC_DI_PAGE_SIZE,
			&pg_size);
		if (ERROR_OK != ret)
			return ret;

		efm32_info->page_size = (1 << ((pg_size+10) & 0xff));

		if ((2048 != efm32_info->page_size) &&
				(4096 != efm32_info->page_size)) {
			LOG_ERROR("Invalid page size %u", efm32_info->page_size);
			return ERROR_FAIL;
		}
	} else {
		LOG_ERROR("Unknown MCU family %d", efm32_info->part_family);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

/* flash bank efm32 <base> <size> 0 0 <target#>
 */
FLASH_BANK_COMMAND_HANDLER(efm32x_flash_bank_command)
{
	struct efm32x_flash_bank *efm32x_info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	efm32x_info = malloc(sizeof(struct efm32x_flash_bank));

	bank->driver_priv = efm32x_info;
	efm32x_info->probed = 0;
	memset(efm32x_info->lb_page, 0xff, LOCKBITS_PAGE_SZ);

	return ERROR_OK;
}

/* set or reset given bits in a register */
static int efm32x_set_reg_bits(struct flash_bank *bank, uint32_t reg,
	uint32_t bitmask, int set)
{
	int ret = 0;
	uint32_t reg_val = 0;

	ret = target_read_u32(bank->target, reg, &reg_val);
	if (ERROR_OK != ret)
		return ret;

	if (set)
		reg_val |= bitmask;
	else
		reg_val &= ~bitmask;

	return target_write_u32(bank->target, reg, reg_val);
}

static int efm32x_set_wren(struct flash_bank *bank, int write_enable)
{
	return efm32x_set_reg_bits(bank, EFM32_MSC_WRITECTRL,
		EFM32_MSC_WRITECTRL_WREN_MASK, write_enable);
}

static int efm32x_wait_status(struct flash_bank *bank, int timeout,
	uint32_t wait_mask, int wait_for_set)
{
	int ret = 0;
	uint32_t status = 0;

	while (1) {
		ret = target_read_u32(bank->target, EFM32_MSC_STATUS, &status);
		if (ERROR_OK != ret)
			break;

		LOG_DEBUG("status: 0x%" PRIx32 "", status);

		if (((status & wait_mask) == 0) && (0 == wait_for_set))
			break;
		else if (((status & wait_mask) != 0) && wait_for_set)
			break;

		if (timeout-- <= 0) {
			LOG_ERROR("timed out waiting for MSC status");
			return ERROR_FAIL;
		}

		alive_sleep(1);
	}

	if (status & EFM32_MSC_STATUS_ERASEABORTED_MASK)
		LOG_WARNING("page erase was aborted");

	return ret;
}

static int efm32x_erase_page(struct flash_bank *bank, uint32_t addr)
{
	/* this function DOES NOT set WREN; must be set already */
	/* 1. write address to ADDRB
	   2. write LADDRIM
	   3. check status (INVADDR, LOCKED)
	   4. write ERASEPAGE
	   5. wait until !STATUS_BUSY
	 */
	int ret = 0;
	uint32_t status = 0;

	LOG_DEBUG("erasing flash page at 0x%08x", addr);

	ret = target_write_u32(bank->target, EFM32_MSC_ADDRB, addr);
	if (ERROR_OK != ret)
		return ret;

	ret = efm32x_set_reg_bits(bank, EFM32_MSC_WRITECMD,
		EFM32_MSC_WRITECMD_LADDRIM_MASK, 1);
	if (ERROR_OK != ret)
		return ret;

	ret = target_read_u32(bank->target, EFM32_MSC_STATUS, &status);
	if (ERROR_OK != ret)
		return ret;

	LOG_DEBUG("status 0x%x", status);

	if (status & EFM32_MSC_STATUS_LOCKED_MASK) {
		LOG_ERROR("Page is locked");
		return ERROR_FAIL;
	} else if (status & EFM32_MSC_STATUS_INVADDR_MASK) {
		LOG_ERROR("Invalid address 0x%x", addr);
		return ERROR_FAIL;
	}

	ret = efm32x_set_reg_bits(bank, EFM32_MSC_WRITECMD,
		EFM32_MSC_WRITECMD_ERASEPAGE_MASK, 1);
	if (ERROR_OK != ret)
		return ret;

	return efm32x_wait_status(bank, EFM32_FLASH_ERASE_TMO,
		EFM32_MSC_STATUS_BUSY_MASK, 0);
}

static int efm32x_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	int i = 0;
	int ret = 0;

	if (TARGET_HALTED != target->state) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = efm32x_set_wren(bank, 1);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to enable MSC write");
		return ret;
	}

	for (i = first; i <= last; i++) {
		ret = efm32x_erase_page(bank, bank->sectors[i].offset);
		if (ERROR_OK != ret)
			LOG_ERROR("Failed to erase page %d", i);
	}

	ret = efm32x_set_wren(bank, 0);

	return ret;
}

static int efm32x_read_lock_data(struct flash_bank *bank)
{
	struct efm32x_flash_bank *efm32x_info = bank->driver_priv;
	struct target *target = bank->target;
	int i = 0;
	int data_size = 0;
	uint32_t *ptr = NULL;
	int ret = 0;

	assert(!(bank->num_sectors & 0x1f));

	data_size = bank->num_sectors / 8; /* number of data bytes */
	data_size /= 4; /* ...and data dwords */

	ptr = (uint32_t *)efm32x_info->lb_page;

	for (i = 0; i < data_size; i++, ptr++) {
		ret = target_read_u32(target, EFM32_MSC_LOCK_BITS+i*4, ptr);
		if (ERROR_OK != ret) {
			LOG_ERROR("Failed to read PLW %d", i);
			return ret;
		}
	}

	/* also, read ULW, DLW and MLW */

	/* ULW, word 126 */
	ptr = ((uint32_t *)efm32x_info->lb_page) + 126;
	ret = target_read_u32(target, EFM32_MSC_LOCK_BITS+126*4, ptr);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read ULW");
		return ret;
	}

	/* DLW, word 127 */
	ptr = ((uint32_t *)efm32x_info->lb_page) + 127;
	ret = target_read_u32(target, EFM32_MSC_LOCK_BITS+127*4, ptr);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read DLW");
		return ret;
	}

	/* MLW, word 125, present in GG and LG */
	ptr = ((uint32_t *)efm32x_info->lb_page) + 125;
	ret = target_read_u32(target, EFM32_MSC_LOCK_BITS+125*4, ptr);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read MLW");
		return ret;
	}

	return ERROR_OK;
}

static int efm32x_write_lock_data(struct flash_bank *bank)
{
	struct efm32x_flash_bank *efm32x_info = bank->driver_priv;
	int ret = 0;

	ret = efm32x_erase_page(bank, EFM32_MSC_LOCK_BITS);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to erase LB page");
		return ret;
	}

	return efm32x_write(bank, efm32x_info->lb_page, EFM32_MSC_LOCK_BITS,
		LOCKBITS_PAGE_SZ);
}

static int efm32x_get_page_lock(struct flash_bank *bank, size_t page)
{
	struct efm32x_flash_bank *efm32x_info = bank->driver_priv;
	uint32_t dw = ((uint32_t *)efm32x_info->lb_page)[page >> 5];
	uint32_t mask = 0;

	mask = 1 << (page & 0x1f);

	return (dw & mask) ? 0 : 1;
}

static int efm32x_set_page_lock(struct flash_bank *bank, size_t page, int set)
{
	struct efm32x_flash_bank *efm32x_info = bank->driver_priv;
	uint32_t *dw = &((uint32_t *)efm32x_info->lb_page)[page >> 5];
	uint32_t mask = 0;

	mask = 1 << (page & 0x1f);

	if (!set)
		*dw |= mask;
	else
		*dw &= ~mask;

	return ERROR_OK;
}

static int efm32x_protect(struct flash_bank *bank, int set, int first, int last)
{
	struct target *target = bank->target;
	int i = 0;
	int ret = 0;

	if (!set) {
		LOG_ERROR("Erase device data to reset page locks");
		return ERROR_FAIL;
	}

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	for (i = first; i <= last; i++) {
		ret = efm32x_set_page_lock(bank, i, set);
		if (ERROR_OK != ret) {
			LOG_ERROR("Failed to set lock on page %d", i);
			return ret;
		}
	}

	ret = efm32x_write_lock_data(bank);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to write LB page");
		return ret;
	}

	return ERROR_OK;
}

static int efm32x_write_block(struct flash_bank *bank, uint8_t *buf,
	uint32_t offset, uint32_t count)
{
	return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
}

static int efm32x_write_word(struct flash_bank *bank, uint32_t addr,
	uint32_t val)
{
	/* this function DOES NOT set WREN; must be set already */
	/* 1. write address to ADDRB
	   2. write LADDRIM
	   3. check status (INVADDR, LOCKED)
	   4. wait for WDATAREADY
	   5. write data to WDATA
	   6. write WRITECMD_WRITEONCE to WRITECMD
	   7. wait until !STATUS_BUSY
	 */

	/* FIXME: EFM32G ref states (7.3.2) that writes should be
	 * performed twice per dword */

	int ret = 0;
	uint32_t status = 0;

	/* if not called, GDB errors will be reported during large writes */
	keep_alive();

	ret = target_write_u32(bank->target, EFM32_MSC_ADDRB, addr);
	if (ERROR_OK != ret)
		return ret;

	ret = efm32x_set_reg_bits(bank, EFM32_MSC_WRITECMD,
		EFM32_MSC_WRITECMD_LADDRIM_MASK, 1);
	if (ERROR_OK != ret)
		return ret;

	ret = target_read_u32(bank->target, EFM32_MSC_STATUS, &status);
	if (ERROR_OK != ret)
		return ret;

	LOG_DEBUG("status 0x%x", status);

	if (status & EFM32_MSC_STATUS_LOCKED_MASK) {
		LOG_ERROR("Page is locked");
		return ERROR_FAIL;
	} else if (status & EFM32_MSC_STATUS_INVADDR_MASK) {
		LOG_ERROR("Invalid address 0x%x", addr);
		return ERROR_FAIL;
	}

	ret = efm32x_wait_status(bank, EFM32_FLASH_WDATAREADY_TMO,
		EFM32_MSC_STATUS_WDATAREADY_MASK, 1);
	if (ERROR_OK != ret) {
		LOG_ERROR("Wait for WDATAREADY failed");
		return ret;
	}

	ret = target_write_u32(bank->target, EFM32_MSC_WDATA, val);
	if (ERROR_OK != ret) {
		LOG_ERROR("WDATA write failed");
		return ret;
	}

	ret = target_write_u32(bank->target, EFM32_MSC_WRITECMD,
		EFM32_MSC_WRITECMD_WRITEONCE_MASK);
	if (ERROR_OK != ret) {
		LOG_ERROR("WRITECMD write failed");
		return ret;
	}

	ret = efm32x_wait_status(bank, EFM32_FLASH_WRITE_TMO,
		EFM32_MSC_STATUS_BUSY_MASK, 0);
	if (ERROR_OK != ret) {
		LOG_ERROR("Wait for BUSY failed");
		return ret;
	}

	return ERROR_OK;
}

static int efm32x_write(struct flash_bank *bank, uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	uint8_t *new_buffer = NULL;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & 0x3) {
		LOG_ERROR("offset 0x%" PRIx32 " breaks required 4-byte "
			"alignment", offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	if (count & 0x3) {
		uint32_t old_count = count;
		count = (old_count | 3) + 1;
		new_buffer = malloc(count);
		if (new_buffer == NULL) {
			LOG_ERROR("odd number of bytes to write and no memory "
				"for padding buffer");
			return ERROR_FAIL;
		}
		LOG_INFO("odd number of bytes to write (%d), extending to %d "
			"and padding with 0xff", old_count, count);
		memset(buffer, 0xff, count);
		buffer = memcpy(new_buffer, buffer, old_count);
	}

	uint32_t words_remaining = count / 4;
	int retval, retval2;

	/* unlock flash registers */
	retval = efm32x_set_wren(bank, 1);
	if (retval != ERROR_OK)
		goto cleanup;

	/* try using a block write */
	retval = efm32x_write_block(bank, buffer, offset, words_remaining);

	if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE) {
		/* if block write failed (no sufficient working area),
		 * we use normal (slow) single word accesses */
		LOG_WARNING("couldn't use block writes, falling back to single "
			"memory accesses");

		while (words_remaining > 0) {
			uint32_t value;
			memcpy(&value, buffer, sizeof(uint32_t));

			retval = efm32x_write_word(bank, offset, value);
			if (retval != ERROR_OK)
				goto reset_pg_and_lock;

			words_remaining--;
			buffer += 4;
			offset += 4;
		}
	}

reset_pg_and_lock:
	retval2 = efm32x_set_wren(bank, 0);
	if (retval == ERROR_OK)
		retval = retval2;

cleanup:
	if (new_buffer)
		free(new_buffer);

	return retval;
}

static int efm32x_probe(struct flash_bank *bank)
{
	struct efm32x_flash_bank *efm32x_info = bank->driver_priv;
	struct efm32_info efm32_mcu_info;
	int ret;
	int i;
	uint32_t base_address = 0x00000000;

	efm32x_info->probed = 0;
	memset(efm32x_info->lb_page, 0xff, LOCKBITS_PAGE_SZ);

	ret = efm32x_read_info(bank, &efm32_mcu_info);
	if (ERROR_OK != ret)
		return ret;

	switch (efm32_mcu_info.part_family) {
		case EFM_FAMILY_ID_GECKO:
			LOG_INFO("Gecko MCU detected");
			break;
		case EFM_FAMILY_ID_GIANT_GECKO:
			LOG_INFO("Giant Gecko MCU detected");
			break;
		case EFM_FAMILY_ID_TINY_GECKO:
			LOG_INFO("Tiny Gecko MCU detected");
			break;
		case EFM_FAMILY_ID_LEOPARD_GECKO:
			LOG_INFO("Leopard Gecko MCU detected");
			break;
		default:
			LOG_ERROR("Unsupported MCU family %d",
				efm32_mcu_info.part_family);
			return ERROR_FAIL;
	}

	LOG_INFO("flash size = %dkbytes", efm32_mcu_info.flash_sz_kib);
	LOG_INFO("flash page size = %dbytes", efm32_mcu_info.page_size);

	assert(0 != efm32_mcu_info.page_size);

	int num_pages = efm32_mcu_info.flash_sz_kib * 1024 /
		efm32_mcu_info.page_size;

	assert(num_pages > 0);

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}

	bank->base = base_address;
	bank->size = (num_pages * efm32_mcu_info.page_size);
	bank->num_sectors = num_pages;

	ret = efm32x_read_lock_data(bank);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read LB data");
		return ret;
	}

	bank->sectors = malloc(sizeof(struct flash_sector) * num_pages);

	for (i = 0; i < num_pages; i++) {
		bank->sectors[i].offset = i * efm32_mcu_info.page_size;
		bank->sectors[i].size = efm32_mcu_info.page_size;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = efm32x_get_page_lock(bank, i);
		if (bank->sectors[i].is_protected)
			LOG_DEBUG("page %d protected", i);
		else
			LOG_DEBUG("page %d NOT protected", i);
	}

	efm32x_info->probed = 1;

	return ERROR_OK;
}

static int efm32x_auto_probe(struct flash_bank *bank)
{
	struct efm32x_flash_bank *efm32x_info = bank->driver_priv;
	if (efm32x_info->probed)
		return ERROR_OK;
	return efm32x_probe(bank);
}

static int efm32x_protect_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	int ret = 0;
	int i = 0;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = efm32x_read_lock_data(bank);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read LB data");
		return ret;
	}

	assert(NULL != bank->sectors);

	for (i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_protected = efm32x_get_page_lock(bank, i);

	return ERROR_OK;
}

static int get_efm32x_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct efm32_info info;
	int ret = 0;
	int printed = 0;

	ret = efm32x_read_info(bank, &info);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read EFM32 info");
		return ret;
	}

	printed = snprintf(buf, buf_size, "EFM32 ");
	buf += printed;
	buf_size -= printed;

	if (0 >= buf_size)
		return ERROR_BUF_TOO_SMALL;

	switch (info.part_family) {
		case EFM_FAMILY_ID_GECKO:
			printed = snprintf(buf, buf_size, "Gecko");
			break;
		case EFM_FAMILY_ID_GIANT_GECKO:
			printed = snprintf(buf, buf_size, "Giant Gecko");
			break;
		case EFM_FAMILY_ID_TINY_GECKO:
			printed = snprintf(buf, buf_size, "Tiny Gecko");
			break;
		case EFM_FAMILY_ID_LEOPARD_GECKO:
			printed = snprintf(buf, buf_size, "Leopard Gecko");
			break;
	}

	buf += printed;
	buf_size -= printed;

	if (0 >= buf_size)
		return ERROR_BUF_TOO_SMALL;

	printed = snprintf(buf, buf_size, " - Rev: %d", info.prod_rev);
	buf += printed;
	buf_size -= printed;

	if (0 >= buf_size)
		return ERROR_BUF_TOO_SMALL;

	return ERROR_OK;
}


static const struct command_registration efm32x_exec_command_handlers[] = {
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration efm32x_command_handlers[] = {
	{
		.name = "efm32",
		.mode = COMMAND_ANY,
		.help = "efm32 flash command group",
		.usage = "",
		.chain = efm32x_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver efm32_flash = {
	.name = "efm32",
	.commands = efm32x_command_handlers,
	.flash_bank_command = efm32x_flash_bank_command,
	.erase = efm32x_erase,
	.protect = efm32x_protect,
	.write = efm32x_write,
	.read = default_flash_read,
	.probe = efm32x_probe,
	.auto_probe = efm32x_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = efm32x_protect_check,
	.info = get_efm32x_info,
};
