/**************************************************************************
*   Copyright (C) 2012 by Andreas Fritiofson                              *
*   andreas.fritiofson@gmail.com                                          *
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

/**
 * @file
 * JTAG adapters based on the FT2232 full and high speed USB parts are
 * popular low cost JTAG debug solutions.  Many FT2232 based JTAG adapters
 * are discrete, but development boards may integrate them as alternatives
 * to more capable (and expensive) third party JTAG pods.
 *
 * JTAG uses only one of the two communications channels ("MPSSE engines")
 * on these devices.  Adapters based on FT4232 parts have four ports/channels
 * (A/B/C/D), instead of just two (A/B).
 *
 * Especially on development boards integrating one of these chips (as
 * opposed to discrete pods/dongles), the additional channels can be used
 * for a variety of purposes, but OpenOCD only uses one channel at a time.
 *
 *  - As a USB-to-serial adapter for the target's console UART ...
 *    which may be able to support ROM boot loaders that load initial
 *    firmware images to flash (or SRAM).
 *
 *  - On systems which support ARM's SWD in addition to JTAG, or instead
 *    of it, that second port can be used for reading SWV/SWO trace data.
 *
 *  - Additional JTAG links, e.g. to a CPLD or * FPGA.
 *
 * FT2232 based JTAG adapters are "dumb" not "smart", because most JTAG
 * request/response interactions involve round trips over the USB link.
 * A "smart" JTAG adapter has intelligence close to the scan chain, so it
 * can for example poll quickly for a status change (usually taking on the
 * order of microseconds not milliseconds) before beginning a queued
 * transaction which require the previous one to have completed.
 *
 * There are dozens of adapters of this type, differing in details which
 * this driver needs to understand.  Those "layout" details are required
 * as part of FT2232 driver configuration.
 *
 * This code uses information contained in the MPSSE specification which was
 * found here:
 * http://www.ftdichip.com/Documents/AppNotes/AN2232C-01_MPSSE_Cmnd.pdf
 * Hereafter this is called the "MPSSE Spec".
 *
 * The datasheet for the ftdichip.com's FT2232D part is here:
 * http://www.ftdichip.com/Documents/DataSheets/DS_FT2232D.pdf
 *
 * Also note the issue with code 0x4b (clock data to TMS) noted in
 * http://developer.intra2net.com/mailarchive/html/libftdi/2009/msg00292.html
 * which can affect longer JTAG state paths.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* project specific includes */
#include <jtag/interface.h>
#include <transport/transport.h>
#include <helper/time_support.h>

#if IS_CYGWIN == 1
#include <windows.h>
#endif

#include <assert.h>

/* FTDI access library includes */
#include "mpsse.h"

/* max TCK for the full speed devices 6000 kHz */
#define FTDI_2232C_MAX_TCK 6000
/* this speed value tells that RTCK is requested */
#define RTCK_SPEED -1

#define JTAG_MODE (LSB_FIRST | POS_EDGE_IN | NEG_EDGE_OUT)

static char *ftdi_device_desc;
static char *ftdi_serial;
static uint8_t ftdi_latency = 255;
static unsigned ftdi_max_tck = FTDI_2232C_MAX_TCK;

#define MAX_USB_IDS 8
/* vid = pid = 0 marks the end of the list */
static uint16_t ftdi_vid[MAX_USB_IDS + 1] = { 0x0403, 0 };
static uint16_t ftdi_pid[MAX_USB_IDS + 1] = { 0x6010, 0 };

struct ftdi_layout {
	char *name;
	int (*init)(void);
	void (*reset)(int trst, int srst);
	void (*blink)(void);
	int channel;
};

/* init procedures for supported layouts */
static int jtagkey_init(void);

/* reset procedures for supported layouts */
static void jtagkey_reset(int trst, int srst);

static const struct ftdi_layout ftdi_layouts[] = {
	{ .name = "jtagkey",
	  .init = jtagkey_init,
	  .reset = jtagkey_reset,},
	{ .name = NULL, /* END OF TABLE */ },
};

