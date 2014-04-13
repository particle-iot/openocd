/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2014 by Andrey Smirnov                                  *
 *   andrew.smirnov@gmail.com                                              *
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
 *   along with this program; see the file COPYING.  If not see            *
 *   <http://www.gnu.org/licenses/>                                        *
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"

#include <binarybuffer.h>
#include <time_support.h>
#include <target/bdm.h>
#include <target/hcs12.h>
#include <target/algorithm.h>
#include <target/register.h>
l
#include "s12xftm.h"

enum {
	S12XFTM_FLASH_SECTOR_SIZE = 1024
};

enum s12xftm_bank_types {
	   S12XFTM_BANK_PFLASH,
	   S12XFTM_BANK_DFLASH,
	   S12XFTM_BANK_NUM
};


struct s12xftm_info {
	struct target *target;
	bool probed;

	struct {
		int (*write) (struct flash_bank *bank,
			      struct s12xftm_info *chip,
			      const uint8_t *buffer, uint32_t offset, uint32_t count);
	} bank[S12XFTM_BANK_NUM];

	const struct s12xftm_part_descriptor *part;
};

struct s12xftm_pflash_block {
	uint32_t start, end, size;
};

enum {
	PFLASH3,
	PFLASH2,
	PFLASH1S,
	PFLASH1N,
	PFLASH0,
};

/*
 * All devices in MC9S12XE would have combination of the follwing
 * P-Flash blocks present. They represent a second level of erase
 * granularity, with the first being erasing a signel sector
 */
const struct s12xftm_pflash_block pflash_block[] = {
	[PFLASH0] = {
		.start = 0x7C0000,
		.end   = 0x7FFFFF,
		.size  = 256 * 1024,
	},
	[PFLASH1N] = {
		.start = 0x7A0000,
		.end   = 0x7BFFFF,
		.size  = 128 * 1024,
	},

	[PFLASH1S] = {
		.start = 0x780000,
		.end   = 0x79FFFF,
		.size  = 128 * 1024,
	},

	[PFLASH2] = {
		.start = 0x740000,
		.end   = 0x77FFFF,
		.size  = 256 * 1024,
	},

	[PFLASH3] = {
		.start = 0x700000,
		.end   = 0x73FFFF,
		.size  = 256 * 1024,
	},
};

/*
 * Structure describing parameters of a prticular member of MC9S12XE
 * family
 */
struct s12xftm_part_descriptor {
	uint16_t partid;	/* Part ID as can be read from PARTID register */
	const char *name;	/* Symbolic name of the part */
	const struct s12xftm_pflash_block *blocks[5];	/* Array containing pointers to flash blocks

							  NOTE: Driver code expects this array to be
							  sorted in start address descending order */
	size_t blocks_count;	/* Number of flash blocks that MCU has */
	uint32_t dflash_start;	/* Start address of the D-Flash region */
};

enum hcs12_reg_fstat_bits {
	HCS12_REG_FSTAT_CCIF	= (1 << 7),
	HCS12_REG_FSTAT_ACCERR	= (1 << 5),
	HCS12_REG_FSTAT_FPVIOL	= (1 << 4),
	HCS12_REG_FSTAT_MGBUSY	= (1 << 3),
	HCS12_REG_FSTAT_MGSTAT_MASK = 0b11,
};

enum s12xftm_fprot_bits {
	HCS12_REG_FPROT_FPOPEN		= (1 << 7),
	HCS12_REG_FPROT_FPHDIS		= (1 << 5),
	HCS12_REG_FPROT_FPHS_SHIFT	= 3,
	HCS12_REG_FPROT_FPHS_MASK	= (0b11 << HCS12_REG_FPROT_FPHS_SHIFT),
	HCS12_REG_FPROT_FPLDIS		= (1 << 2),
	HCS12_REG_FPROT_FPLS_SHIFT	= 0,
	HCS12_REG_FPROT_FPLS_MASK	= (0b11 << HCS12_REG_FPROT_FPLS_SHIFT),
};

enum s12xftm_flash_commands {
	S12XFTM_CMD_ERASE_VERIFY_ALL_BLOCKS	= 0x01,
	S12XFTM_CMD_ERASE_VERIFY_BLOCK		= 0x02,
	S12XFTM_CMD_ERASE_VERIFY_PFLASH_SECTION = 0x03,
	S12XFTM_CMD_READ_ONCE			= 0x04,
	S12XFTM_CMD_LOAD_DATA_FIELD		= 0x05,
	S12XFTM_CMD_PROGRAM_PFLASH		= 0x06,
	S12XFTM_CMD_PROGRAM_ONCE		= 0x07,
	S12XFTM_CMD_ERASE_ALL_BLOCKS		= 0x08,
	S12XFTM_CMD_ERASE_PFLASH_BLOCK		= 0x09,
	S12XFTM_CMD_ERASE_PFLASH_SECTOR		= 0x0A,
	S12XFTM_CMD_UNSECURE_FLASH		= 0x0B,
	S12XFTM_CMD_VERIFY_BACKDOOR_ACCESS_KEY	= 0x0C,
	S12XFTM_CMD_SET_USER_MARGIN_LEVEL	= 0x0D,
	S12XFTM_CMD_SET_FIELD_MARGIN_LEVEL	= 0x0E,
	S12XFTM_CMD_FULL_PARTITION_DFLASH	= 0x0F,
	S12XFTM_CMD_ERASE_VERIFY_DFLASH_SECTION = 0x10,
	S12XFTM_CMD_PROGRAM_DFLASH		= 0x11,
	S12XFTM_CMD_ERASE_DFLASH_SECTOR		= 0x12,
	S12XFTM_CMD_ENABLE_EEPROM_EMULATION	= 0x13,
	S12XFTM_CMD_DISABLE_EEPROM_EMULATION	= 0x14,
	S12XFTM_CMD_EEPROM_EMULATION_QUERY	= 0x15,
	S12XFTM_CMD_PARTITION_DFLASH		= 0x20,
};

