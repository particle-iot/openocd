/***************************************************************************
 *   Copyright (C) 2015  Andrey Yurovsky <yurovsky@gmail.com>              *
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
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <target/target.h>
#include <target/cortex_m.h>

#define SAMD_DCC_DEFAULT_PORT	22000

#define SAMD_DSU		0x41002000	/* Device Service Unit */

/* DSU registers as offsets from SAMD_DSU */
#define SAMD_DSU_CTRLB		0x02
#define SAMD_DSU_DCC		0x10

struct dcc_channel {
	uint8_t id; /* DCC number */
	unsigned int width; /* in bytes: 1, 2, or 4 */
	bool is_running; /* We're polling this channel */
	bool is_write; /* We're writing to this channel (otherwise reading) */
	struct target *target;
};

static struct dcc_channel dcc_channels[] = {
	[0] = { /* Maps to DSU.DCC[0] */
		.id = 0,
	},
	[1] = { /* Maps to DSU.DCC[1] */
		.id = 1,
	},
};

static struct {
	int s; /* Our listening socket */
	int client_s; /* A connected client socket */
	bool is_listening; /* We're actively listening (server enabled) */
} samd_dcc;

/* Pick up a client if we don't have one.  Returns true if we have a client. */
static bool dcc_client(void)
{
	if (samd_dcc.s > -1 && samd_dcc.client_s == -1) {
		/* Try to accept a connection (if any) */
		samd_dcc.client_s = accept(samd_dcc.s, NULL, NULL);
		/* We will poll this socket rather than blocking on it. */
		if (samd_dcc.client_s != -1)
			fcntl(samd_dcc.client_s, F_SETFL, O_NONBLOCK);
	}

	return (samd_dcc.client_s != -1);
}

/* Read from our client socket and write to the corresponding DCC. */
static void dcc_write_channel(struct dcc_channel *ch)
{
	uint32_t reg = SAMD_DSU + SAMD_DSU_DCC + (ch->id * sizeof(uint32_t));

	switch (ch->width) {
		case 1:
			{
				uint8_t db;
				if (recv(samd_dcc.client_s,
					&db, sizeof(db), 0) == sizeof(db)) {
					target_write_u8(ch->target, reg, db);
				}
			}
			break;
		case 2:
			{
				uint16_t dh;
				if (recv(samd_dcc.client_s,
					&dh, sizeof(dh), 0) == sizeof(dh)) {
					target_write_u16(ch->target, reg, dh);
				}
			}
			break;
		case 4:
			{
				uint32_t dw;
				if (recv(samd_dcc.client_s,
					&dw, sizeof(dw), 0) == sizeof(dw)) {
					target_write_u32(ch->target, reg, dw);
				}
			}
			break;
	}
}

/* Read from the corresponding DCC and write to our client socket. */
static void dcc_read_channel(struct dcc_channel *ch)
{
	uint32_t reg = SAMD_DSU + SAMD_DSU_DCC + (ch->id * sizeof(uint32_t));

	switch (ch->width) {
		case 1:
			{
				uint8_t db;
				if (target_read_u8(ch->target,
						reg, &db) == ERROR_OK) {
					send(samd_dcc.client_s,
						&db, sizeof(db), 0);
				}
			}
			break;
		case 2:
			{
				uint16_t dh;
				if (target_read_u16(ch->target,
						reg, &dh) == ERROR_OK) {
					send(samd_dcc.client_s,
						&dh, sizeof(dh), 0);
				}
			}
			break;
		case 4:
			{
				uint32_t dw;
				if (target_read_u32(ch->target,
						reg, &dw) == ERROR_OK) {
					send(samd_dcc.client_s,
						&dw, sizeof(dw), 0);
				}
			}
			break;
	}
}

/* Timer callback: see if this DCC is ready for I/O.  If we're reading, that
 * means the dirty bit is set.  If we're writing that means the dirty bit is
 * cleared. */
static int dcc_poll_channel(void *priv)
{
	if (!dcc_client())
		return ERROR_OK;

	struct dcc_channel *ch = priv;

	uint8_t status;
	int ret = target_read_u8(ch->target,
			SAMD_DSU + SAMD_DSU_CTRLB, &status);
	if (ret != ERROR_OK)
		return ret;

	bool is_dirty = (status & (1 << (ch->id + 2)));

	if (ch->is_write && !is_dirty) /* we can write */
		dcc_write_channel(ch);
	else if (!ch->is_write && is_dirty) /* we can read */
		dcc_read_channel(ch);

	return ERROR_OK;
}

static void start_dcc_channel(unsigned int channel)
{
	target_register_timer_callback(dcc_poll_channel, 1, 1,
			&dcc_channels[channel]);
	dcc_channels[channel].is_running = true;
}

static void stop_dcc_channel(unsigned int channel)
{
	target_unregister_timer_callback(dcc_poll_channel,
			&dcc_channels[channel]);
	dcc_channels[channel].is_running = false;
}