/* bitmask used to drive nTRST; usually a GPIOLx signal */
static uint8_t nTRST;
static uint8_t nTRSTnOE;
/* bitmask used to drive nSRST; usually a GPIOLx signal */
static uint8_t nSRST;
static uint8_t nSRSTnOE;

/** the layout being used with this debug session */
static const struct ftdi_layout *layout;

/** default bitmask values driven on DBUS: TCK/TDI/TDO/TMS and GPIOL(0..4) */
static uint8_t low_output;

/* note that direction bit == 1 means that signal is an output */

/** default direction bitmask for DBUS: TCK/TDI/TDO/TMS and GPIOL(0..4) */
static uint8_t low_direction;
/** default value bitmask for CBUS GPIOH(0..4) */
static uint8_t high_output;
/** default direction bitmask for CBUS GPIOH(0..4) */
static uint8_t high_direction;

static struct mpsse_ctx *mpsse_ctx;

/**
 * Function move_to_state
 * moves the TAP controller from the current state to a
 * \a goal_state through a path given by tap_get_tms_path().  State transition
 * logging is performed by delegation to clock_tms().
 *
 * @param goal_state is the destination state for the move.
 */
static int move_to_state(tap_state_t goal_state)
{
	tap_state_t start_state = tap_get_state();

	/*	goal_state is 1/2 of a tuple/pair of states which allow convenient
		lookup of the required TMS pattern to move to this state from the
		start state.
	*/

	/* do the 2 lookups */
	int tms_bits  = tap_get_tms_path(start_state, goal_state);
	int tms_count = tap_get_tms_path_len(start_state, goal_state);

	DEBUG_JTAG_IO("start=%s goal=%s", tap_state_name(start_state), tap_state_name(goal_state));

	/* Track state transitions step by step */
	for (int i = 0; i < tms_count; i++)
		tap_set_state(tap_state_transition(tap_get_state(), (tms_bits >> i) & 1));

	return mpsse_clock_tms_cs_out(mpsse_ctx,
		(uint8_t *)&tms_bits,
		0,
		tms_count,
		false,
		JTAG_MODE);
}

static int ftdi_speed(int speed)
{
	int retval;

	retval = ERROR_OK;
	bool enable_adaptive_clocking = (RTCK_SPEED == speed);
	/* TODO: just try to set it through mpsse, if it fails it's not supported */
	if (enable_adaptive_clocking) {
		LOG_ERROR("BUG: RTCK is not implemented");
		return ERROR_FAIL;
	}

	/* TODO: automatically set/clear divide-by-5 on H-chips to satisfy speed */
	retval = mpsse_set_divisor(mpsse_ctx, speed);

	if (retval != ERROR_OK) {
		LOG_ERROR("couldn't set FTDI TCK speed");
		return retval;
	}

	return ERROR_OK;
}

static int ftdi_speed_div(int speed, int *khz)
{
	/* Take a look in the FTDI manual,
	 * AN2232C-01 Command Processor for
	 * MPSSE and MCU Host Bus. Chapter 3.8 */

	*khz = (RTCK_SPEED == speed) ? 0 : ftdi_max_tck / (1 + speed);


	return ERROR_OK;
}

static int ftdi_khz(int khz, int *jtag_speed)
{
	/* TODO: Let MPSSE handle this */
	if (khz == 0) {
		LOG_ERROR("BUG: RTCK is not implemented");
		return ERROR_FAIL;
	}

	/* Take a look in the FT2232 manual,
	 * AN2232C-01 Command Processor for
	 * MPSSE and MCU Host Bus. Chapter 3.8
	 *
	 * We will calc here with a multiplier
	 * of 10 for better rounding later. */

	/* Calc speed, (ftdi_max_tck / khz) - 1
	 * Use 65000 for better rounding */
	*jtag_speed = ((ftdi_max_tck*10) / khz) - 10;

	/* Add 0.9 for rounding */
	*jtag_speed += 9;

	/* Calc real speed */
	*jtag_speed = *jtag_speed / 10;

	/* Check if speed is greater than 0 */
	if (*jtag_speed < 0)
		*jtag_speed = 0;

	/* Check max value */
	if (*jtag_speed > 0xFFFF)
		*jtag_speed = 0xFFFF;

	return ERROR_OK;
}

