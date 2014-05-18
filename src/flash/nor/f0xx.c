/***************************************************************************
 *									   *
 * Copyright (c) 2013, Andrey Yurovsky <yurovsky@gmail.com>		   *
 * Copyright (c) 2014, Andrey Smirnov <andrew.smirnov@gmail.com>	   *
 *									   *
 * This file is part of OpenOCD.					   *
 *									   *
 * OpenOCD is free software: you can redistribute it and/or modify it	   *
 * under the terms of the GNU General Public License as published by	   *
 * the Free Software Foundation, either version 2 of the License, or	   *
 * (at your option) any later version.					   *
 *									   *
 * OpenOCD is distributed in the hope that it will be useful, but	   *
 * WITHOUT ANY WARRANTY; without even the implied warranty of		   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU	   *
 * General Public License for more details.				   *
 *									   *
 * You should have received a copy of the GNU General Public License	   *
 * along with OpenOCD. If not, see <http://www.gnu.org/licenses/>.	   *
 *									   *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>

#include <target/arm.h>
#include <target/register.h>
#include <helper/binarybuffer.h>
#include <helper/time_support.h>

#include "f0xx.h"

#include "imp.h"

enum f0xx_flash_banks {
	F0XX_FLASH_BANK0 = 0x00000000,
	F0XX_FLASH_BANK1 = 0x00180000,
	F0XX_FLASH_BANK7 = 0xF0200000,

	F0XX_FLASH_BANK0_ID = 0,
	F0XX_FLASH_BANK1_ID = 1,
	F0XX_FLASH_BANK7_ID = 7,
};



enum f0xx_sys_registers {
	F0XX_SYS_BASE = 0xFFFFFF00,

#define F0XX_SYS_REG(offset) (F0XX_SYS_BASE + (offset))

	F0XX_SYS_DEVID	= F0XX_SYS_REG(0xF0)
};

enum f0xx_flash_registers {
	F0XX_BASE = 0xFFF87000,

#define F0XX_REG(offset) (F0XX_BASE + (offset))

	F0XX_FRDCNTL		= F0XX_REG(0x000),

	F0XX_FSPRD		= F0XX_REG(0x004), /* Undocumented */

	F0XX_FEDACCTRL1		= F0XX_REG(0x008),
	F0XX_FEDACCTRL2		= F0XX_REG(0x00C),
	F0XX_FCOR_ERR_CNT	= F0XX_REG(0x010),
	F0XX_FCOR_ERR_ADD	= F0XX_REG(0x014),
	F0XX_FCOR_ERR_POS	= F0XX_REG(0x018),
	F0XX_FEDACSTATUS	= F0XX_REG(0x01C),
	F0XX_FUNC_ERR_ADD	= F0XX_REG(0x020),
	F0XX_FEDACSDIS		= F0XX_REG(0x024),
	F0XX_FPRIM_ADD_TAG	= F0XX_REG(0x028),
	F0XX_FREDU_ADD_TAG	= F0XX_REG(0x02C),
	F0XX_FBPROT		= F0XX_REG(0x030),
	F0XX_FBSE		= F0XX_REG(0x034),
	F0XX_FBBUSY		= F0XX_REG(0x038),
	F0XX_FBAC		= F0XX_REG(0x03C),
	F0XX_FBFALLBACK		= F0XX_REG(0x040),
	F0XX_FBPRDY		= F0XX_REG(0x044),
	F0XX_FPAC1		= F0XX_REG(0x048),
	F0XX_FPAC2		= F0XX_REG(0x04C),
	F0XX_FMAC		= F0XX_REG(0x050),
	F0XX_FMSTAT		= F0XX_REG(0x054),
	F0XX_FEMUDMSW		= F0XX_REG(0x058),
	F0XX_FEMUDLSW		= F0XX_REG(0x05C),
	F0XX_FEMUECC		= F0XX_REG(0x060),

	F0XX_FLOCK		= F0XX_REG(0x064), /* Undocumented */

	F0XX_FEMUADDR		= F0XX_REG(0x068),
	F0XX_FDIAGCTRL		= F0XX_REG(0x06C),
	F0XX_FRAWDATAH		= F0XX_REG(0x070),
	F0XX_FRAWDATAL		= F0XX_REG(0x074),
	F0XX_FRAWECC		= F0XX_REG(0x078),
	F0XX_FPAROVR		= F0XX_REG(0x07C),

	F0XX_FVREADCT		= F0XX_REG(0x0080), /* Undocumented */
	F0XX_FVHVCT1		= F0XX_REG(0x0084), /* Undocumented */
	F0XX_FVHVCT2		= F0XX_REG(0x0088), /* Undocumented */
	F0XX_FVNVCT		= F0XX_REG(0x008C), /* Undocumented */
	F0XX_FVPPCT		= F0XX_REG(0x0090), /* Undocumented */
	F0XX_FVWLCT		= F0XX_REG(0x0094), /* Undocumented */
	F0XX_FEFUSE		= F0XX_REG(0x0098), /* Undocumented */

	F0XX_FEDACSDIS2		= F0XX_REG(0x00C0),

	F0XX_FBSTROBES		= F0XX_REG(0x0100), /* Undocumented */
	F0XX_FPSTROBES		= F0XX_REG(0x0104), /* Undocumented */
	F0XX_FBMODE		= F0XX_REG(0x0108), /* Undocumented */
	F0XX_FTCR		= F0XX_REG(0x010C), /* Undocumented */

	F0XX_FADDR		= F0XX_REG(0x110),

	F0XX_FWRITE		= F0XX_REG(0x114), /* Undocumented */
	F0XX_FCBITSEL		= F0XX_REG(0x118), /* Undocumented */
	F0XX_FTCTRL		= F0XX_REG(0x11C), /* Undocumented */

	F0XX_FSM_COMMAND	= F0XX_REG(0x20C),
	F0XX_FSM_WR_ENA		= F0XX_REG(0x288),

