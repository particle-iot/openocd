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
 ***************************************************************************/

#ifndef ARMV7M_TRACE_H
#define ARMV7M_TRACE_H

#include <command.h>

/**
 * @file
 * Holds the interface to TPIU, ITM and DWT configuration functions.
 */

enum trace_config_type {
	DISABLED,	/**< tracing is disabled */
	EXTERNAL,	/**< trace output is captured externally */
	INTERNAL	/**< trace output is handled by OpenOCD adapter driver */
};

enum tpio_pin_protocol {
	SYNC,			/**< synchronous trace output */
	ASYNC_MANCHESTER,	/**< asynchronous output with Manchester coding */
	ASYNC_UART		/**< asynchronous output with NRZ coding */
};

struct armv7m_trace_config {
	/** Currently active trace capture mode */
	enum trace_config_type config_type;

	/** Currently active trace output mode */
	enum tpio_pin_protocol pin_protocol;
	/** TPIU formatter enable/disable (in async mode) */
	bool formatter;
	/** Synchronous output port width */
	uint32_t port_size;

	/** Bitmask of currenty enabled ITM stimuli */
	uint32_t itm_ter[8];
	/** Identifier for multi-source trace stream formatting */
	unsigned int trace_bus_id;

	/** Current frequency of TRACECLKIN (usually matches HCLK) */
	unsigned int traceclkin_freq;
	/** Current frequency of trace port */
	unsigned int trace_freq;
	/** Handle to output trace data in INTERNAL capture mode */
	FILE *trace_file;
};

extern const struct command_registration armv7m_trace_command_handlers[];

/**
 * Configure hardware accordingly to the current TPIU target settings
 */
int armv7m_trace_tpiu_config(struct target *target);
/**
 * Configure hardware accordingly to the current ITM target settings
 */
int armv7m_trace_itm_config(struct target *target);

#endif
