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

static int armv7m_poll_trace(void *target)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);
	uint8_t buf[TRACE_BUF_SIZE];
	size_t size = sizeof(buf);
	int retval;

	retval = adapter_poll_trace(buf, &size);
	if (retval != ERROR_OK || !size)
		return retval;

	target_call_trace_callbacks(target, size, buf);

	if (armv7m->trace_config.trace_file != NULL) {
		if (fwrite(buf, 1, size, armv7m->trace_config.trace_file) == size)
			fflush(armv7m->trace_config.trace_file);
		else {
			LOG_ERROR("Error writing to the trace destination file");
			return ERROR_FAIL;
		}
	}

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
	uint32_t tcr;
	int retval;

	retval = target_write_u32(target, ITM_LAR, ITM_LAR_KEY);
	if (retval != ERROR_OK)
		return retval;

	/* build ITM_TCR register value according to trace config parameters */
	tcr =
		(trace_config->itm_tcr.itmena  * TCR_ITMENA)  |
		(trace_config->itm_tcr.tsena   * TCR_TSENA)   |
		(trace_config->itm_tcr.syncena * TCR_SYNCENA) |
		(trace_config->itm_tcr.txena   * TCR_TXENA)   |
		(trace_config->itm_tcr.swoena  * TCR_SWOENA)  |
		((trace_config->itm_tcr.tsprescale & TCR_TSPRESCALE_MSK) << TCR_TSPRESCALE_POS) |
		((trace_config->itm_tcr.gtsfreq    & TCR_GTSFREQ_MSK)    << TCR_GTSFREQ_POS)    |
		((trace_config->itm_tcr.tracebusid & TCR_TRACEBUSID_MSK) << TCR_TRACEBUSID_POS);

	retval = target_write_u32(target, ITM_TCR, tcr);
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

			if (strcmp(CMD_ARGV[cmd_idx], "-") != 0) {
				armv7m->trace_config.trace_file = fopen(CMD_ARGV[cmd_idx], "ab");
				if (!armv7m->trace_config.trace_file) {
					LOG_ERROR("Can't open trace destination file");
					return ERROR_FAIL;
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

static int Jim_GetOpt_Bool(Jim_GetOptInfo *goi, bool *val)
{
	static const Jim_Nvp nvp_bool_opts[] = {
		{ .name = "on",      .value = true },
		{ .name = "enable",  .value = true },
		{ .name = "1",       .value = true },
		{ .name = "off",     .value = false },
		{ .name = "disable", .value = false },
		{ .name = "0",       .value = false },
		{ .name = NULL, .value = -1 }
	};

	int e;
	Jim_Nvp *n;

	e = Jim_GetOpt_Nvp(goi, nvp_bool_opts, &n);
	if (e != JIM_OK) {
		Jim_GetOpt_NvpUnknown(goi, nvp_bool_opts, 0);
		return e;
	}

	*val = n->value;
	return e;
}

enum itm_cfg_param {
	ICFG_ITM_ENABLE,
	ICFG_ITM_DISABLE,
	ICFG_LTS,
	ICFG_SYNC,
	ICFG_TX,
	ICFG_SWO,
	ICFG_GTS,
	ICFG_TRACEBUSID,
};

static Jim_Nvp nvp_config_opts[] = {
	{ .name = "-enable",             .value = ICFG_ITM_ENABLE },
	{ .name = "-disable",            .value = ICFG_ITM_DISABLE },
	{ .name = "-lts",                .value = ICFG_LTS },
	{ .name = "-sync",               .value = ICFG_SYNC },
	{ .name = "-tx",                 .value = ICFG_TX },
	{ .name = "-swo",                .value = ICFG_SWO },
	{ .name = "-gts",                .value = ICFG_GTS },
	{ .name = "-trace-bus-id",       .value = ICFG_TRACEBUSID },
	{ .name = NULL, .value = -1 }
};

static const Jim_Nvp nvp_lts_opts[] = {
	{ .name = "0",         .value = -1 },
	{ .name = "off",       .value = -1 },
	{ .name = "disable",   .value = -1 },
	{ .name = "1",         .value = ITM_TS_PRESCALE1 },
	{ .name = "on",        .value = ITM_TS_PRESCALE1 },
	{ .name = "enable",    .value = ITM_TS_PRESCALE1 },
	{ .name = "div1",      .value = ITM_TS_PRESCALE1 },
	{ .name = "div4",      .value = ITM_TS_PRESCALE4 },
	{ .name = "div16",     .value = ITM_TS_PRESCALE16 },
	{ .name = "div64",     .value = ITM_TS_PRESCALE64 },
	{ .name = NULL, .value = -1 }
};

static const Jim_Nvp nvp_gts_opts[] = {
	{ .name = "0",         .value = ITM_GTS_DISABLE },
	{ .name = "off",       .value = ITM_GTS_DISABLE },
	{ .name = "disable",   .value = ITM_GTS_DISABLE },
	{ .name = "fast",      .value = ITM_GTS_128CYCLE },
	{ .name = "slow",      .value = ITM_GTS_8192CYCLE },
	{ .name = "always",    .value = ITM_GTS_FIFOEMPTY },
	{ .name = NULL, .value = -1 }
};

static int itm_config(Jim_GetOptInfo *goi, struct command_context *ctx)
{
	Jim_Nvp *n;
	jim_wide w;
	int e;
	struct target *target = get_current_target(ctx);
	struct armv7m_common *armv7m = target_to_armv7m(target);

	while (goi->argc > 0) {
		Jim_SetEmptyResult(goi->interp);

		e = Jim_GetOpt_Nvp(goi, nvp_config_opts, &n);
		if (e != JIM_OK) {
			Jim_GetOpt_NvpUnknown(goi, nvp_config_opts, 0);
			return e;
		}
		switch (n->value) {
		case ICFG_ITM_ENABLE:
			armv7m->trace_config.itm_tcr.itmena = true;
			break;
		case ICFG_ITM_DISABLE:
			armv7m->trace_config.itm_tcr.itmena = false;
			break;
		case ICFG_LTS:
			e = Jim_GetOpt_Nvp(goi, nvp_lts_opts, &n);
			if (e != JIM_OK) {
				Jim_GetOpt_NvpUnknown(goi, nvp_lts_opts, 0);
				return e;
			}

			if (n->value < 0) {
				armv7m->trace_config.itm_tcr.tsena = false;
			} else {
				armv7m->trace_config.itm_tcr.tsena = true;
				armv7m->trace_config.itm_tcr.tsprescale = n->value;
			}
			break;
		case ICFG_SYNC:
			e = Jim_GetOpt_Bool(goi, &armv7m->trace_config.itm_tcr.syncena);
			if (e != JIM_OK)
				return e;
			break;
		case ICFG_TX:
			e = Jim_GetOpt_Bool(goi, &armv7m->trace_config.itm_tcr.txena);
			if (e != JIM_OK)
				return e;
			break;
		case ICFG_SWO:
			e = Jim_GetOpt_Bool(goi, &armv7m->trace_config.itm_tcr.swoena);
			if (e != JIM_OK)
				return e;
			break;
		case ICFG_GTS:
			e = Jim_GetOpt_Nvp(goi, nvp_gts_opts, &n);
			if (e != JIM_OK) {
				Jim_GetOpt_NvpUnknown(goi, nvp_gts_opts, 0);
				return e;
			}
			armv7m->trace_config.itm_tcr.gtsfreq = n->value;
			break;
		case ICFG_TRACEBUSID:
			e = Jim_GetOpt_Wide(goi, &w);
			if (e != JIM_OK)
				return e;
			armv7m->trace_config.itm_tcr.tracebusid = w;
			break;
		}
	}

	if (ctx->mode == COMMAND_EXEC)
		if (armv7m_trace_itm_config(target) != ERROR_OK)
			return JIM_ERR;

	return JIM_OK;
}

static int jim_itm_config(Jim_Interp *interp, int argc, Jim_Obj * const *argv)
{
	Jim_GetOptInfo goi;

	Jim_GetOpt_Setup(&goi, interp, argc - 1, argv + 1);
	/* goi.isconfigure = !strcmp(Jim_GetString(argv[0], NULL), "configure"); */
	if (goi.argc < 1) {
		Jim_WrongNumArgs(goi.interp, goi.argc, goi.argv, "missing: -option ...");
		return JIM_ERR;
	}

	return itm_config(&goi, current_command_context(interp));
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
		"((external | internal <filename>) "
		"(sync <port width> | ((manchester | uart) <formatter enable>)) "
		"<TRACECLKIN freq> [<trace freq>]))",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration itm_command_handlers[] = {
	{
		.name = "config",
		.jim_handler = jim_itm_config,
		.mode = COMMAND_ANY,
		.help = "Configure ITM features",
		.usage = "todo",
	},
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