#define F0XX_FSM_WR_ENA_KEY_UNLOCK	(0b101)
#define F0XX_FSM_WR_ENA_KEY_LOCK	(0b010)

	F0XX_FSM_SECTOR		= F0XX_REG(0x2A4),
	F0XX_FSM_EXECUTE	= F0XX_REG(0x2B4),


	F0XX_EEPROM_CONFIG	= F0XX_REG(0x2B8),
	F0XX_FCFG_BANK		= F0XX_REG(0x400),
};
#if 0
static const uint32_t F0XX_FWPWRITE[] = {
	[0] = F0XX_REG(0x120),
	[1] = F0XX_REG(0x124),
	[2] = F0XX_REG(0x128),
	[3] = F0XX_REG(0x12C),
	[4] = F0XX_REG(0x130),
	[5] = F0XX_REG(0x134),
	[6] = F0XX_REG(0x138),
	[7] = F0XX_REG(0x13C),
};
#endif

enum f0xx_fstat_bits {
	F0XX_FSTAT_SLOCK	= 1 << 0,
	F0XX_FSTAT_PSUSP	= 1 << 1,
	F0XX_FSTAT_ESUSP	= 1 << 2,
	F0XX_FSTAT_VOLTSTAT	= 1 << 3,
	F0XX_FSTAT_CSTAT	= 1 << 4,
	F0XX_FSTAT_INVDAT	= 1 << 5,
	F0XX_FSTAT_PGM		= 1 << 6,
	F0XX_FSTAT_ERS		= 1 << 7,
	F0XX_FSTAT_BUSY		= 1 << 8,
	F0XX_FSTAT_EV		= 1 << 10,
	F0XX_FSTAT_PGV		= 1 << 12,
	F0XX_FSTAT_ILA		= 1 << 14,
};


enum {
	F0XX_MAX_BANK_COUNT = 8,
	F0XX_TMS570_PLATORM = 0x05,
	F0XX_OTP_MEMORY_SIZE = 0xF008015C,
};

static const uint32_t f0xx_otp_bank_sector_info_address[F0XX_MAX_BANK_COUNT] = {
	[0] = 0xF0080158,
	[1] = 0xF0082158,
	[2] = 0xF0084158,
	[3] = 0xF0086158,
	[4] = 0xF0088158,
	[5] = 0xF008A158,
	[6] = 0xF008C158,
	[7] = 0xF008E158,
};

enum f0xx_techs {
	F0XX_TECH_C05  = 0x00,
	F0XX_TECH_F05  = 0x01,
	F0XX_TECH_C035 = 0x02,
	F0XX_TECH_F035 = 0x03,
	F0XX_TECH_C021 = 0x04,
	F0XX_TECH_F0XX = 0x05,
};

enum f0xx_flash_ecc_types {
	F0XX_FLASH_ECC_NO_PORTECTION	= 0x00,
	F0XX_FLASH_ECC_SINGLE_BIT	= 0x01,
	F0XX_FLASH_ECC_ECC		= 0x02,
};

/* F0XX Flash FSM "API" commands, see B.2.9 in SPNU501E */
enum f0xx_fsm_commands {
	F0XX_API_PROGRAM_DATA		= 0x02,
	F0XX_API_ERASE_SECTOR		= 0x06,
	F0XX_API_ERASE_BANK		= 0x08,
	F0XX_API_VALIDATE_SECTOR	= 0x0E,
	F0XX_API_CLEAR_STATUS		= 0x10,
	F0XX_API_PROGRAM_RESUME		= 0x14,
	F0XX_API_ERASE_RESUME		= 0x16,
	F0XX_API_CLEAR_MORE		= 0x18,
};

/* Sector capacities.  See Table 2.4 in section 2.2.3.1 of SPNU499B */
static unsigned int sector_capacity_15[] = {
	32, 32, 32, 32, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128
};
static unsigned int sector_capacity_12[] = {
	128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128
};
static unsigned int sector_capacity_4[] = {
	16, 16, 16, 16
};

struct f0xx_flash_bank {
	bool probed;
	unsigned int id; /* bank number on the chip */

	int clk_scale;
	int frequency_mhz;


	struct {
		struct {
			uint32_t address;
			uint32_t value;
		} odd, even, *current;
	} marker;

	struct {
		struct {
			uint32_t address;
		} odd, even, *current;
	} buffer;
};

