/***************************************************************************
 *   Copyright (C) 2012 Andes technology.                                  *
 *   Hsiangkai Wang <hkwang@andestech.com>                                 *
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

#include <helper/command.h>
#include "nds32.h"
#include "nds32_disassembler.h"

COMMAND_HANDLER(handle_nds32_dssim_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0) {
		if (strcmp(CMD_ARGV[0], "on") == 0)
			nds32->step_isr_enable = true;
		if (strcmp(CMD_ARGV[0], "off") == 0)
			nds32->step_isr_enable = false;
	}

	command_print(CMD_CTX, "$INT_MASK.DSSIM: %d", nds32->step_isr_enable);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_memory_access_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);
	struct aice_port_s *aice = target_to_aice(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0) {

		if (strcmp(CMD_ARGV[0], "bus") == 0)
			nds32->memory.access_channel = AICE_MEMORY_ACC_BUS;
		if (strcmp(CMD_ARGV[0], "cpu") == 0)
			nds32->memory.access_channel = AICE_MEMORY_ACC_CPU;

		aice->port->api->memory_access(nds32->memory.access_channel);
	}

	LOG_DEBUG("memory access channel: %s", AICE_MEMORY_ACCESS_NAME[nds32->memory.access_channel]);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_memory_mode_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);
	struct aice_port_s *aice = target_to_aice(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0) {

		if (nds32->edm.access_control == false) {
			command_print(CMD_CTX, "Target does not support ACC_CTL. Set memory mode to MEMORY");
			nds32->memory.mode = AICE_MEMORY_MODE_MEM;
		} else if (nds32->edm.direct_access_local_memory == false) {
			command_print(CMD_CTX, "Target does not support direct access local memory. Set memory mode to MEMORY");
			nds32->memory.mode = AICE_MEMORY_MODE_MEM;

			/* set to ACC_CTL */
			aice->port->api->memory_mode(nds32->memory.mode);
		} else {
			if (strcmp(CMD_ARGV[0], "auto") == 0) {
				nds32->memory.mode = AICE_MEMORY_MODE_AUTO;
			} else if (strcmp(CMD_ARGV[0], "mem") == 0) {
				nds32->memory.mode = AICE_MEMORY_MODE_MEM;
			} else if (strcmp(CMD_ARGV[0], "ilm") == 0) {
				if (nds32->memory.ilm_base == 0)
					command_print(CMD_CTX, "Target does not support ILM");
				else
					nds32->memory.mode = AICE_MEMORY_MODE_ILM;
			} else if (strcmp(CMD_ARGV[0], "dlm") == 0) {
				if (nds32->memory.dlm_base == 0)
					command_print(CMD_CTX, "Target does not support DLM");
				else
					nds32->memory.mode = AICE_MEMORY_MODE_DLM;
			}

			/* set to ACC_CTL */
			aice->port->api->memory_mode(nds32->memory.mode);
		}
	}

	command_print(CMD_CTX, "memory mode: %s", AICE_MEMORY_MODE_NAME[nds32->memory.mode]);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_cache_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);
	struct aice_port_s *aice = target_to_aice(target);
	struct nds32_cache *icache = &(nds32->memory.icache);
	struct nds32_cache *dcache = &(nds32->memory.dcache);
	int result;

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0) {

		if (strcmp(CMD_ARGV[0], "invalidate") == 0) {
			if ((dcache->line_size != 0) && (dcache->enable == true)) {
				/* D$ write back */
				result = aice->port->api->cache_ctl(AICE_CACHE_CTL_L1D_WBALL, 0);
				if (result != ERROR_OK) {
					command_print(CMD_CTX, "Write back data cache...failed");
					return result;
				}

				command_print(CMD_CTX, "Write back data cache...done");

				/* D$ invalidate */
				result = aice->port->api->cache_ctl(AICE_CACHE_CTL_L1D_INVALALL, 0);
				if (result != ERROR_OK) {
					command_print(CMD_CTX, "Invalidate data cache...failed");
					return result;
				}

				command_print(CMD_CTX, "Invalidate data cache...done");
			} else {
				if (dcache->line_size == 0)
					command_print(CMD_CTX, "No data cache");
				else
					command_print(CMD_CTX, "Data cache disabled");
			}

			if ((icache->line_size != 0) && (icache->enable == true)) {
				/* I$ invalidate */
				result = aice->port->api->cache_ctl(AICE_CACHE_CTL_L1I_INVALALL, 0);
				if (result != ERROR_OK) {
					command_print(CMD_CTX, "Invalidate instruction cache...failed");
					return result;
				}

				command_print(CMD_CTX, "Invalidate instruction cache...done");
			} else {
				if (icache->line_size == 0)
					command_print(CMD_CTX, "No instruction cache");
				else
					command_print(CMD_CTX, "Instruction cache disabled");
			}
		} else
			command_print(CMD_CTX, "No valid parameter");
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_auto_break_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0) {
		if (strcmp(CMD_ARGV[0], "on") == 0)
			nds32->auto_convert_hw_bp = true;
		if (strcmp(CMD_ARGV[0], "off") == 0)
			nds32->auto_convert_hw_bp = false;
	}

	if (nds32->auto_convert_hw_bp)
		command_print(CMD_CTX, "convert sw break to hw break on ROM: on");
	else
		command_print(CMD_CTX, "convert sw break to hw break on ROM: off");

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_virtual_hosting_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0) {
		if (strcmp(CMD_ARGV[0], "on") == 0)
			nds32->virtual_hosting = true;
		if (strcmp(CMD_ARGV[0], "off") == 0)
			nds32->virtual_hosting = false;
	}

	if (nds32->virtual_hosting)
		LOG_INFO("virtual hosting: on");
	else
		LOG_INFO("virtual hosting: off");

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_global_stop_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0) {
		if (strcmp(CMD_ARGV[0], "on") == 0)
			nds32->global_stop = true;
		if (strcmp(CMD_ARGV[0], "off") == 0)
			nds32->global_stop = false;
	}

	if (nds32->global_stop)
		LOG_INFO("global stop: on");
	else
		LOG_INFO("global stop: off");

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_soft_reset_halt_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0) {
		if (strcmp(CMD_ARGV[0], "on") == 0)
			nds32->soft_reset_halt = true;
		if (strcmp(CMD_ARGV[0], "off") == 0)
			nds32->soft_reset_halt = false;
	}

	if (nds32->soft_reset_halt)
		LOG_INFO("soft-reset-halt: on");
	else
		LOG_INFO("soft-reset-halt: off");

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_boot_time_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], nds32->boot_time);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_edm_passcode_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	nds32->edm_passcode = strdup(CMD_ARGV[0]);

	LOG_INFO("set EDM passcode: %s", nds32->edm_passcode);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_max_stop_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);
	struct aice_port_s *aice = target_to_aice(target);
	uint32_t edm_ctl_value;

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0) {
		aice->port->api->read_debug_reg(NDS_EDM_SR_EDM_CTL, &edm_ctl_value);

		if (strcmp(CMD_ARGV[0], "on") == 0)
			edm_ctl_value |= (0x1 << 29);
		if (strcmp(CMD_ARGV[0], "off") == 0)
			edm_ctl_value &= ~(0x1 << 29);

		aice->port->api->write_debug_reg(NDS_EDM_SR_EDM_CTL, edm_ctl_value);
	}

	aice->port->api->read_debug_reg(NDS_EDM_SR_EDM_CTL, &edm_ctl_value);

	if ((edm_ctl_value & (0x1 << 29)) != 0)
		command_print(CMD_CTX, "global stop: on");
	else
		command_print(CMD_CTX, "global stop: off");

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_reset_halt_as_init_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0) {
		if (strcmp(CMD_ARGV[0], "on") == 0)
			nds32->reset_halt_as_examine = true;
		if (strcmp(CMD_ARGV[0], "off") == 0)
			nds32->reset_halt_as_examine = false;
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_decode_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 1) {

		uint32_t addr;
		uint32_t insn_count;
		uint32_t opcode;
		uint32_t read_addr;
		uint32_t i;
		struct nds32_instruction instruction;

		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], addr);
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], insn_count);

		read_addr = addr;
		i = 0;
		while (i < insn_count) {
			if (ERROR_OK != nds32_read_opcode(nds32, read_addr, &opcode))
				return ERROR_FAIL;
			if (ERROR_OK != nds32_evaluate_opcode(nds32, opcode, read_addr, &instruction))
				return ERROR_FAIL;

			command_print(CMD_CTX, "%s", instruction.text);

			read_addr += instruction.instruction_size;
			i++;
		}
	} else if (CMD_ARGC == 1) {

		uint32_t addr;
		uint32_t opcode;
		struct nds32_instruction instruction;

		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], addr);

		if (ERROR_OK != nds32_read_opcode(nds32, addr, &opcode))
			return ERROR_FAIL;
		if (ERROR_OK != nds32_evaluate_opcode(nds32, opcode, addr, &instruction))
			return ERROR_FAIL;

		command_print(CMD_CTX, "%s", instruction.text);
	} else
		return ERROR_FAIL;

	return ERROR_OK;
}

