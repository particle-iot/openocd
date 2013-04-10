/***************************************************************************
 *   Copyright (C) 2012 by Hsiangkai Wang                                  *
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

#include <jtag/interface.h>
#include <jtag/commands.h>
#include <transport/transport.h>
#include <target/target.h>
#include <jtag/aice/aice_transport.h>
#include <jtag/drivers/libusb_common.h>
#include "aice_usb.h"

#define AICE_KHZ_TO_SPEED_MAP_SIZE	16
static int aice_khz_to_speed_map[AICE_KHZ_TO_SPEED_MAP_SIZE] = {
	30000,
	15000,
	7500,
	3750,
	1875,
	937,
	468,
	234,
	48000,
	24000,
	12000,
	6000,
	3000,
	1500,
	750,
	375,
};

static struct aice_port_s aice;

/***************************************************************************/
/* External interface implementation */
#define AICE_MAX_TARGET_ID_CODES	0x10
static uint32_t aice_target_id_codes[AICE_MAX_TARGET_ID_CODES];
static uint8_t aice_num_of_target_id_codes;

/***************************************************************************/
/* AICE operations */
int aice_init_target(struct target *t)
{
	int res;

	LOG_DEBUG("aice_init_target");

	if (aice_num_of_target_id_codes == 0) {
		res = aice.port->api->idcode(aice_target_id_codes, &aice_num_of_target_id_codes);
		if (res != ERROR_OK)
			return res;
	}

	t->tap->idcode = aice_target_id_codes[t->tap->abs_chain_position];

	unsigned ii, limit = t->tap->expected_ids_cnt;
	int found = 0;

	for (ii = 0; ii < limit; ii++) {
		uint32_t expected = t->tap->expected_ids[ii];

		/* treat "-expected-id 0" as a "don't-warn" wildcard */
		if (!expected || (t->tap->idcode == expected)) {
			found = 1;
			break;
		}
	}

	if (found == 0) {
		LOG_ERROR
			("aice_init_target: target not found: idcode: %x ",
			 t->tap->idcode);
		return ERROR_FAIL;
	}

	t->tap->priv = &aice;
	t->tap->hasidcode = 1;

	return ERROR_OK;
}

/***************************************************************************/
/* End of External interface implementation */

/* initial aice
 * 1. open usb
 * 2. get/show version number
 * 3. reset
 */
static int aice_init(void)
{
	if (ERROR_OK != aice.port->api->open(&(aice.param))) {
		LOG_ERROR("Cannot find AICE Interface! Please check "
				"connection and permissions.");
		return ERROR_JTAG_INIT_FAILED;
	}

	aice.port->api->set_retry_times(aice.retry_times);

	LOG_INFO("AICE JTAG Interface ready");

	return ERROR_OK;
}

/* cleanup aice resource
 * close usb
 */
static int aice_quit(void)
{
	aice.port->api->close();
	return ERROR_OK;
}

static int aice_execute_reset(struct jtag_command *cmd)
{
	static int last_trst;
	int retval = ERROR_OK;

	DEBUG_JTAG_IO("reset trst: %i", cmd->cmd.reset->trst);

	if (cmd->cmd.reset->trst != last_trst) {
		if (cmd->cmd.reset->trst)
			retval = aice.port->api->reset();

		last_trst = cmd->cmd.reset->trst;
	}

	return retval;
}

static int aice_execute_command(struct jtag_command *cmd)
{
	int retval;

	switch (cmd->type) {
		case JTAG_RESET:
			retval = aice_execute_reset(cmd);
			break;
		default:
			retval = ERROR_OK;
			break;
	}
	return retval;
}

/* aice has no need to implement jtag execution model
*/
static int aice_execute_queue(void)
{
	struct jtag_command *cmd = jtag_command_queue;	/* currently processed command */
	int retval;

	retval = ERROR_OK;

	while (cmd) {
		if (aice_execute_command(cmd) != ERROR_OK)
			retval = ERROR_JTAG_QUEUE_FAILED;

		cmd = cmd->next;
	}

	return retval;
}