#include <contrib/loaders/flash/f0xx/tms570-helper.h>

static unsigned int f0xx_bank_address_to_id(uint32_t addr)
{
	switch (addr) {
		case F0XX_FLASH_BANK0:
			return 0;
		case F0XX_FLASH_BANK1:
			return 1;
		case F0XX_FLASH_BANK7:
			return 7;
	}

	return UINT_MAX;
}

static int f0xx_probe(struct flash_bank *bank);


static int f0xx_get_bank_info_if_halted(struct flash_bank *bank,
					struct f0xx_flash_bank **f0xx_bank)
{
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		*f0xx_bank = NULL;
		return ERROR_TARGET_NOT_HALTED;
	}

	*f0xx_bank = bank->driver_priv;
	return ERROR_OK;
}

static int f0xx_get_probed_bank_info_if_halted(struct flash_bank *bank,
					       struct f0xx_flash_bank **f0xx_bank)
{
	const int ret = f0xx_get_bank_info_if_halted(bank, f0xx_bank);

	if (ret != ERROR_OK)
		return ret;

	if (!(*f0xx_bank)->probed)
		return f0xx_probe(bank);
	else
		return ERROR_OK;
}

struct f0xx_helper_code_blob {
	struct working_area *text;
	size_t outbuffer_size;
	uint32_t outbuffer_offset;
};

static int f0xx_load_helper_code(struct flash_bank *bank, struct f0xx_helper_code_blob *helper)
{
	int ret, err;
	uint8_t zeros[MAX(tms570_helper_bss_size,
			  tms570_helper_outbuffer_size)];
	memset(helper, 0, sizeof(*helper));

	/*
	  Allocate and transfer .text section
	*/
	ret = target_code_u8_to_working_area(bank->target, tms570_helper_text,
					     tms570_helper_text_size,
					     tms570_helper_data_size +
					     tms570_helper_bss_size,
					     &helper->text);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to load helper code to the target");
		return ret;
	}

	if (helper->text->address != tms570_helper_text_offset) {
		ret = ERROR_FAIL;
		LOG_ERROR("Working area for code starts at incorrect offset, 0x%08"PRIx32,
			  helper->text->address);
		goto free_text;
	}

	/*
	  Populate .data
	 */
	ret = target_write_memory(bank->target,
				  tms570_helper_data_offset,
				  1, tms570_helper_data_size,
				  tms570_helper_data);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to copy data to .data section");
		ret = ERROR_FAIL;
		goto free_text;
	}

	/*
	  Zero-out .bss
	 */
	memset(zeros, 0, sizeof(zeros));

	ret = target_write_memory(bank->target,
				  tms570_helper_bss_offset,
				  1, tms570_helper_bss_size,
				  zeros);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to zero-out .bss section");
		ret = ERROR_FAIL;
		goto free_text;
	}

	helper->outbuffer_size = tms570_helper_outbuffer_size;
	helper->outbuffer_offset = tms570_helper_outbuffer_offset;

	return ERROR_OK;

free_text:
	err = target_free_working_area(bank->target, helper->text);
	return ret ?: err;

}

static int f0xx_free_helper_code(struct flash_bank *bank, struct f0xx_helper_code_blob *helper)
{
	if (helper->outbuffer_size) {
		int ret;
		char outbuffer[helper->outbuffer_size];

		ret = target_read_memory(bank->target,
					 helper->outbuffer_offset, 1,
					 sizeof(outbuffer),
					 (uint8_t *)outbuffer);

		if (ret == ERROR_OK) {
			if (outbuffer[sizeof(outbuffer) - 1] != 0) {
				LOG_ERROR("Output log was overflown!");
				outbuffer[sizeof(outbuffer) - 1] = 0;
			}

			if (strlen(outbuffer))
				LOG_INFO("Helper output log:\n%s", outbuffer);
		}
	}

	return target_free_working_area(bank->target, helper->text);
}

static int f0xx_measure_hclk(struct flash_bank *bank, unsigned int *hclk)
{
	int ret, err;
	assert(hclk != NULL);

	struct arm_algorithm algorithm;
	struct duration call_duration;

	struct f0xx_helper_code_blob helper;

	ret = f0xx_load_helper_code(bank, &helper);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to load helper code to the target");
		return ret;
	}

	algorithm.common_magic = ARM_COMMON_MAGIC;
	algorithm.core_mode    = ARM_MODE_SVC;
	algorithm.core_state   = ARM_STATE_THUMB;

	uint32_t clocks_per_iteration;
	const unsigned int sample_size = 10;
	const uint32_t N2 = 100000, N1 = 1;

	float A1 = 0;

	for (unsigned int i = 0; i < sample_size; i++) {
		ret = duration_start(&call_duration);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to start first timer");
			goto free_code_working_area;
		}

		ret = arm_aapcs_call(bank->target,
				     tms570_helper_f0xx_do_perf_counting,
				     100,
				     &algorithm,
				     500,
				     &clocks_per_iteration, NULL,
				     1, N1);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to measure perf loop latency");
			goto free_code_working_area;
		}

		ret = duration_measure(&call_duration);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to stop first timer");
			goto free_code_working_area;
		}

		A1 += duration_elapsed(&call_duration);
	}
	A1 /= sample_size;

	float A2 = 0;

	for (unsigned int i = 0; i < sample_size; i++) {
		ret = duration_start(&call_duration);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to start second timer");
			goto free_code_working_area;
		}

		ret = arm_aapcs_call(bank->target,
				     tms570_helper_f0xx_do_perf_counting,
				     100,
				     &algorithm,
				     1000,
				     NULL, NULL,
				     1, N2);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to measure perf loop latency");
			goto free_code_working_area;
		}
		ret = duration_measure(&call_duration);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to stop second timer");
			goto free_code_working_area;
		}

		A2 += duration_elapsed(&call_duration);
	}
	A2 /= sample_size;


	const float iteration_time = (A1 - A2) / ((float)N1 - (float)N2);
	const float freq = clocks_per_iteration / iteration_time;

	*hclk = (unsigned int)(lrint(freq / 1000000.0) * 1000000);
	ret = ERROR_OK;

