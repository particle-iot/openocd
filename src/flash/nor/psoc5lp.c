/*
 * PSoC 5LP flash driver
 *
 * Copyright (c) 2016 Andreas Färber
 *
 * License: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

#define FASTCLK_IMO_CR		0x40004200
#define PM_ACT_CFG0		0x400043A0
#define SPC_CPU_DATA		0x40004720
#define SPC_SR			0x40004722
#define PANTHER_DBG_CFG		0x4008000C
#define PANTHER_DEVICE_ID	0x4008001C

#define FASTCLK_IMO_CR_F_RANGE_2	(2 << 0)
#define FASTCLK_IMO_CR_F_RANGE_MASK	(7 << 0)

#define SPC_KEY1	0xB6
#define SPC_KEY2	0xD3

#define SPC_READ_HIDDEN_ROW	0x0A
#define SPC_GET_TEMP		0x0E

#define PANTHER_DBG_CFG_BYPASS		(1 << 1)

#define ROW_SIZE	256
#define SECTOR_SIZE	(64 * ROW_SIZE)
#define BLOCK_SIZE	(256 * ROW_SIZE)

struct psoc5lp_device {
	uint32_t id;
	unsigned fam;
	unsigned speed;
	unsigned flash_kb;
};

struct psoc5lp_flash_bank {
	bool probed;
	const struct psoc5lp_device *device;
};

/* Data collected from subfamily datasheets, ignoring temperature range (I/Q). */
static const struct psoc5lp_device psoc5lp_devices[] = {
	// CY8C58LP Family Datasheet
	{ .id = 0x2E11F069, .fam = 8, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E120069, .fam = 8, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E123069, .fam = 8, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E124069, .fam = 8, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E126069, .fam = 8, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E127069, .fam = 8, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E117069, .fam = 8, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E118069, .fam = 8, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E119069, .fam = 8, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E11C069, .fam = 8, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E114069, .fam = 8, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E115069, .fam = 8, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E116069, .fam = 8, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E160069, .fam = 8, .speed = 80, .flash_kb = 256 }, // I + Q
	{ .id = 0x2E161069, .fam = 8, .speed = 80, .flash_kb = 256 }, // I + Q
	{ .id = 0x2E1D2069, .fam = 8, .speed = 80, .flash_kb = 256 },
	{ .id = 0x2E1D6069, .fam = 8, .speed = 80, .flash_kb = 256 },
	// CY8C56LP Family Datasheet
	{ .id = 0x2E10A069, .fam = 6, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E10D069, .fam = 6, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E10E069, .fam = 6, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E106069, .fam = 6, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E108069, .fam = 6, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E109069, .fam = 6, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E101069, .fam = 6, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E104069, .fam = 6, .speed = 67, .flash_kb =  64 }, // I + Q
	{ .id = 0x2E105069, .fam = 6, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E128069, .fam = 6, .speed = 67, .flash_kb = 128 }, // I + Q
	{ .id = 0x2E122069, .fam = 6, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E129069, .fam = 6, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E163069, .fam = 6, .speed = 80, .flash_kb = 256 },
	{ .id = 0x2E156069, .fam = 6, .speed = 80, .flash_kb = 256 },
	{ .id = 0x2E1D3069, .fam = 6, .speed = 80, .flash_kb = 256 },
	// CY8C54LP Family Datasheet
	{ .id = 0x2E11A069, .fam = 4, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E16A069, .fam = 4, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E12A069, .fam = 4, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E103069, .fam = 4, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E16C069, .fam = 4, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E102069, .fam = 4, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E148069, .fam = 4, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E155069, .fam = 4, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E16B069, .fam = 4, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E12B069, .fam = 4, .speed = 67, .flash_kb =  32 },
	{ .id = 0x2E168069, .fam = 4, .speed = 67, .flash_kb =  32 },
	{ .id = 0x2E178069, .fam = 4, .speed = 80, .flash_kb = 256 },
	{ .id = 0x2E15D069, .fam = 4, .speed = 80, .flash_kb = 256 },
	{ .id = 0x2E1D4069, .fam = 4, .speed = 80, .flash_kb = 256 },
	// CY8C52LP Family Datasheet
	{ .id = 0x2E11E069, .fam = 2, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E12F069, .fam = 2, .speed = 67, .flash_kb = 256 },
	{ .id = 0x2E133069, .fam = 2, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E159069, .fam = 2, .speed = 67, .flash_kb = 128 },
	{ .id = 0x2E11D069, .fam = 2, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E121069, .fam = 2, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E184069, .fam = 2, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E196069, .fam = 2, .speed = 67, .flash_kb =  64 },
	{ .id = 0x2E132069, .fam = 2, .speed = 67, .flash_kb =  32 },
	{ .id = 0x2E138069, .fam = 2, .speed = 67, .flash_kb =  32 },
	{ .id = 0x2E13A069, .fam = 2, .speed = 67, .flash_kb =  32 },
	{ .id = 0x2E152069, .fam = 2, .speed = 67, .flash_kb =  32 },
	{ .id = 0x2E15F069, .fam = 2, .speed = 80, .flash_kb = 256 },
	{ .id = 0x2E15A069, .fam = 2, .speed = 80, .flash_kb = 256 },
	{ .id = 0x2E1D5069, .fam = 2, .speed = 80, .flash_kb = 256 },
};