COMMAND_HANDLER(handle_nds32_query_target_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct nds32 *nds32 = target_to_nds32(target);

	if (!is_nds32(nds32)) {
		command_print(CMD_CTX, "current target isn't an Andes core");
		return ERROR_FAIL;
	}

	command_print(CMD_CTX, "OCD");

	return ERROR_OK;
}

static int jim_nds32_bulk_write(Jim_Interp *interp, int argc, Jim_Obj * const *argv)
{
	const char *cmd_name = Jim_GetString(argv[0], NULL);

	Jim_GetOptInfo goi;
	Jim_GetOpt_Setup(&goi, interp, argc - 1, argv + 1);

	if (goi.argc < 3) {
		Jim_SetResultFormatted(goi.interp,
				"usage: %s <address> <count> <data>", cmd_name);
		return JIM_ERR;
	}

	int e;
	jim_wide address;
	e = Jim_GetOpt_Wide(&goi, &address);
	if (e != JIM_OK)
		return e;

	jim_wide count;
	e = Jim_GetOpt_Wide(&goi, &count);
	if (e != JIM_OK)
		return e;

	uint32_t *data = malloc(count * sizeof(uint32_t));
	jim_wide i;
	for (i = 0; i < count; i++) {
		jim_wide tmp;
		e = Jim_GetOpt_Wide(&goi, &tmp);
		if (e != JIM_OK)
			return e;
		data[i] = (uint32_t)tmp;
	}

	/* all args must be consumed */
	if (goi.argc != 0)
		return JIM_ERR;

	struct target *target = Jim_CmdPrivData(goi.interp);
	int result;

	result = target_write_buffer(target, address, count * 4, (const uint8_t *)data);

	free(data);

	return result;
}

