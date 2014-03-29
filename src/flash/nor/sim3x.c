/***************************************************************************
 *   Copyright (C) 2014 by Ladislav Bábel                                  *
 *   ladababel@seznam.cz                                                   *
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
#include <helper/time_support.h>
#include <target/algorithm.h>
#include <target/cortex_m.h>

/* Device auto detection is not working for now,
 * because reading of DEVICEID is not working correct.
 * The reason is commented in the code below
 */
/* #define DEVICE_AUTO_DETECTION */

/* SI32_DEVICEID0 */
#define DEVICEID0_DEVICEID0 (0x400490C0)
#define DEVICEID0_DEVICEID1 (0x400490D0)
#define DEVICEID0_DEVICEID2 (0x400490E0)
#define DEVICEID0_DEVICEID3 (0x400490F0)

/* Flash */
#define FLASH_BASE_ADDRESS (0x00000000)
#define LOCK_WORD_ADDRESS (0x0003FFFC)

/* SI32_FLASHCTRL_0 */
#define FLASHCTRL0_CONFIG ((struct sim3x_register_all_set_clr *)0x4002E000)
#define FLASHCTRL0_CONFIG_ERASEEN_MASK (0x00040000)
#define FLASHCTRL0_CONFIG_BUSYF_MASK (0x00100000)

#define FLASHCTRL0_WRADDR (0x4002E0A0)
#define FLASHCTRL0_WRDATA (0x4002E0B0)

#define FLASHCTRL0_KEY (0x4002E0C0)
#define FLASHCTRL0_KEY_INITIAL_UNLOCK (0x000000A5)
#define FLASHCTRL0_KEY_SINGLE_UNLOCK (0x000000F1)
#define FLASHCTRL0_KEY_MULTIPLE_UNLOCK (0x000000F2)
#define FLASHCTRL0_KEY_MULTIPLE_LOCK (0x0000005A)

#define FLASH_BUSY_TIMEOUT (100)

/* SI32_RSTSRC_0 */
#define RSTSRC0_RESETEN ((struct sim3x_register_all_set_clr *)0x4002D060)
#define RSTSRC0_RESETEN_VMONREN_MASK  (0x00000004)
#define RSTSRC0_RESETEN_SWREN_MASK (0x00000040)

/* SI32_VMON_0 */
#define VMON0_CONTROL ((struct sim3x_register_all_set_clr *)0x4002F000)
#define VMON0_CONTROL_VMONEN_MASK (0x80000000)

/* SI32_CLKCTRL_0 */
#define CLKCTRL0_APBCLKG0 ((struct sim3x_register_all_set_clr *)0x4002D020)
#define CLKCTRL0_APBCLKG0_FLCTRLCEN_MASK (0x40000000)

/* SI32_WDTIMER_0 */
#define WDTIMER0_CONTROL ((struct sim3x_register_all_set_clr *)0x40030000)
#define WDTIMER0_CONTROL_DBGMD_MASK (0x00000002)

#define WDTIMER0_STATUS ((sim3x_register_all_set_clr *)0x40030010)
#define WDTIMER0_STATUS_KEYSTS_MASK (0x00000001)
#define WDTIMER0_STATUS_PRIVSTS_MASK (0x00000002)

#define WDTIMER0_THRESHOLD (0x40030020)

#define WDTIMER0_WDTKEY (0x40030030)
#define WDTIMER0_KEY_ATTN (0x000000A5)
#define WDTIMER0_KEY_WRITE (0x000000F1)
#define WDTIMER0_KEY_RESET (0x000000CC)
#define WDTIMER0_KEY_DISABLE (0x000000DD)
#define WDTIMER0_KEY_START (0x000000EE)
#define WDTIMER0_KEY_LOCK (0x000000FF)

struct sim3x_register_all_set_clr {
	uint32_t all;
	uint32_t set;
	uint32_t clr;
};

struct sim3x_info {
	int probed;
	int flash_locked;
	uint16_t flash_size_kb;
	uint16_t flash_page_size;
	uint16_t ram_size_kb;
	uint16_t part_number;
	char part_family;
	uint8_t device_revision;
	char device_package[4];
};

