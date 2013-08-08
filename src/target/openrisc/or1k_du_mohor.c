/***************************************************************************
 *   Copyright (C) 2011 Julius Baxter                                      *
 *   julius@opencores.org                                                  *
 *                                                                         *
 *   Copyright (C) 2013 by Franck Jullien                                  *
 *   elec4fun@gmail.com                                                    *
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

#include "or1k_tap.h"
#include "or1k.h"
#include "or1k_du.h"

#include <target/target.h>
#include <jtag/jtag.h>

/* Mohor SoC debug interface defines */

/* CPU control register bits mask */
#define OR1K_MOHORDBGIF_CPU_CR_STALL			0x2
#define OR1K_MOHORDBGIF_CPU_CR_RESET			0x1

/* Module selection 4-bits */
#define OR1K_MOHORDBGIF_MODULE_WB			0x0
#define OR1K_MOHORDBGIF_MODULE_CPU0			0x1
#define OR1K_MOHORDBGIF_MODULE_CPU1			0x2

/* Wishbone module commands */
#define OR1K_MOHORDBGIF_WB_MODULE_CMD_GO		0x0
#define OR1K_MOHORDBGIF_WB_MODULE_CMD_READ		0x1
#define OR1K_MOHORDBGIF_WB_MODULE_CMD_WRITE		0x2

/* Wishbone bus access command defines */
#define OR1K_MOHORDBGIF_WB_ACC_WRITE8			0x0
#define OR1K_MOHORDBGIF_WB_ACC_WRITE16			0x1
#define OR1K_MOHORDBGIF_WB_ACC_WRITE32			0x2
#define OR1K_MOHORDBGIF_WB_ACC_READ8			0x4
#define OR1K_MOHORDBGIF_WB_ACC_READ16			0x5
#define OR1K_MOHORDBGIF_WB_ACC_READ32			0x6

/* CPU module command defines */
#define OR1K_MOHORDBGIF_CPU_ACC_WRITE			0x2
#define OR1K_MOHORDBGIF_CPU_ACC_READ			0x6

/* CPU module commands */
#define OR1K_MOHORDBGIF_CPU_MODULE_CMD_GO		0x0
#define OR1K_MOHORDBGIF_CPU_MODULE_CMD_READ		0x1
#define OR1K_MOHORDBGIF_CPU_MODULE_CMD_WRITE		0x2
#define OR1K_MOHORDBGIF_CPU_MODULE_CMD_CTRL_READ	0x3
#define OR1K_MOHORDBGIF_CPU_MODULE_CMD_CTRL_WRITE	0x4

/* Module select response status codes */
#define OR1K_MOHORDBGIF_MODULE_SELECT_OK		0x0
#define OR1K_MOHORDBGIF_MODULE_SELECT_CRC_ERROR		0x1
#define OR1K_MOHORDBGIF_MODULE_SELECT_MODULE_NOT_EXIST	0x2

/* Command status codes */
#define OR1K_MOHORDBGIF_CMD_OK				0x0
#define OR1K_MOHORDBGIF_CMD_CRC_ERROR			0x1
#define OR1K_MOHORDBGIF_CMD_WB_ERROR			0x4
#define OR1K_MOHORDBGIF_CMD_OURUN_ERROR			0x8

/* Polynomial for the CRC calculation
 * Yes, it's backwards.  Yes, this is on purpose.
 * The hardware is designed this way to save on logic and routing,
 * and it's really all the same to us here.
 */
#define OR1K_JTAG_MOHOR_DBG_CRC_POLY			0xedb88320

static const char * const chain_name[] = {"WISHBONE", "CPU0", "CPU1", "JSP"};

static int or1k_mohor_jtag_init(struct or1k_jtag *jtag_info)
{
	int retval;
	struct or1k_tap_ip *tap_ip = jtag_info->tap_ip;

	retval = tap_ip->init(jtag_info);
	if (retval != ERROR_OK)
		return retval;

	/* TAP should now be configured to communicate with debug interface */
	jtag_info->or1k_jtag_inited = 1;

	/* TAP reset - not sure what state debug module chain is in now */
	jtag_info->or1k_jtag_module_selected = -1;

	LOG_DEBUG("Init done");

	return ERROR_OK;
}

static uint32_t mohor_compute_crc(uint32_t crc_in, uint32_t data_in, int length_bits)
{
	int i;
	unsigned int d, c;
	uint32_t crc_out = crc_in;

	for (i = 0; i < length_bits; i++) {
		d = ((data_in >> i) & 0x1) ? 0xffffffff : 0;
		c = (crc_out & 0x1) ? 0xffffffff : 0;
		crc_out = crc_out >> 1;
		crc_out = crc_out ^ ((d ^ c) & OR1K_JTAG_MOHOR_DBG_CRC_POLY);
	}

	return crc_out;
}

