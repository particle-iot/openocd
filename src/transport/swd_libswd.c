/*
 * OpenOCD's SWD Transport Drivers for LibSWD, body file.
 *
 * Copyright (C) 2011-2012 Tomasz Boleslaw CEDRO
 * cederom@tlen.pl, http://www.tomek.cedro.info
 * All rights reserved.
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

/**
 * \file transport_swd_libswd.c OpenOCD's SWD Transport Drivers for LibSWD,
 * body file.
 *
 * This file implements SWD transport in OpenOCD using external LibSWD library.
 * LibSWD makes it possible to generate and anlyze SWD bistream, which can be
 * transported by any generic interface that provides "transfer" and "bitbang"
 * functions (see ft2232 as example).
 *
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

/** OpenOCD as for now use global pointer to driver structure. */
extern struct jtag_interface *jtag_interface;

/******************************************************************************
 * @{ oocd_transport_swd_libswd_arm_adi_v5
 * SWD Transport definitions that use LibSWD for underlying bus operations.
 */

int oocd_transport_swd_libswd_queue_idcode_read(struct adiv5_dap *dap, uint8_t *ack, uint32_t *data)
{
	int retval, *pdata;
	retval = libswd_dp_read_idcode(dap->ctx, LIBSWD_OPERATION_EXECUTE, &pdata);
	if (retval < 0) {
		LOG_ERROR("oocd_transport_swd_libswd_queue_idcode_read(*dap=@%p, ack=@%p, data=@%p) error (%s)", (void *)dap,
				(void *)ack, (void *)data, libswd_error_string(retval));
		return ERROR_FAIL;
	}
	if (pdata != NULL)
		*data = (uint32_t)*pdata;
	return ERROR_OK;
}

int oocd_transport_swd_libswd_queue_dp_read(struct adiv5_dap *dap, unsigned reg, uint32_t *data)
{
	int retval, *pdata;
	retval = libswd_dp_read((libswd_ctx_t *)dap->ctx, LIBSWD_OPERATION_EXECUTE, (char)reg, &pdata);
	if (retval < 0) {
		LOG_ERROR("oocd_transport_swd_libswd_queue_dp_read(dap=@%p, reg=0x%X, data=@%p) error (%s) ", (void *)dap, reg,
				(void *)data, libswd_error_string(retval));
		return ERROR_FAIL;
	}
	if (data != NULL)
		*data = (uint32_t)*pdata;
	return ERROR_OK;
}

int oocd_transport_swd_libswd_queue_dp_write(struct adiv5_dap *dap, unsigned reg, uint32_t data)
{
	int retval;
	retval = libswd_dp_write((libswd_ctx_t *)dap->ctx, LIBSWD_OPERATION_EXECUTE, (char)reg, (int *)&data);
	if (retval < 0) {
		LOG_ERROR("oocd_transport_swd_libswd_queue_dp_write(dap=@%p, reg=0x%X, data=0x%X) error (%s)", (void *)dap, reg,
				data, libswd_error_string(retval));
		return ERROR_FAIL;
	}
	return ERROR_OK;
}

int oocd_transport_swd_libswd_queue_ap_read(struct adiv5_dap *dap, unsigned reg, uint32_t *data)
{
	int retval, *pdata;
	retval = libswd_ap_read((libswd_ctx_t *)dap->ctx, LIBSWD_OPERATION_EXECUTE, (char)reg, &pdata);
	if (retval < 0) {
		LOG_ERROR("oocd_transport_swd_libswd_queue_ap_read(dap=@%p, reg=0x%X, data=@%p) error (%s)", (void *)dap, reg,
				(void *)data, libswd_error_string(retval));
		return ERROR_FAIL;
	}
	if (data != NULL)
		*data = (uint32_t)*pdata;
	return ERROR_OK;
}

