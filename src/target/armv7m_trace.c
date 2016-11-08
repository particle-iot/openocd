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
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <target/target.h>
#include <target/armv7m.h>
#include <target/cortex_m.h>
#include <target/armv7m_trace.h>
#include <jtag/interface.h>

#define TRACE_BUF_SIZE	4096

#define TRACE_HEADER_SYNC0			0x00
#define TRACE_HEADER_SYNC0_MASK		0xFF
#define TRACE_HEADER_SYNC1			0x80
#define TRACE_HEADER_SYNC1_MASK		0xFF
#define TRACE_HEADER_OVERFLOW		0x70
#define TRACE_HEADER_OVERFLOW_MASK	0xFF
#define TRACE_HEADER_TIMESTAMP		0x00
#define TRACE_HEADER_TIMESTAMP_MASK	0x0F
#define TRACE_HEADER_RESERVED		0x04
#define TRACE_HEADER_RESERVED_MASK	0x0F
#define TRACE_HEADER_SWIT			0x00
#define TRACE_HEADER_SWIT_MASK		0x04

#define TRACE_TIMESTAMP_CONT_MASK	0x80
#define TRACE_RESERVED_CONT_MASK	0x80
#define TRACE_SWIT_DATA_MASK		0x03
#define TRACE_SWIT_DATA_SHIFT		0

#define TRACE_TIMESTAMP_MAX_LENGTH	4
#define TRACE_RESERVED_MAX_LENGTH	4
#define TRACE_PAYLOAD_SIZE			4

enum trace_state {
	STATE_HEADER = 0,
	STATE_TIMESTAMP,
	STATE_RESERVED,
	STATE_SWIT
};

struct trace_parser {
	enum trace_state state;

	size_t payload_length;
	size_t payload_index;
	uint8_t payload[TRACE_PAYLOAD_SIZE];
};

static int armv7m_trace_parse_header(uint8_t header, struct trace_parser* parser)
{
	if (((header & TRACE_HEADER_SYNC0_MASK) == TRACE_HEADER_SYNC0) ||
		((header & TRACE_HEADER_SYNC1_MASK) == TRACE_HEADER_SYNC1)) {
		parser->state = STATE_HEADER;

	} else if ((header & TRACE_HEADER_OVERFLOW_MASK) == TRACE_HEADER_OVERFLOW) {
		parser->state = STATE_HEADER;

	} else if ((header & TRACE_HEADER_TIMESTAMP_MASK) == TRACE_HEADER_TIMESTAMP) {
		parser->payload_length = ((header & TRACE_TIMESTAMP_CONT_MASK) == TRACE_TIMESTAMP_CONT_MASK) ? 1 : 0;
		parser->payload_index = 0;
		if (parser->payload_length != 0)
			parser->state = STATE_TIMESTAMP;
		else
			parser->state = STATE_HEADER;

	} else if ((header & TRACE_HEADER_RESERVED_MASK) == TRACE_HEADER_RESERVED) {
		parser->payload_length = ((header & TRACE_RESERVED_CONT_MASK) == TRACE_RESERVED_CONT_MASK) ? 1 : 0;
		parser->payload_index = 0;
		if (parser->payload_length != 0)
			parser->state = STATE_RESERVED;
		else
			parser->state = STATE_HEADER;

	} else if ((header & TRACE_HEADER_SWIT_MASK) == TRACE_HEADER_SWIT) {
		parser->payload_length = (header & TRACE_SWIT_DATA_MASK) >> TRACE_SWIT_DATA_SHIFT;
		parser->payload_index = 0;
		if (parser->payload_length == 0) {
			LOG_ERROR("Error trace SWIT length");
			return ERROR_FAIL;
		}

		parser->state = STATE_SWIT;
	}
	return ERROR_OK;
}

