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

#include <stddef.h>
#include <stdint.h>
#include <helper/log.h>
#include <helper/binarybuffer.h>
#include <helper/command.h>
#include <rtt/rtt.h>

#include "target.h"

static uint8_t rtt_buffer[1024];

static int read_rtt_buffer(struct target *target,
		const struct rtt_control *ctrl, unsigned int channel,
		enum rtt_channel_type type, struct rtt_buffer *buffer)
{
	int ret;
	uint8_t buf[RTT_BUFFER_LENGTH];
	target_addr_t address;

	address = ctrl->address + RTT_CB_LENGTH + (channel * RTT_BUFFER_LENGTH);

	if (type == RTT_CHANNEL_TYPE_DOWN)
		address += ctrl->num_up_buffers * RTT_BUFFER_LENGTH;

	ret = target_read_buffer(target, address, RTT_BUFFER_LENGTH, buf);

	if (ret != ERROR_OK)
		return ret;

	buffer->address = address;
	buffer->name_addr = buf_get_u32(buf, 0, 32);
	buffer->buffer_addr = buf_get_u32(buf + 4, 0, 32);
	buffer->size = buf_get_u32(buf + 8, 0, 32);
	buffer->write_offset = buf_get_u32(buf + 12, 0, 32);
	buffer->read_offset = buf_get_u32(buf + 16, 0, 32);
	buffer->flags = buf_get_u32(buf + 20, 0, 32);

	return ERROR_OK;
}

int target_rtt_start(const struct rtt_control *ctrl, struct target *target,
		void *user_data)
{
	return ERROR_OK;
}

int target_rtt_stop(struct target *target, void *user_data)
{
	return ERROR_OK;
}

static int read_buffer_name(struct target *target, target_addr_t address,
		char *name, size_t length)
{
	size_t offset;

	offset = 0;

	while (offset < length) {
		int ret;
		size_t tmp;

		tmp = MIN(32, length - offset);
		ret = target_read_buffer(target, address + offset, tmp,
			(uint8_t *)name + offset);

		if (ret != ERROR_OK)
			return ret;

		if (memchr(name + offset, '\0', tmp))
			return ERROR_OK;

		offset += tmp;
	}

	name[length - 1] = '\0';

	return ERROR_OK;
}

static int write_to_channel(struct target *target,
		const struct rtt_buffer *rttbuf, const uint8_t *buffer, size_t *length)
{
	int ret;
	uint32_t len;

	if (!*length)
		return ERROR_OK;

	if (rttbuf->write_offset == rttbuf->read_offset) {
		uint32_t first_length;

		len = MIN(*length, rttbuf->size - 1);
		first_length = MIN(len, rttbuf->size - rttbuf->write_offset);

		ret = target_write_buffer(target,
			rttbuf->buffer_addr + rttbuf->write_offset, first_length, buffer);

		if (ret != ERROR_OK)
			return ret;

		ret = target_write_buffer(target, rttbuf->buffer_addr,
			len - first_length, buffer + first_length);

		if (ret != ERROR_OK)
			return ret;
	} else if (rttbuf->write_offset < rttbuf->read_offset) {
		len = MIN(*length, rttbuf->read_offset - rttbuf->write_offset - 1);

		if (!len) {
			*length = 0;
			return ERROR_OK;
		}

		ret = target_write_buffer(target,
			rttbuf->buffer_addr + rttbuf->write_offset, len, buffer);

		if (ret != ERROR_OK)
			return ret;
	} else {
		uint32_t first_length;

		len = MIN(*length,
			rttbuf->size - rttbuf->write_offset + rttbuf->read_offset - 1);

		if (!len) {
			*length = 0;
			return ERROR_OK;
		}

		first_length = MIN(len, rttbuf->size - rttbuf->write_offset);

		ret = target_write_buffer(target,
			rttbuf->buffer_addr + rttbuf->write_offset, first_length, buffer);

		if (ret != ERROR_OK)
			return ret;

		buffer = buffer + first_length;

		ret = target_write_buffer(target, rttbuf->buffer_addr,
			len - first_length, buffer);

		if (ret != ERROR_OK)
			return ret;
	}

	ret = target_write_u32(target, rttbuf->address + 12,
		(rttbuf->write_offset + len) % rttbuf->size);

	if (ret != ERROR_OK)
		return ret;

	*length = len;

	return ERROR_OK;
}

static bool buffer_is_active(const struct rtt_buffer *buf)
{
	if (!buf)
		return false;

	if (!buf->size)
		return false;

	return true;
}

int target_rtt_write_callback(struct rtt_control *ctrl,
		unsigned int channel, const uint8_t *buffer, size_t *length,
		struct target *target, void *user_data)
{
	int ret;
	struct rtt_buffer rttbuf;

	ret = read_rtt_buffer(target, ctrl, channel, RTT_CHANNEL_TYPE_DOWN,
		&rttbuf);

	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read RTT buffer of down-channel %u", channel);
		return ret;
	}

	if (!buffer_is_active(&rttbuf)) {
		LOG_WARNING("Down-channel %u is not active", channel);
		return ERROR_OK;
	}

	if (rttbuf.size < RTT_MIN_BUFFER_SIZE) {
		LOG_WARNING("Down-channel %u is not large enough", channel);
		return ERROR_OK;
	}

	ret = write_to_channel(target, &rttbuf, buffer,	length);

	if (ret != ERROR_OK)
		return ret;

	LOG_DEBUG("Wrote %zu bytes into RTT down-channel %u", *length, channel);

	return ERROR_OK;
}