int oocd_transport_swd_libswd_queue_ap_write(struct adiv5_dap *dap, unsigned reg, uint32_t data)
{
	int retval;
	retval = libswd_ap_write((libswd_ctx_t *)dap->ctx, LIBSWD_OPERATION_EXECUTE, (char) reg, (int *) &data);
	if (retval < 0) {
		LOG_ERROR("oocd_transport_swd_libswd_queue_ap_write(dap=@%p, reg=0x%X, data=0x%X) error (%s)", (void *)dap, reg,
			data, libswd_error_string(retval));
		return ERROR_FAIL;
	}
	return ERROR_OK;
}

int oocd_transport_swd_libswd_queue_ap_abort(struct adiv5_dap *dap, uint8_t *ack)
{
	int retval;
	int abort_flags = LIBSWD_DP_ABORT_ORUNERRCLR | LIBSWD_DP_ABORT_WDERRCLR | LIBSWD_DP_ABORT_STKERRCLR \
					| LIBSWD_DP_ABORT_STKCMPCLR | LIBSWD_DP_ABORT_DAPABORT;
	retval = libswd_dp_write((libswd_ctx_t *)dap->ctx, LIBSWD_OPERATION_ENQUEUE, LIBSWD_DP_ABORT_ADDR, &abort_flags);
	if (retval < 0) {
		LOG_ERROR("oocd_transport_swd_libswd_queue_ap_abort(dap=@%p, ack=@%p) error (%s)", (void *)dap, (void *)ack,
				libswd_error_string(retval));
		return ERROR_FAIL;
	}
	return ERROR_OK;
}

/** This function flushes all enqueued operations into a hardware interface.
 *  libswd_cmdq_flush() is called, then  libswd_drv_transmit() which is using
 *  application specific drivers that are linked into target application binary.
 *  Because in SWD each operation is confirmed by Target with ACK answer
 *  we need to react on errors here. OpenOCD was constructed for use with JTAG
 *  and most functions use series of enqueue functions that are later flushed
 *  into a hardware interface with high level dap_run() / transport_run(), so
 *  this is the only sensible place to place error handling (otherwise code
 *  would need to be changed in lots of places). Caller function simply want
 *  to know if transfer succeeded, so we can perform handling such as retry
 *  on ACK=WAIT unless transfer fail with ACK={FAIL, UNKNOWN}.
 */

int oocd_transport_swd_libswd_run(struct adiv5_dap *dap)
{
	int retval;
	libswd_ctx_t *libswdctx = (libswd_ctx_t *)dap->ctx;
	retval = libswd_cmdq_flush(libswdctx, &libswdctx->cmdq, LIBSWD_OPERATION_EXECUTE);
	if (retval < 0) {
		LOG_ERROR("oocd_transport_swd_libswd_run(dap=@%p) error (%s)", (void *) dap, libswd_error_string(retval));
		return ERROR_FAIL;
	} else
		return ERROR_OK;
}


/**
 * Select SWD transport on interface pointed by global *jtag_interface structure.
 * Select is assumed to be called before transport init. It prepares everything,
 * including context memory and command set for higher layers, but not hardware
 * and does not interrogate target device (with IDCODE read that is done by
 * transport init call). This function does not touch the hardware because
 * hardware use signals that are not yet read from config file at this point!
 */
int oocd_transport_swd_libswd_select(struct command_context *ctx)
{
	int retval;

	retval = oocd_transport_swd_libswd_register_commands(ctx);
	if (retval != ERROR_OK) {
		LOG_ERROR("Unable to register LibSWD commands for SWD Transport!");
		return retval;
	}
	return ERROR_OK;
}

/**
 * Transport initialization routine is responsible for target initialization
 * using previously selected transport.
 * It talks to the hardware using functions selected by transport_select().
 *
 * \param *ctx is the openocd command_context.
 * \return ERROR_OK on success, ERROR_FAIL otherwise.
 */
