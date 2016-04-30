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
#include <target/armv7m.h>

#define SPC_CPU_DATA		0x40004720
#define SPC_SR			0x40004722
#define PANTHER_DEVICE_ID	0x4008001C

#define SPC_KEY1	0xB6
#define SPC_KEY2	0xD3

#define SPC_LOAD_BYTE		0x00
#define SPC_LOAD_MULTI_BYTE	0x01
#define SPC_LOAD_ROW		0x02
#define SPC_READ_BYTE		0x03
#define SPC_READ_MULTI_BYTE	0x04
#define SPC_WRITE_ROW		0x05
#define SPC_WRITE_USER_NVL	0x06
#define SPC_PROGRAM_ROW		0x07
#define SPC_ERASE_SECTOR	0x08
#define SPC_ERASE_ALL		0x09
#define SPC_READ_HIDDEN_ROW	0x0A
#define SPC_PROGRAM_PROTECT_ROW	0x0B
#define SPC_GET_CHECKSUM	0x0C
#define SPC_GET_TEMP		0x0E
#define SPC_READ_VOLATILE_BYTE	0x10

#define SPC_ARRAY_ALL		0x3F
#define SPC_ARRAY_EEPROM	0x40
#define SPC_ARRAY_NVL_USER	0x80
#define SPC_ARRAY_NVL_WO	0xF8

#define SPC_ROW_PROTECTION	0

#define SPC_SR_DATA_READY	(1 << 0)
#define SPC_SR_IDLE		(1 << 1)

#define NVL_3_ECCEN		(1 << 3)

#define ROW_SIZE		256
#define ROWS_PER_SECTOR		64
#define SECTOR_SIZE		(ROWS_PER_SECTOR * ROW_SIZE)
#define BLOCK_SIZE		(256 * ROW_SIZE)
#define SECTORS_PER_BLOCK	(BLOCK_SIZE / SECTOR_SIZE)

#define PART_NUMBER_LEN		(17 + 1)

struct psoc5lp_device {
	uint32_t id;
	unsigned fam;
	unsigned speed_mhz;
	unsigned flash_kb;
	unsigned eeprom_kb;
};

/*
 * Device information collected from datasheets.
 * Different temperature ranges (C/I/Q/A) may share IDs, not differing otherwise.
 */
