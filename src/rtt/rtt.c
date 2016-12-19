/*
 * Copyright (C) 2016-2017 by Marc Schink
 * openocd-dev@marcschink.de
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <helper/log.h>
#include <helper/list.h>
#include <target/target.h>
#include <target/rtt.h>

#include "rtt.h"

static struct rtt_source global_source;
static struct rtt_control global_ctrl;
static struct target *global_target;
static target_addr_t global_addr;
static uint32_t global_length;
static char global_id[RTT_MAX_CB_ID_LENGTH];
static size_t global_id_length;
static bool global_configured;
static bool global_started;
static bool global_changed;
static bool global_found_cb;

static struct rtt_sink_list **global_sink_list;
static size_t global_sink_list_length;

int rtt_init(void)
{
	global_sink_list_length = 1;
	global_sink_list = calloc(global_sink_list_length,
		sizeof(struct rtt_sink_list *));

	if (!global_sink_list)
		return ERROR_FAIL;

	global_sink_list[0] = NULL;
	global_started = false;

	return ERROR_OK;
}

int rtt_exit(void)
{
	free(global_sink_list);

	return ERROR_OK;
}

static int read_channel_callback(void *user_data)
{
	int ret;

	ret = global_source.read(&global_ctrl, global_sink_list,
		global_sink_list_length, global_target, NULL);

	if (ret != ERROR_OK) {
		target_unregister_timer_callback(&read_channel_callback, NULL);
		global_source.stop(global_target, NULL);
		return ret;
	}

	return ERROR_OK;
}

int rtt_register_source(const struct rtt_source source, struct target *target)
{
	global_source = source;
	global_target = target;

	return ERROR_OK;
}

int rtt_start(void)
{
	int ret;
	target_addr_t addr = global_addr;

	if (global_started) {
		LOG_INFO("RTT already started");
		return ERROR_OK;
	}

	if (!global_found_cb || global_changed) {
		global_source.find_cb(&addr, global_length, global_id,
			global_id_length, &global_found_cb, global_target, NULL);

		global_changed = false;

		if (global_found_cb) {
			LOG_INFO("RTT control block found at 0x%" TARGET_PRIxADDR, addr);
			global_ctrl.address = addr;
		} else {
			LOG_INFO("No RTT control block found");
			return ERROR_OK;
		}
	}

	ret = global_source.read_cb(global_ctrl.address, &global_ctrl,
		global_target, NULL);

	if (ret != ERROR_OK)
		return ret;

	ret = global_source.start(&global_ctrl, global_target, NULL);

	if (ret != ERROR_OK)
		return ret;

	target_register_timer_callback(&read_channel_callback, 100, 1, NULL);
	global_started = true;

	return ERROR_OK;
}

int rtt_stop(void)
{
	int ret;

	if (!global_configured) {
		LOG_ERROR("RTT is not configured");
		return ERROR_FAIL;
	}

	target_unregister_timer_callback(&read_channel_callback, NULL);
	global_started = false;

	ret = global_source.stop(global_target, NULL);

	if (ret != ERROR_OK)
		return ret;

	return ERROR_OK;
}

static int adjust_sink_list(size_t length)
{
	size_t i;
	struct rtt_sink_list **tmp;

	if (length <= global_sink_list_length)
		return ERROR_OK;

	tmp = realloc(global_sink_list, sizeof(struct rtt_sink_list *) * length);

	if (!tmp)
		return ERROR_FAIL;

	for (i = global_sink_list_length; i < length; i++)
		tmp[i] = NULL;

	global_sink_list = tmp;
	global_sink_list_length = length;

	return ERROR_OK;
}

int rtt_register_sink(unsigned int channel, rtt_sink_read read,
		void *user_data)
{
	struct rtt_sink_list *tmp;

	if (channel >= global_sink_list_length) {
		if (adjust_sink_list(channel + 1) != ERROR_OK)
			return ERROR_FAIL;
	}

	LOG_DEBUG("Registering sink for RTT channel %u", channel);

	tmp = malloc(sizeof(struct rtt_sink_list));

	if (!tmp)
		return ERROR_FAIL;

	tmp->read = read;
	tmp->user_data = user_data;
	tmp->next = global_sink_list[channel];

	global_sink_list[channel] = tmp;

	return ERROR_OK;
}

int rtt_unregister_sink(unsigned int channel, rtt_sink_read read,
		void *user_data)
{
	struct rtt_sink_list *sink;
	struct rtt_sink_list *prev_sink;

	LOG_DEBUG("Unregistering sink for RTT channel %u", channel);

	if (channel >= global_sink_list_length)
		return ERROR_FAIL;

	prev_sink = global_sink_list[channel];

	for (sink = global_sink_list[channel]; sink; prev_sink = sink,
			sink = sink->next) {
		if (sink->read == read && sink->user_data == user_data) {

			if (sink == global_sink_list[channel])
				global_sink_list[channel] = sink->next;
			else
				prev_sink->next = sink->next;

			free(sink);

			return ERROR_OK;
		}
	}

	return ERROR_OK;
}

int rtt_write_channel(unsigned int channel, const uint8_t *buffer,
		size_t *length)
{
	if (!global_source.write)
		return ERROR_FAIL;

	if (channel >= global_ctrl.num_up_buffers) {
		LOG_WARNING("Down-channel %u is not available", channel);
		return ERROR_OK;
	}

	return global_source.write(&global_ctrl, channel, buffer, length,
		global_target, NULL);
}

COMMAND_HANDLER(handle_rtt_setup_command)
{
	target_addr_t addr;
	uint32_t length;
	struct rtt_source source;

	if (CMD_ARGC != 3)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_NUMBER(target_addr, CMD_ARGV[0], addr);
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], length);

	global_id_length = strlen(CMD_ARGV[2]);

	if (!global_id_length || global_id_length > RTT_MAX_CB_ID_LENGTH) {
		LOG_ERROR("Invalid RTT control block ID");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	source.find_cb = &target_rtt_find_control_block;
	source.read_cb = &target_rtt_read_control_block;
	source.start = &target_rtt_start;
	source.stop = &target_rtt_stop;
	source.read = &target_rtt_read_callback;
	source.write = &target_rtt_write_callback;
	source.read_buffer_info = &target_rtt_read_buffer_info;

	rtt_register_source(source, get_current_target(CMD_CTX));

	global_addr = addr;
	global_length = length;
	memcpy(global_id, CMD_ARGV[2], global_id_length);
	global_changed = true;
	global_configured = true;

	return ERROR_OK;
}

COMMAND_HANDLER(handle_rtt_start_command)
{
	int ret;

	if (CMD_ARGC > 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (global_started) {
		LOG_INFO("RTT already started");
		return ERROR_OK;
	}

	if (!global_configured) {
		LOG_ERROR("RTT is not configured");
		return ERROR_FAIL;
	}

	ret = rtt_start();

	if (ret != ERROR_OK)
		return ret;

	return ERROR_OK;
}

COMMAND_HANDLER(handle_rtt_stop_command)
{
	int ret;

	if (CMD_ARGC > 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	ret = rtt_stop();

	if (ret != ERROR_OK)
		return ret;

	return ERROR_OK;
}

COMMAND_HANDLER(handle_rtt_channels_command)
{
	int ret;
	size_t i;
	char channel_name[32];
	struct rtt_buffer_info info;

	if (!global_found_cb) {
		LOG_ERROR("RTT control block not available");
		return ERROR_FAIL;
	}

	command_print(CMD_CTX, "Channels: up=%u, down=%u",
		global_ctrl.num_up_buffers, global_ctrl.num_down_buffers);

	LOG_INFO("Up-channels:");

	info.name = channel_name;
	info.name_length = sizeof(channel_name);

	for (i = 0; i < global_ctrl.num_up_buffers; i++) {
		ret = global_source.read_buffer_info(&global_ctrl, i,
			RTT_CHANNEL_TYPE_UP, &info, global_target, NULL);

		if (ret != ERROR_OK)
			return ret;

		if (!info.size)
			continue;

		LOG_INFO("%zu: %s %u %u", i, info.name, info.size, info.flags);
	}

	LOG_INFO("Down-channels:");

	for (i = 0; i < global_ctrl.num_down_buffers; i++) {
		ret = global_source.read_buffer_info(&global_ctrl, i,
			RTT_CHANNEL_TYPE_DOWN, &info, global_target, NULL);

		if (ret != ERROR_OK)
			return ret;

		if (!info.size)
			continue;

		LOG_INFO("%zu: %s %u %u", i, info.name, info.size, info.flags);
	}

	return ERROR_OK;
}

static int jim_channel_list(Jim_Interp *interp, int argc,
		Jim_Obj * const *argv)
{
	int ret;
	size_t i;
	Jim_Obj *list;
	Jim_Obj *channel_list;
	char channel_name[128];
	struct rtt_buffer_info info;

	if (!global_found_cb) {
		LOG_ERROR("RTT control block not available");
		return ERROR_FAIL;
	}

	info.name = channel_name;
	info.name_length = sizeof(channel_name);

	list = Jim_NewListObj(interp, NULL, 0);

	channel_list = Jim_NewListObj(interp, NULL, 0);

	for (i = 0; i < global_ctrl.num_up_buffers; i++) {
		ret = global_source.read_buffer_info(&global_ctrl, i,
			RTT_CHANNEL_TYPE_UP, &info, global_target, NULL);

		if (ret != ERROR_OK)
			return ret;

		if (!info.size)
			continue;

		Jim_Obj *tmp = Jim_NewListObj(interp, NULL, 0);

		Jim_ListAppendElement(interp, tmp, Jim_NewStringObj(interp,
			"name", -1));
		Jim_ListAppendElement(interp, tmp, Jim_NewStringObj(interp,
			info.name, -1));

		Jim_ListAppendElement(interp, tmp, Jim_NewStringObj(interp,
			"size", -1));
		Jim_ListAppendElement(interp, tmp, Jim_NewIntObj(interp,
			info.size));

		Jim_ListAppendElement(interp, tmp, Jim_NewStringObj(interp,
			"flags", -1));
		Jim_ListAppendElement(interp, tmp, Jim_NewIntObj(interp,
			info.flags));

		Jim_ListAppendElement(interp, channel_list, tmp);
	}

	Jim_ListAppendElement(interp, list, channel_list);

	channel_list = Jim_NewListObj(interp, NULL, 0);

	for (i = 0; i < global_ctrl.num_down_buffers; i++) {
		ret = global_source.read_buffer_info(&global_ctrl, i,
			RTT_CHANNEL_TYPE_DOWN, &info, global_target, NULL);

		if (ret != ERROR_OK)
			return ret;

		if (!info.size)
			continue;

		Jim_Obj *tmp = Jim_NewListObj(interp, NULL, 0);

		Jim_ListAppendElement(interp, tmp, Jim_NewStringObj(interp,
			"name", -1));
		Jim_ListAppendElement(interp, tmp, Jim_NewStringObj(interp,
			info.name, -1));

		Jim_ListAppendElement(interp, tmp, Jim_NewStringObj(interp,
			"size", -1));
		Jim_ListAppendElement(interp, tmp, Jim_NewIntObj(interp,
			info.size));

		Jim_ListAppendElement(interp, tmp, Jim_NewStringObj(interp,
			"flags", -1));
		Jim_ListAppendElement(interp, tmp, Jim_NewIntObj(interp,
			info.flags));

		Jim_ListAppendElement(interp, channel_list, tmp);
	}

	Jim_ListAppendElement(interp, list, channel_list);
	Jim_SetResult(interp, list);

	return JIM_OK;
}

static const struct command_registration rtt_subcommand_handlers[] = {
	{
		.name = "setup",
		.handler = handle_rtt_setup_command,
		.mode = COMMAND_ANY,
		.help = "setup RTT",
		.usage = "<address> <length> <ID>"
	},
	{
		.name = "start",
		.handler = handle_rtt_start_command,
		.mode = COMMAND_EXEC,
		.help = "start RTT",
		.usage = ""
	},
	{
		.name = "stop",
		.handler = handle_rtt_stop_command,
		.mode = COMMAND_EXEC,
		.help = "stop RTT",
		.usage = ""
	},
	{
		.name = "channels",
		.handler = handle_rtt_channels_command,
		.mode = COMMAND_EXEC,
		.help = "list available channels",
		.usage = ""
	},
	{
		.name = "channellist",
		.jim_handler = jim_channel_list,
		.mode = COMMAND_EXEC,
		.help = "list available channels",
		.usage = ""
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration rtt_command_handlers[] = {
	{
		.name = "rtt",
		.mode = COMMAND_EXEC,
		.help = "RTT commands",
		.usage = "",
		.chain = rtt_subcommand_handlers
	},
	COMMAND_REGISTRATION_DONE
};

int rtt_register_commands(struct command_context *ctx)
{
	return register_commands(ctx, NULL, rtt_command_handlers);
}
