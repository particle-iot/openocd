/*
 * SWD Transport Core Body File for OpenOCD.
 *
 * Copyright (C) 2011-2012 Tomasz Boleslaw CEDRO
 * cederom@tlen.pl, http://www.tomek.cedro.info
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the Tomasz Boleslaw CEDRO nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.*
 *
 * Written by Tomasz Boleslaw CEDRO <cederom@tlen.pl>, 2011-2012;
 *
 */

/** \file swd.c SWD Transport Core Body File for OpenOCD.
 * SWD Transport Layer creates bridge between target and the interface driver
 * functions. Target functions create high level operations on the device's
 * DAP (Debug Access Port), while interface driver passes electrical signals
 * in and out of the physical device. Transport is implemented using LibSWD,
 * and external open-source SWD framework.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <interface/interface.h>
#include <transport/transport.h>
#include <transport/swd.h>
#include <transport/swd_libswd.h>
#include <target/arm.h>
#include <target/arm_adi_v5.h>
#include <helper/log.h>

/** Unfortunately OpenOCD use globals to pass information so we need to use it too. */
extern struct jtag_interface *jtag_interface;

/** @{ swd_transport SWD Transport core definitions. */

/**
 * Select SWD transport on interface pointed by global *jtag_interface structure.
 * Select is assumed to be called before transport init. It prepares everything,
 * including context memory and command set for higher layers, but not hardware
 * and does not interrogate target device (with IDCODE read that is done by
 * transport init call). This function does not touch the hardware because
 * hardware use signals that are not yet read from config file at this point!
 *
 * Note: This is only a generic wrapper for driver specific SWD transport.
 * It validates SWD function set for given driver, selects these functions
 * as the transport to be used.
 *
 * \param *ctx is the openocd command_context.
 * \return ERROR_OK on success, ERROR_FAIL otherwise.
 */

int transport_swd_setup(struct command_context *ctx)
{
	int retval;
	struct feature *feature_arm_dap;

	jtag_interface->transport = &swd_transport;
	if (jtag_interface->transport->configured) {
		LOG_WARNING("Transport '%s' already configured, skipping...", jtag_interface->transport->name);
		return ERROR_OK;
	}

	/* Some interfaces will have arm dap swd feature already defined, search for it... */
	feature_arm_dap = feature_find(jtag_interface->features, FEATURE_ARM_DAP);
	if (feature_arm_dap == NULL) {
		/* If dedicated feature was not found, try to use generic one. */
		LOG_INFO("Selecting LibSWD as default SWD transport mechanism and interface features...");
		if (!jtag_interface->features) {
			jtag_interface->features = (struct feature *)calloc(1, sizeof(struct feature));
			if (!jtag_interface->features) {
				LOG_ERROR("Feature allocation memory failed!");
				return ERROR_FAIL;
			}
		}
		feature_add(jtag_interface->features, &transport_swd_libswd_arm_dap_feature);
		feature_arm_dap = feature_find(jtag_interface->features, FEATURE_ARM_DAP);
		if (!feature_arm_dap) {
			LOG_WARNING("Transport features '%s' failed to attach to interface '%s'!", \
				transport_swd_libswd_arm_dap_feature.name, jtag_interface->name);
			LOG_ERROR("Interface '%s' does not provide/accept features required by transport '%s'!", \
			jtag_interface->name, jtag_interface->transport->name);
			return ERROR_FAIL;
		}
	} else
		LOG_INFO("Interface '%s' defines its own '%s' features.", jtag_interface->name, feature_arm_dap->name);

	struct dap_ops *dap = (struct dap_ops *)feature_arm_dap->body;
	retval = dap->select(ctx);

	if (transport_swd_register_commands(ctx) != ERROR_OK) {
		LOG_ERROR("Unable to select SWD transport!");
		return retval;
	}
	jtag_interface->transport->configured = 1;
	return ERROR_OK;
}

/**
 * This is a SWD transport definition.
 */

struct transport swd_transport = {
	.name  = "swd",
	.setup = transport_swd_setup,
	.quit  = NULL,
	.next  = NULL,
};

/**
 * This is a default SWD transport operation set definition template.
 * Use this to create your own dap operations. These below returns error.
 * These operations will become interface feature or can come from interface
 * features already defined by the driver in case of intelligent dongles..
 */

const struct dap_ops target_arm_dap_ops_swd_default = {
	.is_swd            = true,
	.select            = transport_swd_select,
	.init              = transport_swd_init,
	.queue_idcode_read = transport_swd_queue_idcode_read,
	.queue_dp_read     = transport_swd_queue_dp_read,
	.queue_dp_write    = transport_swd_queue_dp_write,
	.queue_ap_read     = transport_swd_queue_ap_read,
	.queue_ap_write    = transport_swd_queue_ap_write,
	.queue_ap_abort    = transport_swd_queue_ap_abort,
	.run               = transport_swd_run,
};

/**
 * Interface features template to add SWD support for your interface.
 * Attach to driver feature list by driver setup routine.
 */

struct feature transport_swd_template_feature = {
	.name        = FEATURE_ARM_DAP,
	.description = "Example non-functional template ARM DAP SWD transport features.",
	.body        = (void *)&target_arm_dap_ops_swd_default,
	.next        = NULL
};