static int psoc5lp_get_device_id(struct target *target, uint32_t *id)
{
	int retval;

	retval = target_read_u32(target, PANTHER_DEVICE_ID, id); // dummy read
	if (retval != ERROR_OK)
		return retval;

	return target_read_u32(target, PANTHER_DEVICE_ID, id);
}

static int psoc5lp_spc_write_opcode(struct target *target, uint8_t opcode)
{
	int retval;

	retval = target_write_u8(target, SPC_CPU_DATA, SPC_KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SPC_CPU_DATA, SPC_KEY2 + opcode);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SPC_CPU_DATA, opcode);
	return retval;
}

static int psoc5lp_spc_busy_wait_data(struct target *target)
{
	uint8_t sr;
	int retval;

	retval = target_read_u8(target, SPC_SR, &sr); // dummy read
	if (retval != ERROR_OK)
		return retval;
	do {
		retval = target_read_u8(target, SPC_SR, &sr);
		if (retval != ERROR_OK)
			return retval;
	} while (sr != 0x01);

	return ERROR_OK;
}

static int psoc5lp_spc_busy_wait_idle(struct target *target)
{
	uint8_t sr;
	int retval;

	retval = target_read_u8(target, SPC_SR, &sr); // dummy read
	if (retval != ERROR_OK)
		return retval;
	do {
		retval = target_read_u8(target, SPC_SR, &sr);
		if (retval != ERROR_OK)
			return retval;
	} while (sr != 0x02);

	return ERROR_OK;
}