free_code_working_area:
	err = f0xx_free_helper_code(bank, &helper);
	if (err != ERROR_OK)
		LOG_ERROR("Failed to free code working area");

	return ret ?: err;
}

static int f0xx_call_helper(struct flash_bank *bank, uint32_t address, int argc, ...)
{
	int ret, err;
	va_list args;

	uint32_t status;
	struct f0xx_flash_bank *fb;

	ret = f0xx_get_bank_info_if_halted(bank, &fb);
	if (ret != ERROR_OK) {
	    LOG_ERROR("Can't get f0xx flash bank");
	    return ret;
	}

	struct arm_algorithm algorithm;
	struct f0xx_helper_code_blob helper;

	ret = f0xx_load_helper_code(bank, &helper);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to load helper code to the target");
		return ret;
	}

	algorithm.common_magic = ARM_COMMON_MAGIC;
	algorithm.core_mode    = ARM_MODE_SVC;
	algorithm.core_state   = ARM_STATE_THUMB;

	va_start(args, argc);

	ret = arm_aapcs_call_v(bank->target,
			       address,
			       500, /* stack size */
			       &algorithm,
			       1500, /* 1.5 s timeout */
			       &status, NULL,
			       argc, args);
	va_end(args);

	if (ret == ERROR_OK) {
		if (status != F0XX_HELPER_STATUS_SUCCESS) {
			LOG_ERROR("Helper function failed with error code %d", status);
			ret = ERROR_FAIL;
		}
	} else {
		LOG_ERROR("AAPCS call failed %d", ret);
	}

	err = f0xx_free_helper_code(bank, &helper);
	if (err != ERROR_OK)
		LOG_ERROR("Failed to free code working area");

	return ret ?: err;
}

static int f0xx_load_bank_layout(struct flash_bank *bank)
{
	int ret;
	uint32_t sector_info;
	struct f0xx_flash_bank *fb = bank->driver_priv;

	/* OTP contains information about each Flash bank as well as information
	 * about the whole Flash (repeated per bank). We read the entry
	 * corresponding to the bank we're looking at but sanity-check the contents
	 * for the bank info and the whole Flash size.	See section 5.4.2.1 in
	 * SPNU499B for more information. */
	ret = target_read_u32(bank->target,
			      f0xx_otp_bank_sector_info_address[fb->id],
			      &sector_info);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read flash layout information");
		return ret;
	}

	uint8_t num_banks; /* number of banks on the chip */
	uint8_t bank_mask; /* which banks are actually on the chip */

	num_banks = sector_info & 0xFF;
	bank_mask = (sector_info >> 8) & 0xFF;
	bank->num_sectors = sector_info >> 16;

	LOG_INFO("%d banks, %d sectors, mask: 0x%02" PRIx8,
		 num_banks, bank->num_sectors, bank_mask);

	/* Make sure that the bank we're probing is within the number of banks
	 * on the chip and is one of the banks that this chip reports as
	 * having. */
	if (!(bank_mask & (1 << fb->id))) {
		LOG_WARNING("This chip doesn't have bank %d", fb->id);
		return ERROR_FAIL;
	}


	unsigned int *capacity = NULL;

	/* We have lookup tables for sector capacity based on how many
	 * sectors are in the bank, choose the corresponding one. */
	switch (bank->num_sectors) {
	case 15:
		capacity = sector_capacity_15;
		break;
	case 12:
		capacity = sector_capacity_12;
		break;
	case 4:
		capacity = sector_capacity_4;
		break;
	default:
		LOG_ERROR("Unexpected number of sectors, %d\n",
			  bank->num_sectors);
		ret = ERROR_FAIL;
		break;
	}

	/* This looks like a bank that we understand, now we know the
	 * corresponding sector capacities and we can add those up into the
	 * bank size. */
	if (ret != ERROR_OK)
		return ret;

	uint32_t offset = 0x00000000;
	bank->sectors = calloc(bank->num_sectors,
			       sizeof(struct flash_sector));
	for (int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].size		= capacity[i] * 1024;
		bank->sectors[i].offset		= offset;
		bank->sectors[i].is_erased	= -1;
		bank->sectors[i].is_protected	= -1;

		offset	   += bank->sectors[i].size;
	}

	return ERROR_OK;
}