static int jim_nds32_bulk_read(Jim_Interp *interp, int argc, Jim_Obj * const *argv)
{
	const char *cmd_name = Jim_GetString(argv[0], NULL);

	Jim_GetOptInfo goi;
	Jim_GetOpt_Setup(&goi, interp, argc - 1, argv + 1);

	if (goi.argc < 2) {
		Jim_SetResultFormatted(goi.interp,
				"usage: %s <address> <count>", cmd_name);
		return JIM_ERR;
	}

	int e;
	jim_wide address;
	e = Jim_GetOpt_Wide(&goi, &address);
	if (e != JIM_OK)
		return e;

	jim_wide count;
	e = Jim_GetOpt_Wide(&goi, &count);
	if (e != JIM_OK)
		return e;

	/* all args must be consumed */
	if (goi.argc != 0)
		return JIM_ERR;

	struct target *target = Jim_CmdPrivData(goi.interp);
	uint32_t *data = malloc(count * sizeof(uint32_t));
	int result;
	result = target_read_buffer(target, address, count * 4, (uint8_t *)data);
	char data_str[11];

	jim_wide i;
	Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
	for (i = 0; i < count; i++) {
		sprintf(data_str, "0x%08x ", data[i]);
		Jim_AppendStrings(interp, Jim_GetResult(interp), data_str, NULL);
	}

	free(data);

	return result;
}