int target_rtt_read_control_block(target_addr_t address,
		struct rtt_control *ctrl, struct target *target, void *user_data)
{
	int ret;
	uint8_t buf[RTT_CB_LENGTH];

	ret = target_read_buffer(target, address, RTT_CB_LENGTH, buf);

	if (ret != ERROR_OK)
		return ret;

	memcpy(ctrl->id, buf, RTT_MAX_CB_ID_LENGTH);
	ctrl->id[RTT_MAX_CB_ID_LENGTH] = '\0';
	ctrl->num_up_buffers = buf_get_u32(buf + RTT_MAX_CB_ID_LENGTH, 0, 32);
	ctrl->num_down_buffers = buf_get_u32(buf + RTT_MAX_CB_ID_LENGTH + 4, 0,
		32);

	return ERROR_OK;
}

int target_rtt_find_control_block(target_addr_t *address, size_t length,
		const char *id, size_t id_length, bool *found, struct target *target,
		void *user_data)
{
	target_addr_t addr;
	uint8_t buf[1024];
	size_t j;
	size_t start;

	*found = false;

	j = 0;
	start = 0;

	LOG_INFO("Searching for RTT control block '%s'", id);

	for (addr = 0; addr < length; addr = addr + sizeof(buf)) {
		int ret;
		size_t i;

		ret = target_read_buffer(target, *address + addr, sizeof(buf), buf);

		if (ret != ERROR_OK)
			return ret;

		for (i = 0; i < sizeof(buf); i++) {
			if (buf[i] == id[j]) {
				j++;
			} else {
				j = 0;
				start = addr + i + 1;
			}

			if (j == id_length) {
				*address = *address + start;
				*found = true;
				return ERROR_OK;
			}
		}
	}

	return ERROR_OK;
}

int target_rtt_read_buffer_info(const struct rtt_control *ctrl,
		unsigned int channel, enum rtt_channel_type type,
		struct rtt_buffer_info *info, struct target *target, void *user_data)
{
	int ret;
	struct rtt_buffer rttbuf;

	ret = read_rtt_buffer(target, ctrl, channel, type, &rttbuf);

	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to read RTT buffer of channel %u", channel);
		return ret;
	}

	ret = read_buffer_name(target, rttbuf.name_addr, info->name,
		info->name_length);

	if (ret != ERROR_OK)
		return ret;

	info->size = rttbuf.size;
	info->flags = rttbuf.flags;

	return ERROR_OK;
}

static int read_from_channel(struct target *target,
		const struct rtt_buffer *rttbuf, uint8_t *buffer, size_t *length)
{
	int ret;
	uint32_t len;

	if (!*length)
		return ERROR_OK;

	if (rttbuf->read_offset == rttbuf->write_offset) {
		len = 0;
	} else if (rttbuf->read_offset < rttbuf->write_offset) {
		len = MIN(*length, rttbuf->write_offset - rttbuf->read_offset);

		ret = target_read_buffer(target,
			rttbuf->buffer_addr + rttbuf->read_offset, len, buffer);

		if (ret != ERROR_OK)
			return ret;
	} else {
		uint32_t first_length;

		len = MIN(*length,
			rttbuf->size - rttbuf->read_offset + rttbuf->write_offset);
		first_length = MIN(len, rttbuf->size - rttbuf->read_offset);

		ret = target_read_buffer(target,
			rttbuf->buffer_addr + rttbuf->read_offset, first_length, buffer);

		if (ret != ERROR_OK)
			return ret;

		ret = target_read_buffer(target, rttbuf->buffer_addr,
			len - first_length, buffer + first_length);

		if (ret != ERROR_OK)
			return ret;
	}

	if (len > 0) {
		ret = target_write_u32(target, rttbuf->address + 16,
			(rttbuf->read_offset + len) % rttbuf->size);

		if (ret != ERROR_OK)
			return ret;
	}

	*length = len;

	return ERROR_OK;
}

int target_rtt_read_callback(const struct rtt_control *ctrl,
		struct rtt_sink_list **sinks, size_t num_channels,
		struct target *target, void *user_data)
{
	size_t channel;

	num_channels = MIN(num_channels, ctrl->num_up_buffers);

	for (channel = 0; channel < num_channels; channel++) {
		int ret;
		struct rtt_buffer rttbuf;
		size_t length;
		struct rtt_sink_list *tmp;

		if (!sinks[channel])
			continue;

		ret = read_rtt_buffer(target, ctrl, channel, RTT_CHANNEL_TYPE_UP,
			&rttbuf);

		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to read RTT buffer of up-channel %zu", channel);
			return ret;
		}

		if (!buffer_is_active(&rttbuf)) {
			LOG_WARNING("Up-channel %zu is not active", channel);
			continue;
		}

		if (rttbuf.size < RTT_MIN_BUFFER_SIZE) {
			LOG_WARNING("Up-channel %zu is not large enough", channel);
			continue;
		}

		length = sizeof(rtt_buffer);
		ret = read_from_channel(target, &rttbuf, rtt_buffer, &length);

		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to read from RTT up-channel %zu", channel);
			return ret;
		}

		for (tmp = sinks[channel]; tmp; tmp = tmp->next)
			tmp->read(channel, rtt_buffer, length, tmp->user_data);
	}

	return ERROR_OK;
}