static int or1k_jtag_mohor_debug_select_module(struct or1k_jtag *jtag_info,
					uint32_t module)
{
	struct jtag_tap *tap;
	struct or1k_tap_ip *tap_ip = jtag_info->tap_ip;
	struct scan_field fields[5];
	int extra_bit_fix = 0;
	int retval;
	uint32_t out_module_select_bit;
	uint32_t out_module;
	uint32_t out_crc;
	uint32_t in_crc;
	uint32_t expected_in_crc;
	uint32_t in_status;

	tap = jtag_info->tap;
	if (tap == NULL)
		return ERROR_FAIL;

	/*
	 * CPU control register write
	 * Send:
	 * {1,4'moduleID,32'CRC,36'x           }
	 * Receive:
	 * {37'x               ,4'status,32'CRC}
	 */

	/* 1st bit is module select, set to '1' */
	out_module_select_bit = 1;

	fields[0].num_bits = 1;
	fields[0].out_value = (uint8_t *)&out_module_select_bit;
	fields[0].in_value = NULL;

	/* Module number */
	out_module = flip_u32(module, 4);
	fields[1].num_bits = 4;
	fields[1].out_value = (uint8_t *)&out_module;
	fields[1].in_value = NULL;

	/* CRC calculations */
	out_crc = 0xffffffff;
	out_crc = mohor_compute_crc(out_crc, out_module_select_bit, 1);
	out_crc = mohor_compute_crc(out_crc, out_module, 4);

	/* CRC going out */
	fields[2].num_bits = 32;
	fields[2].out_value = (uint8_t *)&out_crc;
	fields[2].in_value = NULL;

	/* When used with virtual jtag tap, add one extra bit while reading
	 * to compensate for TDO extra registering in mohor du. */
	if (!strcmp(tap_ip->name, "vjtag"))
		extra_bit_fix = 1;

	/* Status coming in */
	fields[3].num_bits = 4 + extra_bit_fix;
	fields[3].out_value = NULL;
	fields[3].in_value = (uint8_t *)&in_status;

	/* CRC coming in */
	fields[4].num_bits = 32;
	fields[4].out_value = NULL;
	fields[4].in_value = (uint8_t *)&in_crc;

	LOG_DEBUG("Setting mohor debug IF module: %s", chain_name[module]);

	jtag_add_dr_scan(tap, 5, fields, TAP_IDLE);

	retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("Performing module change failed");
		return retval;
	}

	/* Remove extra bit fix */
	if (!strcmp(tap_ip->name, "vjtag"))
		in_status >>= 1;

	/* Calculate expected CRC for status */
	expected_in_crc = 0xffffffff;
	expected_in_crc = mohor_compute_crc(expected_in_crc, in_status, 4);

	if (in_crc != expected_in_crc) {
		LOG_ERROR("Received CRC (0x%08x) not same as calculated CRC (0x%08x)"
			  , in_crc, expected_in_crc);
		return ERROR_FAIL;
	}

	if (in_status & OR1K_MOHORDBGIF_MODULE_SELECT_CRC_ERROR) {
		LOG_ERROR("Debug IF module select status: CRC error");
		return ERROR_FAIL;
	} else if (in_status & OR1K_MOHORDBGIF_MODULE_SELECT_MODULE_NOT_EXIST) {
		LOG_ERROR("Debug IF module select status: Invalid module %s",
			  chain_name[module]);
		return ERROR_FAIL;
	} else if ((in_status & 0xf) == OR1K_MOHORDBGIF_MODULE_SELECT_OK) {
		LOG_DEBUG("Setting mohor debug IF OK");
		jtag_info->or1k_jtag_module_selected = module;
	} else {
		LOG_ERROR("Debug IF module select status: Unknown status (%x)",
			  in_status & 0xf);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int or1k_jtag_mohor_debug_set_command(struct or1k_jtag *jtag_info,
				       uint32_t out_accesstype,
				       uint32_t out_address,
				       uint32_t out_length_bytes)
{
	struct jtag_tap *tap;
	struct or1k_tap_ip *tap_ip = jtag_info->tap_ip;
	struct scan_field fields[8];
	int extra_bit_fix = 0;
	int retval;
	uint32_t out_module_select_bit;
	uint32_t out_cmd;
	uint32_t out_crc;
	uint32_t in_status;
	uint32_t in_crc;
	uint32_t expected_in_crc;

	LOG_DEBUG("Setting mohor debug command. TYPE:0x%01x ADR:0x%08x LEN:%d",
		  out_accesstype, out_address, out_length_bytes);

	tap = jtag_info->tap;
	if (tap == NULL)
		return ERROR_FAIL;

	/*
	 * Command register write
	 * Send:
	 * {1'0, 4'writecmd,4'type,32'address,16'len,32'crc,36'x
	 * Receive:
	 * {89'x                                           ,4'status, 32'CRC}
	 */

	/* 1st bit is module select, set to '0', we're not selecting a module */
	out_module_select_bit = 0;
	fields[0].num_bits = 1;
	fields[0].out_value = (uint8_t *)&out_module_select_bit;
	fields[0].in_value = NULL;

	/* Instruction: write command register, 4-bits */
	out_cmd = flip_u32(OR1K_MOHORDBGIF_CPU_MODULE_CMD_WRITE, 4);
	fields[1].num_bits = 4;
	fields[1].out_value = (uint8_t *)&out_cmd;
	fields[1].in_value = NULL;

	/* 4-bit access type */
	out_accesstype = flip_u32(out_accesstype, 4);
	fields[2].num_bits = 4;
	fields[2].out_value = (uint8_t *)&out_accesstype;
	fields[2].in_value = NULL;

	/*32-bit address */
	out_address = flip_u32(out_address, 32);
	fields[3].num_bits = 32;
	fields[3].out_value = (uint8_t *)&out_address;
	fields[3].in_value = NULL;

	/*16-bit length */
	/* Subtract 1 off it, as module does length + 1 accesses */
	out_length_bytes--;
	out_length_bytes = flip_u32(out_length_bytes, 16);
	fields[4].num_bits = 16;
	fields[4].out_value = (uint8_t *)&out_length_bytes;
	fields[4].in_value = NULL;

	/* CRC calculations */
	out_crc = 0xffffffff;
	out_crc = mohor_compute_crc(out_crc, out_module_select_bit, 1);
	out_crc = mohor_compute_crc(out_crc, out_cmd, 4);
	out_crc = mohor_compute_crc(out_crc, out_accesstype, 4);
	out_crc = mohor_compute_crc(out_crc, out_address, 32);
	out_crc = mohor_compute_crc(out_crc, out_length_bytes, 16);

	/* CRC going out */
	fields[5].num_bits = 32;
	fields[5].out_value = (uint8_t *)&out_crc;
	fields[5].in_value = NULL;

	/* When used with virtual jtag tap, add one extra bit while reading
	 * to compensate for TDO extra registering in mohor du. */
	if (!strcmp(tap_ip->name, "vjtag"))
		extra_bit_fix = 1;

	/* Status coming in */
	fields[6].num_bits = 4 + extra_bit_fix;
	fields[6].out_value = NULL;
	fields[6].in_value = (uint8_t *)&in_status;

	/* CRC coming in */
	fields[7].num_bits = 32;
	fields[7].out_value = NULL;
	fields[7].in_value = (uint8_t *)&in_crc;

	jtag_add_dr_scan(tap, 8, fields, TAP_IDLE);

	retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR(" performing CPU CR write failed");
		return retval;
	}

	/* Remove extra bit fix */
	if (!strcmp(tap_ip->name, "vjtag"))
		in_status >>= 1;

	/* Calculate expected CRC for status */
	expected_in_crc = 0xffffffff;
	expected_in_crc = mohor_compute_crc(expected_in_crc, in_status, 4);

	if (in_crc != expected_in_crc) {
		LOG_ERROR("Received CRC (0x%08x) not same as calculated CRC (0x%08x)"
			  , in_crc, expected_in_crc);
		return ERROR_FAIL;
	}

	if (in_status & OR1K_MOHORDBGIF_CMD_CRC_ERROR) {
		LOG_ERROR("Debug IF CPU command write status: CRC error"
			  );
		return ERROR_FAIL;
	} else if ((in_status & 0xf) == OR1K_MOHORDBGIF_CMD_OK) {
		/*LOG_DEBUG(" debug IF command write OK");*/
	} else {
		LOG_ERROR("Debug IF command write: Unknown status (%d)"
			  , in_status);
		return ERROR_FAIL;
	}

	return ERROR_OK;

}