/* set jtag frequency(base frequency/frequency divider) to your jtag adapter */
static int aice_speed(int speed)
{
	return aice.port->api->set_jtag_clock(speed);
}

/* convert jtag adapter frequency(base frequency/frequency divider) to human readable KHz value */
static int aice_speed_div(int speed, int *khz)
{
	*khz = aice_khz_to_speed_map[speed];

	return ERROR_OK;
}

/* convert human readable KHz value to jtag adapter frequency(base frequency/frequency divider) */
static int aice_khz(int khz, int *jtag_speed)
{
	int i;
	for (i = 0 ; i < AICE_KHZ_TO_SPEED_MAP_SIZE ; i++) {
		if (khz == aice_khz_to_speed_map[i]) {
			if (8 <= i)
				*jtag_speed = i | AICE_TCK_CONTROL_TCK3048;
			else
				*jtag_speed = i;
			break;
		}
	}

	if (i == AICE_KHZ_TO_SPEED_MAP_SIZE)
		*jtag_speed = 0xF;  /* 0x7 | AICE_TCK_CONTROL_TCK3048 */

	return ERROR_OK;
}

/***************************************************************************/
/* Command handlers */
COMMAND_HANDLER(aice_handle_aice_info_command)
{
	LOG_DEBUG("aice_handle_aice_info_command");

	return ERROR_OK;
}

COMMAND_HANDLER(aice_handle_aice_port_command)
{
	LOG_DEBUG("aice_handle_aice_port_command");

	if (CMD_ARGC != 1) {
		LOG_ERROR("Need exactly one argument to 'aice adapter'");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	for (const struct aice_port *l = aice_port_get_list(); l->name; l++) {
		if (strcmp(l->name, CMD_ARGV[0]) == 0) {
			aice.port = l;
			return ERROR_OK;
		}
	}

	LOG_ERROR("No AICE port '%s' found", CMD_ARGV[0]);
	return ERROR_FAIL;
}

COMMAND_HANDLER(aice_handle_aice_desc_command)
{
	LOG_DEBUG("aice_handle_aice_desc_command");

	if (CMD_ARGC == 1)
		aice.param.device_desc = strdup(CMD_ARGV[0]);
	else
		LOG_ERROR("expected exactly one argument to aice desc <description>");

	return ERROR_OK;
}

COMMAND_HANDLER(aice_handle_aice_serial_command)
{
	LOG_DEBUG("aice_handle_aice_serial_command");

	if (CMD_ARGC == 1)
		aice.param.serial = strdup(CMD_ARGV[0]);
	else
		LOG_ERROR("expected exactly one argument to aice serial <serial-number>");

	return ERROR_OK;
}

COMMAND_HANDLER(aice_handle_aice_vid_pid_command)
{
	LOG_DEBUG("aice_handle_aice_vid_pid_command");

	if (CMD_ARGC != 2) {
		LOG_WARNING("ignoring extra IDs in aice vid_pid (maximum is 1 pair)");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	COMMAND_PARSE_NUMBER(u16, CMD_ARGV[0], aice.param.vid);
	COMMAND_PARSE_NUMBER(u16, CMD_ARGV[1], aice.param.pid);

	return ERROR_OK;
}

COMMAND_HANDLER(aice_handle_aice_adapter_command)
{
	LOG_DEBUG("aice_handle_aice_adapter_command");

	if (CMD_ARGC == 1)
		aice.param.adapter_name = strdup(CMD_ARGV[0]);
	else
		LOG_ERROR("expected exactly one argument to aice adapter <adapter-name>");

	return ERROR_OK;
}

COMMAND_HANDLER(aice_handle_aice_retry_times_command)
{
	LOG_DEBUG("aice_handle_aice_retry_times_command");

	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], aice.retry_times);
	else
		LOG_ERROR("expected exactly one argument to aice retry_times <num_of_retry>");

	return ERROR_OK;
}