int oocd_transport_swd_libswd_init(struct command_context *ctx)
{
	LOG_DEBUG("entering function...");
	int retval, *idcode;

	struct target *target = get_current_target(ctx);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	dap->ops = &oocd_dap_ops_swd_libswd;

	/* Create LIBSWD_CTX if nesessary */
	if (!dap->ctx) {
		/** Transport was not yet initialized. */
		dap->ctx = libswd_init();
		if (dap->ctx == NULL) {
			LOG_ERROR("Cannot initialize SWD context!");
			return ERROR_FAIL;
		}
		LOG_INFO("New SWD context initialized at 0x%p", (void *)dap->ctx);
		/* Now inherit the log level from OpenOCD settings. */
		retval = libswd_log_level_inherit((libswd_ctx_t *)dap->ctx, debug_level);
		if (retval < 0) {
			LOG_ERROR("Unable to set log level: %s", libswd_error_string(retval));
			return ERROR_FAIL;
		}
	} else
		LOG_INFO("Working on existing transport context at 0x%p...", (void *)dap->ctx);

	/** We enable automatic error handling on error */
	libswd_ctx_t *libswdctx = (libswd_ctx_t *)dap->ctx;
	libswdctx->config.autofixerrors = 0;

	/**
	 * Initialize driver and detect target working with selected transport.
	 * Because we can work on existing context there is no need to destroy it,
	 * as it can be used on next try.
	 */
	retval = libswd_dap_detect((libswd_ctx_t *)dap->ctx, LIBSWD_OPERATION_EXECUTE, &idcode);
	if (retval < 0) {
		LOG_ERROR("libswd_dap_detect() error %d (%s)", retval, libswd_error_string(retval));
		return retval;
	}

	LOG_INFO("SWD transport initialization complete. Found IDCODE=0x%08X.", *idcode);
	return ERROR_OK;
}

/**
 * SWD Tranport based DAP Operations using LibSWD for underlying operations.
 */
const struct dap_ops oocd_dap_ops_swd_libswd = {
	.is_swd = true,
	.select            = oocd_transport_swd_libswd_select,
	.init              = oocd_transport_swd_libswd_init,
	.queue_idcode_read = oocd_transport_swd_libswd_queue_idcode_read,
	.queue_dp_read     = oocd_transport_swd_libswd_queue_dp_read,
	.queue_dp_write    = oocd_transport_swd_libswd_queue_dp_write,
	.queue_ap_read     = oocd_transport_swd_libswd_queue_ap_read,
	.queue_ap_write    = oocd_transport_swd_libswd_queue_ap_write,
	.queue_ap_abort    = oocd_transport_swd_libswd_queue_ap_abort,
	.run               = oocd_transport_swd_libswd_run,
};

/**
 * Interface features adds SWD support using LibSWD as middleware.
 */
oocd_feature_t oocd_transport_swd_libswd_feature = {
	.name        = OOCD_FEATURE_ARM_DAP,
	.description = "ARM DAP SWD transport features based on LibSWD.",
	.body        = (void *)&oocd_dap_ops_swd_libswd,
	.next        = NULL
};

/** @} */

/******************************************************************************
 * @{ oocd_transport_swd_libswd_drv
 * Driver bridge between OpenOCD and LibSWD.
 */

/**
 * Driver code to write 8-bit data (char type).
 * MOSI (Master Output Slave Input) is a SWD Write Operation.
 *
 * \param *libswdctx swd context to work on.
 * \param *cmd point to the actual command being sent.
 * \param *data points to the char data.
 * \bits tells how many bits to send (at most 8).
 * \bits nLSBfirst tells the shift direction: 0 = LSB first, other MSB first.
 * \return data count transferred, or negative LIBSWD_ERROR code on failure.
ar)*/
int libswd_drv_mosi_8(libswd_ctx_t *libswdctx, libswd_cmd_t *cmd, char *data, int bits, int nLSBfirst)
{
	LOG_DEBUG("OpenOCD's libswd_drv_mosi_8(libswdctx=@%p, cmd=@%p, data=0x%02X, bits=%d, nLSBfirst=0x%02X)",
			(void *)libswdctx, (void *)cmd, *data, bits, nLSBfirst);
	if (data == NULL)
		return LIBSWD_ERROR_NULLPOINTER;
	if (bits < 0 && bits > 8)
		return LIBSWD_ERROR_PARAM;
	if (nLSBfirst != 0 && nLSBfirst != 1)
		return LIBSWD_ERROR_PARAM;

	static unsigned int i;
	static signed int res;
	static char misodata[8], mosidata[8];

	/* Split output data into char array. */
	for (i = 0; i < 8; i++)
		mosidata[(nLSBfirst == LIBSWD_DIR_LSBFIRST) ? i : (bits - 1 - i)] = ((1 << i) & (*data)) ? 1 : 0;
	/* Then send that array into interface hardware. */
	res = jtag_interface->transfer(NULL, bits, mosidata, misodata, 0);
	if (res < 0)
		return LIBSWD_ERROR_DRIVER;

	return res;
}

