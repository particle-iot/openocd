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

#ifndef OPENOCD_TARGET_ARMV7M_TRACE_H
#define OPENOCD_TARGET_ARMV7M_TRACE_H

#include <target/target.h>
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

enum itm_ts_prescaler {
	ITM_TS_PRESCALE1,	/**< no prescaling for the timestamp counter */
	ITM_TS_PRESCALE4,	/**< refclock divided by 4 for the timestamp counter */
	ITM_TS_PRESCALE16,	/**< refclock divided by 16 for the timestamp counter */
	ITM_TS_PRESCALE64,	/**< refclock divided by 64 for the timestamp counter */
};

enum itm_gts_freq {
	ITM_GTS_DISABLE,
	ITM_GTS_128CYCLE,
	ITM_GTS_8192CYCLE,
	ITM_GTS_FIFOEMPTY,
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

	struct {
		/** Enables the ITM */
		bool itmena;
		/** Enables Local timestamp generation */
		bool tsena;
		/** Enables Synchronization packet transmission for a synchronous TPIU */
		bool syncena;
		/** Enables forwarding of hardware event packet from the DWT unit to the ITM for output to the TPIU */
		bool txena;
		/** Enables asynchronous clocking of the timestamp counter */
		bool swoena;
		/** Local timestamp prescaler, used with the trace packet reference clock */
		enum itm_ts_prescaler tsprescale;
		/* Global timestamp frequency */
		enum itm_gts_freq gtsfreq;
		/** Identifier for multi-source trace stream formatting */
		unsigned int tracebusid;
	} itm_tcr;

	/** Enable async timestamps model */
	bool itm_async_timestamps;

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

#endif /* OPENOCD_TARGET_ARMV7M_TRACE_H */