static int sim3x_flash_lock_check(struct flash_bank *bank)
{
	int ret;
	uint32_t lock_word;

	ret = target_read_u32(bank->target, LOCK_WORD_ADDRESS, &lock_word);
	if (ERROR_OK != ret) {
		LOG_ERROR("Can not read Lock Word");
		return ret;
	}

	if(lock_word == 0xFFFFFFFF) {
		lock_word = 0; /* Unlocked */
	} else {
		lock_word = 1; /* Locked */
	}

	((struct sim3x_info *) bank->driver_priv)->flash_locked = lock_word;

	return ERROR_OK;
}

static int sim3x_read_info(struct flash_bank *bank)
{
	int ret;
	uint32_t cpuid = 0;
	struct sim3x_info * sim3x_info;

#ifdef DEVICE_AUTO_DETECTION
	uint32_t device_id;
	int part_number;
	char part_num_string[4];
#endif

	sim3x_info = bank->driver_priv;

	/* Core check */
	ret = target_read_u32(bank->target, CPUID, &cpuid);
	if (ERROR_OK != ret)
		return ret;

	if (((cpuid >> 4) & 0xfff) != 0xc23) {
		LOG_ERROR("Target is not CortexM3");
		return ERROR_FAIL;
	}

#ifdef DEVICE_AUTO_DETECTION
	/* Autodetection does not work,
	 * because unexpected values are read from MCU
	 */

	/* MCU check */
	ret = target_read_u32(bank->target, DEVICEID0_DEVICEID2, &device_id);
	if (ERROR_OK != ret)
		return ret;

	/* According to the documentation DEVICEID2 must by 0x00004D33 (..M3),
	 * but in my case it is 0x4DF79C9D
	 */
	if(device_id != 0x00004D33) {
		LOG_ERROR("Unsupported MCU");
		return ERROR_FAIL;
	}

	/* Family and Part number */
	ret = target_read_u32(bank->target, DEVICEID0_DEVICEID1, &device_id);
	if (ERROR_OK != ret)
		return ret;

	/* According to the documentation DEVICEID1 looks for example like this 0x55313637 (U167),
	 * but in my case it is 0x11E1BCCF
	 */
	part_num_string[0] = device_id >> 16;
	part_num_string[1] = device_id >> 8;
	part_num_string[2] = device_id;
	part_num_string[3] = 0;

	part_number = atoi(part_num_string);

	if (!isalpha(device_id >> 24) || part_number < 100 || part_number > 999) {
		LOG_ERROR("Unsupported MCU");
		return ERROR_FAIL;
	}

	sim3x_info->part_family = device_id >> 24;
	sim3x_info->part_number = part_number;

	/* Package and Revision */
	ret = target_read_u32(bank->target, DEVICEID0_DEVICEID0, &device_id);
	if (ERROR_OK != ret)
		return ret;

	/* According to the documentation DEVICEID0 looks for example like this 0x51000001 (package = Q, revision = 1(B)),
	 * but in my case it is 0x2F6466D9
	 */
	sim3x_info->device_package[0] = device_id >> 24;
	sim3x_info->device_package[1] = device_id >> 16;
	sim3x_info->device_package[2] = device_id >> 8;
	sim3x_info->device_package[3] = 0;

	sim3x_info->device_revision = device_id;
#else
	/* hardcoded to SiM3U167-B-GQ for now */
	sim3x_info->part_family = 'U';
	sim3x_info->part_number = 167;
	sim3x_info->device_package[0] = 'Q';
	sim3x_info->device_package[1] = 0;
	sim3x_info->device_package[2] = 0;
	sim3x_info->device_package[3] = 0;
	sim3x_info->device_revision = 1;
#endif

	/* Flash lock */
	ret = sim3x_flash_lock_check(bank);
	if (ERROR_OK != ret)
		return ret;

	/* Size can not by selected automatically,
	 * because device auto detection is not working
	 */
	sim3x_info->ram_size_kb = 32;
	sim3x_info->flash_size_kb = bank->size == 0 ? 256 : bank->size / 1024;
	sim3x_info->flash_page_size = 1024;

	return ERROR_OK;
}

