/***************************************************************************
 *
 *   Copyright (C) 2010 by David Brownell
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ***************************************************************************/

/**
 * @file
 * Utilities to support ARM "Serial Wire Debug" (SWD), a low pin-count debug
 * link protocol used in cases where JTAG is not wanted.  This is coupled to
 * recent versions of ARM's "CoreSight" debug framework.  This specific code
 * is a transport level interface, with "target/arm_adi_v5.[hc]" code
 * understanding operation semantics, shared with the JTAG transport.
 *
 * Single-DAP support only.
 *
 * for details, see "ARM IHI 0031A"
 * ARM Debug Interface v5 Architecture Specification
 * especially section 5.3 for SWD protocol
 *
 * On many chips (most current Cortex-M3 parts) SWD is a run-time alternative
 * to JTAG.  Boards may support one or both.  There are also SWD-only chips,
 * (using SW-DP not SWJ-DP).
 *
 * Even boards that also support JTAG can benefit from SWD support, because
 * usually there's no way to access the SWO trace view mechanism in JTAG mode.
 * That is, trace access may require SWD support.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arm.h"
#include "arm_adi_v5.h"
#include <helper/time_support.h>

#include <transport/transport.h>
#include <jtag/interface.h>

#include <jtag/swd.h>

static bool do_sync;

struct swd_multidrop_session_state {
	struct list_head queued_requests;
	bool active_on_wire;
};

static inline struct swd_multidrop_session_state *adiv5_dap_swd_multidrop_session_state(struct adiv5_dap *dap)
{
	return (struct swd_multidrop_session_state *)adiv5_dap_swd_transport_private(dap);
}

static int swd_multidrop_select_target(struct adiv5_dap *dap, uint32_t *dpidr, uint32_t *dlpidr, bool reconnecting);

static void swd_finish_read(struct adiv5_dap *dap)
{
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);
	if (dap->last_read != NULL) {
		swd->read_reg(swd_cmd(true, false, DP_RDBUFF), dap->last_read, 0);
		dap->last_read = NULL;
	}
}

static int swd_queue_dp_write(struct adiv5_dap *dap, unsigned reg,
		uint32_t data);
static int swd_queue_dp_read(struct adiv5_dap *dap, unsigned reg,
		uint32_t *data);

static void swd_clear_sticky_errors(struct adiv5_dap *dap)
{
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);
	assert(swd);

	swd->write_reg(swd_cmd(false,  false, DP_ABORT),
		STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR, 0);
}

static int swd_run_inner(struct adiv5_dap *dap)
{
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);
	int retval;

	retval = swd->run();

	if (retval != ERROR_OK) {
		/* fault response */
		dap->do_reconnect = true;
	}

	return retval;
}

static int swd_connect(struct adiv5_dap *dap)
{
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);
	uint32_t dpidr;
	int status;

	/* FIXME validate transport config ... is the
	 * configured DAP present (check IDCODE)?
	 * Is *only* one DAP configured?
	 *
	 * MUST READ DPIDR
	 */

	/* Check if we should reset srst already when connecting, but not if reconnecting. */
	if (!dap->do_reconnect) {
		enum reset_types jtag_reset_config = jtag_get_reset_config();

		if (jtag_reset_config & RESET_CNCT_UNDER_SRST) {
			if (jtag_reset_config & RESET_SRST_NO_GATING)
				swd_add_reset(1);
			else
				LOG_WARNING("\'srst_nogate\' reset_config option is required");
		}
	}

	/* Note, debugport_init() does setup too */
	swd->switch_seq(JTAG_TO_SWD);

	/* Clear link state, including the SELECT cache. */
	dap->do_reconnect = false;
	dap_invalidate_cache(dap);

	swd_queue_dp_read(dap, DP_DPIDR, &dpidr);

	/* force clear all sticky faults */
	swd_clear_sticky_errors(dap);

	status = swd_run_inner(dap);

	if (status == ERROR_OK) {
		LOG_INFO("SWD DPIDR %#8.8" PRIx32, dpidr);
		dap->do_reconnect = false;
		status = dap_dp_init(dap);
	} else
		dap->do_reconnect = true;

	return status;
}