const struct s12xftm_part_descriptor s12xftm_supported_chips[] = {
	{
		.partid = 0xcc94,
		.name   = "MC9S12XEP100, MC9S12XEP768",
		.blocks = {
			&pflash_block[PFLASH3],
			&pflash_block[PFLASH2],
			&pflash_block[PFLASH1S],
			&pflash_block[PFLASH1N],
			&pflash_block[PFLASH0],
		},
		.blocks_count = 5,
		.dflash_start = 0x100000,
	},
};

/*
  Binary generated by compiling s12xftm flash helper from 'contrib'
  directory.
 */
static const uint8_t s12xftm_helper_binary[] = {
	0xb7, 0x56,
	0x87,
	0xe6, 0x00,
	0x26, 0x1e,
	0xec, 0x44,
	0xee, 0x42,
	0x83, 0x00, 0x01,
	0x24, 0x01,
	0x09,
	0x6c, 0x44,
	0x6e, 0x42,
	0xec, 0x44,
	0xee, 0x42,
	0x8e, 0xff, 0xff,
	0x26, 0xe9,
	0x8c, 0xff, 0xff,
	0x26, 0xe4,
	0x00,
	0x69, 0x41,
	0x3d,
};

struct s12xftm_flash_clock_divider_entry {
	struct {
		int min, max;
	} oscclk_hz;

	uint8_t fdiv;
};