static int armv7m_trace_to_ascii_file(uint8_t data[TRACE_BUF_SIZE], size_t len, FILE* trace_file)
{
	static struct trace_parser parser = {
		.state = STATE_HEADER,
		.payload_length = 0,
		.payload_index = 0
	};

	for (size_t index = 0; index < len; index++) {

		uint8_t* data_to_output = 0;
		size_t size_of_output = 0;

		if (parser.state == STATE_HEADER)
		{
			if (armv7m_trace_parse_header(data[index], &parser) != ERROR_OK)
				return ERROR_FAIL;

		} else if (parser.state == STATE_TIMESTAMP) {
			parser.payload[parser.payload_index] = data[index];
			parser.payload_index++;

			if (((data[index] & TRACE_TIMESTAMP_CONT_MASK) == TRACE_TIMESTAMP_CONT_MASK) &&
				(parser.payload_length <= TRACE_TIMESTAMP_MAX_LENGTH))
				parser.payload_length++;

			if (parser.payload_index == parser.payload_length)
				parser.state = STATE_HEADER;

		} else if (parser.state == STATE_RESERVED) {
			parser.payload[parser.payload_index] = data[index];
			parser.payload_index++;

			if (((data[index] & TRACE_RESERVED_CONT_MASK) == TRACE_RESERVED_CONT_MASK) &&
				(parser.payload_length <= TRACE_RESERVED_MAX_LENGTH))
				parser.payload_length++;

			if (parser.payload_index == parser.payload_length)
				parser.state = STATE_HEADER;

		} else if (parser.state == STATE_SWIT) {
			parser.payload[parser.payload_index] = data[index];
			parser.payload_index++;

			if (parser.payload_index == parser.payload_length) {
				data_to_output = parser.payload;
				size_of_output = parser.payload_length;

				parser.state = STATE_HEADER;
			}
		}

		if ((size_of_output > 0) && (data_to_output != NULL))
		{
			if (fwrite(data_to_output, 1, size_of_output, trace_file) != size_of_output)
				return ERROR_FAIL;
		}
	}

	return ERROR_OK;
}

static int armv7m_trace_to_file(struct target *target, size_t len, uint8_t data[TRACE_BUF_SIZE])
{
	struct armv7m_common *armv7m = target_to_armv7m(target);

	if (armv7m->trace_config.trace_file != NULL) {
		if (armv7m->trace_config.display_type == RAW) {
			if (fwrite(data, 1, len, armv7m->trace_config.trace_file) == len)
				fflush(armv7m->trace_config.trace_file);
			else {
				LOG_ERROR("Error writing to the trace destination file");
				return ERROR_FAIL;
			}
		} else if (armv7m->trace_config.display_type == ASCII) {
			if (armv7m_trace_to_ascii_file(data, len, armv7m->trace_config.trace_file) == ERROR_OK)
				fflush(armv7m->trace_config.trace_file);
			else {
				LOG_ERROR("Error processing trace");
				return ERROR_FAIL;
			}
		}
	}

	return ERROR_OK;
}