static int or1k_jtag_mohor_debug_read_go(struct or1k_jtag *jtag_info,
					   int type_size_bytes, int length,
					   uint8_t *data)
{
	struct jtag_tap *tap;
	struct or1k_tap_ip *tap_ip = jtag_info->tap_ip;
	struct scan_field *fields;
	int num_bytes = length * type_size_bytes;
	int num_32bit_fields;
	int spare_bytes;
	int num_data_fields;
	int i;
	int extra_bit_fix = 0;
	int retval;
	uint32_t out_module_select_bit;
	uint32_t out_cmd;
	uint32_t out_crc;
	uint32_t in_status;
	uint32_t in_crc;
	uint32_t expected_in_crc;

	LOG_DEBUG("Doing mohor debug read go for %d bytes", (type_size_bytes *
							    length));

	tap = jtag_info->tap;
	if (tap == NULL)
		return ERROR_FAIL;

	/*
	 * Debug GO
	 * Send:
	 * {1'0, 4'gocmd,32'crc, ((len-1)*8)+4+32'x                 }
	 * Receive:
	 * {37'x               , ((len-1)*8)'data, 4'status, 32'crc }
	 */

	num_32bit_fields = num_bytes / 4;
	spare_bytes = num_bytes % 4;
	num_data_fields = num_32bit_fields + !!spare_bytes;

	fields = malloc((num_data_fields + 6) * sizeof(struct scan_field));

	/* 1st bit is module select, set to '0', we're not selecting a module */
	out_module_select_bit = 0;

	fields[0].num_bits = 1;
	fields[0].out_value = (uint8_t *)&out_module_select_bit;
	fields[0].in_value = NULL;

	/* Instruction: go command , 4-bits */
	out_cmd = flip_u32(OR1K_MOHORDBGIF_CPU_MODULE_CMD_GO, 4);
	fields[1].num_bits = 4;
	fields[1].out_value = (uint8_t *)&out_cmd;
	fields[1].in_value = NULL;

	/* CRC calculations */
	out_crc = 0xffffffff;
	out_crc = mohor_compute_crc(out_crc, out_module_select_bit, 1);
	out_crc = mohor_compute_crc(out_crc, out_cmd, 4);

	/* CRC going out */
	fields[2].num_bits = 32;
	fields[2].out_value = (uint8_t *)&out_crc;
	fields[2].in_value = NULL;

	/* When used with virtual jtag tap, add one extra bit while reading
	 * to compensate for TDO extra registering in mohor du. */
	if (!strcmp(tap_ip->name, "vjtag"))
		extra_bit_fix = 1;

	fields[3].num_bits = extra_bit_fix;
	fields[3].out_value = NULL;
	fields[3].in_value = NULL;

	for (i = 0; i < num_32bit_fields; i++) {
		fields[4 + i].num_bits = 32;
		fields[4 + i].out_value = NULL;
		fields[4 + i].in_value = &data[i * 4];
	}

	if (spare_bytes) {
		fields[4 + i].num_bits = spare_bytes * 8;
		fields[4 + i].out_value = &data[i * 4];
		fields[4 + i].in_value = NULL;
	}

	/* Status coming in */
	fields[4 + num_data_fields].num_bits = 4;
	fields[4 + num_data_fields].out_value = NULL;
	fields[4 + num_data_fields].in_value = (uint8_t *)&in_status;

	/* CRC coming in */
	fields[4 + num_data_fields + 1].num_bits = 32;
	fields[4 + num_data_fields + 1].out_value = NULL;
	fields[4 + num_data_fields + 1].in_value = (uint8_t *)&in_crc;

	jtag_add_dr_scan(tap, num_data_fields + 6, fields, TAP_IDLE);

	retval = jtag_execute_queue();

	free(fields);

	if (retval != ERROR_OK) {
		LOG_ERROR("Performing GO command failed");
		return retval;
	}

	/* Calculate expected CRC for data and status */
	expected_in_crc = 0xffffffff;
	for (i = 0; i < num_bytes; i++) {
		expected_in_crc = mohor_compute_crc(expected_in_crc, data[i], 8);
		data[i] = flip_u32((uint32_t)data[i], 8);
	}

	expected_in_crc = mohor_compute_crc(expected_in_crc, in_status, 4);

	if (in_crc != expected_in_crc) {
		LOG_ERROR("Received CRC (0x%08x) not same as calculated CRC (0x%08x)"
			  , in_crc, expected_in_crc);
		return ERROR_FAIL;
	}

	if (in_status & OR1K_MOHORDBGIF_CMD_CRC_ERROR) {
		LOG_ERROR("Debug IF go command status: CRC error");
		return ERROR_FAIL;
	} else if (in_status & OR1K_MOHORDBGIF_CMD_WB_ERROR) {
		LOG_ERROR("Debug IF go command status: Wishbone error");
		return ERROR_FAIL;
	} else if (in_status & OR1K_MOHORDBGIF_CMD_OURUN_ERROR) {
		/*LOG_ERROR("Debug IF go command status: Overrun/underrun error");*/
	} else if ((in_status & 0xf) == OR1K_MOHORDBGIF_CMD_OK) {
		/*LOG_DEBUG(" debug IF go command OK");*/
	} else {
		LOG_ERROR("Debug IF go command: Unknown status (%d)",
			  in_status);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

int or1k_jtag_mohor_debug_write_go(struct or1k_jtag *jtag_info,
				  int type_size_bytes,
				  int length,
				  uint8_t *data)
{
	struct jtag_tap *tap;
	struct or1k_tap_ip *tap_ip = jtag_info->tap_ip;
	struct scan_field *fields;
	int num_bytes = length * type_size_bytes;
	int num_32bit_fields;
	int spare_bytes;
	int num_data_fields;
	int i;
	int extra_bit_fix = 0;
	int retval;
	uint32_t out_module_select_bit;
	uint32_t out_cmd;
	uint32_t out_crc;
	uint32_t in_status;
	uint32_t in_crc;
	uint32_t expected_in_crc;

	LOG_DEBUG("Doing mohor debug write go for %d bytes", (type_size_bytes *
							    length));

	tap = jtag_info->tap;
	if (tap == NULL)
		return ERROR_FAIL;

	/*
	 * Debug GO for write
	 * Send:
	 * {1'0, 4'gocmd,((len-1)*8)'data,32'crc, 4+32'x           }
	 * Receive:
	 * {37+((len-1)*8)'x                    , 4'status, 32'crc }
	 */

	num_32bit_fields = num_bytes / 4;
	spare_bytes = num_bytes % 4;
	num_data_fields = num_32bit_fields + !!spare_bytes;

	fields = malloc((num_data_fields + 3) * sizeof(struct scan_field));

	/* 1st bit is module select, set to '0', we're not selecting a module */
	out_module_select_bit = 0;

	fields[0].num_bits = 1;
	fields[0].out_value = (uint8_t *)&out_module_select_bit;
	fields[0].in_value = NULL;

	/* Instruction: go command , 4-bits */
	out_cmd = flip_u32(OR1K_MOHORDBGIF_CPU_MODULE_CMD_GO, 4);
	fields[1].num_bits = 4;
	fields[1].out_value = (uint8_t *)&out_cmd;
	fields[1].in_value = NULL;

	for (i = 0; i < num_32bit_fields; i++) {
		fields[2 + i].num_bits = 32;
		fields[2 + i].out_value = &data[i * 4];
		fields[2 + i].in_value = NULL;
	}

	if (spare_bytes) {
		fields[2 + i].num_bits = spare_bytes * 8;
		fields[2 + i].out_value = &data[i * 4];
		fields[2 + i].in_value = NULL;
	}

	/* CRC calculations */
	out_crc = 0xffffffff;
	out_crc = mohor_compute_crc(out_crc, out_module_select_bit, 1);
	out_crc = mohor_compute_crc(out_crc, out_cmd, 4);

	for (i = 0; i < num_bytes; i++) {
		data[i] = flip_u32((uint32_t)data[i], 8);
		out_crc = mohor_compute_crc(out_crc, data[i], 8);
	}

	/* CRC going out */
	fields[2 + num_data_fields].num_bits = 32;
	fields[2 + num_data_fields].out_value = (uint8_t *)&out_crc;
	fields[2 + num_data_fields].in_value = NULL;

	jtag_add_dr_scan(tap, num_data_fields + 3, fields, TAP_DRSHIFT);

	/* When used with virtual jtag tap, add one extra bit while reading
	 * to compensate for TDO extra registering in mohor du. */
	if (!strcmp(tap_ip->name, "vjtag"))
		extra_bit_fix = 1;

	/* Status coming in */
	fields[0].num_bits = 4 + extra_bit_fix;
	fields[0].out_value = NULL;
	fields[0].in_value = (uint8_t *)&in_status;

	/* CRC coming in */
	fields[1].num_bits = 32;
	fields[1].out_value = NULL;
	fields[1].in_value = (uint8_t *)&in_crc;

	jtag_add_dr_scan(tap, 2, fields, TAP_IDLE);

	retval = jtag_execute_queue();

	free(fields);

	if (retval != ERROR_OK) {
		LOG_ERROR("Performing GO command failed");
		return retval;
	}

	/* Remove extra bit fix */
	if (!strcmp(tap_ip->name, "vjtag"))
		in_status >>= 1;

	/* Calculate expected CRC for data and status */
	expected_in_crc = 0xffffffff;
	expected_in_crc = mohor_compute_crc(expected_in_crc, in_status, 4);

	/* Check CRCs now */
	if (in_crc != expected_in_crc) {
		LOG_ERROR("Received CRC (0x%08x) not same as calculated CRC (0x%08x)"
			  , in_crc, expected_in_crc);
		return ERROR_FAIL;
	}

	if (in_status & OR1K_MOHORDBGIF_CMD_CRC_ERROR) {
		LOG_ERROR("Debug IF go command status: CRC error");
		return ERROR_FAIL;
	} else if (in_status & OR1K_MOHORDBGIF_CMD_WB_ERROR) {
		LOG_ERROR("Debug IF go command status: Wishbone error");
		return ERROR_FAIL;
	} else if (in_status & OR1K_MOHORDBGIF_CMD_OURUN_ERROR) {
		/*LOG_ERROR("Debug IF go command status: Overrun/underrun error");*/
	} else if ((in_status & 0xf) == OR1K_MOHORDBGIF_CMD_OK) {
		/*LOG_DEBUG(" debug IF go command OK");*/
	} else {
		LOG_ERROR("Debug IF go command: Unknown status (%d)",
			  in_status);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int or1k_mohor_jtag_read_cpu(struct or1k_jtag *jtag_info,
		uint32_t addr, int count, uint32_t *value)
{
	int i;

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	if (jtag_info->or1k_jtag_module_selected != OR1K_MOHORDBGIF_MODULE_CPU0)
		or1k_jtag_mohor_debug_select_module(jtag_info,
						    OR1K_MOHORDBGIF_MODULE_CPU0);

	for (i = 0; i < count; i++) {
		/* Set command register to read a single word */
		if (or1k_jtag_mohor_debug_set_command(jtag_info,
				OR1K_MOHORDBGIF_CPU_ACC_READ,
				addr + i,
				4) != ERROR_OK)
			return ERROR_FAIL;

		if (or1k_jtag_mohor_debug_read_go(jtag_info, 4, 1, (uint8_t *)&value[i]) !=
				ERROR_OK)
			return ERROR_FAIL;

		h_u32_to_be((uint8_t *)&value[i], value[i]);
	}

	return ERROR_OK;
}

static int or1k_mohor_jtag_write_cpu(struct or1k_jtag *jtag_info,
		uint32_t addr, int count, const uint32_t *value)
{
	int i;
	uint32_t value_be;

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	if (jtag_info->or1k_jtag_module_selected != OR1K_MOHORDBGIF_MODULE_CPU0)
		or1k_jtag_mohor_debug_select_module(jtag_info,
						    OR1K_MOHORDBGIF_MODULE_CPU0);

	for (i = 0; i < count; i++) {
		/* Set command register to write a single word */
		or1k_jtag_mohor_debug_set_command(jtag_info,
				OR1K_MOHORDBGIF_CPU_ACC_WRITE,
				addr + i,
				4);

		h_u32_to_be((uint8_t *)&value_be, value[i]);

		if (or1k_jtag_mohor_debug_write_go(jtag_info, 4, 1,
				(uint8_t *)&value_be) != ERROR_OK)
			return ERROR_FAIL;
	}

	return ERROR_OK;

}

static int or1k_mohor_jtag_read_cpu_cr(struct or1k_jtag *jtag_info,
			  uint32_t *value)
{
	struct jtag_tap *tap;
	struct or1k_tap_ip *tap_ip = jtag_info->tap_ip;
	struct scan_field fields[8];
	int extra_bit_fix = 0;
	int retval;
	uint32_t out_module_select_bit;
	uint32_t out_cmd;
	uint32_t out_crc;
	uint32_t in_status;
	uint32_t in_crc;
	uint32_t expected_in_crc;
	uint32_t in_zeroes0;
	uint32_t in_zeroes1;

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	if (jtag_info->or1k_jtag_module_selected != OR1K_MOHORDBGIF_MODULE_CPU0)
		or1k_jtag_mohor_debug_select_module(jtag_info,
						    OR1K_MOHORDBGIF_MODULE_CPU0);
	tap = jtag_info->tap;
	if (tap == NULL)
		return ERROR_FAIL;

	/*
	 * CPU control register write
	 * Send:
	 * {1'0, 4'command, 32'CRC, (52+4+32)'x                             }
	 * Receive:
	 * {37'x                  , 1'reset, 1'stall, 50'x, 4'status, 32'CRC}
	 */

	/* 1st bit is module select, set to '0', we're not selecting a module */
	out_module_select_bit = 0;

	fields[0].num_bits = 1;
	fields[0].out_value = (uint8_t *)&out_module_select_bit;
	fields[0].in_value = NULL;

	/* Command, 4-bits */
	out_cmd = flip_u32(OR1K_MOHORDBGIF_CPU_MODULE_CMD_CTRL_READ, 4);
	fields[1].num_bits = 4;
	fields[1].out_value = (uint8_t *)&out_cmd;
	fields[1].in_value = NULL;

	/* CRC calculations */
	out_crc = 0xffffffff;
	out_crc = mohor_compute_crc(out_crc, out_module_select_bit, 1);
	out_crc = mohor_compute_crc(out_crc, out_cmd, 4);

	/* CRC going out */
	fields[2].num_bits = 32;
	fields[2].out_value = (uint8_t *)&out_crc;
	fields[2].in_value = NULL;

	/* When used with virtual jtag tap, add one extra bit while reading
	 * to compensate for TDO extra registering in mohor du. */
	if (!strcmp(tap_ip->name, "vjtag"))
		extra_bit_fix = 1;

	/* 52-bit control register */
	fields[3].num_bits = 2 + extra_bit_fix;
	fields[3].out_value = NULL;
	fields[3].in_value = (uint8_t *)value;

	/* Assuming the next 50 bits will always be 0 */
	fields[4].num_bits = 32;
	fields[4].out_value = NULL;
	fields[4].in_value = (uint8_t *)&in_zeroes0;

	fields[5].num_bits = 18;
	fields[5].out_value = NULL;
	fields[5].in_value = (uint8_t *)&in_zeroes1;

	/* Status coming in */
	fields[6].num_bits = 4;
	fields[6].out_value = NULL;
	fields[6].in_value = (uint8_t *)&in_status;

	/* CRC coming in */
	fields[7].num_bits = 32;
	fields[7].out_value = NULL;
	fields[7].in_value = (uint8_t *)&in_crc;

	jtag_add_dr_scan(tap, 8, fields, TAP_IDLE);

	retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("Performing CPU CR read failed");
		return retval;
	}

	/* Remove extra bit fix */
	if (!strcmp(tap_ip->name, "vjtag"))
		*value >>= 1;

	/* Calculate expected CRC for status */
	expected_in_crc = 0xffffffff;
	expected_in_crc = mohor_compute_crc(expected_in_crc, *value, 2);
	expected_in_crc = mohor_compute_crc(expected_in_crc, in_zeroes0, 32);
	expected_in_crc = mohor_compute_crc(expected_in_crc, in_zeroes1, 18);
	expected_in_crc = mohor_compute_crc(expected_in_crc, in_status, 4);

	if (in_crc != expected_in_crc) {
		LOG_ERROR("Received CRC (0x%08x) not same as calculated CRC (0x%08x)"
			  , in_crc, expected_in_crc);
		return ERROR_FAIL;
	}

	if (in_status & OR1K_MOHORDBGIF_CMD_CRC_ERROR) {
		LOG_ERROR("Debug IF CPU CR read status: CRC error");
		return ERROR_FAIL;
	} else if ((in_status & 0xf) == OR1K_MOHORDBGIF_CMD_OK) {
		/*LOG_DEBUG(" debug IF CPU CR read OK");*/
	} else {
		LOG_ERROR("Debug IF CPU CR read: Unknown status (%d)"
			  , in_status);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int or1k_mohor_jtag_write_cpu_cr(struct or1k_jtag *jtag_info, uint32_t *value)
{
	struct jtag_tap *tap;
	struct or1k_tap_ip *tap_ip = jtag_info->tap_ip;
	struct scan_field fields[8];
	int extra_bit_fix = 0;
	int retval;
	uint32_t out_module_select_bit;
	uint32_t out_cmd;
	uint32_t out_crc;
	uint32_t in_status;
	uint32_t in_crc;
	uint32_t expected_in_crc;

	LOG_DEBUG("Writing CPU control 0x%08x, reset: %d, stall: %d",
		  *value, !!(*value & OR1K_MOHORDBGIF_CPU_CR_RESET),
		  !!(*value & OR1K_MOHORDBGIF_CPU_CR_STALL));

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	if (jtag_info->or1k_jtag_module_selected != OR1K_MOHORDBGIF_MODULE_CPU0)
		or1k_jtag_mohor_debug_select_module(jtag_info,
						    OR1K_MOHORDBGIF_MODULE_CPU0);

	tap = jtag_info->tap;
	if (tap == NULL)
		return ERROR_FAIL;

	/*
	 * CPU control register write
	 * Send:
	 * {1'0, 4'command, 1'reset, 1'stall, 50'0, 32'CRC, 36'x            }
	 * Receive:
	 * {89'x                                          , 4'status, 32'CRC}
	 */

	/* 1st bit is module select, set to '0', we're not selecting a module */
	out_module_select_bit = 0;

	fields[0].num_bits = 1;
	fields[0].out_value = (uint8_t *)&out_module_select_bit;
	fields[0].in_value = NULL;

	/* Command, 4-bits */
	out_cmd = flip_u32(OR1K_MOHORDBGIF_CPU_MODULE_CMD_CTRL_WRITE, 4);
	fields[1].num_bits = 4;
	fields[1].out_value = (uint8_t *)&out_cmd;
	fields[1].in_value = NULL;

	/* 52-bit control register */
	fields[2].num_bits = 2;
	fields[2].out_value = (uint8_t *)value;
	fields[2].in_value = NULL;

	fields[3].num_bits = 32;
	fields[3].out_value = NULL;
	fields[3].in_value = NULL;

	fields[4].num_bits = 18;
	fields[4].out_value = NULL;
	fields[4].in_value = NULL;

	/* CRC calculations */
	out_crc = 0xffffffff;
	out_crc = mohor_compute_crc(out_crc, out_module_select_bit, 1);
	out_crc = mohor_compute_crc(out_crc, out_cmd, 4);
	out_crc = mohor_compute_crc(out_crc, *value, 2);
	out_crc = mohor_compute_crc(out_crc, 0, 50);

	/* CRC going out */
	fields[5].num_bits = 32;
	fields[5].out_value = (uint8_t *)&out_crc;
	fields[5].in_value = NULL;

	/* When used with virtual jtag tap, add one extra bit while reading
	 * to compensate for TDO extra registering in mohor du. */
	if (!strcmp(tap_ip->name, "vjtag"))
		extra_bit_fix = 1;

	/* Status coming in */
	fields[6].num_bits = 4 + extra_bit_fix;
	fields[6].out_value = NULL;
	fields[6].in_value = (uint8_t *)&in_status;

	/* CRC coming in */
	fields[7].num_bits = 32;
	fields[7].out_value = NULL;
	fields[7].in_value = (uint8_t *)&in_crc;

	jtag_add_dr_scan(tap, 8, fields, TAP_IDLE);

	retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("Performing CPU CR write failed");
		return retval;
	}

	/* Remove extra bit fix */
	if (!strcmp(tap_ip->name, "vjtag"))
		in_status >>= 1;

	/* Calculate expected CRC for status */
	expected_in_crc = 0xffffffff;
	expected_in_crc = mohor_compute_crc(expected_in_crc, in_status, 4);

	/* Check CRCs now */
	if (in_crc != expected_in_crc) {
		LOG_ERROR("Received CRC (0x%08x) not same as calculated CRC (0x%08x)"
			  , in_crc, expected_in_crc);
		return ERROR_FAIL;
	}

	if (in_status & OR1K_MOHORDBGIF_CMD_CRC_ERROR) {
		LOG_ERROR("Debug IF CPU CR write status: CRC error");
		return ERROR_FAIL;
	} else if ((in_status & 0xf) == OR1K_MOHORDBGIF_CMD_OK) {
		LOG_DEBUG("Debug IF CPU CR write OK");
	} else {
		LOG_ERROR("Debug IF module select status: Unknown status (%d)"
			  , in_status);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int or1k_mohor_cpu_stall(struct or1k_jtag *jtag_info, int action)
{
	uint32_t cpu_cr;
	int retval;

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	retval = or1k_mohor_jtag_read_cpu_cr(jtag_info, &cpu_cr);
	if (retval != ERROR_OK)
		return retval;

	if (action == CPU_STALL)
		cpu_cr |= OR1K_MOHORDBGIF_CPU_CR_STALL;
	else
		cpu_cr &= ~OR1K_MOHORDBGIF_CPU_CR_STALL;

	retval = or1k_mohor_jtag_write_cpu_cr(jtag_info, &cpu_cr);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int or1k_mohor_is_cpu_running(struct or1k_jtag *jtag_info, int *running)
{
	uint32_t cpu_cr;
	int retval;

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	retval = or1k_mohor_jtag_read_cpu_cr(jtag_info, &cpu_cr);
	if (retval != ERROR_OK)
		return retval;

	if (cpu_cr & OR1K_MOHORDBGIF_CPU_CR_STALL)
		*running = 0;
	else
		*running = 1;

	return ERROR_OK;
}

static int or1k_mohor_cpu_reset(struct or1k_jtag *jtag_info, int action)
{
	uint32_t cpu_cr;
	int retval;

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	retval = or1k_mohor_jtag_read_cpu_cr(jtag_info, &cpu_cr);
	if (retval != ERROR_OK)
		return retval;

	if (action == CPU_STALL)
		cpu_cr |= OR1K_MOHORDBGIF_CPU_CR_RESET;
	else
		cpu_cr &= ~OR1K_MOHORDBGIF_CPU_CR_RESET;

	retval = or1k_mohor_jtag_write_cpu_cr(jtag_info, &cpu_cr);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int or1k_mohor_jtag_read_memory32(struct or1k_jtag *jtag_info,
			    uint32_t addr, int count, uint32_t *buffer)
{
	int i;
	int retval;

	LOG_DEBUG("Reading WB32 at 0x%08x", addr);

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	if (jtag_info->or1k_jtag_module_selected != OR1K_MOHORDBGIF_MODULE_WB) {
		retval = or1k_jtag_mohor_debug_select_module(jtag_info,
						    OR1K_MOHORDBGIF_MODULE_WB);
		if (retval != ERROR_OK)
			return retval;
	}

	/* At the moment, old Mohor can't read multiple bytes */
	for (i = 0; i < count; i++) {
		/* Set command register to read a single word */
		retval = or1k_jtag_mohor_debug_set_command(jtag_info,
							   OR1K_MOHORDBGIF_WB_ACC_READ32,
							   addr + 4 * i, 4);
		if (retval != ERROR_OK)
			return retval;

		retval = or1k_jtag_mohor_debug_read_go(jtag_info, 4, 1, (uint8_t *)&buffer[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int or1k_mohor_jtag_read_memory16(struct or1k_jtag *jtag_info,
			    uint32_t addr, int count, uint16_t *buffer)
{
	int i;
	int retval;

	LOG_DEBUG("Reading WB16 at 0x%08x", addr);

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	if (jtag_info->or1k_jtag_module_selected != OR1K_MOHORDBGIF_MODULE_WB) {
		retval = or1k_jtag_mohor_debug_select_module(jtag_info,
						    OR1K_MOHORDBGIF_MODULE_WB);
		if (retval != ERROR_OK)
			return retval;
	}

	/* At the moment, old Mohor can't read multiple bytes */
	for (i = 0; i < count; i++) {
		/* Set command register to read a single word */
		retval = or1k_jtag_mohor_debug_set_command(jtag_info,
							   OR1K_MOHORDBGIF_WB_ACC_READ16,
							   addr + 2 * i, 2);
		if (retval != ERROR_OK)
			return retval;

		retval = or1k_jtag_mohor_debug_read_go(jtag_info, 2, 1, (uint8_t *)&buffer[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int or1k_mohor_jtag_read_memory8(struct or1k_jtag *jtag_info,
			   uint32_t addr, int count, uint8_t *buffer)
{
	int i;
	int retval;

	LOG_DEBUG("Reading WB8 at 0x%08x", addr);

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	if (jtag_info->or1k_jtag_module_selected != OR1K_MOHORDBGIF_MODULE_WB) {
		retval = or1k_jtag_mohor_debug_select_module(jtag_info,
						    OR1K_MOHORDBGIF_MODULE_WB);
		if (retval != ERROR_OK)
			return retval;
	}

	/* At the moment, old Mohor can't read multiple bytes */
	for (i = 0; i < count; i++) {
		/* Set command register to read a single word */
		retval = or1k_jtag_mohor_debug_set_command(jtag_info,
							   OR1K_MOHORDBGIF_WB_ACC_READ8,
							   addr + i, 1);
		if (retval != ERROR_OK)
			return retval;

		retval = or1k_jtag_mohor_debug_read_go(jtag_info, 1, 1, (uint8_t *)&buffer[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int or1k_mohor_jtag_write_memory32(struct or1k_jtag *jtag_info,
			     uint32_t addr, int count, const uint32_t *buffer)
{
	int retval;

	LOG_DEBUG("Writing WB32 at 0x%08x", addr);

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	if (jtag_info->or1k_jtag_module_selected != OR1K_MOHORDBGIF_MODULE_WB)
		or1k_jtag_mohor_debug_select_module(jtag_info,
						    OR1K_MOHORDBGIF_MODULE_WB);

	retval = or1k_jtag_mohor_debug_set_command(jtag_info,
						   OR1K_MOHORDBGIF_WB_ACC_WRITE32,
						   addr, count * 4);
	if (retval != ERROR_OK)
		return retval;

	retval = or1k_jtag_mohor_debug_write_go(jtag_info, 4, count, (uint8_t *)buffer);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;

}

static int or1k_mohor_jtag_write_memory16(struct or1k_jtag *jtag_info,
			     uint32_t addr, int count, const uint16_t *buffer)
{
	int retval;

	LOG_DEBUG("Writing WB16 at 0x%08x", addr);

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	if (jtag_info->or1k_jtag_module_selected != OR1K_MOHORDBGIF_MODULE_WB)
		or1k_jtag_mohor_debug_select_module(jtag_info,
						    OR1K_MOHORDBGIF_MODULE_WB);

	retval = or1k_jtag_mohor_debug_set_command(jtag_info,
						   OR1K_MOHORDBGIF_WB_ACC_WRITE16,
						   addr, count * 2);
	if (retval != ERROR_OK)
		return retval;

	retval = or1k_jtag_mohor_debug_write_go(jtag_info, 2, count, (uint8_t *)buffer);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int or1k_mohor_jtag_write_memory8(struct or1k_jtag *jtag_info,
			    uint32_t addr, int count, const uint8_t *buffer)
{
	int retval;

	LOG_DEBUG("Writing WB8 at 0x%08x", addr);

	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	if (jtag_info->or1k_jtag_module_selected != OR1K_MOHORDBGIF_MODULE_WB)
		or1k_jtag_mohor_debug_select_module(jtag_info,
						    OR1K_MOHORDBGIF_MODULE_WB);

	retval = or1k_jtag_mohor_debug_set_command(jtag_info,
						   OR1K_MOHORDBGIF_WB_ACC_WRITE8,
						   addr, count);
	if (retval != ERROR_OK)
		return retval;

	retval = or1k_jtag_mohor_debug_write_go(jtag_info, 1, count, (uint8_t *)buffer);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static struct or1k_du or1k_du_mohor = {
	.name = "mohor",
	.or1k_jtag_init           = or1k_mohor_jtag_init,

	.or1k_is_cpu_running      = or1k_mohor_is_cpu_running,
	.or1k_cpu_stall           = or1k_mohor_cpu_stall,
	.or1k_cpu_reset           = or1k_mohor_cpu_reset,

	.or1k_jtag_read_cpu       = or1k_mohor_jtag_read_cpu,
	.or1k_jtag_write_cpu      = or1k_mohor_jtag_write_cpu,

	.or1k_jtag_read_memory32  = or1k_mohor_jtag_read_memory32,
	.or1k_jtag_read_memory16  = or1k_mohor_jtag_read_memory16,
	.or1k_jtag_read_memory8   = or1k_mohor_jtag_read_memory8,

	.or1k_jtag_write_memory32 = or1k_mohor_jtag_write_memory32,
	.or1k_jtag_write_memory16 = or1k_mohor_jtag_write_memory16,
	.or1k_jtag_write_memory8  = or1k_mohor_jtag_write_memory8,
};

int or1k_du_mohor_register(void)
{
	list_add_tail(&or1k_du_mohor.list, &du_list);
	return 0;
}

int test_mohor(struct or1k_jtag *jtag_info)
{
	if (!jtag_info->or1k_jtag_inited)
		or1k_mohor_jtag_init(jtag_info);

	or1k_jtag_mohor_debug_select_module(jtag_info, OR1K_MOHORDBGIF_MODULE_CPU0);

	return 0;
}