static void ftdi_end_state(tap_state_t state)
{
	if (tap_is_state_stable(state))
		tap_set_end_state(state);
	else {
		LOG_ERROR("BUG: %s is not a stable end state", tap_state_name(state));
		exit(-1);
	}
}

static void jtagkey_reset(int trst, int srst)
{
	enum reset_types jtag_reset_config = jtag_get_reset_config();
	if (trst == 1) {
		if (jtag_reset_config & RESET_TRST_OPEN_DRAIN)
			high_output &= ~nTRSTnOE;
		else
			high_output &= ~nTRST;
	} else if (trst == 0)   {
		if (jtag_reset_config & RESET_TRST_OPEN_DRAIN)
			high_output |= nTRSTnOE;
		else
			high_output |= nTRST;
	}

	if (srst == 1) {
		if (jtag_reset_config & RESET_SRST_PUSH_PULL)
			high_output &= ~nSRST;
		else
			high_output &= ~nSRSTnOE;
	} else if (srst == 0)   {
		if (jtag_reset_config & RESET_SRST_PUSH_PULL)
			high_output |= nSRST;
		else
			high_output |= nSRSTnOE;
	}

	/* command "set data bits high byte" */
	mpsse_set_data_bits_high_byte(mpsse_ctx, high_output, high_direction);
	LOG_DEBUG("trst: %i, srst: %i, high_output: 0x%2.2x, high_direction: 0x%2.2x",
		trst,
		srst,
		high_output,
		high_direction);
}

static int ftdi_execute_runtest(struct jtag_command *cmd)
{
	int retval = ERROR_OK;
	int i;
	uint8_t zero = 0;

	DEBUG_JTAG_IO("runtest %i cycles, end in %s",
		cmd->cmd.runtest->num_cycles,
		tap_state_name(cmd->cmd.runtest->end_state));

	if (tap_get_state() != TAP_IDLE)
		move_to_state(TAP_IDLE);

	/* TODO: Reuse ftdi_execute_stableclocks */
	i = cmd->cmd.runtest->num_cycles;
	while (i > 0 && retval == ERROR_OK) {
		/* there are no state transitions in this code, so omit state tracking */
		unsigned this_len = i > 7 ? 7 : i;
		retval = mpsse_clock_tms_cs_out(mpsse_ctx, &zero, 0, this_len, false, JTAG_MODE);
		i -= this_len;
	}

	ftdi_end_state(cmd->cmd.runtest->end_state);

	if (tap_get_state() != tap_get_end_state())
		move_to_state(tap_get_end_state());

	DEBUG_JTAG_IO("runtest: %i, end in %s",
		cmd->cmd.runtest->num_cycles,
		tap_state_name(tap_get_end_state()));
	return retval;
}

static int ftdi_execute_statemove(struct jtag_command *cmd)
{
	int retval = ERROR_OK;

	DEBUG_JTAG_IO("statemove end in %s",
		tap_state_name(cmd->cmd.statemove->end_state));

	ftdi_end_state(cmd->cmd.statemove->end_state);

	/* shortest-path move to desired end state */
	if (tap_get_state() != tap_get_end_state() || tap_get_end_state() == TAP_RESET)
		move_to_state(tap_get_end_state());

	return retval;
}

/**
 * Clock a bunch of TMS (or SWDIO) transitions, to change the JTAG
 * (or SWD) state machine. REVISIT: Not the best method, perhaps.
 */
static int ftdi_execute_tms(struct jtag_command *cmd)
{
	DEBUG_JTAG_IO("TMS: %d bits", cmd->cmd.tms->num_bits);

	/* TODO: Missing tap state tracking, also missing from ft2232.c! */
	return mpsse_clock_tms_cs_out(mpsse_ctx,
		cmd->cmd.tms->bits,
		0,
		cmd->cmd.tms->num_bits,
		false,
		JTAG_MODE);
}