/*
  Table used to detemine the falue of the Flash clock divider, so the mandatory 1Mhz clock can be
  provided to the Flash block.

  All the values are taken from:

  Table 29-9. FDIV vs OSCCLK Frequency
  MC9S12XE-Family Reference Manual Rev. 1.25
*/
static const struct s12xftm_flash_clock_divider_entry
s12xftm_flash_clock_divider_table[] = {
	{ .oscclk_hz = { .min = 1600000,  .max = 2100000,  },  .fdiv = 0x01, },
	{ .oscclk_hz = { .min = 2400000,  .max = 3150000,  },  .fdiv = 0x02, },
	{ .oscclk_hz = { .min = 3200000,  .max = 4200000,  },  .fdiv = 0x03, },
	{ .oscclk_hz = { .min = 4200000,  .max = 5250000,  },  .fdiv = 0x04, },
	{ .oscclk_hz = { .min = 5250000,  .max = 6300000,  },  .fdiv = 0x05, },
	{ .oscclk_hz = { .min = 6300000,  .max = 7350000,  },  .fdiv = 0x06, },
	{ .oscclk_hz = { .min = 7350000,  .max = 8400000,  },  .fdiv = 0x07, },
	{ .oscclk_hz = { .min = 8400000,  .max = 9450000,  },  .fdiv = 0x08, },
	{ .oscclk_hz = { .min = 9450000,  .max = 10500000, },  .fdiv = 0x09, },
	{ .oscclk_hz = { .min = 10500000, .max = 11550000, },  .fdiv = 0x0A, },
	{ .oscclk_hz = { .min = 11550000, .max = 12600000, },  .fdiv = 0x0B, },
	{ .oscclk_hz = { .min = 12600000, .max = 13650000, },  .fdiv = 0x0C, },
	{ .oscclk_hz = { .min = 13650000, .max = 14700000, },  .fdiv = 0x0D, },
	{ .oscclk_hz = { .min = 14700000, .max = 15750000, },  .fdiv = 0x0E, },
	{ .oscclk_hz = { .min = 15750000, .max = 16800000, },  .fdiv = 0x0F, },
	{ .oscclk_hz = { .min = 16800000, .max = 17850000, },  .fdiv = 0x10, },
	{ .oscclk_hz = { .min = 17850000, .max = 18900000, },  .fdiv = 0x11, },
	{ .oscclk_hz = { .min = 18900000, .max = 19950000, },  .fdiv = 0x12, },
	{ .oscclk_hz = { .min = 19950000, .max = 21000000, },  .fdiv = 0x13, },
	{ .oscclk_hz = { .min = 21000000, .max = 22050000, },  .fdiv = 0x14, },
	{ .oscclk_hz = { .min = 22050000, .max = 23100000, },  .fdiv = 0x15, },
	{ .oscclk_hz = { .min = 23100000, .max = 24150000, },  .fdiv = 0x16, },
	{ .oscclk_hz = { .min = 24150000, .max = 25200000, },  .fdiv = 0x17, },
	{ .oscclk_hz = { .min = 25200000, .max = 26250000, },  .fdiv = 0x18, },
	{ .oscclk_hz = { .min = 26250000, .max = 27300000, },  .fdiv = 0x19, },
	{ .oscclk_hz = { .min = 27300000, .max = 28350000, },  .fdiv = 0x1A, },
	{ .oscclk_hz = { .min = 28350000, .max = 29400000, },  .fdiv = 0x1B, },
	{ .oscclk_hz = { .min = 29400000, .max = 30450000, },  .fdiv = 0x1C, },
	{ .oscclk_hz = { .min = 30450000, .max = 31500000, },  .fdiv = 0x1D, },
	{ .oscclk_hz = { .min = 31500000, .max = 32550000, },  .fdiv = 0x1E, },
	{ .oscclk_hz = { .min = 32550000, .max = 33600000, },  .fdiv = 0x1F, },
	{ .oscclk_hz = { .min = 33600000, .max = 34650000, },  .fdiv = 0x20, },
	{ .oscclk_hz = { .min = 34650000, .max = 35700000, },  .fdiv = 0x21, },
	{ .oscclk_hz = { .min = 35700000, .max = 36750000, },  .fdiv = 0x22, },
	{ .oscclk_hz = { .min = 36750000, .max = 37800000, },  .fdiv = 0x23, },
	{ .oscclk_hz = { .min = 37800000, .max = 38850000, },  .fdiv = 0x24, },
	{ .oscclk_hz = { .min = 38850000, .max = 39900000, },  .fdiv = 0x25, },
	{ .oscclk_hz = { .min = 39900000, .max = 40950000, },  .fdiv = 0x26, },
	{ .oscclk_hz = { .min = 40950000, .max = 42000000, },  .fdiv = 0x27, },
	{ .oscclk_hz = { .min = 42000000, .max = 43050000, },  .fdiv = 0x28, },
	{ .oscclk_hz = { .min = 43050000, .max = 44100000, },  .fdiv = 0x29, },
	{ .oscclk_hz = { .min = 44100000, .max = 45150000, },  .fdiv = 0x2A, },
	{ .oscclk_hz = { .min = 45150000, .max = 46200000, },  .fdiv = 0x2B, },
	{ .oscclk_hz = { .min = 46200000, .max = 47250000, },  .fdiv = 0x2C, },
	{ .oscclk_hz = { .min = 47250000, .max = 48300000, },  .fdiv = 0x2D, },
	{ .oscclk_hz = { .min = 48300000, .max = 49350000, },  .fdiv = 0x2E, },
	{ .oscclk_hz = { .min = 49350000, .max = 50400000, },  .fdiv = 0x2F, },
	{ .oscclk_hz = { .min = 50400000, .max = 51450000, },  .fdiv = 0x30, },
	{ .oscclk_hz = { .min = 51450000, .max = 52500000, },  .fdiv = 0x31, },
	{ .oscclk_hz = { .min = 52500000, .max = 53550000, },  .fdiv = 0x32, },
	{ .oscclk_hz = { .min = 53550000, .max = 54600000, },  .fdiv = 0x33, },
	{ .oscclk_hz = { .min = 54600000, .max = 55650000, },  .fdiv = 0x34, },
	{ .oscclk_hz = { .min = 55650000, .max = 56700000, },  .fdiv = 0x35, },
	{ .oscclk_hz = { .min = 56700000, .max = 57750000, },  .fdiv = 0x36, },
	{ .oscclk_hz = { .min = 57750000, .max = 58800000, },  .fdiv = 0x37, },
	{ .oscclk_hz = { .min = 58800000, .max = 59850000, },  .fdiv = 0x38, },
	{ .oscclk_hz = { .min = 59850000, .max = 60900000, },  .fdiv = 0x39, },
	{ .oscclk_hz = { .min = 60900000, .max = 61950000, },  .fdiv = 0x3A, },
	{ .oscclk_hz = { .min = 61950000, .max = 63000000, },  .fdiv = 0x3B, },
	{ .oscclk_hz = { .min = 63000000, .max = 64050000, },  .fdiv = 0x3C, },
	{ .oscclk_hz = { .min = 64050000, .max = 65100000, },  .fdiv = 0x3D, },
	{ .oscclk_hz = { .min = 65100000, .max = 66150000, },  .fdiv = 0x3E, },
	{ .oscclk_hz = { .min = 66150000, .max = 67200000, },  .fdiv = 0x3F, },
	{ .oscclk_hz = { .min = 67200000, .max = 68250000, },  .fdiv = 0x40, },
	{ .oscclk_hz = { .min = 68250000, .max = 69300000, },  .fdiv = 0x41, },
	{ .oscclk_hz = { .min = 69300000, .max = 70350000, },  .fdiv = 0x42, },
	{ .oscclk_hz = { .min = 70350000, .max = 71400000, },  .fdiv = 0x43, },
	{ .oscclk_hz = { .min = 71400000, .max = 72450000, },  .fdiv = 0x44, },
	{ .oscclk_hz = { .min = 72450000, .max = 73500000, },  .fdiv = 0x45, },
	{ .oscclk_hz = { .min = 73500000, .max = 74550000, },  .fdiv = 0x46, },
	{ .oscclk_hz = { .min = 74550000, .max = 75600000, },  .fdiv = 0x47, },
	{ .oscclk_hz = { .min = 75600000, .max = 76650000, },  .fdiv = 0x48, },
	{ .oscclk_hz = { .min = 76650000, .max = 77700000, },  .fdiv = 0x49, },
	{ .oscclk_hz = { .min = 77700000, .max = 78750000, },  .fdiv = 0x4A, },
	{ .oscclk_hz = { .min = 78750000, .max = 79800000, },  .fdiv = 0x4B, },
	{ .oscclk_hz = { .min = 79800000, .max = 80850000, },  .fdiv = 0x4C, },
	{ .oscclk_hz = { .min = 80850000, .max = 81900000, },  .fdiv = 0x4D, },
	{ .oscclk_hz = { .min = 81900000, .max = 82950000, },  .fdiv = 0x4E, },
	{ .oscclk_hz = { .min = 82950000, .max = 84000000, },  .fdiv = 0x4F, },
	{ .oscclk_hz = { .min = 84000000, .max = 85050000, },  .fdiv = 0x50, },
	{ .oscclk_hz = { .min = 85050000, .max = 86100000, },  .fdiv = 0x51, },
	{ .oscclk_hz = { .min = 86100000, .max = 87150000, },  .fdiv = 0x52, },
	{ .oscclk_hz = { .min = 87150000, .max = 88200000, },  .fdiv = 0x53, },
	{ .oscclk_hz = { .min = 88200000, .max = 89250000, },  .fdiv = 0x54, },
	{ .oscclk_hz = { .min = 89250000, .max = 90300000, },  .fdiv = 0x55, },
	{ .oscclk_hz = { .min = 90300000, .max = 91350000, },  .fdiv = 0x56, },
	{ .oscclk_hz = { .min = 91350000, .max = 92400000, },  .fdiv = 0x57, },
	{ .oscclk_hz = { .min = 92400000, .max = 93450000, },  .fdiv = 0x58, },
	{ .oscclk_hz = { .min = 93450000, .max = 94500000, },  .fdiv = 0x59, },
	{ .oscclk_hz = { .min = 94500000, .max = 95550000, },  .fdiv = 0x5A, },
	{ .oscclk_hz = { .min = 95550000, .max = 96600000, },  .fdiv = 0x5B, },
	{ .oscclk_hz = { .min = 96600000, .max = 97650000, },  .fdiv = 0x5C, },
	{ .oscclk_hz = { .min = 97650000, .max = 98700000, },  .fdiv = 0x5D, },
	{ .oscclk_hz = { .min = 98700000, .max = 99750000, },  .fdiv = 0x5E, },
	{ .oscclk_hz = { .min = 99750000, .max = 100800000, }, .fdiv = 0x5F, },
};