static int jim_nds32_read_edm_sr(Jim_Interp *interp, int argc, Jim_Obj * const *argv)
{
	const char *cmd_name = Jim_GetString(argv[0], NULL);

	Jim_GetOptInfo goi;
	Jim_GetOpt_Setup(&goi, interp, argc - 1, argv + 1);

	if (goi.argc < 1) {
		Jim_SetResultFormatted(goi.interp,
				"usage: %s <edm_sr_name>", cmd_name);
		return JIM_ERR;
	}

	int e;
	char *edm_sr_name;
	int edm_sr_name_len;
	e = Jim_GetOpt_String(&goi, &edm_sr_name, &edm_sr_name_len);
	if (e != JIM_OK)
		return e;

	/* all args must be consumed */
	if (goi.argc != 0)
		return JIM_ERR;

	uint32_t edm_sr_number;
	uint32_t edm_sr_value;
	if (strncmp(edm_sr_name, "edm_dtr", edm_sr_name_len) == 0)
		edm_sr_number = NDS_EDM_SR_EDM_DTR;
	else if (strncmp(edm_sr_name, "edmsw", edm_sr_name_len) == 0)
		edm_sr_number = NDS_EDM_SR_EDMSW;
	else
		return ERROR_FAIL;

	struct target *target = Jim_CmdPrivData(goi.interp);
	struct aice_port_s *aice = target_to_aice(target);
	char data_str[11];

	aice->port->api->read_debug_reg(edm_sr_number, &edm_sr_value);

	sprintf(data_str, "0x%08x", edm_sr_value);
	Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
	Jim_AppendStrings(interp, Jim_GetResult(interp), data_str, NULL);

	return ERROR_OK;
}

static int jim_nds32_write_edm_sr(Jim_Interp *interp, int argc, Jim_Obj * const *argv)
{
	const char *cmd_name = Jim_GetString(argv[0], NULL);

	Jim_GetOptInfo goi;
	Jim_GetOpt_Setup(&goi, interp, argc - 1, argv + 1);

	if (goi.argc < 2) {
		Jim_SetResultFormatted(goi.interp,
				"usage: %s <edm_sr_name> <value>", cmd_name);
		return JIM_ERR;
	}

	int e;
	char *edm_sr_name;
	int edm_sr_name_len;
	e = Jim_GetOpt_String(&goi, &edm_sr_name, &edm_sr_name_len);
	if (e != JIM_OK)
		return e;

	jim_wide value;
	e = Jim_GetOpt_Wide(&goi, &value);
	if (e != JIM_OK)
		return e;

	/* all args must be consumed */
	if (goi.argc != 0)
		return JIM_ERR;

	uint32_t edm_sr_number;
	if (strncmp(edm_sr_name, "edm_dtr", edm_sr_name_len) == 0)
		edm_sr_number = NDS_EDM_SR_EDM_DTR;
	else
		return ERROR_FAIL;

	struct target *target = Jim_CmdPrivData(goi.interp);
	struct aice_port_s *aice = target_to_aice(target);

	aice->port->api->write_debug_reg(edm_sr_number, value);

	return ERROR_OK;
}

static const struct command_registration nds32_query_command_handlers[] = {
	{
		.name = "target",
		.handler = handle_nds32_query_target_command,
		.mode = COMMAND_EXEC,
		.usage = "",
		.help = "reply 'OCD' for gdb to identify server-side is OpenOCD",
	},

	COMMAND_REGISTRATION_DONE
};