static uint32_t last_target_id;

static int swd_multidrop_connect(struct adiv5_dap *dap)
{
	struct swd_multidrop_session_state *state = adiv5_dap_swd_multidrop_session_state(dap);
	if (state->active_on_wire) {
		/* No recursion for you */
		return ERROR_FAIL;
	}
	state->active_on_wire = true;
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);
	uint32_t dpidr, dlpidr;
	int status;

	/* FIXME validate transport config ... is the
	 * configured DAP present (check IDCODE)?
	 * Is *only* one DAP configured?
	 *
	 * MUST READ DPIDR
	 */

	/* Check if we should reset srst already when connecting, but not if reconnecting. */
	if (!dap->do_reconnect) {
		enum reset_types jtag_reset_config = jtag_get_reset_config();

		if (jtag_reset_config & RESET_CNCT_UNDER_SRST) {
			if (jtag_reset_config & RESET_SRST_NO_GATING)
				swd_add_reset(1);
			else
				LOG_WARNING("\'srst_nogate\' reset_config option is required");
		}
	}

	/* Note, debugport_init() does setup too */
	swd->switch_seq(DORMANT_TO_SWD);

	/* Clear link state, including the SELECT cache. */
	dap->do_reconnect = false;
	dap_invalidate_cache(dap);
	last_target_id = DP_TARGETSEL_INVALID;

	/* Pass true for reconnect, clearing sticky errors */
	status = swd_multidrop_select_target(dap, &dpidr, &dlpidr, true);

	if (status == ERROR_OK) {
		LOG_INFO("SWD DPIDR %#8.8" PRIx32, dpidr);
		LOG_INFO("SWD DLPIDR %#8.8" PRIx32, dlpidr);
		dap->do_reconnect = false;
		status = dap_dp_init(dap);
	} else {
		dap->do_reconnect = true;
	}

	state->active_on_wire = false;
	return status;
}

static inline int check_sync(struct adiv5_dap *dap)
{
	return do_sync ? swd_run_inner(dap) : ERROR_OK;
}

static int swd_check_reconnect(struct adiv5_dap *dap)
{
	int status = ERROR_OK;
	/* todo this is a bit of an abstraction failure */

	/* check_reconnect is a noop for multidrop as everything is queued */
	if (!transport_is_multidrop()) {
		if (dap->do_reconnect)
			status = swd_connect(dap);
	}

	return status;
}

static int swd_queue_ap_abort(struct adiv5_dap *dap, uint8_t *ack)
{
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);
	assert(swd);

	swd->write_reg(swd_cmd(false,  false, DP_ABORT),
		DAPABORT | STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR, 0);
	return check_sync(dap);
}

/** Select the DP register bank matching bits 7:4 of reg. */
static void swd_queue_dp_bankselect(struct adiv5_dap *dap, unsigned reg)
{
	/* Only register address 4 is banked. */
	if ((reg & 0xf) != 4)
		return;

	uint32_t select_dp_bank = (reg & 0x000000F0) >> 4;
	uint32_t sel = select_dp_bank
			| (dap->select & (DP_SELECT_APSEL | DP_SELECT_APBANK));

	if (sel == dap->select)
		return;

	dap->select = sel;

	swd_queue_dp_write(dap, DP_SELECT, sel);
}

static int swd_queue_dp_read(struct adiv5_dap *dap, unsigned reg,
		uint32_t *data)
{
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);
	assert(swd);

	int retval = swd_check_reconnect(dap);
	if (retval != ERROR_OK)
		return retval;

	swd_queue_dp_bankselect(dap, reg);
	swd->read_reg(swd_cmd(true,  false, reg), data, 0);

	return check_sync(dap);
}

static int swd_queue_dp_write(struct adiv5_dap *dap, unsigned reg,
		uint32_t data)
{
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);
	assert(swd);

	int retval = swd_check_reconnect(dap);
	if (retval != ERROR_OK)
		return retval;

	swd_finish_read(dap);
	swd_queue_dp_bankselect(dap, reg);
	swd->write_reg(swd_cmd(false,  false, reg), data, 0);

	return check_sync(dap);
}

