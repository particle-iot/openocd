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

#ifndef OPENOCD_TARGET_RTT_H
#define OPENOCD_TARGET_RTT_H

#include <stdint.h>
#include <stdbool.h>

#include <target/target.h>
#include <rtt/rtt.h>

int target_rtt_start(const struct rtt_control *ctrl, struct target *target,
		void *user_data);
int target_rtt_stop(struct target *target, void *user_data);
int target_rtt_find_control_block(target_addr_t *address, size_t length,
		const char *id, size_t id_length, bool *found, struct target *target,
		void *uer_data);
int target_rtt_read_control_block(target_addr_t address,
		struct rtt_control *ctrl, struct target *target, void *user_data);
int target_rtt_write_callback(struct rtt_control *ctrl,
		unsigned int channel, const uint8_t *buffer, size_t *length,
		struct target *target, void *user_data);
int target_rtt_read_callback(const struct rtt_control *ctrl,
		struct rtt_sink_list **sinks, size_t length, struct target *target,
		void *user_data);
int target_rtt_read_buffer_info(const struct rtt_control *ctrl,
		unsigned int channel, enum rtt_channel_type type,
		struct rtt_buffer_info *info, struct target *target, void *user_data);

#endif /* OPENOCD_TARGET_RTT_H */