#if 0
static int f0xx_parse_device_id(uint32_t devid)
{
	int ret;
	const uint8_t  platform_id		= (devid >> 0) & 0b111;
	const uint8_t  revision			= (devid >> 3) & 0b11111;
	const uint8_t  ram_ecc			= (devid >> 8) & 0b1;
	const uint8_t  flash_ecc		= (devid >> 9) & 0b11;
	const uint8_t  peripheral_parity	= (devid >> 11) & 0b1;
	const uint8_t  io_voltage		= (devid >> 12) & 0b1;
	const uint8_t  tech			= (devid >> 13) & 0b1111;
	const uint16_t unique_id		= (devid >> 17) & 0b11111111111111;
	const uint8_t  cp15			= (devid >> 31) & 0b1;

	if (platform_id != F0XX_TMS570_PLATORM) {
		LOG_ERROR("Device with unknown platform ID detected");
		return ERROR_FAIL;
	}

	LOG_INFO("TM570 device detected");
	LOG_INFO("CP15: %s", (cp15) ? "present" : "absent");
	LOG_INFO("Unique ID: 0x%04x", unique_id);

	const char *technology;

	switch (tech) {
	case F0XX_TECH_C05:
		technology = "C05";
		ret = ERROR_FAIL;
		break;
	case F0XX_TECH_F05:
		technology = "F05";
		ret = ERROR_FAIL;
		break;
	case F0XX_TECH_C035:
		technology = "C035";
		ret = ERROR_FAIL;
		break;
	case F0XX_TECH_F035:
		technology = "F035";
		ret = ERROR_FAIL;
		break;
	default:
		technology = "Unknown";
		ret = ERROR_FAIL;
		break;

	case F0XX_TECH_C021:
		technology = "C021";
		ret = ERROR_OK;
		break;
	case F0XX_TECH_F0XX:
		technology = "F0XX";
		ret = ERROR_OK;
		break;
	}

	if (ret != ERROR_OK) {
		LOG_ERROR("This device uses %s process technology, which is not supported", technology);
		return ret;
	}

	LOG_INFO("Process technology: %s", technology);

	LOG_INFO("I/O voltage: %sV", (io_voltage) ? "3.3" : "5");
	LOG_INFO("Peripheral parity: %s", (peripheral_parity) ? "yes" : "no");

	const char *flash_ecc_type;

	switch (flash_ecc) {
	case F0XX_FLASH_ECC_NO_PORTECTION:
		flash_ecc_type = "no protection";
		break;
	case F0XX_FLASH_ECC_SINGLE_BIT:
		flash_ecc_type = "single bit parity";
		break;
	case F0XX_FLASH_ECC_ECC:
		flash_ecc_type = "ECC";
		break;
	default:
		flash_ecc_type = "reserved";
		break;
	}

	LOG_INFO("Flash ECC: %s", flash_ecc_type);
	LOG_INFO("RAM ECC: %s", (ram_ecc) ? "yes" : "no");
	LOG_INFO("Device revision: 0x%02x", revision);

	return ERROR_OK;
}
#endif

/* Some Flash registers are banked, that is the register is per-bank but there
 * is only one location for it and the actual register written is dependant on
 * what bank ID is set.	 See 5.7.44 in SPNU499B. */