/* flash bank sim3x 0 0 0 0 <target#> */
FLASH_BANK_COMMAND_HANDLER(sim3x_flash_bank_command)
{
	struct sim3x_info * sim3x_info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	sim3x_info = malloc(sizeof(struct sim3x_info));
	sim3x_info->probed = 0;
	bank->driver_priv = sim3x_info;

	return ERROR_OK;
}

static int sim3x_erase_page(struct flash_bank *bank, uint32_t addr)
{
	int ret, i;
	uint32_t temp;
	struct target * target;

	target = bank->target;

	for(i = 0; i < FLASH_BUSY_TIMEOUT; i++) {
		ret = target_read_u32(target, (uint32_t) &FLASHCTRL0_CONFIG->all, &temp);
		if (ERROR_OK != ret)
			return ret;

		if ((temp & FLASHCTRL0_CONFIG_BUSYF_MASK) == 0) { /* If is not busy */
			if ((temp & FLASHCTRL0_CONFIG_ERASEEN_MASK) == 0) { /* If erase is not enabled */
				/* Enter Flash Erase Mode */
				ret = target_write_u32(target,
						(uint32_t) &FLASHCTRL0_CONFIG->set,
						FLASHCTRL0_CONFIG_ERASEEN_MASK);
				if (ERROR_OK != ret)
					return ret;
			}

			/* Write the address of the Flash page to WRADDR */
			ret = target_write_u32(target, FLASHCTRL0_WRADDR, addr);
			if (ERROR_OK != ret)
				return ret;

			/* Write the inital unlock value to KEY */
			ret = target_write_u32(target, FLASHCTRL0_KEY,
			FLASHCTRL0_KEY_INITIAL_UNLOCK);
			if (ERROR_OK != ret)
				return ret;

			/* Write the single unlock value to KEY */
			ret = target_write_u32(target, FLASHCTRL0_KEY,
			FLASHCTRL0_KEY_SINGLE_UNLOCK);
			if (ERROR_OK != ret)
				return ret;

			/* Write any value to WRDATA to initiate the page erase */
			ret = target_write_u32(target, FLASHCTRL0_WRDATA, 0);
			if (ERROR_OK != ret)
				return ret;

			return ERROR_OK;
		}

		alive_sleep(1);
	}

	LOG_ERROR("timed out waiting for FLASHCTRL0_CONFIG_BUSYF");
	return ERROR_FAIL;
}

static int sim3x_flash_erase(struct flash_bank *bank, int first, int last)
{
	int ret, i;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	for (i = first; i <= last; i++) {
		ret = sim3x_erase_page(bank, bank->sectors[i].offset);
		if (ERROR_OK != ret)
			return ret;
	}

	return ERROR_OK;
}