COMMAND_HANDLER(aice_handle_aice_custom_srst_script_command)
{
	LOG_DEBUG("aice_handle_aice_custom_srst_script_command");

	if (CMD_ARGC > 0) {
		aice.port->api->set_custom_srst_script(CMD_ARGV[0]);
		return ERROR_OK;
	}

	return ERROR_FAIL;
}

COMMAND_HANDLER(aice_handle_aice_custom_trst_script_command)
{
	LOG_DEBUG("aice_handle_aice_custom_trst_script_command");

	if (CMD_ARGC > 0) {
		aice.port->api->set_custom_trst_script(CMD_ARGV[0]);
		return ERROR_OK;
	}

	return ERROR_FAIL;
}

COMMAND_HANDLER(aice_handle_aice_custom_restart_script_command)
{
	LOG_DEBUG("aice_handle_aice_custom_restart_script_command");

	if (CMD_ARGC > 0) {
		aice.port->api->set_custom_restart_script(CMD_ARGV[0]);
		return ERROR_OK;
	}

	return ERROR_FAIL;
}


static const struct command_registration aice_subcommand_handlers[] = {
	{
		.name = "info",
		.handler = &aice_handle_aice_info_command,
		.mode = COMMAND_EXEC,
		.help = "show aice info",
		.usage = "aice info",
	},
	{
		.name = "port",
		.handler = &aice_handle_aice_port_command,
		.mode = COMMAND_CONFIG,
		.help = "set the port of the AICE",
		.usage = "aice port",
	},
	{
		.name = "desc",
		.handler = &aice_handle_aice_desc_command,
		.mode = COMMAND_CONFIG,
		.help = "set the aice device description",
		.usage = "aice desc [desciption string]",
	},
	{
		.name = "serial",
		.handler = &aice_handle_aice_serial_command,
		.mode = COMMAND_CONFIG,
		.help = "set the serial number of the AICE device",
		.usage = "aice serial [serial string]",
	},
	{
		.name = "vid_pid",
		.handler = &aice_handle_aice_vid_pid_command,
		.mode = COMMAND_CONFIG,
		.help = "the vendor and product ID of the AICE device",
		.usage = "aice vid_pid (vid pid)*",
	},
	{
		.name = "adapter",
		.handler = &aice_handle_aice_adapter_command,
		.mode = COMMAND_CONFIG,
		.help = "set the file name of adapter",
		.usage = "aice adapter [adapter name]",
	},
	{
		.name = "retry_times",
		.handler = &aice_handle_aice_retry_times_command,
		.mode = COMMAND_CONFIG,
		.help = "set retry times as AICE timeout",
		.usage = "aice retry_times num_of_retry",
	},
	{
		.name = "custom_srst_script",
		.handler = &aice_handle_aice_custom_srst_script_command,
		.mode = COMMAND_CONFIG,
		.usage = "custom_srst_script script_file_name",
		.help = "set custom srst script",
	},
	{
		.name = "custom_trst_script",
		.handler = &aice_handle_aice_custom_trst_script_command,
		.mode = COMMAND_CONFIG,
		.usage = "custom_trst_script script_file_name",
		.help = "set custom trst script",
	},
	{
		.name = "custom_restart_script",
		.handler = &aice_handle_aice_custom_restart_script_command,
		.mode = COMMAND_CONFIG,
		.usage = "custom_restart_script script_file_name",
		.help = "set custom restart script",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration aice_command_handlers[] = {
	{
		.name = "aice",
		.mode = COMMAND_ANY,
		.help = "perform aice management",
		.usage = "aice [subcommand]",
		.chain = aice_subcommand_handlers,
	},
	COMMAND_REGISTRATION_DONE
};
/***************************************************************************/
/* End of Command handlers */

struct jtag_interface aice_interface = {
	.name = "aice",
	.commands = aice_command_handlers,
	.transports = aice_transports,
	.init = aice_init,
	.quit = aice_quit,
	.execute_queue = aice_execute_queue,
	.speed = aice_speed,		/* set interface speed */
	.speed_div = aice_speed_div,	/* return readable value */
	.khz = aice_khz,		/* convert khz to interface speed value */
};