/**
 * Driver code to write 32-bit data (int type).
 * MOSI (Master Output Slave Input) is a SWD Write Operation.
 *
 * \param *libswdctx swd context to work on.
 * \param *cmd point to the actual command being sent.
 * \param *data points to the char buffer array.
 * \bits tells how many bits to send (at most 32).
 * \bits nLSBfirst tells the shift direction: 0 = LSB first, other MSB first.
 * \return data count transferred, or negative LIBSWD_ERROR code on failure.
 */
int libswd_drv_mosi_32(libswd_ctx_t *libswdctx, libswd_cmd_t *cmd, int *data, int bits, int nLSBfirst)
{
	LOG_DEBUG("OpenOCD's libswd_drv_mosi_32(libswdctx=@%p, cmd=@%p, data=0x%08X, bits=%d, nLSBfirst=0x%02X)",
		(void *)libswdctx, (void *)cmd, *data, bits, nLSBfirst);
	if (data == NULL)
		return LIBSWD_ERROR_NULLPOINTER;
	if (bits < 0 && bits > 8)
		return LIBSWD_ERROR_PARAM;
	if (nLSBfirst != 0 && nLSBfirst != 1)
		return LIBSWD_ERROR_PARAM;

	static unsigned int i;
	static signed int res;
	static char misodata[32], mosidata[32];

	/* UrJTAG drivers shift data LSB-First. */
	for (i = 0; i < 32; i++)
		mosidata[(nLSBfirst == LIBSWD_DIR_LSBFIRST) ? i : (bits - 1 - i)] = ((1 << i) & (*data)) ? 1 : 0;
	res = jtag_interface->transfer(NULL, bits, mosidata, misodata, 0);
	if (res < 0)
		return LIBSWD_ERROR_DRIVER;
	return res;
}

/**
 * Use UrJTAG's driver to read 8-bit data (char type).
 * MISO (Master Input Slave Output) is a SWD Read Operation.
 *
 * \param *libswdctx swd context to work on.
 * \param *cmd point to the actual command being sent.
 * \param *data points to the char buffer array.
 * \bits tells how many bits to send (at most 8).
 * \bits nLSBfirst tells the shift direction: 0 = LSB first, other MSB first.
 * \return data count transferred, or negative LIBSWD_ERROR code on failure.
 */
int libswd_drv_miso_8(libswd_ctx_t *libswdctx, libswd_cmd_t *cmd, char *data, int bits, int nLSBfirst)
{
	if (data == NULL)
		return LIBSWD_ERROR_NULLPOINTER;
	if (bits < 0 && bits > 8)
		return LIBSWD_ERROR_PARAM;
	if (nLSBfirst != 0 && nLSBfirst != 1)
		return LIBSWD_ERROR_PARAM;

	static int i;
	static signed int res;
	static char misodata[8], mosidata[8];

	res = jtag_interface->transfer(NULL, bits, mosidata, misodata, LIBSWD_DIR_LSBFIRST);
	if (res < 0)
		return LIBSWD_ERROR_DRIVER;
	/* Now we need to reconstruct the data byte from shifted in LSBfirst byte array. */
	*data = 0;
	for (i = 0; i < bits; i++)
		*data |= misodata[(nLSBfirst == LIBSWD_DIR_LSBFIRST) ? i : (bits - 1 - i)] ? (1 << i) : 0;
	LOG_DEBUG("OpenOCD's libswd_drv_miso_8(libswdctx=@%p, cmd=@%p, data=@%p, bits=%d, nLSBfirst=0x%02X) reads: 0x%02X",
			(void *)libswdctx, (void *)cmd, (void *)data, bits, nLSBfirst, *data);
	return res;
}