static int ftdi_execute_pathmove(struct jtag_command *cmd)
{
	int retval = ERROR_OK;

	tap_state_t *path = cmd->cmd.pathmove->path;
	int num_states  = cmd->cmd.pathmove->num_states;

	DEBUG_JTAG_IO("pathmove: %i states, current: %s  end: %s", num_states,
		tap_state_name(tap_get_state()),
		tap_state_name(path[num_states-1]));

	int state_count = 0;
	unsigned bit_count = 0;
	uint8_t tms_byte = 0;

	DEBUG_JTAG_IO("-");

	/* this loop verifies that the path is legal and logs each state in the path */
	while (num_states-- && retval == ERROR_OK) {

		/* either TMS=0 or TMS=1 must work ... */
		if (tap_state_transition(tap_get_state(), false)
		    == path[state_count])
			buf_set_u32(&tms_byte, bit_count++, 1, 0x0);
		else if (tap_state_transition(tap_get_state(), true)
			 == path[state_count]) {
			buf_set_u32(&tms_byte, bit_count++, 1, 0x1);

			/* ... or else the caller goofed BADLY */
		} else {
			LOG_ERROR("BUG: %s -> %s isn't a valid "
				"TAP state transition",
				tap_state_name(tap_get_state()),
				tap_state_name(path[state_count]));
			exit(-1);
		}

		tap_set_state(path[state_count]);
		state_count++;

		if (bit_count == 7 || num_states == 0) {
			retval = mpsse_clock_tms_cs_out(mpsse_ctx,
					&tms_byte,
					0,
					bit_count,
					false,
					JTAG_MODE);
			bit_count = 0;
		}
	}
	tap_set_end_state(tap_get_state());

	return retval;
}

static int ftdi_execute_scan(struct jtag_command *cmd)
{
	int retval = ERROR_OK;

	DEBUG_JTAG_IO("%s type:%d", cmd->cmd.scan->ir_scan ? "IRSCAN" : "DRSCAN",
		jtag_scan_type(cmd->cmd.scan));

	if (cmd->cmd.scan->ir_scan) {
		if (tap_get_state() != TAP_IRSHIFT)
			move_to_state(TAP_IRSHIFT);
	} else {
		if (tap_get_state() != TAP_DRSHIFT)
			move_to_state(TAP_DRSHIFT);
	}

	ftdi_end_state(cmd->cmd.scan->end_state);

	struct scan_field *field = cmd->cmd.scan->fields;
	unsigned scan_size = 0;

	for (int i = 0; i < cmd->cmd.scan->num_fields; i++, field++) {
		unsigned bit_offset = 0;
		scan_size += field->num_bits;
		DEBUG_JTAG_IO("%s%s field %d/%d %d bits",
			field->in_value ? "in" : "",
			field->out_value ? "out" : "",
			i,
			cmd->cmd.scan->num_fields,
			field->num_bits);

		if (i == cmd->cmd.scan->num_fields - 1 && tap_get_state() != tap_get_end_state()) {
			/* Last field, and we're leaving IRSHIFT/DRSHIFT. Clock last bit during tap
			 *movement */
			mpsse_clock_data(mpsse_ctx,
				field->out_value,
				bit_offset,
				field->in_value,
				bit_offset,
				field->num_bits - 1,
				JTAG_MODE);
			bit_offset += field->num_bits - 1;
			uint8_t last_bit = 0;
			if (field->out_value) {
				bit_copy(&last_bit, 0, field->out_value, bit_offset, 1);
				bit_offset++;
			}
			uint8_t tms_bits = 0x01;
			retval = mpsse_clock_tms_cs(mpsse_ctx,
					&tms_bits,
					0,
					field->in_value,
					bit_offset,
					1,
					last_bit,
					JTAG_MODE);
			tap_set_state(tap_state_transition(tap_get_state(), 1));
			retval = mpsse_clock_tms_cs_out(mpsse_ctx,
					&tms_bits,
					1,
					1,
					last_bit,
					JTAG_MODE);
			tap_set_state(tap_state_transition(tap_get_state(), 0));
		} else
			mpsse_clock_data(mpsse_ctx,
				field->out_value,
				0,
				field->in_value,
				0,
				field->num_bits,
				JTAG_MODE);
		if (retval != ERROR_OK) {
			LOG_ERROR("failed to add field %d in scan", i);
			return retval;
		}
	}

	if (tap_get_state() != tap_get_end_state())
		move_to_state(tap_get_end_state());

	DEBUG_JTAG_IO("%s scan, %i bits, end in %s",
		(cmd->cmd.scan->ir_scan) ? "IR" : "DR", scan_size,
		tap_state_name(tap_get_end_state()));
	return retval;

}