/**
 * Find appropriate clock divider value given the CPU clock speed
 * @param oscclk_hz CPU clock in Hz
 * @returns ERROR_FAIL for failure and poistive 8-bit clock divisor
 * value for success.
 */
static int s12xftm_find_clock_divider(int oscclk_hz)
{
	for (size_t i = 0; i < ARRAY_SIZE(s12xftm_flash_clock_divider_table); i++) {
		if (oscclk_hz >= s12xftm_flash_clock_divider_table[i].oscclk_hz.min &&
		    oscclk_hz <= s12xftm_flash_clock_divider_table[i].oscclk_hz.max) {
			return s12xftm_flash_clock_divider_table[i].fdiv;
		}
	}

	return ERROR_FAIL;
}

static int s12xftm_probe(struct flash_bank *bank);
/**
 * Return proped chip handle if the target is halted
 * @param bank bank for which chip handle is being retreived
 * @param chip chip handle pointer to store the result into
 * @returns ERROR_OK on success, negative error code on failure
 */
static int s12xftm_get_probed_chip_if_halted(struct flash_bank *bank,
					     struct s12xftm_info **chip)
{
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	*chip = bank->driver_priv;

	if (!(*chip)->probed)
		return s12xftm_probe(bank);

	return ERROR_OK;
}

/**
 * Determine the type of a bank
 * @param bank flash bank to determint the type of
 * @returns ERROR_FAIL on failure, bank type on success
 */
static enum s12xftm_bank_types s12xftm_get_bank_type(struct flash_bank *bank)
{
	struct s12xftm_info *chip = bank->driver_priv;

	if (bank->base == chip->part->blocks[0]->start ||
	    bank->base == pflash_block[PFLASH0].end)
		return S12XFTM_BANK_PFLASH;
	else if (bank->base == chip->part->dflash_start) {
		return S12XFTM_BANK_PFLASH;

	}

	return ERROR_FAIL;
}

FLASH_BANK_COMMAND_HANDLER(s12xftm_flash_bank_command)
{
	struct s12xftm_info *chip;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	chip = calloc(1, sizeof(*chip));
	chip->probed = false;
	bank->driver_priv = chip;
	chip->target = bank->target;

	return ERROR_OK;
}

static int s12xftm_protect(struct flash_bank *bank, int set, int first, int last)
{
	return ERROR_OK;
}

/**
 * Execute a signle flash controller command
 * @param chip chip handle
 * @fccob contents to write to FCCOB register
 * @fccob_size size of @fccob
 * @returns ERROR_OK on success, negative error code on failure

 */
static int s12xftm_execute_flash_command(struct s12xftm_info *chip,
					 uint16_t fccob[], uint8_t fccob_size)
{
	uint8_t _fstat;
	struct target *target = chip->target;
	const char *reason = NULL;

	int ret, timeout;
	/* Clear the status registers */
	ret = target_write_u8(target, HCS12_REG_FSTAT, 0x00);
	if (ret < 0) {
		reason = "Failed to clear FSTAT";
		ret = ERROR_FAIL;
		goto fail;
	}

	ret = target_read_u8(target, HCS12_REG_FSTAT, &_fstat);
	if (ret < 0) {
		reason = "Failed to read flash status";
		ret = ERROR_FAIL;
		goto fail;
	}

	if (!(_fstat & HCS12_REG_FSTAT_CCIF)) {
		reason = "Previous flash operation is still active";
		ret = ERROR_FAIL;
		goto fail;
	}

	for (int i = 0; i < fccob_size; i++) {
		ret = target_write_u8(target, HCS12_REG_FCCOBIX, i);
		if (ret < 0) {
			reason = "Failed to clear FCCOBIX";
			ret = ERROR_FAIL;
			goto fail;
		}

		ret = target_write_u16(target, HCS12_REG_FCCOB, fccob[i]);
		if (ret < 0) {
			reason = "Failed to clear FCCOB";
			ret = ERROR_FAIL;
			goto fail;
		}
	}

	ret = target_write_u8(target, HCS12_REG_FSTAT, HCS12_REG_FSTAT_CCIF);
	if (ret < 0) {
		reason = "Failed to set CCIF in FSTAT";
		goto fail;
	}

	timeout = 20;
	do {
		ret = target_read_u8(target, HCS12_REG_FSTAT, &_fstat);
		if (ret < 0) {
			reason = "Failed to read flash status";
			ret = ERROR_FAIL;
			goto fail;
		}

		if (_fstat & (HCS12_REG_FSTAT_FPVIOL | HCS12_REG_FSTAT_ACCERR)) {
			reason = "Flash command failed";
			ret = ERROR_FAIL;
			goto fail;
		}

		if (_fstat & HCS12_REG_FSTAT_CCIF)
			return ERROR_OK;
	} while (timeout--);

	reason = "Operation timed out";
	ret = ERROR_FAIL;

fail:
	/* TODO: It would be good to decode those messages and
	 * present them in a human-readable form */
	LOG_ERROR("fccob = %04x%04x%04x%04x%04x%04x, fstat = %02x",
		  fccob[0], fccob[1], fccob[2], fccob[3],
		  fccob[4], fccob[5],
		  _fstat);
	LOG_ERROR("%s", reason);
	return ret;
}