/**
 * Driver code to read 32-bit data (int type).
 * MISO (Master Input Slave Output) is a SWD Read Operation.
 *
 * \param *libswdctx swd context to work on.
 * \param *cmd point to the actual command being sent.
 * \param *data points to the char buffer array.
 * \bits tells how many bits to send (at most 32).
 * \bits nLSBfirst tells the shift direction: 0 = LSB first, other MSB first.
 * \return data count transferred, or negative LIBSWD_ERROR code on failure.
 */
int libswd_drv_miso_32(libswd_ctx_t *libswdctx, libswd_cmd_t *cmd, int *data, int bits, int nLSBfirst)
{
	if (data == NULL)
		return LIBSWD_ERROR_NULLPOINTER;
	if (bits < 0 && bits > 8)
		return LIBSWD_ERROR_PARAM;
	if (nLSBfirst != 0 && nLSBfirst != 1)
		return LIBSWD_ERROR_PARAM;

	static int i;
	static signed int res;
	static char misodata[32], mosidata[32];

	res = jtag_interface->transfer(NULL, bits, mosidata, misodata, LIBSWD_DIR_LSBFIRST);
	if (res < 0)
		return LIBSWD_ERROR_DRIVER;
	/* Now we need to reconstruct the data byte from shifted in LSBfirst byte array. */
	*data = 0;
	for (i = 0; i < bits; i++)
		*data |= (misodata[(nLSBfirst == LIBSWD_DIR_LSBFIRST) ? i : (bits - 1 - i)] ? (1 << i) : 0);
	LOG_DEBUG("OpenOCD's libswd_drv_miso_32(libswdctx=@%p, cmd=@%p, data=@%p, bits=%d, nLSBfirst=0x%02X) reads: 0x%08X",
		(void *)libswdctx, (void *)cmd, (void *)data, bits, nLSBfirst, *data);
	LOG_DEBUG("OpenOCD's libswd_drv_miso_32() reads: 0x%08X\n", *data);
	return res;
}

/**
 * This function sets interface buffers to MOSI direction.
 * MOSI (Master Output Slave Input) is a SWD Write operation.
 * OpenOCD use global "struct jtag_interface" pointer as interface driver.
 * OpenOCD driver must support "RnW" signal to drive output buffers for TRN.
 *
 * \param *libswdctx is the swd context to work on.
 * \param bits specify how many clock cycles must be used for TRN.
 * \return number of bits transmitted or negative LIBSWD_ERROR code on failure.
 */
int libswd_drv_mosi_trn(libswd_ctx_t *libswdctx, int bits)
{
	LOG_DEBUG("OpenOCD's libswd_drv_mosi_trn(libswdctx=@%p, bits=%d)\n", (void *)libswdctx, bits);
	if (bits < LIBSWD_TURNROUND_MIN_VAL && bits > LIBSWD_TURNROUND_MAX_VAL)
		return LIBSWD_ERROR_TURNAROUND;

	int res, val = 0;
	static char buf[LIBSWD_TURNROUND_MAX_VAL];
	/* Use driver method to set low (write) signal named RnW. */
	res = jtag_interface->bitbang(NULL, "RnW", 0, &val);
	if (res < 0)
		return LIBSWD_ERROR_DRIVER;

	/* Clock specified number of bits for proper TRN transaction. */
	res = jtag_interface->transfer(NULL, bits, buf, buf, 0);
	if (res < 0)
		return LIBSWD_ERROR_DRIVER;

	return bits;
}