static int ftdi_execute_reset(struct jtag_command *cmd)
{
	DEBUG_JTAG_IO("reset trst: %i srst %i",
		cmd->cmd.reset->trst, cmd->cmd.reset->srst);

	if (cmd->cmd.reset->trst == 1
	    || (cmd->cmd.reset->srst
		&& (jtag_get_reset_config() & RESET_SRST_PULLS_TRST)))
		tap_set_state(TAP_RESET);

	layout->reset(cmd->cmd.reset->trst, cmd->cmd.reset->srst);

	DEBUG_JTAG_IO("trst: %i, srst: %i",
		cmd->cmd.reset->trst, cmd->cmd.reset->srst);
	return ERROR_OK;
}

static int ftdi_execute_sleep(struct jtag_command *cmd)
{
	int retval = ERROR_OK;

	DEBUG_JTAG_IO("sleep %" PRIi32, cmd->cmd.sleep->us);

	retval = mpsse_flush(mpsse_ctx);
	jtag_sleep(cmd->cmd.sleep->us);
	DEBUG_JTAG_IO("sleep %" PRIi32 " usec while in %s",
		cmd->cmd.sleep->us,
		tap_state_name(tap_get_state()));
	return retval;
}

static int ftdi_execute_stableclocks(struct jtag_command *cmd)
{
	int retval = ERROR_OK;

	/* this is only allowed while in a stable state.  A check for a stable
	 * state was done in jtag_add_clocks()
	 */
	int num_cycles = cmd->cmd.stableclocks->num_cycles;

	/* 7 bits of either ones or zeros. */
	uint8_t tms = tap_get_state() == TAP_RESET ? 0x7f : 0x00;

	/* TODO: Use mpsse_clock_data with in=out=0 for this, if TMS can be set to
	 * the correct level and remain there during the scan */
	while (num_cycles > 0 && retval == ERROR_OK) {
		/* there are no state transitions in this code, so omit state tracking */
		unsigned this_len = num_cycles > 7 ? 7 : num_cycles;
		retval = mpsse_clock_tms_cs_out(mpsse_ctx, &tms, 0, this_len, false, JTAG_MODE);
		num_cycles -= this_len;
	}

	DEBUG_JTAG_IO("clocks %i while in %s",
		cmd->cmd.stableclocks->num_cycles,
		tap_state_name(tap_get_state()));
	return retval;
}

static int ftdi_execute_command(struct jtag_command *cmd)
{
	int retval;

	switch (cmd->type) {
		case JTAG_RESET:
			retval = ftdi_execute_reset(cmd);
			break;
		case JTAG_RUNTEST:
			retval = ftdi_execute_runtest(cmd);
			break;
		case JTAG_TLR_RESET:
			retval = ftdi_execute_statemove(cmd);
			break;
		case JTAG_PATHMOVE:
			retval = ftdi_execute_pathmove(cmd);
			break;
		case JTAG_SCAN:
			retval = ftdi_execute_scan(cmd);
			break;
		case JTAG_SLEEP:
			retval = ftdi_execute_sleep(cmd);
			break;
		case JTAG_STABLECLOCKS:
			retval = ftdi_execute_stableclocks(cmd);
			break;
		case JTAG_TMS:
			retval = ftdi_execute_tms(cmd);
			break;
		default:
			LOG_ERROR("BUG: unknown JTAG command type encountered: %d", cmd->type);
			retval = ERROR_JTAG_QUEUE_FAILED;
			break;
	}
	return retval;
}

static int ftdi_execute_queue(void)
{
	int retval = ERROR_OK;

	/* blink, if the current layout has that feature */
	if (layout->blink)
		layout->blink();

	for (struct jtag_command *cmd = jtag_command_queue; cmd; cmd = cmd->next) {
		/* fill the write buffer with the desired command */
		if (ftdi_execute_command(cmd) != ERROR_OK)
			retval = ERROR_JTAG_QUEUE_FAILED;
	}

	retval = mpsse_flush(mpsse_ctx);
	if (retval != ERROR_OK)
		LOG_ERROR("error while flushing MPSSE queue: %d", retval);

	return retval;
}