static int s12xftm_cmd_erase_pflash_sector(struct s12xftm_info *chip, uint32_t address)
{
	uint16_t fccob[] = {
		[0] = S12XFTM_CMD_ERASE_PFLASH_SECTOR << 8 | ((address >> 16) & 0xFF),
		[1] = address & 0xFFFF,
	};

	return s12xftm_execute_flash_command(chip, fccob, ARRAY_SIZE(fccob));
}

static int s12xftm_cmd_erase_pflash_block(struct s12xftm_info *chip, uint32_t address)
{
	uint16_t fccob[] = {
		[0] = S12XFTM_CMD_ERASE_PFLASH_BLOCK << 8 | ((address >> 16) & 0xFF),
		[1] = address & 0xFFFF,
	};

	return s12xftm_execute_flash_command(chip, fccob, ARRAY_SIZE(fccob));
}

/**
 * A generic wrapper for flash commands that require no arguments
 * @param chip chip handle
 * @param cmd command to execute
 * @returns ERROR_OK on success, negative error code on failure
 */
static int s12xftm_noarg_cmd(struct s12xftm_info *chip, uint8_t cmd)
{
	uint16_t fccob[] = {
		[0] = cmd << 8,
	};

	return s12xftm_execute_flash_command(chip, fccob, ARRAY_SIZE(fccob));
}

static struct flash_sector *s12xftm_find_sector_by_address(struct flash_bank *bank, uint32_t address)
{
	for (int i = 0; i < bank->num_sectors; i++)
		if (bank->sectors[i].offset <= address &&
		    address < (bank->sectors[i].offset + bank->sectors[i].size))
			return &bank->sectors[i];
	return NULL;
}

static bool s12xftm_range_is_subrange_of(uint32_t subrange_start,
					 uint32_t subrange_end,
					 uint32_t range_start,
					 uint32_t range_end)
{
	return (subrange_start >= range_start &&
		subrange_end <= range_end);
}

static int s12xftm_erase(struct flash_bank *bank, int first, int last)
{
	int ret, s;
	struct s12xftm_info *chip;

	ret = s12xftm_get_probed_chip_if_halted(bank, &chip);
	if (ret != ERROR_OK)
		return ret;

	/*
	   First pass: erase in block granularity
	 */
	const uint32_t erase_start_address = bank->sectors[first].offset;
	const uint32_t erase_end_address   = bank->sectors[last].offset + bank->sectors[last].size - 1;

	for (size_t b = 0; b < chip->part->blocks_count; b++) {
		const bool block_belongs_to_erase_region = s12xftm_range_is_subrange_of(chip->part->blocks[b]->start,
											chip->part->blocks[b]->end,
											erase_start_address,
											erase_end_address);
		if (block_belongs_to_erase_region) {
			ret = s12xftm_cmd_erase_pflash_block(chip, chip->part->blocks[b]->start);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to erase P-Flash block @ 0x%08" PRIx32, chip->part->blocks[b]->start);
				return ret;
			}

			/* Mark all of the sector within that block
			 * as erased, so we won't double-erase them
			 * during the second pass */
			for (s = first; s <= last; s++) {
				const uint32_t sector_start = bank->sectors[s].offset;
				const uint32_t sector_end   = bank->sectors[s].offset + bank->sectors[s].size - 1;
				const bool sector_is_within_the_block = s12xftm_range_is_subrange_of(sector_start,
												     sector_end,
												     chip->part->blocks[b]->start,
												     chip->part->blocks[b]->end);

				bank->sectors[s].is_erased = sector_is_within_the_block;
			}
		}
	}

	/*
	  Second pass: erase remaining pages
	 */
	for (s = first; s <= last; s++) {
		if (bank->sectors[s].is_erased)
			continue;

		if (bank->sectors[s].is_protected)
			return ERROR_FAIL;

		ret = s12xftm_cmd_erase_pflash_sector(chip, bank->sectors[s].offset);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to erase P-Flash sector @ 0x%08" PRIx32, bank->sectors[s].offset);
			return ret;
		}

		bank->sectors[s].is_erased = 1;
	}

	return ERROR_OK;
}

static int s12xftm_cmd_program_pflash(struct s12xftm_info *chip, uint32_t address,
				  const uint8_t *data)
{
	if (address & 0b111) {
		LOG_ERROR("Address must be 8-byte block aligned");
		return ERROR_FAIL;
	}

	uint16_t fccob[] = {
		[0] = S12XFTM_CMD_PROGRAM_PFLASH << 8 | ((address >> 16) & 0xFF),
		[1] = address & 0xFFFF,
		[2] = target_buffer_get_u16(chip->target, &data[0]),
		[3] = target_buffer_get_u16(chip->target, &data[2]),
		[4] = target_buffer_get_u16(chip->target, &data[4]),
		[5] = target_buffer_get_u16(chip->target, &data[6])
	};

	return s12xftm_execute_flash_command(chip, fccob, ARRAY_SIZE(fccob));
}


static int s12xftm_overwrite_page(struct flash_bank *bank,
				  struct s12xftm_info *chip, uint32_t address,
				  const uint8_t *data)
{
	int ret, i;
	assert(address % S12XFTM_FLASH_SECTOR_SIZE == 0);
	const size_t chunk_size = 8;

	struct flash_sector *sector = s12xftm_find_sector_by_address(bank, address);

	assert(sector);

	if (sector->is_protected)
		return ERROR_FAIL;

	if (!sector->is_erased) {
		ret = s12xftm_cmd_erase_pflash_sector(chip, address);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to erase sector @ 0x%06x", address);
			return ret;
		}
	} else {
		sector->is_erased = 0;
	}

	for (i = 0; i < S12XFTM_FLASH_SECTOR_SIZE;
	     i += chunk_size, address += chunk_size) {
		ret = s12xftm_cmd_program_pflash(chip, address, &data[i]);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to write block @ 0x%06x", address);
			return ret;
		}
	}

	return ERROR_OK;
}