/** Select the AP register bank matching bits 7:4 of reg. */
static void swd_queue_ap_bankselect(struct adiv5_ap *ap, unsigned reg)
{
	struct adiv5_dap *dap = ap->dap;
	uint32_t sel = ((uint32_t)ap->ap_num << 24)
			| (reg & 0x000000F0)
			| (dap->select & DP_SELECT_DPBANK);

	if (sel == dap->select)
		return;

	dap->select = sel;

	swd_queue_dp_write(dap, DP_SELECT, sel);
}

static int swd_queue_ap_read(struct adiv5_ap *ap, unsigned reg,
		uint32_t *data)
{
	struct adiv5_dap *dap = ap->dap;
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);
	assert(swd);

	int retval = swd_check_reconnect(dap);
	if (retval != ERROR_OK)
		return retval;

	swd_queue_ap_bankselect(ap, reg);
	swd->read_reg(swd_cmd(true,  true, reg), dap->last_read, ap->memaccess_tck);
	dap->last_read = data;

	return check_sync(dap);
}

static int swd_queue_ap_write(struct adiv5_ap *ap, unsigned reg,
		uint32_t data)
{
	struct adiv5_dap *dap = ap->dap;
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);
	assert(swd);

	int retval = swd_check_reconnect(dap);
	if (retval != ERROR_OK)
		return retval;

	swd_finish_read(dap);
	swd_queue_ap_bankselect(ap, reg);
	swd->write_reg(swd_cmd(false,  true, reg), data, ap->memaccess_tck);

	return check_sync(dap);
}

/** Executes all queued DAP operations. */
static int swd_run(struct adiv5_dap *dap)
{
	swd_finish_read(dap);
	return swd_run_inner(dap);
}

/** Put the SWJ-DP back to JTAG mode */
static void swd_quit(struct adiv5_dap *dap)
{
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);

	swd->switch_seq(SWD_TO_JTAG);
	/* flush the queue before exit */
	swd->run();
	free(adiv5_dap_swd_transport_private(dap));
}

const struct dap_ops swd_dap_ops = {
	.connect = swd_connect,
	.queue_dp_read = swd_queue_dp_read,
	.queue_dp_write = swd_queue_dp_write,
	.queue_ap_read = swd_queue_ap_read,
	.queue_ap_write = swd_queue_ap_write,
	.queue_ap_abort = swd_queue_ap_abort,
	.run = swd_run,
	.quit = swd_quit,
};