static const struct command_registration nds32_exec_command_handlers[] = {
	{
		.name = "dssim",
		.handler = handle_nds32_dssim_command,
		.mode = COMMAND_EXEC,
		.usage = "['on'|'off']",
		.help = "display/change $INT_MASK.DSSIM status",
	},
	{
		.name = "mem_access",
		.handler = handle_nds32_memory_access_command,
		.mode = COMMAND_EXEC,
		.usage = "['bus'|'cpu']",
		.help = "display/change memory access channel",
	},
	{
		.name = "mem_mode",
		.handler = handle_nds32_memory_mode_command,
		.mode = COMMAND_EXEC,
		.usage = "['auto'|'mem'|'ilm'|'dlm']",
		.help = "display/change memory mode",
	},
	{
		.name = "cache",
		.handler = handle_nds32_cache_command,
		.mode = COMMAND_EXEC,
		.usage = "['invalidate'|'dumpi'|'dumpd']",
		.help = "cache control",
	},
	{
		.name = "auto_break",
		.handler = handle_nds32_auto_break_command,
		.mode = COMMAND_EXEC,
		.usage = "['on'|'off']",
		.help = "convert software breakpoints to hardware breakpoints if needed",
	},
	{
		.name = "virtual_hosting",
		.handler = handle_nds32_virtual_hosting_command,
		.mode = COMMAND_ANY,
		.usage = "['on'|'off']",
		.help = "turn on/off virtual hosting",
	},
	{
		.name = "global_stop",
		.handler = handle_nds32_global_stop_command,
		.mode = COMMAND_ANY,
		.usage = "['on'|'off']",
		.help = "turn on/off global stop. After turning on, every load/store" \
			 "instructions will be stopped to check memory access.",
	},
	{
		.name = "soft_reset_halt",
		.handler = handle_nds32_soft_reset_halt_command,
		.mode = COMMAND_ANY,
		.usage = "['on'|'off']",
		.help = "as issuing rest-halt, to use soft-reset-halt or not." \
			 "the feature is for backward-compatible.",
	},
	{
		.name = "boot_time",
		.handler = handle_nds32_boot_time_command,
		.mode = COMMAND_ANY,
		.usage = "milliseconds",
		.help = "set the period to wait after srst.",
	},
	{
		.name = "edm_passcode",
		.handler = handle_nds32_edm_passcode_command,
		.mode = COMMAND_ANY,
		.usage = "passcode",
		.help = "set EDM passcode for secure MCU debugging.",
	},
	{
		.name = "max_stop",
		.handler = handle_nds32_max_stop_command,
		.mode = COMMAND_EXEC,
		.usage = "['on'|'off']",
		.help = "turn on/off max stop. After turning on, it stalls CPU immediately" \
			 "for debugging when maximum interruption stack level is reached for" \
			 "the first time.",
	},
	{
		.name = "reset_halt_as_init",
		.handler = handle_nds32_reset_halt_as_init_command,
		.mode = COMMAND_ANY,
		.usage = "['on'|'off']",
		.help = "reset halt as openocd init.",
	},
	{
		.name = "decode",
		.handler = handle_nds32_decode_command,
		.mode = COMMAND_EXEC,
		.usage = "address icount",
		.help = "decode instruction.",
	},
	{
		.name = "bulk_write",
		.jim_handler = jim_nds32_bulk_write,
		.mode = COMMAND_EXEC,
		.help = "Write multiple 32-bit words to target memory",
		.usage = "address count data",
	},
	{
		.name = "bulk_read",
		.jim_handler = jim_nds32_bulk_read,
		.mode = COMMAND_EXEC,
		.help = "Read multiple 32-bit words from target memory",
		.usage = "address count",
	},
	{
		.name = "read_edmsr",
		.jim_handler = jim_nds32_read_edm_sr,
		.mode = COMMAND_EXEC,
		.help = "Read EDM system register",
		.usage = "['edmsw'|'edm_dtr']",
	},
	{
		.name = "write_edmsr",
		.jim_handler = jim_nds32_write_edm_sr,
		.mode = COMMAND_EXEC,
		.help = "Write EDM system register",
		.usage = "['edm_dtr'] value",
	},
	{
		.name = "query",
		.mode = COMMAND_ANY,
		.help = "Andes query command group",
		.usage = "",
		.chain = nds32_query_command_handlers,
	},

	COMMAND_REGISTRATION_DONE
};

const struct command_registration nds32_command_handlers[] = {
	{
		.name = "nds",
		.mode = COMMAND_ANY,
		.help = "Andes command group",
		.usage = "",
		.chain = nds32_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