static int s12xftm_write_pflash(struct flash_bank *bank,
				struct s12xftm_info *chip, const uint8_t *data,
				uint32_t address, uint32_t size)
{
	int ret;
	uint8_t  page[S12XFTM_FLASH_SECTOR_SIZE];

	address += bank->base;

	const uint32_t data_start     = address;
	const uint32_t data_end       = address + size;
	const uint32_t alignment_mask = ~((uint32_t)sizeof(page) - 1);
	const uint32_t aligned_start  = (address % sizeof(page)) ? (address & alignment_mask) + sizeof(page) : address;
	/* We can't calculate aligned_end just as (address + size) &
	 * alignment_mask, since in cases where size is less the
	 * sizeof(page) it would produce alingend_end that is
	 * located before aligned start, which would break one of the
	 * major assumptions */
	const uint32_t aligned_end    = MAX(aligned_start, (address + size) & alignment_mask);
	const uint32_t aligned_size   = aligned_end - aligned_start;

	/*
	   Write aligned portion of the data buffer(provided that it
	   is at least one page long
	 */
	if (aligned_size >= sizeof(page)) {
		uint32_t idx     = aligned_start - data_start;
		uint32_t offset  = aligned_start;

		for (; offset < aligned_end; idx += sizeof(page), offset += sizeof(page)) {
			ret = s12xftm_overwrite_page(bank, chip, offset, &data[idx]);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to program pflash bloack @ 0x%06x", offset);
				return ret;
			}
		}
	}

	/*
	  Write residue at the head of the data buffer
	  FIXME: This code can be improved by handling cases where the
	  page to which we are writing the residue is empty and we
	  don't really need to preserve any information
	*/
	if (data_start < aligned_start) {
		const uint32_t offset = aligned_start - sizeof(page);

		ret = target_read_memory(chip->target, offset, 1, sizeof(page), page);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to read residue data at the beginning of the data page");
			return ret;
		}

		const size_t head_idx  = data_start - offset;
		/* In cases when total write size is less that one
		 * page and it doesn't cross alignment border we
		 * should just use size of the whole write as
		 * head_size  */
		const size_t head_size = MIN(sizeof(page) - head_idx, size);

		memcpy(&page[head_idx], data, head_size);

		ret = s12xftm_overwrite_page(bank, chip, offset, page);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to program pflash page @ 0x%06x", offset);
			return ret;
		}
	}

	/*
	  Write residue at the tail of the data buffer
	  FIXME: This code can be improved by handling cases where the
	  page to which we are wrigin the residue is empty and we
	  don't really need to preserve any information
	*/
	if (data_end > aligned_end) {
		const uint32_t offset = aligned_end;

		ret = target_read_memory(chip->target, aligned_end, 1, sizeof(page), page);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to read residue data at the beginning of the data page");
			return ret;
		}

		const size_t tail_idx  = aligned_end - data_start;
		const size_t tail_size = data_end - aligned_end;

		memcpy(page, &data[tail_idx], tail_size);

		ret = s12xftm_overwrite_page(bank, chip, offset, page);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to program pflash page @ 0x%06x", offset);
			return ret;
		}
	}

	return ERROR_OK;
}

static int s12xftm_write(struct flash_bank *bank, const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	int ret;
	struct s12xftm_info *chip;

	ret = s12xftm_get_probed_chip_if_halted(bank, &chip);
	if (ret != ERROR_OK)
		return ret;

	return chip->bank[bank->bank_number].write(bank, chip, buffer, offset, count);
}

static int s12xftm_protect_check(struct flash_bank *bank)
{
	int ret;
	const size_t fpls_region_start = 0x7F8000;
	const size_t fphs_region_end   = 0x7FFFFF;

	const size_t fphs_size_lut[] = {
		[0b00] = 2048,
		[0b01] = 4096,
		[0b10] = 8192,
		[0b11] = 16384,
	};

	const size_t fpls_size_lut[] = {
		[0b00] = 1024,
		[0b01] = 2048,
		[0b10] = 4096,
		[0b11] = 8192,
	};

	uint8_t fprot;

	ret = target_read_u8(bank->target, HCS12_REG_FPROT, &fprot);
	if (ret < 0) {
		LOG_ERROR("Failed to get FPROT");
		return ret;
	}

	const size_t fpls_idx          = (fprot & HCS12_REG_FPROT_FPLS_MASK) >> HCS12_REG_FPROT_FPLS_SHIFT;
	const size_t fpls_region_size  = fpls_size_lut[fpls_idx];
	/* denotes the last valid byte addres within fpls region */
	const size_t fpls_region_end   = fpls_region_start + fpls_region_size - 1;
	const size_t fphs_idx	       = (fprot & HCS12_REG_FPROT_FPHS_MASK) >> HCS12_REG_FPROT_FPHS_SHIFT;
	const size_t fphs_region_size  = fphs_size_lut[fphs_idx];
	/* denotes the first valid byte addres within fphs region */
	const size_t fphs_region_start = fphs_region_end - fphs_region_size + 1;

	const bool fpxs_region_defines_protected_range = !!(fprot & HCS12_REG_FPROT_FPOPEN);
	const bool fpls_enabled = !(fprot & HCS12_REG_FPROT_FPLDIS);
	const bool fphs_enabled = !(fprot & HCS12_REG_FPROT_FPHDIS);

	for (int s = 0; s < bank->num_sectors; s++) {
		if (bank->sectors[s].offset >= fphs_region_start &&
		    (bank->sectors[s].offset + bank->sectors[s].size - 1) <= fphs_region_end) {
			bank->sectors[s].is_protected = (fpxs_region_defines_protected_range == fphs_enabled);
		} else if (bank->sectors[s].offset >= fpls_region_start &&
			   (bank->sectors[s].offset + bank->sectors[s].size - 1) <= fpls_region_end) {
			bank->sectors[s].is_protected = (fpxs_region_defines_protected_range == fpls_enabled);
		} else {
			bank->sectors[s].is_protected = !fpxs_region_defines_protected_range;
		}
	}

	return ERROR_OK;
}
/**
 * Execute a "subroutine" implemented as a code running on the target
 * @param chip chip handle
 * @param scratchpad subroutine call arguments
 * @returns ERROR_OK on success, negative error code on failure
 */