static int f0xx_select_bank(struct flash_bank *bank)
{
	struct f0xx_flash_bank *fb;
	uint32_t fmac;
	int ret;

	ret = f0xx_get_bank_info_if_halted(bank, &fb);
	if (ret != ERROR_OK) {
	    LOG_ERROR("Can't get f0xx flash bank");
	    return ret;
	}

	ret = target_write_u32(bank->target, F0XX_FMAC, fb->id);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to write to FMAC register");
		return ret;
	}

	ret = target_read_u32(bank->target, F0XX_FMAC, &fmac);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read FMAC register");
		return ret;
	}

	if (fmac != fb->id) {
		LOG_ERROR("Read out FMAC value does not match what we set it to");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int f0xx_protect_check(struct flash_bank *bank)
{
	int ret;

	if (bank->target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = f0xx_select_bank(bank);
	if (ret != ERROR_OK) {
		LOG_ERROR("Can't select bank");
		return ret;
	}

	uint32_t fbse;

	ret = target_read_u32(bank->target, F0XX_FBSE, &fbse);
	if (ret != ERROR_OK) {
		LOG_ERROR("Cannot read device identification register.");
		return ret;
	}

	/* Success: update our information */
	for (int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].is_protected = !(fbse & 0b1);
		fbse >>= 1;
	}

	return ERROR_OK;
}

static int f0xx_write_fbse(struct flash_bank *bank, uint32_t value)
{
	int ret;
	ret = target_write_u32(bank->target, F0XX_FBPROT, 1);
	if (ret != ERROR_OK) {
		LOG_ERROR("Can't disable level 1 protection");
		return ret;
	}

	ret = target_write_u32(bank->target, F0XX_FBSE, value);
	if (ret != ERROR_OK) {
		LOG_ERROR("Can't set protection mask");
		return ret;
	}

	ret = target_write_u32(bank->target, F0XX_FBPROT, 0);
	if (ret != ERROR_OK)
		LOG_ERROR("Can't enable level 1 protection");

	return ret;
}

static int f0xx_probe(struct flash_bank *bank)
{
	int ret;
	uint32_t devid;
	struct f0xx_flash_bank *fb = bank->driver_priv;

	if (bank->target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Map the passed-in bank base address to a F0XX bank ID (bank number),
	 * we can then look up the sector information for the corresponding bank
	 * as sector information is stored per bank and this doesn't directly
	 * correlate to the base address. */
	fb->id = f0xx_bank_address_to_id(bank->base);
	if (fb->id == UINT_MAX) {
		LOG_ERROR("Base address 0x%08" PRIx32 " is not a known bank",
				bank->base);
		return ERROR_FAIL;
	}

	/* The SYS registers contain a device identifier which at least tells us
	 * that we're talking to a F0XX.  This register seems to be facotry
	 * programmed in the opposite endianness of the CPU so we byte swap to fix
	 * that. */
	ret = target_read_u32(bank->target, F0XX_SYS_DEVID, &devid);
	if (ret != ERROR_OK) {
		LOG_ERROR("Cannot read device identification register.");
		return ret;
	}
#if 0
	ret = f0xx_parse_device_id(devid);
	if (ret != ERROR_OK)
		return ret;
#endif
#if 0
	/* Now sanity-check the Flash controller itself. */
	ret = target_read_u32(bank->target, F0XX_FCFG_BANK,
			      &config);
	if (ret != ERROR_OK) {
		LOG_ERROR("Cannot read Flash bank configuration.");
		return ret;
	}

	/* The Flash bank configuration is hard-coded and there's only one possible
	 * value from the TRM.	The meaning is, for bits:
	 * 31-20: bank 7 width (0x90, or 144 bits)
	 * 19-16: reserved (written as 1)
	 * 15-4:  main bank widths (0x90, or 144 bits)
	 * 3-0:	  reserved (written as 2)
	 * ...a different value here would indicate different bank widths from what
	 * we expect so we consider that an error at this time. */
	if (config != 0x02090109) {
		LOG_ERROR("F0XX: unexpected Flash bank configuration.");
		return ERROR_FAIL;
	}
#endif

	uint32_t memsize;
	ret = target_read_u32(bank->target, F0XX_OTP_MEMORY_SIZE, &memsize);
	if (ret != ERROR_OK) {
		LOG_ERROR("Cannot read Flash size register.");
		return ret;
	}

	const uint32_t flash_size = memsize & 0xFFFF;
	const uint32_t pin_count  = (memsize >> 16) & 0x0FFF;

	LOG_INFO("Package: %d pins", pin_count);
	LOG_INFO("Flash size: %d Kbytes", flash_size);

	bank->size = flash_size * 1024;

	/* Retrieve information about the particular bank we're probing and fill in
	 * the bank structure accordingly. */
	ret = f0xx_load_bank_layout(bank);
	if (ret != ERROR_OK) {
		LOG_ERROR("Unable to load bank information.");
		return ERROR_FAIL;
	}

	unsigned int hclk = 0;

	ret = f0xx_measure_hclk(bank, &hclk);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to measure HCLK, cannot proceed any further");
		return ret;
	}

	/* FIXME: This only needs to be called once, instead every
	 * time for each bank */
	fb->frequency_mhz = hclk / 1000000;

	ret = f0xx_protect_check(bank);
	if (ret != ERROR_OK) {
		LOG_ERROR("Unable to get flash protection status.");
		return ERROR_FAIL;
	}


	fb->probed = true;
	return ret;
}

static int f0xx_auto_probe(struct flash_bank *bank)
{
	struct f0xx_flash_bank *fb = bank->driver_priv;

	if (fb->probed)
		return ERROR_OK;
	else
		return f0xx_probe(bank);
}

static int f0xx_protect(struct flash_bank *bank, int set, int first, int last)
{
	int ret;
	struct f0xx_flash_bank *fb;

	ret = f0xx_get_probed_bank_info_if_halted(bank, &fb);
	if (ret != ERROR_OK) {
		LOG_ERROR("Can't get f0xx flash bank");
		return ret;
	}

	ret = f0xx_select_bank(bank);
	if (ret != ERROR_OK) {
		LOG_ERROR("Can't select bank");
		return ret;
	}

	uint32_t fbse;

	ret = target_read_u32(bank->target, F0XX_FBSE, &fbse);
	if (ret != ERROR_OK) {
		LOG_ERROR("Cannot read device identification register.");
		return ret;
	}

	if (set) {
		uint32_t mask = 0xFFFF;
		uint32_t bit_switch = 1 << first;

		for (int i = first; i <= last; i++) {
			mask &= ~bit_switch;
			bit_switch <<= 1;
		}

		fbse &= mask;
	} else {
		uint32_t mask = 0x0000;
		uint32_t bit_switch = 1 << first;

		for (int i = first; i <= last; i++) {
			mask |= bit_switch;
			bit_switch <<= 1;
		}

		fbse |= mask;
	}

	ret = f0xx_write_fbse(bank, fbse);
	if (ret != ERROR_OK) {
		LOG_ERROR("Can't enable write/erase in FBSE");
		return ret;
	}

	return f0xx_protect_check(bank);
}
#if 0
static int f0xx_check_fsm_for_errors(struct flash_bank *bank)
{
	uint32_t fmstat;
	int ret;

	ret = target_read_u32(bank->target, F0XX_FMSTAT, &fmstat);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read FMSTAT register");
		return ret;
	}

	LOG_ERROR("FMSTAT: 0x%08x", fmstat);

	if (fmstat & F0XX_FSTAT_SLOCK) {
		LOG_ERROR("Target sector is locked in FBAC or FBSE");
		ret = ERROR_FAIL;
	}

	if (fmstat & F0XX_FSTAT_PSUSP) {
		LOG_ERROR("Suspended program operation");
		ret = ERROR_FAIL;
	}

	if (fmstat & F0XX_FSTAT_ESUSP) {
		LOG_ERROR("Suspended erase operation");
		ret = ERROR_FAIL;
	}

	if (fmstat & F0XX_FSTAT_VOLTSTAT) {
		LOG_ERROR("Voltage dropout during programming");
		ret = ERROR_FAIL;
	}

	if (fmstat & F0XX_FSTAT_CSTAT) {
		LOG_ERROR("Program, erase or validate sector command failed");
		ret = ERROR_FAIL;
	}

	if (fmstat & F0XX_FSTAT_INVDAT) {
		LOG_ERROR("Attempted to program a 1 where a 0 was already present");
		ret = ERROR_FAIL;
	}
#if 0
	if (fmstat & F0XX_FSTAT_PGM) {
		LOG_ERROR("Attempted to program a “1” where a “0” was already present");
		ret = ERROR_FAIL;
	}

	F0XX_FSTAT_ERS		= 1 << 7,
	F0XX_FSTAT_BUSY		= 1 << 8,
#endif

	if (fmstat & F0XX_FSTAT_EV) {
		LOG_ERROR("Sector is not successfully erased");
		ret = ERROR_FAIL;
	}

	if (fmstat & F0XX_FSTAT_PGV) {
		LOG_ERROR("Word is not successfully programmed");
		ret = ERROR_FAIL;
	}

	if (fmstat & F0XX_FSTAT_ILA) {
		LOG_ERROR("Illegal address detected");
		ret = ERROR_FAIL;
	}

	return ret;
}
#endif
static int f0xx_erase(struct flash_bank *bank, int first, int last)
{
	int ret;
	struct f0xx_flash_bank *fb;

	ret = f0xx_get_probed_bank_info_if_halted(bank, &fb);
	if (ret != ERROR_OK) {
		LOG_ERROR("Can't get f0xx flash bank");
		return ret;
	}

	return f0xx_call_helper(bank,
				tms570_helper_f0xx_erase_sectors,
				6,
				fb->frequency_mhz, fb->id,
				first, last,
				bank->base,
				bank->sectors[first].size);
}

