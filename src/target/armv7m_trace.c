/***************************************************************************
 *   Copyright (C) 2015  Paul Fertser <fercerpav@gmail.com>                *
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
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <target/target.h>
#include <target/armv7m.h>
#include <target/cortex_m.h>
#include <target/armv7m_trace.h>
#include <jtag/interface.h>

static int armv7m_trace_received(struct target *target, uint8_t *buf,
					 size_t size)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);

	if (fwrite(buf, 1, size, armv7m->trace_config.trace_file) == size)
		fflush(armv7m->trace_config.trace_file);
	else {
		LOG_ERROR("Error writing to the trace destination file");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}


int armv7m_trace_tpiu_config(struct target *target)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);
	struct armv7m_trace_config *trace_config = &armv7m->trace_config;
	int retval;

	retval = adapter_config_trace(trace_config->config_type == INTERNAL,
				      trace_config->pin_protocol,
				      trace_config->port_size,
				      trace_config->trace_freq,
				      armv7m_trace_received,
				      target);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, TPIU_CSPSR, 1 << trace_config->port_size);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, TPIU_ACPR, trace_config->prescaler - 1);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, TPIU_SPPR, trace_config->pin_protocol);
	if (retval != ERROR_OK)
		return retval;

	uint32_t ffcr;
	retval = target_read_u32(target, TPIU_FFCR, &ffcr);
	if (retval != ERROR_OK)
		return retval;
	if (trace_config->formatter)
		ffcr |= (1 << 1);
	else
		ffcr &= ~(1 << 1);
	retval = target_write_u32(target, TPIU_FFCR, ffcr);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

int armv7m_trace_itm_config(struct target *target)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);
	struct armv7m_trace_config *trace_config = &armv7m->trace_config;
	int retval;

	retval = target_write_u32(target, ITM_LAR, ITM_LAR_KEY);
	if (retval != ERROR_OK)
		return retval;

	/* Enable ITM and set TraceBusID */
	retval = target_write_u32(target, ITM_TCR, (1 << 0) |
				  (trace_config->trace_bus_id << 16));
	if (retval != ERROR_OK)
		return retval;

	for (unsigned int i = 0; i < 8; i++) {
		retval = target_write_u32(target, ITM_TER0 + i * 4,
					  trace_config->itm_ter[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_tpiu_config_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct armv7m_common *armv7m = target_to_armv7m(target);

	unsigned int cmd_idx = 0;

	if (CMD_ARGC == cmd_idx)
		return ERROR_COMMAND_SYNTAX_ERROR;
	if (!strcmp(CMD_ARGV[cmd_idx], "disable")) {
		if (CMD_ARGC == cmd_idx + 1) {
			armv7m->trace_config.config_type = NONE;
			if (CMD_CTX->mode == COMMAND_EXEC)
				return armv7m_trace_tpiu_config(target);
			else
				return ERROR_OK;
		}
	} else if (!strcmp(CMD_ARGV[cmd_idx], "external") ||
		   !strcmp(CMD_ARGV[cmd_idx], "internal")) {
		if (!strcmp(CMD_ARGV[cmd_idx], "internal")) {
			armv7m->trace_config.config_type = INTERNAL;

			cmd_idx++;
			if (CMD_ARGC == cmd_idx)
				return ERROR_COMMAND_SYNTAX_ERROR;

			COMMAND_PARSE_NUMBER(uint, CMD_ARGV[cmd_idx], armv7m->trace_config.trace_freq);

			cmd_idx++;
			if (CMD_ARGC == cmd_idx)
				return ERROR_COMMAND_SYNTAX_ERROR;

			if (armv7m->trace_config.trace_file)
				fclose(armv7m->trace_config.trace_file);

			armv7m->trace_config.trace_file = fopen(CMD_ARGV[cmd_idx], "ab");
			if (!armv7m->trace_config.trace_file) {
				LOG_ERROR("Can't open trace destination file");
				return ERROR_FAIL;
			}
		} else
			armv7m->trace_config.config_type = EXTERNAL;
		cmd_idx++;
		if (CMD_ARGC == cmd_idx)
			return ERROR_COMMAND_SYNTAX_ERROR;

		if (!strcmp(CMD_ARGV[cmd_idx], "sync")) {
			armv7m->trace_config.pin_protocol = SYNC;

			cmd_idx++;
			if (CMD_ARGC == cmd_idx)
				return ERROR_COMMAND_SYNTAX_ERROR;

			COMMAND_PARSE_NUMBER(u32, CMD_ARGV[cmd_idx], armv7m->trace_config.port_size);
		} else {
			if (!strcmp(CMD_ARGV[cmd_idx], "manchester"))
				armv7m->trace_config.pin_protocol = ASYNC_MANCHESTER;
			else if (!strcmp(CMD_ARGV[cmd_idx], "uart"))
				armv7m->trace_config.pin_protocol = ASYNC_UART;
			else
				return ERROR_COMMAND_SYNTAX_ERROR;

			cmd_idx++;
			if (CMD_ARGC == cmd_idx)
				return ERROR_COMMAND_SYNTAX_ERROR;

			COMMAND_PARSE_ON_OFF(CMD_ARGV[cmd_idx], armv7m->trace_config.formatter);
		}

		cmd_idx++;
		if (CMD_ARGC == cmd_idx)
			return ERROR_COMMAND_SYNTAX_ERROR;
		COMMAND_PARSE_NUMBER(u16, CMD_ARGV[cmd_idx++], armv7m->trace_config.prescaler);

		if (CMD_ARGC == cmd_idx) {
			if (CMD_CTX->mode == COMMAND_EXEC)
				return armv7m_trace_tpiu_config(target);
			else
				return ERROR_OK;
		}
	}

	return ERROR_COMMAND_SYNTAX_ERROR;
}

COMMAND_HANDLER(handle_itm_port_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	unsigned int reg_idx;
	uint8_t port;
	bool enable;

	if (CMD_ARGC != 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_NUMBER(u8, CMD_ARGV[0], port);
	COMMAND_PARSE_ON_OFF(CMD_ARGV[1], enable);
	reg_idx = port / 8;
	port = port % 8;
	if (enable)
		armv7m->trace_config.itm_ter[reg_idx] |= (1 << port);
	else
		armv7m->trace_config.itm_ter[reg_idx] &= ~(1 << port);

	if (CMD_CTX->mode == COMMAND_EXEC)
		return armv7m_trace_itm_config(target);
	else
		return ERROR_OK;
}

static const struct command_registration tpiu_command_handlers[] = {
	{
		.name = "config",
		.handler = handle_tpiu_config_command,
		.mode = COMMAND_ANY,
		.help = "Configure TPIU features",
		.usage = "(disable | "
		"((external | internal <baud> <filename>) "
		"(sync <port_size> | ((manchester | uart) <formatter_en>)) <prescaler>))",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration itm_command_handlers[] = {
	{
		.name = "port",
		.handler = handle_itm_port_command,
		.mode = COMMAND_ANY,
		.help = "Enable or disable ITM stimulus port",
		.usage = "<port> (on|off)",
	},
	COMMAND_REGISTRATION_DONE
};

const struct command_registration armv7m_trace_command_handlers[] = {
	{
		.name = "tpiu",
		.mode = COMMAND_ANY,
		.help = "tpiu command group",
		.usage = "",
		.chain = tpiu_command_handlers,
	},
	{
		.name = "itm",
		.mode = COMMAND_ANY,
		.help = "itm command group",
		.usage = "",
		.chain = itm_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};