/**
 * This function sets interface buffers to MISO direction.
 * MISO (Master Input Slave Output) is a SWD Read operation.
 * OpenOCD use global "struct jtag_interface" pointer as interface driver.
 * OpenOCD driver must support "RnW" signal to drive output buffers for TRN.
 *
 * \param *libswdctx is the swd context to work on.
 * \param bits specify how many clock cycles must be used for TRN.
 * \return number of bits transmitted or negative LIBSWD_ERROR code on failure.
 */
int libswd_drv_miso_trn(libswd_ctx_t *libswdctx, int bits)
{
	LOG_DEBUG("OpenOCD's libswd_drv_miso_trn(libswdctx=@%p, bits=%d)\n", (void *)libswdctx, bits);
	if (bits < LIBSWD_TURNROUND_MIN_VAL && bits > LIBSWD_TURNROUND_MAX_VAL)
		return LIBSWD_ERROR_TURNAROUND;

	static int res, val = 1;
	static char buf[LIBSWD_TURNROUND_MAX_VAL];

	/* Use driver method to set high (read) signal named RnW. */
	res = jtag_interface->bitbang(NULL, "RnW", 0xFFFFFFFF, &val);
	if (res < 0)
		return LIBSWD_ERROR_DRIVER;

	/* Clock specified number of bits for proper TRN transaction. */
	res = jtag_interface->transfer(NULL, bits, buf, buf, 0);
	if (res < 0)
		return LIBSWD_ERROR_DRIVER;

	return bits;
}

/**
 * Set SWD debug level according to OpenOCD settings.
 *
 * \param *libswdctx is the context to work on.
 * \param loglevel is the OpenOCD numerical value of actual loglevel to force
 *  on LibSWD, or -1 to inherit from actual global settings of OpenOCD.
 * \return LIBSWD_OK on success, negative LIBSWD_ERROR code on failure.
 */
int libswd_log_level_inherit(libswd_ctx_t *libswdctx, int loglevel)
{
	LOG_DEBUG("OpenOCD's libswd_log_level_inherit(libswdctx=@%p, loglevel=%d)\n", (void *)libswdctx, loglevel);
	if (libswdctx == NULL) {
		LOG_WARNING("libswd_log_level_inherit(): SWD Context not (yet) initialized...\n");
		return LIBSWD_OK;
	}

	libswd_loglevel_t new_swdlevel;
	switch ((loglevel == -1) ? debug_level : loglevel) {
		case LOG_LVL_DEBUG:
			new_swdlevel = LIBSWD_LOGLEVEL_PAYLOAD;
			break;
		case LOG_LVL_INFO:
			new_swdlevel = LIBSWD_LOGLEVEL_INFO;
			break;
		case LOG_LVL_WARNING:
			new_swdlevel = LIBSWD_LOGLEVEL_WARNING;
			break;
		case LOG_LVL_ERROR:
			new_swdlevel = LIBSWD_LOGLEVEL_ERROR;
			break;
		case LOG_LVL_USER:
		case LOG_LVL_OUTPUT:
			new_swdlevel = LIBSWD_LOGLEVEL_NORMAL;
			break;
		case LOG_LVL_SILENT:
			new_swdlevel = LIBSWD_LOGLEVEL_SILENT;
			break;
		default:
			new_swdlevel = LIBSWD_LOGLEVEL_NORMAL;
	}

	int res = libswd_log_level_set(libswdctx, new_swdlevel);
	if (res < 0) {
		LOG_ERROR("libswd_log_level_set() failed (%s)\n", libswd_error_string(res));
		return ERROR_FAIL;
	}
	return new_swdlevel;
}

/** We will use OpenOCD's logging mechanisms to show LibSWD messages.
 * SWD can have different loglevel set than the OpenOCD itself, so we need to
 * log all messages at OpenOCD level that will not block swd messages.
 * It is also possible to 'inherit' loglevel to swd from openocd.
 *
 * \param *libswdctx is the pointer to the libswd context to work with.
 * \param loglevel is the desired log level to show message at.
 * \param *msg, ... is the printf like message to be logged.
 * \return LIBSWD_OK on success, or error code otherwise.
 */