static int armv7m_poll_trace(void *target)
{
	uint8_t buf[TRACE_BUF_SIZE];
	size_t size = sizeof(buf);
	int retval;

	retval = adapter_poll_trace(buf, &size);
	if (retval != ERROR_OK || !size)
		return retval;

	target_call_trace_callbacks(target, size, buf);

	retval = armv7m_trace_to_file(target, size, buf);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

int armv7m_trace_tpiu_config(struct target *target)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);
	struct armv7m_trace_config *trace_config = &armv7m->trace_config;
	int prescaler;
	int retval;

	target_unregister_timer_callback(armv7m_poll_trace, target);


	retval = adapter_config_trace(trace_config->config_type == INTERNAL,
				      trace_config->pin_protocol,
				      trace_config->port_size,
				      &trace_config->trace_freq);
	if (retval != ERROR_OK)
		return retval;

	if (!trace_config->trace_freq) {
		LOG_ERROR("Trace port frequency is 0, can't enable TPIU");
		return ERROR_FAIL;
	}

	prescaler = trace_config->traceclkin_freq / trace_config->trace_freq;

	if (trace_config->traceclkin_freq % trace_config->trace_freq) {
		prescaler++;
		int trace_freq = trace_config->traceclkin_freq / prescaler;
		LOG_INFO("Can not obtain %u trace port frequency from %u TRACECLKIN frequency, using %u instead",
			  trace_config->trace_freq, trace_config->traceclkin_freq,
			  trace_freq);
		trace_config->trace_freq = trace_freq;
		retval = adapter_config_trace(trace_config->config_type == INTERNAL,
					      trace_config->pin_protocol,
					      trace_config->port_size,
					      &trace_config->trace_freq);
		if (retval != ERROR_OK)
			return retval;
	}

	retval = target_write_u32(target, TPIU_CSPSR, 1 << trace_config->port_size);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, TPIU_ACPR, prescaler - 1);
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

	if (trace_config->config_type == INTERNAL)
		target_register_timer_callback(armv7m_poll_trace, 1, 1, target);

	target_call_event_callbacks(target, TARGET_EVENT_TRACE_CONFIG);

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

	/* Enable ITM, TXENA, set TraceBusID and other parameters */
	retval = target_write_u32(target, ITM_TCR, (1 << 0) | (1 << 3) |
				  (trace_config->itm_diff_timestamps << 1) |
				  (trace_config->itm_synchro_packets << 2) |
				  (trace_config->itm_async_timestamps << 4) |
				  (trace_config->itm_ts_prescale << 8) |
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

static void close_trace_file(struct armv7m_common *armv7m)
{
	if (armv7m->trace_config.trace_file)
		fclose(armv7m->trace_config.trace_file);
	armv7m->trace_config.trace_file = NULL;
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
			close_trace_file(armv7m);

			armv7m->trace_config.config_type = DISABLED;
			if (CMD_CTX->mode == COMMAND_EXEC)
				return armv7m_trace_tpiu_config(target);
			else
				return ERROR_OK;
		}
	} else if (!strcmp(CMD_ARGV[cmd_idx], "external") ||
		   !strcmp(CMD_ARGV[cmd_idx], "internal")) {
		close_trace_file(armv7m);

		armv7m->trace_config.config_type = EXTERNAL;
		if (!strcmp(CMD_ARGV[cmd_idx], "internal")) {
			cmd_idx++;
			if (CMD_ARGC == cmd_idx)
				return ERROR_COMMAND_SYNTAX_ERROR;

			armv7m->trace_config.config_type = INTERNAL;
			armv7m->trace_config.display_type = RAW;

			if (strcmp(CMD_ARGV[cmd_idx], "-") != 0) {
				armv7m->trace_config.trace_file = fopen(CMD_ARGV[cmd_idx], "ab");
				if (!armv7m->trace_config.trace_file) {
					LOG_ERROR("Can't open trace destination file");
					return ERROR_FAIL;
				}
				if (CMD_ARGC != (cmd_idx + 1)) {
					if (!strcmp(CMD_ARGV[cmd_idx + 1], "ascii")) {
						cmd_idx++;
						armv7m->trace_config.display_type = ASCII;
					} else if (!strcmp(CMD_ARGV[cmd_idx + 1], "raw")) {
						cmd_idx++;
						armv7m->trace_config.display_type = RAW;
					}
				}
			}
		}
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

		COMMAND_PARSE_NUMBER(uint, CMD_ARGV[cmd_idx], armv7m->trace_config.traceclkin_freq);

		cmd_idx++;
		if (CMD_ARGC != cmd_idx) {
			COMMAND_PARSE_NUMBER(uint, CMD_ARGV[cmd_idx], armv7m->trace_config.trace_freq);
			cmd_idx++;
		} else {
			if (armv7m->trace_config.config_type != INTERNAL) {
				LOG_ERROR("Trace port frequency can't be omitted in external capture mode");
				return ERROR_COMMAND_SYNTAX_ERROR;
			}
			armv7m->trace_config.trace_freq = 0;
		}

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
	reg_idx = port / 32;
	port = port % 32;
	if (enable)
		armv7m->trace_config.itm_ter[reg_idx] |= (1 << port);
	else
		armv7m->trace_config.itm_ter[reg_idx] &= ~(1 << port);

	if (CMD_CTX->mode == COMMAND_EXEC)
		return armv7m_trace_itm_config(target);
	else
		return ERROR_OK;
}

COMMAND_HANDLER(handle_itm_ports_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	bool enable;

	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_ON_OFF(CMD_ARGV[0], enable);
	memset(armv7m->trace_config.itm_ter, enable ? 0xff : 0,
	       sizeof(armv7m->trace_config.itm_ter));

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
		"((external | internal (<filename> | -) [raw | ascii]) "
		"(sync <port width> | ((manchester | uart) <formatter enable>)) "
		"<TRACECLKIN freq> [<trace freq>]))",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration itm_command_handlers[] = {
	{
		.name = "port",
		.handler = handle_itm_port_command,
		.mode = COMMAND_ANY,
		.help = "Enable or disable ITM stimulus port",
		.usage = "<port> (0|1|on|off)",
	},
	{
		.name = "ports",
		.handler = handle_itm_ports_command,
		.mode = COMMAND_ANY,
		.help = "Enable or disable all ITM stimulus ports",
		.usage = "(0|1|on|off)",
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