/**
 * This function does IDCODE read on selected DAP using underlying driver call.
 *
 * \param *dap is the pointer to the target DAP to work on.
 * \param *ack is the pointer to target response.
 * \param *data is the pointer to target IDCODE response.
 * \return ERROR_OK on success, ERROR_FAIL otherwise.
 */

int transport_swd_queue_idcode_read(struct adiv5_dap *dap, uint8_t *ack, uint32_t *data)
{
	LOG_ERROR("Your driver did not define oocd_transport_swd_queue_idcode_read()");
	return ERROR_FAIL;
}

/**
 * This function does DP read on selected DAP using underlying driver call.
 *
 * \param *dap is the pointer to the target DAP to work on.
 * \param reg is the register address to read.
 * \param *data is the pointer to resulting data.
 * \return ERROR_OK on success, ERROR_FAIL otherwise.
 */

int transport_swd_queue_dp_read(struct adiv5_dap *dap, unsigned reg, uint32_t *data)
{
	LOG_ERROR("Your driver did not define oocd_transport_swd_queue_dp_read()");
	return ERROR_FAIL;
}

/**
 * This function does DP write on selected DAP using underlying driver call.
 *
 * \param *dap is the pointer to the target DAP to work on.
 * \param reg is the register address to read.
 * \param *data is the pointer containing data.
 * \return ERROR_OK on success, ERROR_FAIL otherwise.
 */

int transport_swd_queue_dp_write(struct adiv5_dap *dap, unsigned reg, uint32_t data)
{
	LOG_ERROR("Your driver did not define oocd_transport_swd_queue_dp_write()");
	return ERROR_FAIL;
}

/**
 * This function does AP read on selected DAP using underlying driver call.
 *
 * \param *dap is the pointer to the target DAP to work on.
 * \param reg is the register address to read.
 * \param *data is the pointer to resulting data.
 * \return ERROR_OK on success, ERROR_FAIL otherwise.
 */

int transport_swd_queue_ap_read(struct adiv5_dap *dap, unsigned reg, uint32_t *data)
{
	LOG_ERROR("Your driver did not define oocd_transport_swd_queue_ap_read()");
	return ERROR_FAIL;
}

/**
 * This function does AP write on selected DAP using underlying driver call.
 *
 * \param *dap is the pointer to the target DAP to work on.
 * \param reg is the register address to read.
 * \param *data is the pointer containing data.
 * \return ERROR_OK on success, ERROR_FAIL otherwise.
 */

int transport_swd_queue_ap_write(struct adiv5_dap *dap, unsigned reg, uint32_t data)
{
	LOG_ERROR("Your driver did not define oocd_transport_swd_queue_ap_write()");
	return ERROR_FAIL;
}

/**
 * This function aborts all operations on selected DAP using underlying driver
 * call. This may be useful on target stall.
 *
 * \param *dap is the pointer to the target DAP to work on.
 * \param *ack is the pointer to target response.
 * \return ERROR_OK on success, ERROR_FAIL otherwise.
 */

int transport_swd_queue_ap_abort(struct adiv5_dap *dap, uint8_t *ack)
{
	LOG_ERROR("Your driver did not define oocd_transport_swd_queue_ap_abort()");
	return ERROR_FAIL;
}

/**
 * This function flushes all enqueued operations into a hardware interface.
 *
 * Because in SWD each operation is confirmed by Target with ACK answer
 * we need to react on errors here, unless DP/AP operations are executed
 * on enqueue which is driver specific behaviour - some drivers simply pass
 * such enqueue to the interface that executes the operation and can return
 * error code right away, other drivers will first enqueue a series
 * of operations and the flush the queue with this function.
 *
 * OpenOCD was constructed at first for use only with with JTAG transport
 * and most functions use series of enqueue functions that are later flushed
 * into a hardware interface with high level dap_run() / transport_run(), so
 * this is the only sensible place to place error handling in that case.
 * However in case of error a series of enqueued operations becomes invalid,
 * which should be handled by upper layers (target) code.
 *
 * Note: Do not use long queues of operations with SWD as each operation gives
 * ACK status code right away and may require an immediate error handling...
 *
 * \param *dap is the pointer to the target DAP to work on.
 * \return ERROR_OK on success, ERROR_FAIL otherwise.
 */

int transport_swd_run(struct adiv5_dap *dap)
{
	LOG_ERROR("Your driver did not define oocd_transport_swd_run()");
	return ERROR_FAIL;
}

/**
 * Select prepares transport internals for use.
 */

int transport_swd_select(struct command_context *ctx)
{
	LOG_ERROR("Your driver did not define oocd_transport_swd_select()");
	return ERROR_FAIL;
}

/**
 * Transport initialization routine is responsible for target initialization
 * using previously selected transport.
 * It talks to the hardware using functions set by transport_select().
 *
 * \param *ctx is the openocd command_context.
 * \return ERROR_OK on success, ERROR_FAIL otherwise.
 */

int transport_swd_init(struct command_context *ctx)
{
	LOG_ERROR("Your driver did not define oocd_transport_swd_init()");
	return ERROR_FAIL;
}

/**
 * Returns true if the current debug session is using SWD as its transport.
 */

bool transport_is_swd(void)
{
	return get_current_transport() == &swd_transport;
}