COMMAND_HANDLER(handle_samd_dcc_channel_command)
{
	if (CMD_ARGC < 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	int channel = atoi(CMD_ARGV[0]);
	if (channel != 0 && channel != 1) {
		command_print(CMD_CTX, "Channel must be 0 or 1");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	struct dcc_channel *ch = &dcc_channels[channel];

	/* Just stop the channel, we're done. */
	if (!strcmp(CMD_ARGV[1], "off")) {
		stop_dcc_channel(channel);
		return ERROR_OK;
	}

	bool is_write;
	/* Set up a read or write channel by opening the corresponding file. */
	if (!strcmp(CMD_ARGV[1], "read") && CMD_ARGC >= 2)
		is_write = false;
	else if (!strcmp(CMD_ARGV[1], "write") && CMD_ARGC >= 2)
		is_write = true;
	else
		return ERROR_COMMAND_SYNTAX_ERROR;

	/* Figure out the "width" for this channel.  Assume one byte if nothing
	 * was specified. */
	ch->width = 1;
	if (CMD_ARGC >= 3 && strlen(CMD_ARGV[2]) == 1) {
		switch (CMD_ARGV[2][0]) {
			case 'w':
				ch->width = 4;
				break;

			case 'h':
				ch->width = 2;
				break;

			case 'b':
				ch->width = 1;
				break;

			default:
				command_print(CMD_CTX,
					"Width must be one of b, h, or w");
				break;
		}
	}

	if (ch->is_running)
		stop_dcc_channel(channel);

	ch->is_write = is_write;

	/* Now try to start the channel and begin streaming data. */
	ch->target = get_current_target(CMD_CTX);
	start_dcc_channel(channel);

	command_print(CMD_CTX, "DCC channel started");
	return ERROR_OK;
}

static void stop_dcc_server(void)
{
	if (samd_dcc.client_s > -1) {
		close(samd_dcc.client_s);
		samd_dcc.client_s = -1;
	}

	if (samd_dcc.s > -1) {
		close(samd_dcc.s);
		samd_dcc.s = -1;
	}

	samd_dcc.is_listening = false;
}

static int start_dcc_server(unsigned int port)
{
	stop_dcc_server();

	/* Create a TCP socket. */
	samd_dcc.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (samd_dcc.s == -1)
		return ERROR_FAIL;

	/* We'll poll for connections rather than blocking. */
	fcntl(samd_dcc.s, F_SETFL, O_NONBLOCK);

	/* We'll reuse this address. */
	int opt = 1;
	if (setsockopt(samd_dcc.s, SOL_SOCKET, SO_REUSEADDR,
				(void *)&opt, sizeof(opt)) == -1) {
		return ERROR_FAIL;
	}

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(port);

	if (bind(samd_dcc.s, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
		close(samd_dcc.s);
		samd_dcc.s = -1;
		return ERROR_FAIL;
	}

	if (listen(samd_dcc.s, 1) == -1) {
		close(samd_dcc.s);
		samd_dcc.s = -1;
		return ERROR_FAIL;
	}

	samd_dcc.is_listening = true;
	return ERROR_OK;
}

COMMAND_HANDLER(handle_samd_dcc_server_command)
{
	if (CMD_ARGC >= 1) {
		bool enable;
		COMMAND_PARSE_ENABLE(CMD_ARGV[0], enable);

		if (enable && !samd_dcc.is_listening) {
			uint32_t port = SAMD_DCC_DEFAULT_PORT;
			if (CMD_ARGC >= 2)
				COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], port);

			if (start_dcc_server(port) != ERROR_OK) {
				command_print(CMD_CTX,
					"Failed to start DCC server");
				return ERROR_FAIL;
			}
		} else if (!enable && samd_dcc.is_listening) {
			stop_dcc_server();
			command_print(CMD_CTX, "DCC server stopped");
		}
	}

	if (samd_dcc.is_listening)
		command_print(CMD_CTX, "DCC server is running");
	else
		command_print(CMD_CTX, "DCC sever is not running");

	return ERROR_OK;
}

const struct command_registration samd_dcc_command_handlers[] = {
	{
		.name = "server",
		.handler = handle_samd_dcc_server_command,
		.mode = COMMAND_ANY,
		.help = "Enable or disable SAMD DCC server",
		.usage = "<enable|disable> (port)",
	},
	{
		.name = "channel",
		.handler = handle_samd_dcc_channel_command,
		.mode = COMMAND_ANY,
		.help = "Enable or disable SAMD DCC channel",
		.usage = "<0|1> <read|write|off> (b|h|w)",
	},
	COMMAND_REGISTRATION_DONE
};

const struct command_registration at91samd_dcc_command_handlers[] = {
	{
		.name = "dcc",
		.mode = COMMAND_ANY,
		.help = "dcc command group",
		.usage = "",
		.chain = samd_dcc_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};