static int s12xftm_execute_helper_command(struct s12xftm_info *chip,
					  struct s12xftm_helper_context *scratchpad)
{
	int ret, err;
	struct working_area *code;
	struct working_area *context;
	struct hcs12_algorithm helper;
	struct target *target = chip->target;

	/*
	  Allocate area for helper code
	*/
	ret = target_alloc_working_area(target,
					sizeof(s12xftm_helper_binary),
					&code);
	if (ret != ERROR_OK) {
		LOG_ERROR("No working area available, can't upload helper binary");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}
	/*
	  Allocate area for the context of helper's execution
	 */
	ret = target_alloc_working_area(target,
					sizeof(*scratchpad),
					&context);
	if (ret != ERROR_OK) {
		LOG_ERROR("No working area available, can't upload helper binary");
		ret = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto free_working_area_for_code;
	}

	/*
	  Transfer the code of the helper to MCU
	 */
	ret = target_write_buffer(target, code->address,
				  code->size,
				  s12xftm_helper_binary);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to upload helper binary");
		goto free_working_area_for_code_and_context;
	}

	struct reg_param ctxptr;
	struct mem_param ctx;

	ret = hcs12_ram_map_global_to_local(target,
					    context->address);
	if (ret < 0) {
		LOG_ERROR("Failed to map context address");
		goto free_working_area_for_code_and_context;
	}

	const uint16_t ctxaddr = (uint16_t)ret;

	/*
	  Subroine code running on the target CPU implements the
	  following calling discipline. It takes two parameters for
	  its exectuion:
		- Memory parameter which specifies is used for data
		exchange
		- Register parameter(passed in X register) that
		contanis the address of the memory area.
	 */

	init_reg_param(&ctxptr, "x", 16, PARAM_IN);
	buf_set_u16(ctxptr.value, 0, 16, ctxaddr);

	ctx.address	= context->address;
	ctx.size	= context->size;
	ctx.value	= (uint8_t *)scratchpad;
	ctx.direction	= PARAM_IN_OUT;

	ret = target_run_algorithm(target,
				   1, &ctx,
				   1, &ctxptr,
				   code->address, 0x0000,
				   1000, /* FIXME: is 1 second enough? */
				   &helper);

	destroy_reg_param(&ctxptr);

free_working_area_for_code_and_context:
	err = target_free_working_area(target, context);
	if (err != ERROR_OK)
		LOG_ERROR("Failed to free context working area");

free_working_area_for_code:
	err = target_free_working_area(target, code);
	if (err != ERROR_OK)
		LOG_ERROR("Failed to free code working area");

	return ret;
}

/**
 * Measure the CPU clock speed by executing as simple program on the
 * target that continuously decrements a counter.
 * @param chip chip handle
 * @returns CPU clock speed in Hz on success, negative error code on failure
 */
static int s12xftm_measure_cpu_speed(struct s12xftm_info *chip)
{
	int ret;

	struct s12xftm_helper_context scratchpad;

	const uint32_t start_value = 20000;

	memset(&scratchpad, 0, sizeof(scratchpad));

	scratchpad.command = S12XFTM_HELPER_CMD_PERF;

	h_u32_to_be((uint8_t *) &scratchpad.in.perf.counter,  start_value);

	const int64_t t1 = timeval_ms();

	ret = s12xftm_execute_helper_command(chip,
					     &scratchpad);

	const int64_t t2 = timeval_ms();

	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to execute speed measurement algorithm");
		return ret;
	}

	if (scratchpad.out.retcode != ERROR_OK) {
		LOG_ERROR("Speed measurement algorithm exited with error");
		return ret;
	}
	/*
	 * This is an empirical constant the value of which was
	 * obtained by doing revers calulation on the board with a
	 * known CPU clock speed(4Mhz), hopefully it would produce
	 * useful results for other clock speeds
	 */
	const int64_t k        = 57;
	const int64_t dt       = t2 - t1;
	const int64_t speed_hz = start_value * k * 1000 / dt;

	LOG_INFO("Measured CPU clock speed is %" PRId64 " Hz", speed_hz);
	return speed_hz;
}