static const struct psoc5lp_device psoc5lp_devices[] = {
	/* CY8C58LP Family Datasheet */
	{ .id = 0x2E11F069, .fam = 8, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E120069, .fam = 8, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E123069, .fam = 8, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E124069, .fam = 8, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E126069, .fam = 8, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E127069, .fam = 8, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E117069, .fam = 8, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E118069, .fam = 8, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E119069, .fam = 8, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E11C069, .fam = 8, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E114069, .fam = 8, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E115069, .fam = 8, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E116069, .fam = 8, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E160069, .fam = 8, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },
	/*           ''                                                               */
	{ .id = 0x2E161069, .fam = 8, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },
	/*           ''                                                               */
	{ .id = 0x2E1D2069, .fam = 8, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E1D6069, .fam = 8, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },

	/* CY8C56LP Family Datasheet */
	{ .id = 0x2E10A069, .fam = 6, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E10D069, .fam = 6, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E10E069, .fam = 6, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E106069, .fam = 6, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E108069, .fam = 6, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E109069, .fam = 6, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E101069, .fam = 6, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E104069, .fam = 6, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	/*           ''                                                               */
	{ .id = 0x2E105069, .fam = 6, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E128069, .fam = 6, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	/*           ''                                                               */
	{ .id = 0x2E122069, .fam = 6, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E129069, .fam = 6, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E163069, .fam = 6, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E156069, .fam = 6, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E1D3069, .fam = 6, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },

	/* CY8C54LP Family Datasheet */
	{ .id = 0x2E11A069, .fam = 4, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E16A069, .fam = 4, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E12A069, .fam = 4, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E103069, .fam = 4, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E16C069, .fam = 4, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E102069, .fam = 4, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E148069, .fam = 4, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E155069, .fam = 4, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E16B069, .fam = 4, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E12B069, .fam = 4, .speed_mhz = 67, .flash_kb =  32, .eeprom_kb = 2 },
	{ .id = 0x2E168069, .fam = 4, .speed_mhz = 67, .flash_kb =  32, .eeprom_kb = 2 },
	{ .id = 0x2E178069, .fam = 4, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E15D069, .fam = 4, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E1D4069, .fam = 4, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },

	/* CY8C52LP Family Datasheet */
	{ .id = 0x2E11E069, .fam = 2, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E12F069, .fam = 2, .speed_mhz = 67, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E133069, .fam = 2, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E159069, .fam = 2, .speed_mhz = 67, .flash_kb = 128, .eeprom_kb = 2 },
	{ .id = 0x2E11D069, .fam = 2, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E121069, .fam = 2, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E184069, .fam = 2, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E196069, .fam = 2, .speed_mhz = 67, .flash_kb =  64, .eeprom_kb = 2 },
	{ .id = 0x2E132069, .fam = 2, .speed_mhz = 67, .flash_kb =  32, .eeprom_kb = 2 },
	{ .id = 0x2E138069, .fam = 2, .speed_mhz = 67, .flash_kb =  32, .eeprom_kb = 2 },
	{ .id = 0x2E13A069, .fam = 2, .speed_mhz = 67, .flash_kb =  32, .eeprom_kb = 2 },
	{ .id = 0x2E152069, .fam = 2, .speed_mhz = 67, .flash_kb =  32, .eeprom_kb = 2 },
	{ .id = 0x2E15F069, .fam = 2, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E15A069, .fam = 2, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },
	{ .id = 0x2E1D5069, .fam = 2, .speed_mhz = 80, .flash_kb = 256, .eeprom_kb = 2 },
};

static void psoc5lp_get_part_number(const struct psoc5lp_device *dev, char *str)
{
	strcpy(str, "CY8Cabcdefg-LPxxx");

	str[4] = '5';
	str[5] = '0' + dev->fam;

	switch (dev->speed_mhz) {
	case 67:
		str[6] = '6';
		break;
	case 80:
		str[6] = '8';
		break;
	default:
		str[6] = '?';
	}

	switch (dev->flash_kb) {
	case 32:
		str[7] = '5';
		break;
	case 64:
		str[7] = '6';
		break;
	case 128:
		str[7] = '7';
		break;
	case 256:
		str[7] = '8';
		break;
	default:
		str[7] = '?';
	}

	/* Package does not matter. */
	strncpy(str + 8, "xx", 2);

	/* Temperate range cannot uniquely be identified. */
	str[10] = 'x';
}

static int psoc5lp_get_device_id(struct target *target, uint32_t *id)
{
	int retval;

	retval = target_read_u32(target, PANTHER_DEVICE_ID, id); /* dummy read */
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u32(target, PANTHER_DEVICE_ID, id);
	return retval;
}

static int psoc5lp_find_device(struct target *target,
	const struct psoc5lp_device **device)
{
	uint32_t device_id;
	unsigned i;
	int retval;

	*device = NULL;

	retval = psoc5lp_get_device_id(target, &device_id);
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("PANTHER_DEVICE_ID = 0x%08" PRIX32, device_id);

	for (i = 0; i < ARRAY_SIZE(psoc5lp_devices); i++) {
		if (psoc5lp_devices[i].id == device_id) {
			*device = &psoc5lp_devices[i];
			return ERROR_OK;
		}
	}

	LOG_ERROR("Device 0x%08" PRIX32 " not supported", device_id);
	return ERROR_FLASH_OPER_UNSUPPORTED;
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

	retval = target_read_u8(target, SPC_SR, &sr); /* dummy read */
	if (retval != ERROR_OK)
		return retval;

	do {
		retval = target_read_u8(target, SPC_SR, &sr);
		if (retval != ERROR_OK)
			return retval;
	} while (sr != SPC_SR_DATA_READY);

	return ERROR_OK;
}

static int psoc5lp_spc_busy_wait_idle(struct target *target)
{
	uint8_t sr;
	int retval;

	retval = target_read_u8(target, SPC_SR, &sr); /* dummy read */
	if (retval != ERROR_OK)
		return retval;

	do {
		retval = target_read_u8(target, SPC_SR, &sr);
		if (retval != ERROR_OK)
			return retval;
	} while (sr != SPC_SR_IDLE);

	return ERROR_OK;
}

static int psoc5lp_spc_read_byte(struct target *target,
	uint8_t array_id, uint8_t offset, uint8_t *data)
{
	int retval;

	retval = psoc5lp_spc_write_opcode(target, SPC_READ_BYTE);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SPC_CPU_DATA, array_id);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SPC_CPU_DATA, offset);
	if (retval != ERROR_OK)
		return retval;

	retval = psoc5lp_spc_busy_wait_data(target);
	if (retval != ERROR_OK)
		return retval;

	retval = target_read_u8(target, SPC_CPU_DATA, data);
	if (retval != ERROR_OK)
		return retval;

	retval = psoc5lp_spc_busy_wait_idle(target);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int psoc5lp_spc_erase_sector(struct target *target,
	uint8_t array_id, uint8_t row_id)
{
	int retval;

	retval = psoc5lp_spc_write_opcode(target, SPC_ERASE_SECTOR);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SPC_CPU_DATA, array_id);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u8(target, SPC_CPU_DATA, row_id);
	if (retval != ERROR_OK)
		return retval;

	retval = psoc5lp_spc_busy_wait_idle(target);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int psoc5lp_spc_erase_all(struct target *target)
{
	int retval;

	retval = psoc5lp_spc_write_opcode(target, SPC_ERASE_ALL);
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

	for (i = 0; i < ROW_SIZE; i++) {
		retval = target_read_u8(target, SPC_CPU_DATA, &data[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	retval = psoc5lp_spc_busy_wait_idle(target);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int psoc5lp_spc_get_temp(struct target *target, uint8_t samples,
	uint8_t *data)
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

	retval = target_read_u8(target, SPC_CPU_DATA, &data[0]);
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u8(target, SPC_CPU_DATA, &data[1]);
	if (retval != ERROR_OK)
		return retval;

	retval = psoc5lp_spc_busy_wait_idle(target);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

/*
 * Program Flash
 */

struct psoc5lp_flash_bank {
	bool probed;
	const struct psoc5lp_device *device;
	bool ecc_enabled;
};

static int psoc5lp_erase(struct flash_bank *bank, int first, int last)
{
	struct psoc5lp_flash_bank *psoc_bank = bank->driver_priv;
	int i, retval;

	if (!psoc_bank->ecc_enabled)
		return ERROR_FLASH_OPER_UNSUPPORTED;

	for (i = first; i <= last; i++) {
		retval = psoc5lp_spc_erase_sector(bank->target,
				i / SECTORS_PER_BLOCK, i % SECTORS_PER_BLOCK);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

/* Derived from core.c:default_flash_blank_check() */
static int psoc5lp_erase_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	uint32_t blank;
	int i, retval;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	for (i = 0; i < bank->num_sectors; i++) {
		uint32_t address = bank->base + bank->sectors[i].offset;
		uint32_t size = bank->sectors[i].size;

		retval = armv7m_0_blank_check_memory(target, address, size, &blank);
		if (retval != ERROR_OK)
			return retval;

		if (blank == 0x00)
			bank->sectors[i].is_erased = 1;
		else
			bank->sectors[i].is_erased = 0;
	}

	return ERROR_OK;
}

static int psoc5lp_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t byte_count)
{
	struct psoc5lp_flash_bank *psoc_bank = bank->driver_priv;
	struct target *target = bank->target;
	uint8_t temp[2];
	int retval;

	retval = psoc5lp_spc_get_temp(target, 3, temp);
	if (retval != ERROR_OK) {
		LOG_ERROR("Unable to read Die temperature");
		return retval;
	}
	LOG_DEBUG("Get_Temp sign 0x%02" PRIx8 ", magnitude 0x%02" PRIx8,
		temp[0], temp[1]);

	if (!psoc_bank->ecc_enabled)
		return ERROR_FLASH_OPER_UNSUPPORTED;

	return ERROR_FLASH_OPER_UNSUPPORTED; /* TODO */
}

static int psoc5lp_protect_check(struct flash_bank *bank)
{
	uint8_t row_data[ROW_SIZE];
	const unsigned protection_bytes_per_sector = ROWS_PER_SECTOR * 2 / 8;
	unsigned i, j, k, num_sectors;
	int retval;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	for (i = 0; i < DIV_ROUND_UP(bank->size, BLOCK_SIZE); i++) {
		retval = psoc5lp_spc_read_hidden_row(bank->target, i,
				SPC_ROW_PROTECTION, row_data);
		if (retval != ERROR_OK)
			return retval;

		/* Last flash array may have less rows, but in practice full sectors. */
		if (i == bank->size / BLOCK_SIZE)
			num_sectors = (bank->size % BLOCK_SIZE) / SECTOR_SIZE;
		else
			num_sectors = SECTORS_PER_BLOCK;

		for (j = 0; j < num_sectors; j++) {
			struct flash_sector *sector = &bank->sectors[i * SECTORS_PER_BLOCK + j];

			sector->is_protected = 0;
			for (k = protection_bytes_per_sector * j;
			     k < protection_bytes_per_sector * (j + 1); k++) {
				assert(k < protection_bytes_per_sector * SECTORS_PER_BLOCK);
				LOG_DEBUG("row[%u][%02u] = 0x%02" PRIx8, i, k, row_data[k]);
				if (row_data[k] != 0x00) {
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
	char part_number[PART_NUMBER_LEN];
	const char *ecc;

	psoc5lp_get_part_number(psoc_bank->device, part_number);
	ecc = psoc_bank->ecc_enabled ? "ECC enabled" : "ECC disabled";

	snprintf(buf, buf_size, "%s %s", part_number, ecc);

	return ERROR_OK;
}

static int psoc5lp_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct psoc5lp_flash_bank *psoc_bank = bank->driver_priv;
	uint32_t flash_addr = bank->base;
	uint8_t nvl[4], temp[2];
	int i, retval;

	if (psoc_bank->probed)
		return ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = psoc5lp_find_device(target, &psoc_bank->device);
	if (retval != ERROR_OK)
		return retval;

	/* First values read are inaccurate, so do it once now. */
	retval = psoc5lp_spc_get_temp(target, 3, temp);
	if (retval != ERROR_OK) {
		LOG_ERROR("Unable to read Die temperature");
		return retval;
	}

	for (i = 0; i < 4; i++) {
		retval = psoc5lp_spc_read_byte(target, SPC_ARRAY_NVL_USER, i, &nvl[i]);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("NVL[%d] = 0x%02" PRIx8, i, nvl[i]);
	}
	psoc_bank->ecc_enabled = nvl[3] & NVL_3_ECCEN;
	if (!psoc_bank->ecc_enabled)
		LOG_WARNING("Disabled flash ECC mode not supported");

	bank->size = psoc_bank->device->flash_kb * 1024;
	bank->num_sectors = DIV_ROUND_UP(bank->size, SECTOR_SIZE);
	bank->sectors = calloc(bank->num_sectors,
			       sizeof(struct flash_sector));
	for (i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].size = SECTOR_SIZE;
		bank->sectors[i].offset = flash_addr - bank->base;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = -1;

		flash_addr += bank->sectors[i].size;
	}

	bank->default_padded_value = 0x00;

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

COMMAND_HANDLER(psoc5lp_handle_mass_erase_command)
{
	struct flash_bank *bank;
	int retval;

	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (retval != ERROR_OK)
		return retval;

	retval = psoc5lp_spc_erase_all(bank->target);
	if (retval == ERROR_OK)
		command_print(CMD_CTX, "PSoC 5LP erase succeeded");
	else
		command_print(CMD_CTX, "PSoC 5LP erase failed");

	return retval;
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
	{
		.name = "mass_erase",
		.handler = psoc5lp_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Erase all flash data and ECC/configuration bytes, "
			"all flash protection rows, "
			"and all row latches in all flash arrays on the device.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration psoc5lp_command_handlers[] = {
	{
		.name = "psoc5lp",
		.mode = COMMAND_ANY,
		.help = "PSoC 5LP flash command group",
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
	.erase = psoc5lp_erase,
	.erase_check = psoc5lp_erase_check,
	.write = psoc5lp_write,
};
