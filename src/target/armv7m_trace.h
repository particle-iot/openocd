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

#ifndef ARMV7M_TRACE_H
#define ARMV7M_TRACE_H

#include "command.h"

enum trace_config_type {
	NONE,
	EXTERNAL,
	INTERNAL
};

enum tpio_pin_protocol {
	SYNC,
	ASYNC_MANCHESTER,
	ASYNC_UART
};

struct armv7m_trace_config {
	enum trace_config_type config_type;

	enum tpio_pin_protocol pin_protocol;
	bool formatter;
	uint32_t port_size;
	uint16_t prescaler;

	uint32_t itm_ter[8];
	unsigned int trace_bus_id;

	FILE *trace_file;
	unsigned int trace_freq;
};

extern const struct command_registration armv7m_trace_command_handlers[];

int armv7m_trace_tpiu_config(struct target *target);
int armv7m_trace_itm_config(struct target *target);

#endif