int libswd_log(libswd_ctx_t *libswdctx, libswd_loglevel_t loglevel, char *msg, ...)
{
	if (libswdctx == NULL)
		return LIBSWD_ERROR_NULLCONTEXT;
	if (loglevel > LIBSWD_LOGLEVEL_MAX)
		return LIBSWD_ERROR_PARAM;

	if (loglevel > libswdctx->config.loglevel)
		return LIBSWD_OK;
	va_list ap;
	va_start(ap, msg);
	/* Calling OpenOCD log functions here will cause program crash (va recurrent). */
	vprintf(msg, ap);
	va_end(ap);
	return LIBSWD_OK;
}

/******************************************************************************
 * @{ oocd_transport_swd_libswd_tcl
 * TCL interface for LibSWD based SWD Transport in OpenOCD.
 */

COMMAND_HANDLER(handle_oocd_transport_swd_libswd_loglevel)
{
	int loglevel;
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	libswd_ctx_t *swdctx = (libswd_ctx_t *)arm->dap->ctx;

	switch (CMD_ARGC) {
	case 0:
		LOG_USER("Current SWD LogLevel[%d..%d] is: %d (%s)", LIBSWD_LOGLEVEL_MIN, LIBSWD_LOGLEVEL_MAX,
				swdctx->config.loglevel, libswd_log_level_string(swdctx->config.loglevel));
		break;
	case 1:
		/* We want to allow inherit current OpenOCD's debuglevel. */
		if (strncasecmp(CMD_ARGV[0], "inherit", 7) == 0) {
			loglevel = libswd_log_level_inherit(swdctx, debug_level);
			if (loglevel < 0) {
				LOG_ERROR("LogLevel inherit failed!");
				return ERROR_FAIL;
			} else {
				LOG_USER("Using OpenOCD settings, SWD LogLevel[%d..%d] set to: %d (%s)", LIBSWD_LOGLEVEL_MIN,
						LIBSWD_LOGLEVEL_MAX, loglevel, libswd_log_level_string(loglevel));
				return ERROR_OK;
			}
		}
		/* Or we want to set log level for SWD transport by hand. */
		loglevel = atoi(CMD_ARGV[0]);
		if (loglevel < LIBSWD_LOGLEVEL_MIN || loglevel > LIBSWD_LOGLEVEL_MAX) {
			LOG_ERROR("Bad SWD LogLevel value!");
			return ERROR_FAIL;
		} else
			LOG_USER("Setting SWD LogLevel[%d..%d] to: %d (%s)", LIBSWD_LOGLEVEL_MIN, LIBSWD_LOGLEVEL_MAX, loglevel,
					libswd_log_level_string(loglevel));
		if (libswd_log_level_set(swdctx, loglevel) < 0)
			return ERROR_FAIL;
		else
			return ERROR_OK;
	}
	LOG_INFO("Available values:");
	for (int i = 0; i <= LIBSWD_LOGLEVEL_MAX; i++)
		LOG_INFO(" %d (%s)", i, libswd_log_level_string(i));
	return ERROR_OK;
}

static const
struct command_registration oocd_transport_swd_libswd_subcommand_handlers[] = {
	{
		.name = "loglevel",
		.handler = handle_oocd_transport_swd_libswd_loglevel,
		.mode = COMMAND_ANY,
		.help = "set/inherit/get loglevel for LibSWD-based SWD transport.",
	},

	COMMAND_REGISTRATION_DONE
};

static const
struct command_registration oocd_transport_swd_libswd_command_handlers[] = {
	{
		.name = "libswd",
		.mode = COMMAND_ANY,
		.help = "LibSWD-based SWD transport command group",
		.chain = oocd_transport_swd_libswd_subcommand_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

int oocd_transport_swd_libswd_register_commands(struct command_context *cmd_ctx)
{
	return register_commands(cmd_ctx, NULL, oocd_transport_swd_libswd_command_handlers);
}

/** }@ */