static const struct command_registration swd_commands[] = {
	{
		/*
		 * Set up SWD and JTAG targets identically, unless/until
		 * infrastructure improves ...  meanwhile, ignore all
		 * JTAG-specific stuff like IR length for SWD.
		 *
		 * REVISIT can we verify "just one SWD DAP" here/early?
		 */
		.name = "newdap",
		.jim_handler = jim_jtag_newtap,
		.mode = COMMAND_CONFIG,
		.help = "declare a new SWD DAP"
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration swd_handlers[] = {
	{
		.name = "swd",
		.mode = COMMAND_ANY,
		.help = "SWD command group",
		.chain = swd_commands,
	},
	COMMAND_REGISTRATION_DONE
};

/* FIXME: only place where global 'jtag_interface' is still needed */
extern struct jtag_interface *jtag_interface;
static int swd_select(struct command_context *ctx)
{
	const struct swd_driver *swd = jtag_interface->swd;
	int retval;

	retval = register_commands(ctx, NULL, swd_handlers);
	if (retval != ERROR_OK)
		return retval;

	 /* be sure driver is in SWD mode; start
	  * with hardware default TRN (1), it can be changed later
	  */
	if (!swd || !swd->read_reg || !swd->write_reg || !swd->init) {
		LOG_DEBUG("no SWD driver?");
		return ERROR_FAIL;
	}

	retval = swd->init();
	if (retval != ERROR_OK) {
		LOG_DEBUG("can't init SWD driver");
		return retval;
	}

	return retval;
}

static int swd_init(struct command_context *ctx)
{
	/* nothing done here, SWD is initialized
	 * together with the DAP */
	return ERROR_OK;
}

enum queued_request_type {
	dp_read,
	dp_write,
	ap_read,
	ap_write,
	ap_abort
};

struct queued_request {
	struct list_head lh;
	enum queued_request_type type;
	unsigned reg;
	struct adiv5_ap *ap;
	uint32_t write;
	uint32_t *read;
	uint8_t *ack;
};

static const struct command_registration swd_multidrop_handlers[] = {
	{
		.name = "swd-multidrop",
		.mode = COMMAND_ANY,
		.help = "SWD-multidrop command group",
		.chain = swd_commands,
	},
	COMMAND_REGISTRATION_DONE
};

static int swd_multidrop_select(struct command_context *ctx)
{
	/* FIXME: only place where global 'jtag_interface' is still needed */
	const struct swd_driver *driver = jtag_interface->swd;
	int retval;

	retval = register_commands(ctx, NULL, swd_handlers);
	if (retval != ERROR_OK)
		return retval;

	retval = register_commands(ctx, NULL, swd_multidrop_handlers);
	if (retval != ERROR_OK)
		return retval;

	/*
	 * be sure driver is in SWD mode; start
	 * with hardware default TRN (1), it can be changed later
	 */
	if (!driver || !driver->read_reg || !driver->write_reg || !driver->init) {
		LOG_DEBUG("no SWD session driver?");
		return ERROR_FAIL;
	}

	retval = driver->init();
	if (retval != ERROR_OK) {
		LOG_DEBUG("can't init SWD driver");
		return retval;
	}

	return retval;
}

static int swd_multidrop_init(struct command_context *ctx)
{
	/* nothing done here, SWD is initialized
	 * together with the DAP */
	return ERROR_OK;
}

static int swd_multidrop_queue_request(struct swd_multidrop_session_state *state, struct queued_request **request)
{
	*request = (struct queued_request *)calloc(1, sizeof(struct queued_request));
	if (*request) {
		list_add_tail(&(*request)->lh, &state->queued_requests);
		return ERROR_OK;
	} else
		return ERROR_FAIL;
}

static int swd_multidrop_queue_dp_read(struct adiv5_dap *dap, unsigned reg,
							 uint32_t *data)
{
	struct swd_multidrop_session_state *state = adiv5_dap_swd_multidrop_session_state(dap);
	int retval;
	if (state->active_on_wire) {
		retval = swd_queue_dp_read(dap, reg, data);
	} else {
		struct queued_request *r = NULL;
		retval = swd_multidrop_queue_request(state, &r);
		if (retval == ERROR_OK) {
			r->type = dp_read;
			r->reg = reg;
			r->read = data;
		}
	}
	return retval;
}

static int swd_multidrop_queue_dp_write(struct adiv5_dap *dap, unsigned reg,
							  uint32_t data)
{
	struct swd_multidrop_session_state *state = adiv5_dap_swd_multidrop_session_state(dap);
	int retval;
	if (state->active_on_wire) {
		retval = swd_queue_dp_write(dap, reg, data);
	} else {
		struct queued_request *r = NULL;
		retval = swd_multidrop_queue_request(state, &r);
		if (retval == ERROR_OK) {
			r->type = dp_write;
			r->reg = reg;
			r->write = data;
		}
	}
	return retval;
}

static int swd_multidrop_queue_ap_read(struct adiv5_ap *ap, unsigned reg,
							 uint32_t *data)
{
	struct swd_multidrop_session_state *state = adiv5_dap_swd_multidrop_session_state(ap->dap);
	int retval;
	if (state->active_on_wire) {
		retval = swd_queue_ap_read(ap, reg, data);
	} else {
		struct queued_request *r = NULL;
		retval = swd_multidrop_queue_request(state, &r);
		if (retval == ERROR_OK) {
			r->type = ap_read;
			r->ap = ap;
			r->reg = reg;
			r->read = data;
		}
	}
	return retval;
}

static int swd_multidrop_queue_ap_write(struct adiv5_ap *ap, unsigned reg,
							  uint32_t data)
{
	struct swd_multidrop_session_state *state = adiv5_dap_swd_multidrop_session_state(ap->dap);
	int retval;

	if (state->active_on_wire) {
		retval = swd_queue_ap_write(ap, reg, data);
	} else {
		struct queued_request *r = NULL;
		retval = swd_multidrop_queue_request(state, &r);
		if (retval == ERROR_OK) {
			r->type = ap_write;
			r->ap = ap;
			r->reg = reg;
			r->write = data;
		}
	}
	return retval;
}

static int swd_multidrop_queue_ap_abort(struct adiv5_dap *dap, uint8_t *ack)
{
	struct swd_multidrop_session_state *state = adiv5_dap_swd_multidrop_session_state(dap);
	int retval;
	if (state->active_on_wire) {
		retval = swd_queue_ap_abort(dap, ack);
	} else {
		struct queued_request *r = NULL;
		retval = swd_multidrop_queue_request(state, &r);
		if (retval == ERROR_OK) {
			r->type = ap_abort;
			r->ack = ack;
		}
	}
	return retval;
}

/* todo: remove this (only used to reset linkt to dormant on last quit) or find it a better home */
static int32_t swd_multidrop_session_count;

static int swd_multidrop_select_target(struct adiv5_dap *dap, uint32_t *dpidr, uint32_t *dlpidr, bool reconnecting)
{
	assert(adiv5_dap_swd_multidrop_session_state(dap)->active_on_wire);
	assert(dap->swd_target_id != DP_TARGETSEL_INVALID);

	int retval = ERROR_OK;
	if (last_target_id != dap->swd_target_id || dap->do_reconnect) {
		const struct swd_driver *driver = adiv5_dap_swd_driver(dap);
		const int MAX_TRIES = 3;
		for (int retry = MAX_TRIES; retry > 0; retry--) {
			driver->switch_seq(LINE_RESET);
			swd_queue_dp_write(dap, DP_TARGETSEL, dap->swd_target_id);
			*dpidr = *dlpidr = 0;
			swd_queue_dp_read(dap, DP_DPIDR, dpidr);
			if (!reconnecting && retry == MAX_TRIES) {
				/* Ideally just clear ORUN flag which is set by reset */
				swd_queue_dp_write(dap, DP_ABORT, ORUNERRCLR);
			} else {
				/* Clear all sticky errors during (including ORUN) */
				swd_clear_sticky_errors(dap);
			}
			swd_queue_dp_read(dap, DP_DLPIDR, dlpidr);
			retval = swd_run(dap);
			if (retval == ERROR_OK) {
				if (1 != (*dlpidr & DP_TARGETSEL_DPID_MASK) ||
					(dap->swd_target_id & DP_TARGETSEL_INSTANCEID_MASK) != (*dlpidr & DP_TARGETSEL_INSTANCEID_MASK)) {
					LOG_INFO("Read incorrect DLIPDR %08x (possibly CTRL/STAT value) when selecting coreid %d", *dlpidr,
							 dap->swd_target_id >> DP_TARGETSEL_INSTANCEID_SHIFT);
					retval = ERROR_FAIL;
				} else {
					LOG_DEBUG_IO("Selected core %d\n", dap->swd_target_id >> DP_TARGETSEL_INSTANCEID_SHIFT);
					last_target_id = dap->swd_target_id;
				}
				break;
			} else {
				last_target_id = DP_TARGETSEL_INVALID;
				LOG_DEBUG("Failed to select core %d%s", dap->swd_target_id >> DP_TARGETSEL_INSTANCEID_SHIFT,
						  retry > 1 ? ", retrying..." : "");
			}
		}
	}
	return retval;
}

static int swd_multidrop_deselect_target(struct adiv5_dap *dap)
{
	assert(adiv5_dap_swd_multidrop_session_state(dap)->active_on_wire);
	int retval = ERROR_OK;
	static const bool unselect_target; /* = false */
	/* todo: decide if we really want to deselect the target */
	if (unselect_target) {
		const struct swd_driver *driver = adiv5_dap_swd_driver(dap);
		driver->switch_seq(LINE_RESET);
		swd_queue_dp_write(dap, DP_TARGETSEL, DP_TARGETSEL_INVALID);
		retval = swd_run(dap);
	}
	return retval;
}

/** Executes all queued DAP operations. */
static int swd_multidrop_run(struct adiv5_dap *dap)
{
	struct swd_multidrop_session_state *state = adiv5_dap_swd_multidrop_session_state(dap);
	int retval;
	if (state->active_on_wire) {
		retval = swd_run(dap);
	} else {
		if (dap->do_reconnect) {
			retval = swd_multidrop_connect(dap);
		} else {
			uint32_t dpidr, dlpidr;
			state->active_on_wire = true;
			retval = swd_multidrop_select_target(dap, &dpidr, &dlpidr, false);
			state->active_on_wire = false;
		}

		state->active_on_wire = true;
		/* Send the queued command if all is well, but free cmds as we go anyway */
		struct queued_request *request, *tmp;
		list_for_each_entry_safe(request, tmp, &state->queued_requests, lh) {
			if (retval == ERROR_OK) {
				switch (request->type) {
					case dp_write:
						retval = swd_queue_dp_write(dap, request->reg, request->write);
						break;
					case dp_read:
						retval = swd_queue_dp_read(dap, request->reg, request->read);
						break;
					case ap_write:
						retval = swd_queue_ap_write(request->ap, request->reg, request->write);
						break;
					case ap_read:
						retval = swd_queue_ap_read(request->ap, request->reg, request->read);
						break;
					case ap_abort:
						retval = swd_queue_ap_abort(dap, request->ack);
						break;
					default:
						assert(false);
				}
			}
			list_del(&request->lh);
			free(request);
		}

		if (retval == ERROR_OK)
			retval = swd_run(dap);

		if (retval == ERROR_OK)
			swd_multidrop_deselect_target(dap);

		state->active_on_wire = false;
	}
	return retval;
}

/** Put the SWJ-DP back to dormant mode */
static void swd_multidrop_quit(struct adiv5_dap *dap)
{
	const struct swd_driver *swd = adiv5_dap_swd_driver(dap);

	if (0 == --swd_multidrop_session_count)
		swd->switch_seq(SWD_TO_DORMANT);

	/* flush the queue before exit - this also frees any pending requests */
	swd_multidrop_run(dap);

	free(adiv5_dap_swd_transport_private(dap));
}

const struct dap_ops swd_multidrop_dap_ops = {
	.connect = swd_multidrop_connect,
	.queue_dp_read = swd_multidrop_queue_dp_read,
	.queue_dp_write = swd_multidrop_queue_dp_write,
	.queue_ap_read = swd_multidrop_queue_ap_read,
	.queue_ap_write = swd_multidrop_queue_ap_write,
	.queue_ap_abort = swd_multidrop_queue_ap_abort,
	.run = swd_multidrop_run,
	.quit = swd_multidrop_quit,
};

int swd_create_transport_session(struct adiv5_dap *dap, const struct dap_ops **dap_ops, void **transport_private)
{
	if (transport_is_multidrop()) {
		struct swd_multidrop_session_state *state =
				(struct swd_multidrop_session_state *)calloc(1, sizeof(struct swd_multidrop_session_state));
		if (state) {
			*dap_ops = &swd_multidrop_dap_ops;
			*transport_private = state;
			INIT_LIST_HEAD(&state->queued_requests);
			swd_multidrop_session_count++;
			return ERROR_OK;
		} else {
			return ERROR_FAIL;
		}
	} else {
		*dap_ops = &swd_dap_ops;
		return ERROR_OK;
	}
}

static struct transport swd_transport = {
	.name = "swd",
	.select = swd_select,
	.init = swd_init,
};

static struct transport swd_multidrop_transport = {
	.name = "swd-multidrop",
	.select = swd_multidrop_select,
	.init = swd_multidrop_init,
};

static void swd_constructor(void) __attribute__((constructor));
static void swd_constructor(void)
{
	transport_register(&swd_transport);
	transport_register(&swd_multidrop_transport);
}

/**
 * Returns true if the current debug session
 * is using SWD (or SWD-multidrop) as its transport.
 */
bool transport_is_swd(void)
{
	return get_current_transport() == &swd_transport || get_current_transport() == &swd_multidrop_transport;
}

/**
 * Returns true if the current debug session
 * is using SWD-multidrop as its transport.
 */
bool transport_is_multidrop(void)
{
	return get_current_transport() == &swd_multidrop_transport;
}