static int f0xx_fill_current_buffer(struct flash_bank *bank,
				    const uint8_t **buffer, uint32_t *residue)
{
	struct f0xx_flash_bank *f0xx;
	int ret;
	const uint32_t size = MIN(*residue, F0XX_DATA_BUFFER_SIZE);

	f0xx = (struct f0xx_flash_bank *)bank->driver_priv;


	LOG_INFO("Filling %s buffer with %d bytes",
		 (f0xx->buffer.current == &f0xx->buffer.odd) ? "odd" : "even", size);

	ret = target_write_memory(bank->target,
				  f0xx->buffer.current->address,
				  1, size, *buffer);
	if (ret != ERROR_OK)
		return ret;

	*residue -= size;
	*buffer	 += size;

	ret = target_write_u32(bank->target,
			       f0xx->marker.current->address,
			       f0xx->marker.current->value);
	if (ret != ERROR_OK)
		return ret;

	return ERROR_OK;
}

static int f0xx_wait_for_current_buffer_to_be_read_out(struct flash_bank *bank,
						       int timeout)
{
	struct f0xx_flash_bank *f0xx;
	int ret;
	uint32_t marker;

	f0xx = (struct f0xx_flash_bank *)bank->driver_priv;

	while (--timeout > 0) {
		if (bank->target->state != TARGET_DEBUG_RUNNING) {
			LOG_ERROR("Target is not running");
			return -ERROR_FAIL;
		}

		ret = target_read_u32(bank->target,
				      f0xx->marker.current->address,
				      &marker);
		if (ret != ERROR_OK)
			return ret;

		if (marker == f0xx->marker.current->value)
			continue;
		else if (marker == F0XX_DATA_NO_MARKER)
			return ERROR_OK;
		else
			break;
	}

	return ERROR_FAIL;
}

static void f0xx_toggle_current_buffer(struct flash_bank *bank)
{
	struct f0xx_flash_bank *f0xx;

	f0xx = (struct f0xx_flash_bank *)bank->driver_priv;

	if (f0xx->marker.current == &f0xx->marker.odd) {
		f0xx->marker.current = &f0xx->marker.even;
		f0xx->buffer.current = &f0xx->buffer.even;
	} else {
		f0xx->marker.current = &f0xx->marker.odd;
		f0xx->buffer.current = &f0xx->buffer.odd;
	}
}