static int sim3x_write_block(struct flash_bank *bank, const uint8_t *buf,
		uint32_t offset, uint32_t count) /* count is count of half words (2 bytes)! */
{
	struct target *target = bank->target;
	uint32_t buffer_size = 16384;
	struct working_area *write_algorithm;
	struct working_area *source;
	uint32_t address = bank->base + offset;
	struct reg_param reg_params[5];
	struct armv7m_algorithm armv7m_info;
	int ret = ERROR_OK;

	static const uint8_t sim3x_flash_write_code[] = {
		/* Disable erase operations (ERASEEN = 0) */
		0x4F, 0xF4, 0x80, 0x26, /* mov     r6, #ERASEEN */
		0x86, 0x60, /* str     r6, [r0, #FLASHCTRL_CONFIG + BIT_CLR] */

		/* Write the initial unlock value to KEY (0xA5) */
		0xA5, 0x26, /* movs    r6, #INITIAL_UNLOCK */
		0xC0, 0xF8, 0xC0, 0x60, /* str     r6, [r0, #FLASHCTRL_KEY] */

		/* Write the multiple unlock value to KEY (0xF2) */
		0xF2, 0x26, /* movs    r6, #MULTIPLE_UNLOCK */
		0xC0, 0xF8, 0xC0, 0x60, /* str     r6, [r0, #FLASHCTRL_KEY] */

		/* wait_fifo: */
		0x16, 0x68, /* ldr     r6, [r2, #0] */
		0x00, 0x2E, /* cmp     r6, #0 */
		0x16, 0xD0, /* beq     exit */
		0x55, 0x68, /* ldr     r5, [r2, #4] */
		0xB5, 0x42, /* cmp     r5, r6 */
		0xF9, 0xD0, /* beq     wait_fifo */

		/* wait for BUSYF flag */
		/* wait_busy: */
		0x06, 0x68, /* ldr     r6, [r0, #FLASHCTRL_CONFIG] */
		0x16, 0xF4, 0x80, 0x1F, /* tst     r6, #BUSYF */
		0xFB, 0xD1, /* bne     wait_busy */

		/* Write the destination address to WRADDR */
		0xC0, 0xF8, 0xA0, 0x40, /* str     r4, [r0, #FLASHCTRL_WRADDR] */

		/* Write the data half-word to WRDATA in right-justified format */
		0x2E, 0x88, /* ldrh    r6, [r5] */
		0xC0, 0xF8, 0xB0, 0x60, /* str     r6, [r0, #FLASHCTRL_WRDATA] */

		0x02, 0x35, /* adds    r5, #2 */
		0x02, 0x34, /* adds    r4, #2 */

		/* wrap rp at end of buffer */
		0x9D, 0x42, /* cmp     r5, r3 */
		0x01, 0xD3, /* bcc     no_wrap */
		0x15, 0x46, /* mov     r5, r2 */
		0x08, 0x35, /* adds    r5, #8 */

		/* no_wrap: */
		0x55, 0x60, /* str     r5, [r2, #4] */
		0x49, 0x1E, /* subs    r1, r1, #1 */
		0x00, 0x29, /* cmp     r1, #0 */
		0x00, 0xD0, /* beq     exit */
		0xE5, 0xE7, /* b       wait_fifo */

		/* exit: */
		0x5A, 0x26, /* movs    r6, #MULTIPLE_LOCK */
		0xC0, 0xF8, 0xC0, 0x60, /* str     r6, [r0, #FLASHCTRL_KEY] */
		0x00, 0xBE /* bkpt    #0 */
	};

	/* flash write code */
	if (target_alloc_working_area(target, sizeof(sim3x_flash_write_code),
			&write_algorithm) != ERROR_OK) {
		LOG_WARNING("no working area available, can't do block memory writes");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	ret = target_write_buffer(target, write_algorithm->address,
			sizeof(sim3x_flash_write_code), (uint8_t *) sim3x_flash_write_code);
	if (ret != ERROR_OK)
		return ret;

	/* memory buffer */
	while (target_alloc_working_area_try(target, buffer_size, &source)
			!= ERROR_OK) {
		buffer_size /= 2;
		buffer_size &= ~1UL; /* Make sure it's 2 byte aligned */
		if (buffer_size <= 256) {
			/* we already allocated the writing code, but failed to get a
			 * buffer, free the algorithm
			 */
			target_free_working_area(target, write_algorithm);

			LOG_WARNING("no large enough working area available, can't do block memory writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT); /* flash base */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT); /* count */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT); /* buffer start */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT); /* buffer end */
	init_reg_param(&reg_params[4], "r4", 32, PARAM_IN_OUT); /* target address */

	buf_set_u32(reg_params[0].value, 0, 32, (uint32_t) &FLASHCTRL0_CONFIG->all);
	buf_set_u32(reg_params[1].value, 0, 32, count);
	buf_set_u32(reg_params[2].value, 0, 32, source->address);
	buf_set_u32(reg_params[3].value, 0, 32, source->address + source->size);
	buf_set_u32(reg_params[4].value, 0, 32, address);

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;

	ret = target_run_flash_async_algorithm(target, buf, count, 2, 0, NULL, 5,
			reg_params, source->address, source->size, write_algorithm->address,
			0, &armv7m_info);

	if (ret == ERROR_FLASH_OPERATION_FAILED) {
		LOG_ERROR("flash write failed at address 0x%"PRIx32,
			buf_get_u32(reg_params[4].value, 0, 32));
	}

	target_free_working_area(target, source);
	target_free_working_area(target, write_algorithm);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);

	return ret;
}

static int sim3x_flash_write(struct flash_bank *bank, const uint8_t * buffer, uint32_t offset, uint32_t count)
{
	int ret;
	struct target * target;
	uint8_t *new_buffer = NULL;

	target = bank->target;
	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & 0x1) {
		LOG_ERROR("offset 0x%" PRIx32 " breaks required 2-byte alignment", offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	if (count & 0x1) {
		uint32_t old_count = count;
		count++;
		new_buffer = malloc(count);

		if (new_buffer == NULL) {
			LOG_ERROR("odd number of bytes to write and no memory "
					"for padding buffer");
			return ERROR_FAIL;
		}
		LOG_INFO("odd number of bytes to write (%d), extending to %d "
				"and padding with 0xff", old_count, count);

		memset((uint8_t *)buffer, 0xff, count);
		buffer = memcpy(new_buffer, buffer, old_count);
	}

	uint32_t half_words_remaining = count / 2;

	ret = sim3x_write_block(bank, buffer, offset, half_words_remaining);

	if (new_buffer)
		free(new_buffer);

	return ret;
}

static int sim3x_flash_protect(struct flash_bank *bank, int set, int first, int last)
{
	int ret;
	uint32_t lock_word_number;
	struct sim3x_info * sim3x_info;
	struct target * target;

	target = bank->target;
	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	sim3x_info = bank->driver_priv;
	lock_word_number = 0xFFFFFFFE;

	if (sim3x_info->flash_locked) {
		LOG_INFO("Flash is already locked");
	} else {
		ret = sim3x_flash_write(bank, (uint8_t *)&lock_word_number, LOCK_WORD_ADDRESS, 4);
		if (ERROR_OK != ret)
			return ret;

		LOG_INFO("Flash locked");
	}

	return ERROR_OK;
}

static int sim3x_set_debug_mode(struct flash_bank *bank)
{
	int ret;
	struct target * target;

	target = bank->target;

	/* Disable watchdog timer */
	ret = target_write_u32(target, WDTIMER0_WDTKEY, WDTIMER0_KEY_ATTN);
	if (ERROR_OK != ret)
		return ret;

	ret = target_write_u32(target, WDTIMER0_WDTKEY, WDTIMER0_KEY_DISABLE);
	if (ERROR_OK != ret)
		return ret;

	/* Enable one write command */
	ret = target_write_u32(target, WDTIMER0_WDTKEY, WDTIMER0_KEY_ATTN);
	if (ERROR_OK != ret)
		return ret;

	ret = target_write_u32(target, WDTIMER0_WDTKEY, WDTIMER0_KEY_WRITE);
	if (ERROR_OK != ret)
		return ret;

	/* Watchdog Timer Debug Mode */
	ret = target_write_u32(target, (uint32_t) &WDTIMER0_CONTROL->set,
	WDTIMER0_CONTROL_DBGMD_MASK);
	if (ERROR_OK != ret)
		return ret;

	/* Enable VDD Supply Monitor */
	ret = target_write_u32(target, (uint32_t) &VMON0_CONTROL->set,
	VMON0_CONTROL_VMONEN_MASK);
	if (ERROR_OK != ret)
		return ret;

	/* Set VDD Supply Monitor as a reset source */
	ret = target_write_u32(target, (uint32_t) &RSTSRC0_RESETEN->set,
	RSTSRC0_RESETEN_VMONREN_MASK);
	if (ERROR_OK != ret)
		return ret;

	/* Flash Controller Clock Enable */
	ret = target_write_u32(target, (uint32_t) &CLKCTRL0_APBCLKG0->set,
	CLKCTRL0_APBCLKG0_FLCTRLCEN_MASK);
	if (ERROR_OK != ret)
		return ret;

	return ERROR_OK;
}

static int sim3x_device_probe(struct flash_bank *bank)
{
	int ret, i, num_pages;
	struct sim3x_info * sim3x_info;

	sim3x_info = bank->driver_priv;
	sim3x_info->probed = 0;

	ret = sim3x_set_debug_mode(bank);
	if (ERROR_OK != ret)
		return ret;

	ret = sim3x_read_info(bank);
	if (ERROR_OK != ret)
		return ret;

#ifdef DEVICE_AUTO_DETECTION
	switch(sim3x_info->part_family) {
	case 'c':
	case 'C':
		LOG_INFO("SiM3Cx detected");
		break;

	case 'u':
	case 'U':
		LOG_INFO("SiM3Ux detected");
		break;

	case 'l':
	case 'L':
		LOG_INFO("SiM3Lx detected");
		break;

	default:
		LOG_ERROR("Unsupported MCU family %c", sim3x_info->part_family);
		return ERROR_FAIL;
	}

	LOG_INFO("flash size = %dKB", sim3x_info->flash_size_kb);
	LOG_INFO("flash page size = %dB", sim3x_info->flash_page_size);
#else
	LOG_INFO("SiM3x detected");
#endif

	num_pages = sim3x_info->flash_size_kb * 1024 / sim3x_info->flash_page_size;

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}

	bank->base = FLASH_BASE_ADDRESS;
	bank->size = num_pages * sim3x_info->flash_page_size;
	bank->num_sectors = num_pages;
	bank->sectors = malloc(sizeof(struct flash_sector) * num_pages);

	for (i = 0; i < num_pages; i++) {
		bank->sectors[i].offset = i * sim3x_info->flash_page_size;
		bank->sectors[i].size = sim3x_info->flash_page_size;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = sim3x_info->flash_locked > 0;
	}

	sim3x_info->probed = 1;

	return ERROR_OK;
}

static int sim3x_probe(struct flash_bank *bank)
{
	return sim3x_device_probe(bank);
}

static int sim3x_auto_probe(struct flash_bank *bank)
{
	if (((struct sim3x_info *) bank->driver_priv)->probed)
		return ERROR_OK;

	return sim3x_device_probe(bank);
}

static int sim3x_flash_protect_check(struct flash_bank *bank)
{
	int ret, i;
	struct sim3x_info * sim3x_info;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = sim3x_flash_lock_check(bank);
	if (ERROR_OK != ret)
		return ret;

	sim3x_info = bank->driver_priv;

	for (i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_protected = sim3x_info->flash_locked > 0;

	return ERROR_OK;
}

static int sim3x_flash_info(struct flash_bank *bank, char *buf, int buf_size)
{
	int ret;
	int printed = 0;
#ifdef DEVICE_AUTO_DETECTION
	struct sim3x_info * sim3x_info;
#endif

	ret = sim3x_read_info(bank);
	if (ERROR_OK != ret)
		return ret;

#ifdef DEVICE_AUTO_DETECTION
	sim3x_info = bank->driver_priv;

	/* Part */
	printed = snprintf(buf, buf_size, "SiM3%c%d", sim3x_info->part_family, sim3x_info->part_number);
	buf += printed;
	buf_size -= printed;

	if(buf_size <= 0)
		return ERROR_BUF_TOO_SMALL;

	/* Revision */
	if(sim3x_info->device_revision <= 'Z' - 'A') {
		printed = snprintf(buf, buf_size, "-%c", sim3x_info->device_revision + 'A');
		buf += printed;
		buf_size -= printed;

		if(buf_size <= 0)
			return ERROR_BUF_TOO_SMALL;
	}

	/* Package */
	printed = snprintf(buf, buf_size, "-G%s", sim3x_info->device_package);
	buf += printed;
	buf_size -= printed;

	if(buf_size <= 0)
		return ERROR_BUF_TOO_SMALL;

#else
	printed = snprintf(buf, buf_size, "SiM3x");
	buf += printed;
	buf_size -= printed;

	if(buf_size <= 0)
		return ERROR_BUF_TOO_SMALL;
#endif

	return ERROR_OK;
}

static const struct command_registration sim3x_exec_command_handlers[] = {
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration sim3x_command_handlers[] = {
	{
		.name = "sim3x",
		.mode = COMMAND_ANY,
		.help = "sim3x flash command group",
		.usage = "",
		.chain = sim3x_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver sim3x_flash = {
	.name = "sim3x", .commands = sim3x_command_handlers,
	.flash_bank_command = sim3x_flash_bank_command,
	.erase = sim3x_flash_erase,
	.protect = sim3x_flash_protect,
	.write = sim3x_flash_write,
	.read = default_flash_read,
	.probe = sim3x_probe,
	.auto_probe = sim3x_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = sim3x_flash_protect_check,
	.info = sim3x_flash_info
};