static int s12xftm_probe(struct flash_bank *bank)
{
	size_t i;
	int ret;
	struct target *target = bank->target;
	struct s12xftm_info *chip = bank->driver_priv;
	uint16_t partid;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = target_read_u16(target, HCS12_REG_PARTID, &partid);
	if (ret < 0) {
		LOG_ERROR("Failed to get part id information");
		return ret;
	}

	chip->part = NULL;
	for (i = 0; i < ARRAY_SIZE(s12xftm_supported_chips); i++) {
		if (partid == s12xftm_supported_chips[i].partid)
			chip->part = &s12xftm_supported_chips[i];
	}

	if (!chip->part) {
		LOG_ERROR("Unsupported part with id 0x%04" PRIx16, partid);
		return ERROR_FAIL;
	} else {
		LOG_INFO("Detected %s chip", chip->part->name);
	}

	const int speed_hz = s12xftm_measure_cpu_speed(chip);
	if (speed_hz < 0) {
		LOG_ERROR("Failed to measure CPU speed");
		return speed_hz;
	}

	const int fclkdiv = s12xftm_find_clock_divider(speed_hz);
	if (fclkdiv < 0) {
		LOG_ERROR("No matching flash clock divider found, try adjusting your CPU speed");
		return fclkdiv;
	}

	ret = target_write_u8(target, HCS12_REG_FCLKDIV, (uint8_t)fclkdiv);
	if (ret < 0) {
		LOG_ERROR("Failed to get part id information");
		return ret;
	}

	enum s12xftm_bank_types bank_type = s12xftm_get_bank_type(bank);

	switch (bank_type) {

	case S12XFTM_BANK_PFLASH:
		/* Since we do not have a fixed flash start
		 * address, and only the end of flash is fixed we
		 * expect TCL scripts to pass the end address of the
		 * P-Flash block for when they try to declare
		 * P-flash block Re-assign bank base to reflect the
		 * actual start of the flash */
		bank->base = chip->part->blocks[0]->start;

		bank->size = 0;
		for (i = 0; i < chip->part->blocks_count; i++)
			bank->size += chip->part->blocks[i]->size;

		bank->num_sectors = bank->size / 1024; /* Sector size is
							* always 1024 */

		assert(bank->num_sectors > 0);
		bank->sectors = calloc(bank->num_sectors, sizeof(struct flash_sector));

		uint32_t offset = chip->part->blocks[0]->start;

		for (i = 0; i < (unsigned) bank->num_sectors; i++, offset += 1024) {
			bank->sectors[i].offset		= offset;
			bank->sectors[i].size		= S12XFTM_FLASH_SECTOR_SIZE;
			bank->sectors[i].is_erased	= 0;
			bank->sectors[i].is_protected	= -1;
		}


		bank->bank_number = S12XFTM_BANK_PFLASH;
		chip->bank[bank->bank_number].write = s12xftm_write_pflash;

		ret = s12xftm_protect_check(bank);
		if (ret != ERROR_OK)
			return ret;

		chip->probed = true;

		return ERROR_OK;

	case S12XFTM_BANK_DFLASH:
		bank->bank_number = S12XFTM_BANK_DFLASH;
		LOG_ERROR("D-Flash is not yet supported by this driver");
		return ERROR_FAIL;

	default:
		LOG_ERROR("Unknown flash bank type @ 0x%08" PRIx32, bank->base);
		return ERROR_FAIL;
	}
}

static int s12xftm_auto_probe(struct flash_bank *bank)
{
	struct s12xftm_info *chip = bank->driver_priv;
	if (chip->probed)
		return ERROR_OK;
	return s12xftm_probe(bank);
}

static int s12xftm_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct target *target = bank->target;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	buf[0] = 0;

	return ERROR_OK;
}

COMMAND_HANDLER(s12xftm_handle_mass_erase_or_unsecure_command)
{
	int i;
	struct flash_bank *bank;
	int ret = get_flash_bank_by_num(0, &bank);
	if (ret != ERROR_OK)
		return ret;

	struct flash_bank *bnk = bank;
	while (bnk) {
		for (i = 0; i < bank->num_sectors; i++)
			if (bnk->sectors[i].is_protected) {
				LOG_ERROR("Region @ 0x%08" PRIx32 " is protected, can't do mass erase",
					  bnk->base + bnk->sectors[i].offset);
				return ERROR_FAIL;
			}
		bnk = bnk->next;
	}

	struct s12xftm_info *chip;

	ret = s12xftm_get_probed_chip_if_halted(bank, &chip);
	if (ret != ERROR_OK)
		return ret;

	uint8_t flashcmd;
	if (!strcmp(CMD_NAME, "mass_erase")) {
		flashcmd = S12XFTM_CMD_ERASE_VERIFY_ALL_BLOCKS;
	} else if (!strcmp(CMD_NAME, "unsecure")) {
		flashcmd = S12XFTM_CMD_UNSECURE_FLASH;
	} else {
		LOG_ERROR("Unknown command: %s", CMD_NAME);
		return ERROR_FAIL;
	}

	ret = s12xftm_noarg_cmd(chip, flashcmd);
	if (ret != ERROR_OK) {
		LOG_ERROR("%s command failed", CMD_NAME);
		return ret;
	}

	while (bank) {
		for (i = 0; i < bank->num_sectors; i++)
			bank->sectors[i].is_erased = 1;
		bank = bank->next;
	}

	return ERROR_OK;
}

static const struct command_registration s12xftm_exec_command_handlers[] = {
	{
		.name		= "mass_erase",
		.usage		= "<bank>",
		.handler	= s12xftm_handle_mass_erase_or_unsecure_command,
		.mode		= COMMAND_EXEC,
		.help		= "erase entire device",
	},

	{
		.name		= "unsecure",
		.usage		= "<bank>",
		.handler	= s12xftm_handle_mass_erase_or_unsecure_command,
		.mode		= COMMAND_EXEC,
		.help		= "unsecure the device",
	},
	COMMAND_REGISTRATION_DONE
};
static const struct command_registration s12xftm_command_handlers[] = {
	{
		.name = "s12xftm",
		.mode = COMMAND_ANY,
		.help = "HCS12 flash command group",
		.usage = "",
		.chain = s12xftm_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver s12xftm_flash = {
	.name			= "s12xftm",
	.commands		= s12xftm_command_handlers,
	.flash_bank_command	= s12xftm_flash_bank_command,
	.erase			= s12xftm_erase,
	.protect		= s12xftm_protect,
	.write			= s12xftm_write,
	.read			= default_flash_read,
	.probe			= s12xftm_probe,
	.auto_probe		= s12xftm_auto_probe,
	.erase_check		= default_flash_blank_check,
	.protect_check		= s12xftm_protect_check,
	.info			= s12xftm_info,
};
