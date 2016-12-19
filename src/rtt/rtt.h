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

#ifndef OPENOCD_RTT_RTT_H
#define OPENOCD_RTT_RTT_H

#include <stdint.h>
#include <stdbool.h>

#include <helper/command.h>
#include <target/target.h>

#define RTT_MAX_CB_ID_LENGTH	16
#define RTT_MIN_BUFFER_SIZE	2
#define RTT_CB_LENGTH		(RTT_MAX_CB_ID_LENGTH + 4 + 4)
#define RTT_BUFFER_LENGTH	24

struct rtt_control {
	target_addr_t address;
	char id[RTT_MAX_CB_ID_LENGTH + 1];
	uint32_t num_up_buffers;
	uint32_t num_down_buffers;
};

struct rtt_buffer {
	target_addr_t address;
	target_addr_t name_addr;
	target_addr_t buffer_addr;
	uint32_t size;
	uint32_t write_offset;
	uint32_t read_offset;
	uint32_t flags;
};

struct rtt_buffer_info {
	char *name;
	size_t name_length;
	uint32_t size;
	uint32_t flags;
};

typedef int (*rtt_sink_read)(unsigned int channel, const uint8_t *buffer,
		size_t length, void *user_data);

struct rtt_sink_list {
	rtt_sink_read read;
	void *user_data;

	struct rtt_sink_list *next;
};

enum rtt_channel_type {
	RTT_CHANNEL_TYPE_UP,
	RTT_CHANNEL_TYPE_DOWN
};

typedef int (*rtt_source_find_ctrl_block)(target_addr_t *address,
		size_t length, const char *id, size_t id_length, bool *found,
		struct target *target, void *user_data);
typedef int (*rtt_source_read_ctrl_block)(target_addr_t address,
		struct rtt_control *ctrl_block, struct target *target,
		void *user_data);
typedef int (*rtt_source_read_buffer_info)(const struct rtt_control *ctrl,
		unsigned int channel, enum rtt_channel_type type,
		struct rtt_buffer_info *info, struct target *target, void *user_data);
typedef int (*rtt_source_start)(const struct rtt_control *ctrl,
		struct target *target, void *user_data);
typedef int (*rtt_source_stop)(struct target *target, void *user_data);
typedef int (*rtt_source_read)(const struct rtt_control *ctrl,
		struct rtt_sink_list **sinks, size_t num_channels,
		struct target *target, void *user_data);
typedef int (*rtt_source_write)(struct rtt_control *ctrl,
		unsigned int channel, const uint8_t *buffer, size_t *length,
		struct target *target, void *user_data);

struct rtt_source {
	rtt_source_find_ctrl_block find_cb;
	rtt_source_read_ctrl_block read_cb;
	rtt_source_read_buffer_info read_buffer_info;
	rtt_source_start start;
	rtt_source_stop stop;
	rtt_source_read read;
	rtt_source_write write;
};

int rtt_init(void);
int rtt_exit(void);

int rtt_register_source(const struct rtt_source source, struct target *target);

int rtt_start(void);
int rtt_stop(void);

int rtt_register_sink(unsigned int channel, rtt_sink_read read,
		void *user_data);
int rtt_unregister_sink(unsigned int channel, rtt_sink_read read,
		void *user_data);

int rtt_write_channel(unsigned int channel, const uint8_t *buffer,
		size_t *length);

int rtt_register_commands(struct command_context *ctx);

#endif /* OPENOCD_RTT_RTT_H */