#define F0XX_PING_PONG_ADDR(base, field, subfield)  ((base)->address +	\
						     offsetof(struct f0xx_data_exchange_buffer, \
							      field.subfield))

static int f0xx_write(struct flash_bank *bank, const uint8_t *buffer,
		      uint32_t offset, uint32_t count)
{
	int ret, err1, err2;

	struct working_area *data_exchange_buffer;
	struct f0xx_helper_code_blob helper;

	if (!count) {
		LOG_WARNING("Zero length write requested");
		return ERROR_OK;
	}

	struct f0xx_flash_bank *f0xx;

	ret = f0xx_get_probed_bank_info_if_halted(bank, &f0xx);
	if (ret != ERROR_OK) {
		LOG_ERROR("Can't get f0xx flash bank");
		return ret;
	}

	uint32_t residue = count;

	/*
	  This allocation MUST happen at this point since we depend on working
	  area for the code starting at 0x08000000
	 */
	ret = f0xx_load_helper_code(bank, &helper);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to load helper code to the target");
		return ret;
	}

	ret = target_alloc_working_area(bank->target,
					sizeof(struct f0xx_data_exchange_buffer),
					&data_exchange_buffer);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to allocate data exchange buffer");
		goto free_helper_code;
	}

	f0xx->marker.odd.address  = F0XX_PING_PONG_ADDR(data_exchange_buffer,
						      marker, odd);
	f0xx->marker.odd.value	  = F0XX_DATA_MARKER_1;
	f0xx->marker.even.address = F0XX_PING_PONG_ADDR(data_exchange_buffer,
						      marker, even);
	f0xx->marker.even.value	  = F0XX_DATA_MARKER_2;
	f0xx->buffer.odd.address  = F0XX_PING_PONG_ADDR(data_exchange_buffer,
						      buffer, odd);
	f0xx->buffer.even.address = F0XX_PING_PONG_ADDR(data_exchange_buffer,
						      buffer, even);

	/*
	  Set odd buffer to be current
	 */
	f0xx->marker.current = &f0xx->marker.odd;
	f0xx->buffer.current = &f0xx->buffer.odd;

	/*
	  Fill it with first chunk of data
	 */
	ret = f0xx_fill_current_buffer(bank,
				       &buffer, &residue);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to transfer write data");
		goto free_exchange_buffer;
	}

	/*
	  Start helper write algorithm
	 */
	struct arm_algorithm algorithm;
	arm_aapcs_call_artifacts_t artifacts;

	algorithm.common_magic = ARM_COMMON_MAGIC;
	algorithm.core_mode    = ARM_MODE_SVC;
	algorithm.core_state   = ARM_STATE_THUMB;

	ret = arm_aapcs_call_async(bank->target,
				   tms570_helper_f0xx_write_block,
				   512,
				   &algorithm,
				   &artifacts,
				   5,
				   f0xx->frequency_mhz, f0xx->id,
				   offset, count,
				   data_exchange_buffer->address);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to start write subroutine");
		goto free_exchange_buffer;
	}

	/*
	  Start asynchronous ping-pong style transfer loop
	 */
	while (residue) {
		f0xx_toggle_current_buffer(bank);

		ret = f0xx_wait_for_current_buffer_to_be_read_out(bank,
								  10000);

		if (ret != ERROR_OK) {
			LOG_ERROR("Failed while waiting for buffer to be read out");
			break;
		}

		ret = f0xx_fill_current_buffer(bank,
					       &buffer, &residue);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to transfer write data");
			break;
		}
	}

	uint32_t result;

	if (ret == ERROR_OK)
		ret = arm_aapcs_call_wait(bank->target,
					  artifacts,
					  10000,
					  &result, NULL);

	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to wait for write subroutine to finish");
		goto free_exchange_buffer;
	}

	if (result != F0XX_ERROR_OK) {
		LOG_ERROR("Write helper code returned %d", (int32_t)result);
		ret = ERROR_FAIL;
	}


free_exchange_buffer:
	err2 = target_free_working_area(bank->target,
				       data_exchange_buffer);

free_helper_code:
	err1 = f0xx_free_helper_code(bank, &helper);

	return ret ?: err1 ?: err2;
}


FLASH_BANK_COMMAND_HANDLER(f0xx_flash_bank_command)
{
	bank->driver_priv = calloc(1, sizeof(struct f0xx_flash_bank));

	if (!bank->driver_priv)
		return ERROR_FLASH_OPERATION_FAILED;

	return ERROR_OK;
}

struct flash_driver f0xx_flash = {
	.name			= "f0xx",
	/* .commands		= f0xx_command_handlers, */
	.flash_bank_command	= f0xx_flash_bank_command,
	.erase			= f0xx_erase,
	.protect		= f0xx_protect,
	.write			= f0xx_write,
	.read			= default_flash_read,
	.probe			= f0xx_probe,
	.auto_probe		= f0xx_auto_probe,
	.erase_check		= default_flash_blank_check,
	.protect_check		= f0xx_protect_check,
};