static int ftdi_initialize(void)
{
	int retval;

	if (tap_get_tms_path_len(TAP_IRPAUSE, TAP_IRPAUSE) == 7)
		LOG_DEBUG("ftdi interface using 7 step jtag state transitions");
	else
		LOG_DEBUG("ftdi interface using shortest path jtag state transitions");

	if (layout == NULL) {
		LOG_WARNING("No ftdi layout specified'");
		return ERROR_JTAG_INIT_FAILED;
	}

	for (int i = 0; ftdi_vid[i] || ftdi_pid[i]; i++) {
		mpsse_ctx = mpsse_open(ftdi_vid[i], ftdi_pid[i], ftdi_device_desc,
				ftdi_serial, layout->channel, ftdi_latency);
		if (mpsse_ctx)
			break;
	}

	if (!mpsse_ctx)
		return ERROR_JTAG_INIT_FAILED;

	if (layout->init() != ERROR_OK)
		return ERROR_JTAG_INIT_FAILED;

	retval = mpsse_loopback_config(mpsse_ctx, false);
	if (retval != ERROR_OK) {
		LOG_ERROR("couldn't write to FTDI to disable loopback");
		return ERROR_JTAG_INIT_FAILED;
	}

	return mpsse_flush(mpsse_ctx);
}

static int jtagkey_init(void)
{
	low_output    = 0x08;
	low_direction = 0x1b;

	/* initialize low byte for jtag */
	if (mpsse_set_data_bits_low_byte(mpsse_ctx, low_output, low_direction) != ERROR_OK) {
		LOG_ERROR("couldn't initialize FTDI with 'JTAGkey' layout");
		return ERROR_JTAG_INIT_FAILED;
	}

	if (strcmp(layout->name, "jtagkey") == 0) {
		nTRST    = 0x01;
		nTRSTnOE = 0x4;
		nSRST    = 0x02;
		nSRSTnOE = 0x08;
	} else if ((strcmp(layout->name, "jtagkey_prototype_v1") == 0)
		   || (strcmp(layout->name, "oocdlink") == 0)) {
		nTRST    = 0x02;
		nTRSTnOE = 0x1;
		nSRST    = 0x08;
		nSRSTnOE = 0x04;
	} else {
		LOG_ERROR("BUG: jtagkey_init called for non jtagkey layout");
		exit(-1);
	}

	high_output    = 0x0;
	high_direction = 0x0f;

	enum reset_types jtag_reset_config = jtag_get_reset_config();
	if (jtag_reset_config & RESET_TRST_OPEN_DRAIN) {
		high_output |= nTRSTnOE;
		high_output &= ~nTRST;
	} else {
		high_output &= ~nTRSTnOE;
		high_output |= nTRST;
	}

	if (jtag_reset_config & RESET_SRST_PUSH_PULL) {
		high_output &= ~nSRSTnOE;
		high_output |= nSRST;
	} else {
		high_output |= nSRSTnOE;
		high_output &= ~nSRST;
	}

	/* initialize high byte for jtag */
	if (mpsse_set_data_bits_high_byte(mpsse_ctx, high_output, high_direction) != ERROR_OK) {
		LOG_ERROR("couldn't initialize FTDI with 'JTAGkey' layout");
		return ERROR_JTAG_INIT_FAILED;
	}

	return ERROR_OK;
}

static int ftdi_quit(void)
{
	mpsse_close(mpsse_ctx);

	return ERROR_OK;
}

COMMAND_HANDLER(ftdi_handle_device_desc_command)
{
	if (CMD_ARGC == 1)
		ftdi_device_desc = strdup(CMD_ARGV[0]);
	else
		LOG_ERROR("expected exactly one argument to ftdi_device_desc <description>");

	return ERROR_OK;
}

COMMAND_HANDLER(ftdi_handle_serial_command)
{
	if (CMD_ARGC == 1)
		ftdi_serial = strdup(CMD_ARGV[0]);
	else
		return ERROR_COMMAND_SYNTAX_ERROR;

	return ERROR_OK;
}