static int psoc5lp_spc_get_temp(struct target *target, uint8_t samples,
	uint8_t *sign, uint8_t *magnitude)
{
	int retval;

	retval = psoc5lp_spc_write_opcode(target, SPC_GET_TEMP);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SPC_CPU_DATA, samples);
	if (retval != ERROR_OK)
		return retval;

	retval = psoc5lp_spc_busy_wait_data(target);
	if (retval != ERROR_OK)
		return retval;

	retval = target_read_u8(target, SPC_CPU_DATA, sign); // dummy read
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u8(target, SPC_CPU_DATA, sign);
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u8(target, SPC_CPU_DATA, magnitude);
	if (retval != ERROR_OK)
		return retval;

	retval = psoc5lp_spc_busy_wait_idle(target);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int psoc5lp_spc_read_hidden_row(struct target *target,
	uint8_t array_id, uint8_t row_id, uint8_t *data)
{
	int i, retval;

	retval = psoc5lp_spc_write_opcode(target, SPC_READ_HIDDEN_ROW);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SPC_CPU_DATA, array_id);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SPC_CPU_DATA, row_id);
	if (retval != ERROR_OK)
		return retval;

	retval = psoc5lp_spc_busy_wait_data(target);
	if (retval != ERROR_OK)
		return retval;

	retval = target_read_u8(target, SPC_CPU_DATA, &data[0]); // dummy read
	if (retval != ERROR_OK)
		return retval;
	for (i = 0; i < 256; i++) {
		retval = target_read_u8(target, SPC_CPU_DATA, &data[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	retval = psoc5lp_spc_busy_wait_idle(target);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

extern struct flash_driver xmc4xxx_flash;

static int psoc5lp_erase_check(struct flash_bank *bank)
{
	return xmc4xxx_flash.erase_check(bank);
}

static int psoc5lp_protect_check(struct flash_bank *bank)
{
	uint8_t row_data[256];
	unsigned i, j, k;
	int retval;

	for (i = 0; i < DIV_ROUND_UP(bank->size, BLOCK_SIZE); i++) {
		retval = psoc5lp_spc_read_hidden_row(bank->target, i, 0, row_data);
		if (retval != ERROR_OK)
			return retval;

		// 4 sectors per block
		for (j = 0; j < 4; j++) {
			struct flash_sector *sector = &bank->sectors[i * 4 + j];

			sector->is_protected = 0;
			for (k = 0; k < 16; k++) {
				if (row_data[j * 16 + k] != 0x00) {
					sector->is_protected = 1;
					break;
				}
			}
		}
	}

	return ERROR_OK;
}

static int psoc5lp_get_info_command(struct flash_bank *bank, char *buf, int buf_size)
{
	struct psoc5lp_flash_bank *psoc_bank = bank->driver_priv;
	const char *flash_capacity, *speed_grade;

	switch (psoc_bank->device->flash_kb) {
	case 32:
		flash_capacity = "5";
		break;
	case 64:
		flash_capacity = "6";
		break;
	case 128:
		flash_capacity = "7";
		break;
	case 256:
		flash_capacity = "8";
		break;
	default:
		flash_capacity = "?";
	}

	switch (psoc_bank->device->speed) {
	case 67:
		speed_grade = "6";
		break;
	case 80:
		speed_grade = "8";
		break;
	default:
		speed_grade = "?";
	}

	snprintf(buf, buf_size, "CY8C5%u%s%sxxx-LPxxx",
		psoc_bank->device->fam,
		speed_grade, flash_capacity);

	return ERROR_OK;
}

static int psoc5lp_probe(struct flash_bank *bank)
{
	struct psoc5lp_flash_bank *psoc_bank = bank->driver_priv;
	uint32_t flash_addr = bank->base;
	uint32_t val, device_id;
	uint8_t temp_sign, temp_magnitude;
	unsigned dev;
	int i, retval;

	if (psoc_bank->probed)
		return ERROR_OK;

	if (bank->target->state != TARGET_HALTED) {
		LOG_WARNING("Cannot communicate... target not halted.");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Configure Target Device (5.2) */
	retval = target_read_u32(bank->target, PANTHER_DBG_CFG, &val);
	if (retval != ERROR_OK)
		return retval;
	val |= PANTHER_DBG_CFG_BYPASS;
	retval = target_write_u32(bank->target, PANTHER_DBG_CFG, val);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(bank->target, PM_ACT_CFG0, 0xBF);
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u32(bank->target, FASTCLK_IMO_CR, &val);
	if (retval != ERROR_OK)
		return retval;
	val &= ~FASTCLK_IMO_CR_F_RANGE_MASK;
	val |= FASTCLK_IMO_CR_F_RANGE_2;
	retval = target_write_u32(bank->target, FASTCLK_IMO_CR, val);
	if (retval != ERROR_OK)
		return retval;

	/* Verify JTAG ID (5.3) */
	retval = psoc5lp_get_device_id(bank->target, &device_id);
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("PANTHER_DEVICE_ID = 0x%08" PRIX32, device_id);

	psoc_bank->device = NULL;
	for (dev = 0; dev < ARRAY_SIZE(psoc5lp_devices); dev++) {
		if (psoc5lp_devices[dev].id == device_id) {
			psoc_bank->device = &psoc5lp_devices[dev];
			break;
		}
	}
	if (!psoc_bank->device) {
		LOG_ERROR("Device 0x%08" PRIX32 " not supported", device_id);
		return ERROR_FLASH_OPER_UNSUPPORTED;
	}

	bank->size = psoc_bank->device->flash_kb * 1024;
	bank->num_sectors = DIV_ROUND_UP(bank->size, SECTOR_SIZE);
	bank->sectors = calloc(bank->num_sectors,
			       sizeof(struct flash_sector));
	for (i = 0; i < bank->num_sectors; i++) {
		if (i == bank->num_sectors - 1)
			bank->sectors[i].size = bank->size - i * SECTOR_SIZE;
		else
			bank->sectors[i].size = SECTOR_SIZE;

		bank->sectors[i].offset = flash_addr - bank->base;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = -1;

		flash_addr += bank->sectors[i].size;
	}

	bank->default_padded_value = 0x00;

	for (i = 0; i < 2; i++) {
		psoc5lp_spc_get_temp(bank->target, 3, &temp_sign, &temp_magnitude);
	}
	LOG_DEBUG("Get_Temp sign 0x%02" PRIx8 ", magnitude 0x%02" PRIx8,
		temp_sign, temp_magnitude);

	psoc_bank->probed = true;

	return ERROR_OK;
}

static int psoc5lp_auto_probe(struct flash_bank *bank)
{
	struct psoc5lp_flash_bank *psoc_bank = bank->driver_priv;

	if (psoc_bank->probed)
		return ERROR_OK;

	return psoc5lp_probe(bank);
}

FLASH_BANK_COMMAND_HANDLER(psoc5lp_flash_bank_command)
{
	struct psoc5lp_flash_bank *psoc_bank;

	psoc_bank = malloc(sizeof(struct psoc5lp_flash_bank));
	if (!psoc_bank)
		return ERROR_FLASH_OPERATION_FAILED;

	psoc_bank->probed = false;
	psoc_bank->device = NULL;

	bank->driver_priv = psoc_bank;

	return ERROR_OK;
}

static const struct command_registration psoc5lp_exec_command_handlers[] = {
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration psoc5lp_command_handlers[] = {
	{
		.name = "psoc5lp",
		.mode = COMMAND_ANY,
		.help = "psoc5lp flash command group",
		.usage = "",
		.chain = psoc5lp_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver psoc5lp_flash = {
	.name = "psoc5lp",
	.commands = psoc5lp_command_handlers,
	.flash_bank_command = psoc5lp_flash_bank_command,
	.info = psoc5lp_get_info_command,
	.probe = psoc5lp_probe,
	.auto_probe = psoc5lp_auto_probe,
	.protect_check = psoc5lp_protect_check,
	.read = default_flash_read,
	.erase_check = psoc5lp_erase_check,
};