COMMAND_HANDLER(ftdi_handle_layout_command)
{
	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (layout) {
		LOG_ERROR("already specified ftdi_layout %s",
			layout->name);
		return (strcmp(layout->name, CMD_ARGV[0]) != 0)
		       ? ERROR_FAIL
		       : ERROR_OK;
	}

	for (const struct ftdi_layout *l = ftdi_layouts; l->name; l++) {
		if (strcmp(l->name, CMD_ARGV[0]) == 0) {
			layout = l;
			return ERROR_OK;
		}
	}

	LOG_ERROR("No FTDI layout '%s' found", CMD_ARGV[0]);
	return ERROR_FAIL;
}

COMMAND_HANDLER(ftdi_handle_vid_pid_command)
{
	if (CMD_ARGC > MAX_USB_IDS * 2) {
		LOG_WARNING("ignoring extra IDs in ftdi_vid_pid "
			"(maximum is %d pairs)", MAX_USB_IDS);
		CMD_ARGC = MAX_USB_IDS * 2;
	}
	if (CMD_ARGC < 2 || (CMD_ARGC & 1)) {
		LOG_WARNING("incomplete ftdi_vid_pid configuration directive");
		if (CMD_ARGC < 2)
			return ERROR_COMMAND_SYNTAX_ERROR;
		/* remove the incomplete trailing id */
		CMD_ARGC -= 1;
	}

	unsigned i;
	for (i = 0; i < CMD_ARGC; i += 2) {
		COMMAND_PARSE_NUMBER(u16, CMD_ARGV[i], ftdi_vid[i >> 1]);
		COMMAND_PARSE_NUMBER(u16, CMD_ARGV[i + 1], ftdi_pid[i >> 1]);
	}

	/*
	 * Explicitly terminate, in case there are multiples instances of
	 * ftdi_vid_pid.
	 */
	ftdi_vid[i >> 1] = ftdi_pid[i >> 1] = 0;

	return ERROR_OK;
}

COMMAND_HANDLER(ftdi_handle_latency_command)
{
	if (CMD_ARGC == 1)
		ftdi_latency = atoi(CMD_ARGV[0]);
	else
		return ERROR_COMMAND_SYNTAX_ERROR;

	return ERROR_OK;
}

static const struct command_registration ftdi_command_handlers[] = {
	{
		.name = "ftdi_device_desc",
		.handler = &ftdi_handle_device_desc_command,
		.mode = COMMAND_CONFIG,
		.help = "set the USB device description of the FTDI device",
		.usage = "description_string",
	},
	{
		.name = "ftdi_serial",
		.handler = &ftdi_handle_serial_command,
		.mode = COMMAND_CONFIG,
		.help = "set the serial number of the FTDI device",
		.usage = "serial_string",
	},
	{
		.name = "ftdi_layout",
		.handler = &ftdi_handle_layout_command,
		.mode = COMMAND_CONFIG,
		.help = "set the layout of the FTDI GPIO signals used "
			"to control output-enables and reset signals",
		.usage = "layout_name",
	},
	{
		.name = "ftdi_vid_pid",
		.handler = &ftdi_handle_vid_pid_command,
		.mode = COMMAND_CONFIG,
		.help = "the vendor ID and product ID of the FTDI device",
		.usage = "(vid pid)* ",
	},
	{
		.name = "ftdi_latency",
		.handler = &ftdi_handle_latency_command,
		.mode = COMMAND_CONFIG,
		.help = "set the FTDI latency timer to a new value",
		.usage = "value",
	},
	COMMAND_REGISTRATION_DONE
};

struct jtag_interface ftdi_interface = {
	.name = "ftdi",
	.supported = DEBUG_CAP_TMS_SEQ,
	.commands = ftdi_command_handlers,
	.transports = jtag_only,

	.init = ftdi_initialize,
	.quit = ftdi_quit,
	.speed = ftdi_speed,
	.speed_div = ftdi_speed_div,
	.khz = ftdi_khz,
	.execute_queue = ftdi_execute_queue,
};
