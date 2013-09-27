/***************************************************************************
 *   Copyright (C) 2011 by Mathias Kuester                                 *
 *   Mathias Kuester <kesmtp@freenet.de>                                   *
 *                                                                         *
 *   Copyright (C) 2011 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   revised:  4/25/13 by brent@mbari.org [DCC target request support]	   *
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
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "jtag/jtag.h"
#include "jtag/hla/hla_transport.h"
#include "jtag/hla/hla_interface.h"
#include "jtag/hla/hla_layout.h"
#include "register.h"
#include "algorithm.h"
#include "target.h"
#include "breakpoints.h"
#include "target_type.h"
#include "armv7m.h"
#include "cortex_m.h"
#include "arm_semihosting.h"
#include "server/server.h"
#include "target_request.h"

#define savedDCRDR  dbgbase  /* FIXME: using target->dbgbase to preserve DCRDR */

#define ARMV7M_SCS_DCRSR	DCB_DCRSR
#define ARMV7M_SCS_DCRDR	DCB_DCRDR

static inline struct hl_interface_s *target_to_adapter(struct target *target)
{
	return target->tap->priv;
}

static int adapter_load_core_reg_u32(struct target *target,
		uint32_t num, uint32_t *value)
{
	int retval;
	struct hl_interface_s *adapter = target_to_adapter(target);

	LOG_DEBUG("%s", __func__);

	/* NOTE:  we "know" here that the register identifiers used
	 * in the v7m header match the Cortex-M3 Debug Core Register
	 * Selector values for R0..R15, xPSR, MSP, and PSP.
	 */
	switch (num) {
	case 0 ... 18:
		/* read a normal core register */
		retval = adapter->layout->api->read_reg(adapter->fd, num, value);

		if (retval != ERROR_OK) {
			LOG_ERROR("JTAG failure %i", retval);
			return ERROR_JTAG_DEVICE_ERROR;
		}
		LOG_DEBUG("load from core reg %i  value 0x%" PRIx32 "", (int)num, *value);
		break;

	case ARMV7M_FPSID:
	case ARMV7M_FPEXC:
		*value = 0;
		break;

	case ARMV7M_FPSCR:
		/* Floating-point Status and Registers */
		retval = target_write_u32(target, ARMV7M_SCS_DCRSR, 33);
		if (retval != ERROR_OK)
			return retval;
		retval = target_read_u32(target, ARMV7M_SCS_DCRDR, value);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("load from core reg %i  value 0x%" PRIx32 "", (int)num, *value);
		break;

	case ARMV7M_S0 ... ARMV7M_S31:
		/* Floating-point Status and Registers */
		retval = target_write_u32(target, ARMV7M_SCS_DCRSR, num-ARMV7M_S0+64);
		if (retval != ERROR_OK)
			return retval;
		retval = target_read_u32(target, ARMV7M_SCS_DCRDR, value);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("load from core reg %i  value 0x%" PRIx32 "", (int)num, *value);
		break;

	case ARMV7M_D0 ... ARMV7M_D15:
		value = 0;
		break;

	case ARMV7M_PRIMASK:
	case ARMV7M_BASEPRI:
	case ARMV7M_FAULTMASK:
	case ARMV7M_CONTROL:
		/* Cortex-M3 packages these four registers as bitfields
		 * in one Debug Core register.  So say r0 and r2 docs;
		 * it was removed from r1 docs, but still works.
		 */
		retval = adapter->layout->api->read_reg(adapter->fd, 20, value);
		if (retval != ERROR_OK)
			return retval;

		switch (num) {
		case ARMV7M_PRIMASK:
			*value = buf_get_u32((uint8_t *) value, 0, 1);
			break;

		case ARMV7M_BASEPRI:
			*value = buf_get_u32((uint8_t *) value, 8, 8);
			break;

		case ARMV7M_FAULTMASK:
			*value = buf_get_u32((uint8_t *) value, 16, 1);
			break;

		case ARMV7M_CONTROL:
			*value = buf_get_u32((uint8_t *) value, 24, 2);
			break;
		}

		LOG_DEBUG("load from special reg %i value 0x%" PRIx32 "",
			  (int)num, *value);
		break;

	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	return ERROR_OK;
}

static int adapter_store_core_reg_u32(struct target *target,
		uint32_t num, uint32_t value)
{
	int retval;
	uint32_t reg;
	struct armv7m_common *armv7m = target_to_armv7m(target);
	struct hl_interface_s *adapter = target_to_adapter(target);

	LOG_DEBUG("%s", __func__);

	/* NOTE:  we "know" here that the register identifiers used
	 * in the v7m header match the Cortex-M3 Debug Core Register
	 * Selector values for R0..R15, xPSR, MSP, and PSP.
	 */
	switch (num) {
	case 0 ... 18:
		retval = adapter->layout->api->write_reg(adapter->fd, num, value);

		if (retval != ERROR_OK) {
			struct reg *r;

			LOG_ERROR("JTAG failure");
			r = armv7m->arm.core_cache->reg_list + num;
			r->dirty = r->valid;
			return ERROR_JTAG_DEVICE_ERROR;
		}
		LOG_DEBUG("write core reg %i value 0x%" PRIx32 "", (int)num, value);
		break;

	case ARMV7M_FPSID:
	case ARMV7M_FPEXC:
		break;

	case ARMV7M_FPSCR:
		/* Floating-point Status and Registers */
		retval = target_write_u32(target, ARMV7M_SCS_DCRDR, value);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target, ARMV7M_SCS_DCRSR, 33 | (1<<16));
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("write core reg %i value 0x%" PRIx32 "", (int)num, value);
		break;

	case ARMV7M_S0 ... ARMV7M_S31:
		/* Floating-point Status and Registers */
		retval = target_write_u32(target, ARMV7M_SCS_DCRDR, value);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target, ARMV7M_SCS_DCRSR, (num-ARMV7M_S0+64) | (1<<16));
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("write core reg %i value 0x%" PRIx32 "", (int)num, value);
		break;

	case ARMV7M_D0 ... ARMV7M_D15:
		break;

	case ARMV7M_PRIMASK:
	case ARMV7M_BASEPRI:
	case ARMV7M_FAULTMASK:
	case ARMV7M_CONTROL:
		/* Cortex-M3 packages these four registers as bitfields
		 * in one Debug Core register.  So say r0 and r2 docs;
		 * it was removed from r1 docs, but still works.
		 */

		adapter->layout->api->read_reg(adapter->fd, 20, &reg);

		switch (num) {
		case ARMV7M_PRIMASK:
			buf_set_u32((uint8_t *) &reg, 0, 1, value);
			break;

		case ARMV7M_BASEPRI:
			buf_set_u32((uint8_t *) &reg, 8, 8, value);
			break;

		case ARMV7M_FAULTMASK:
			buf_set_u32((uint8_t *) &reg, 16, 1, value);
			break;

		case ARMV7M_CONTROL:
			buf_set_u32((uint8_t *) &reg, 24, 2, value);
			break;
		}

		adapter->layout->api->write_reg(adapter->fd, 20, reg);

		LOG_DEBUG("write special reg %i value 0x%" PRIx32 " ", (int)num, value);
		break;

	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	return ERROR_OK;
}

static int adapter_examine_debug_reason(struct target *target)
{
	if ((target->debug_reason != DBG_REASON_DBGRQ)
			&& (target->debug_reason != DBG_REASON_SINGLESTEP)) {
		target->debug_reason = DBG_REASON_BREAKPOINT;
	}

	return ERROR_OK;
}

static int hl_dcc_read(struct hl_interface_s *hl_if, uint8_t *value, uint8_t *ctrl)
{
	uint16_t dcrdr;
	int retval = hl_if->layout->api->read_mem(hl_if->fd,
			DCB_DCRDR, 1, sizeof(dcrdr), (uint8_t *)&dcrdr);
	if (retval == ERROR_OK) {
	    *ctrl = (uint8_t)dcrdr;
	    *value = (uint8_t)(dcrdr >> 8);

	    LOG_DEBUG("data 0x%x ctrl 0x%x", *value, *ctrl);

	    if (dcrdr & 1) {
			/* write ack back to software dcc register
			 * to signify we have read data */
			/* atomically clear just the byte containing the busy bit */
			static const uint8_t zero;
			retval = hl_if->layout->api->write_mem(hl_if->fd, DCB_DCRDR, 1, 1, &zero);
		}
	}
	return retval;
}

static int hl_target_request_data(struct target *target,
	uint32_t size, uint8_t *buffer)
{
	struct hl_interface_s *hl_if = target_to_adapter(target);
	uint8_t data;
	uint8_t ctrl;
	uint32_t i;

	for (i = 0; i < (size * 4); i++) {
		hl_dcc_read(hl_if, &data, &ctrl);
		buffer[i] = data;
	}

	return ERROR_OK;
}

static int hl_handle_target_request(void *priv)
{
	struct target *target = priv;
	if (!target_was_examined(target))
		return ERROR_OK;
	struct hl_interface_s *hl_if = target_to_adapter(target);

	if (!target->dbg_msg_enabled)
		return ERROR_OK;

	if (target->state == TARGET_RUNNING) {
		uint8_t data;
		uint8_t ctrl;

		hl_dcc_read(hl_if, &data, &ctrl);

		/* check if we have data */
		if (ctrl & (1 << 0)) {
			uint32_t request;

			/* we assume target is quick enough */
			request = data;
			hl_dcc_read(hl_if, &data, &ctrl);
			request |= (data << 8);
			hl_dcc_read(hl_if, &data, &ctrl);
			request |= (data << 16);
			hl_dcc_read(hl_if, &data, &ctrl);
			request |= (data << 24);
			target_request(target, request);
		}
	}

	return ERROR_OK;
}

static int adapter_init_arch_info(struct target *target,
				       struct cortex_m3_common *cortex_m3,
				       struct jtag_tap *tap)
{
	struct armv7m_common *armv7m;

	LOG_DEBUG("%s", __func__);

	armv7m = &cortex_m3->armv7m;
	armv7m_init_arch_info(target, armv7m);

	armv7m->load_core_reg_u32 = adapter_load_core_reg_u32;
	armv7m->store_core_reg_u32 = adapter_store_core_reg_u32;

	armv7m->examine_debug_reason = adapter_examine_debug_reason;
	armv7m->stlink = true;

	target_register_timer_callback(hl_handle_target_request, 1, 1, target);

	return ERROR_OK;
}

/* TODO:CONSIDER: In reality this ITM stimulus port "console" support should be
 * generic (part of armv7m maybe) and not tied to this HLA interface support. In
 * fact the "telnet" connection support source should probably be somewhere like
 * "src/server/itm_console.c" or even a generic "console" support file, since
 * there is NOTHING ITM specific about it as regards being given characters to
 * output. The aim is that "nc <ipaddr> <port>" should be useable to capture
 * binary data output on a "console" stream. */

static const char *itm_packet_port;
static const char *itm_stimport_network[ITM_MAX_STIMPORT];
#if ((ITM_MAX_STIMPORT % 32) != 0)
# error ITM maximum stimulus port count should be a multiple of 32
#endif
/* Bitmap of whether we are capturing the specific stimport output to a file: */
static uint32_t itm_stimport_capture[ITM_MAX_STIMPORT / 32];

struct itm_stimport_connection {
	struct connection *connection; /* back reference to parent */
	int closed;
};

struct itm_stimport_service {
	struct itm_stimport_connection *cc;
	uint8_t stimport;
	int do_capture;
	FILE *cFile;
	/* CONSIDER:IMPLEMENT: We could hold a buffer of data received since the
	 * "last target TPIU reset", where we can supply buffered data to any
	 * new connection. We do not actually need this since we can just rely
	 * on the user attaching to the relevant telnet port prior to executing
	 * their application/test. We already have support for saving all of the
	 * captured trace output to a file for later processing if needed. */
};

static int itm_stimport_write(struct connection *connection, const void *data, int len)
{
	struct itm_stimport_connection *cc = connection->priv;

	if (cc) {
		if (cc->closed)
			return ERROR_SERVER_REMOTE_CLOSED;

		if (connection_write(connection, data, len) == len)
			return ERROR_OK;

		cc->closed = 1;
	} else {
		LOG_ERROR("BUG: ITM console connection->priv == NULL");
	}

	return ERROR_SERVER_REMOTE_CLOSED;
}

int itm_stimport_output(void *priv, unsigned char db)
{
	struct itm_stimport_service *cs = priv;

	assert(cs != NULL);

	if (cs->do_capture) {
		if (cs->cFile == NULL) {
			char fnbuffer[256];
			char *filename = "capture.bin";
			time_t now = time(NULL);
#if defined(__USE_MINGW_ANSI_STDIO) /* mingw32 FC14 does not seem to have localtime_r() */
			struct tm *pln = localtime(&now);
			snprintf(fnbuffer, 256, "%04d%02d%02d_%02d%02d%02d_stimport-%u_capture.bin", \
				 (1900 + pln->tm_year), (pln->tm_mon + 1), pln->tm_mday, \
				 pln->tm_hour, pln->tm_min, pln->tm_sec, cs->stimport);
			filename = fnbuffer;
#else /* !MINGW */
			struct tm ln;
			if (localtime_r(&now, &ln)) {
				snprintf(fnbuffer, 256, "%04d%02d%02d_%02d%02d%02d_stimport-%u_capture.bin", \
					 (1900 + ln.tm_year), (ln.tm_mon + 1), ln.tm_mday, \
					 ln.tm_hour, ln.tm_min, ln.tm_sec, cs->stimport);
				filename = fnbuffer;
			}
#endif /* !MINGW */
			cs->cFile = fopen(filename, "w+"); /* create or overwrite */
			setvbuf(cs->cFile, NULL, _IONBF, 0);
		}
		if (cs->cFile) {
			unsigned char lc = db;
			if (fwrite(&lc, 1, 1, cs->cFile) != 1) {
				LOG_ERROR("Failed to write to capture file (disabling capture)");
				cs->do_capture = 0;
				(void)fclose(cs->cFile);
				cs->cFile = NULL;
			}
		} else {
			LOG_ERROR("Failed to create capture file (disabling capture)");
			cs->do_capture = 0;
		}
	}

	struct itm_stimport_connection *cc = cs->cc;

	if (cc) {
		unsigned char lc = db;
		assert(cc->connection != NULL);
		return itm_stimport_write(cc->connection, &lc, sizeof(unsigned char));
	} else {
		/* CONSIDER: As commented above, we could be buffering this for
		 * later "telnet" console connection. We can either have a large
		 * pre-allocated buffer, that we realloc() occasionally; or just
		 * drop old data (have a wrapping buffer). */
	}

	return ERROR_OK;
}

static int itm_stimport_new_connection(struct connection *connection)
{
	struct itm_stimport_connection *cc = calloc(1, sizeof(struct itm_stimport_connection));

	connection->priv = cc;
	if (cc) {
		struct itm_stimport_service *cs = connection->service->priv;
		assert(cs != NULL);

		cc->connection = connection;
		cs->cc = cc;

		/* CONSIDER: If we do add pre-connection buffering then we
		 * should output the buffer at this point by calling
		 * "itm_stimport_write(connection,...)". */
	}

	return ERROR_OK;
}

/* Arbitrary size buffer to cope with characters being received on the telnet
 * connection. At the moment input data is not processed, but if support is
 * required then the buffering can be tuned to match the expected usage. */
#define ITM_STIMPORT_BUFFER_SIZE (32)

static int itm_stimport_input(struct connection *connection)
{
	/* struct itm_stimport_connection *cc = connection->priv; */
	unsigned char buffer[ITM_STIMPORT_BUFFER_SIZE];
	int bytes_read;

	bytes_read = connection_read(connection, buffer, ITM_STIMPORT_BUFFER_SIZE);
	if (bytes_read == 0) {
		return ERROR_SERVER_REMOTE_CLOSED;
	} else if (bytes_read == -1) {
		LOG_ERROR("error during ITM console read: %s", strerror(errno));
		return ERROR_SERVER_REMOTE_CLOSED;
	}

	LOG_OUTPUT("TODO: process input of %d bytes", bytes_read);
	/* We could use the "standard" approach of writing explicit patterns to
	 * known target locations to provide a back-channel to the target if
	 * bi-directional communication is required. For the moment we just drop
	 * input, since we are adding the support initially for unidirectional
	 * trace and diagnostic output from the target. */

	return ERROR_OK;
}

static int itm_stimport_connection_closed(struct connection *connection)
{
	struct itm_stimport_connection *cc = connection->priv;

	if (cc) {
		free(cc);
		connection->priv = NULL;
	} else {
		LOG_ERROR("BUG: ITM console connection->priv == NULL");
	}

	return ERROR_OK;
}

static int itm_stimport_init(struct target *target)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);
	int res;
	unsigned int idx;

	assert(armv7m != NULL);

	if (strcmp(itm_packet_port, "disabled") != 0) {
		struct itm_stimport_service *tpiu_packet_service = calloc(1, sizeof(struct itm_stimport_service));
		if (tpiu_packet_service) {
			armv7m->tpiu_packet_service = tpiu_packet_service;
			res = add_service("itm_packets", itm_packet_port, 1, itm_stimport_new_connection, \
					  itm_stimport_input, itm_stimport_connection_closed, tpiu_packet_service);
			if (ERROR_OK != res)
				LOG_ERROR("Failed to add ITM packet service (res %d)", res);
		}
	}

	res = ERROR_OK;
	for (idx = 0; (idx < ITM_MAX_STIMPORT); idx++) {
		if (strcmp(itm_stimport_network[idx], "disabled") != 0) {
			struct itm_stimport_service *itm_stimport_service = calloc(1, sizeof(struct itm_stimport_service));
			if (itm_stimport_service) {
				itm_stimport_service->stimport = idx;

				if (itm_stimport_capture[idx / 32] & (1 << (idx % 32)))
					itm_stimport_service->do_capture = -1;

				/* hook to allow ITM trace to output */
				armv7m->itm_stimport_service[idx] = itm_stimport_service;
				res = add_service("itm_stimport", itm_stimport_network[idx], 1, itm_stimport_new_connection, \
						  itm_stimport_input, itm_stimport_connection_closed, itm_stimport_service);
				if (ERROR_OK != res)
					break;

				LOG_INFO("ITM stimulus port %i available on server port %s%s", idx, \
					 itm_stimport_network[idx], (itm_stimport_service->do_capture ? " (capturing to file)" : ""));
			}
		}
	}

	return res;
}

COMMAND_HANDLER(handle_cortex_m_itm_stimport_command)
{
	if (CMD_ARGC < 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	unsigned spnum;
	COMMAND_PARSE_NUMBER(uint, CMD_ARGV[0], spnum);

	if (spnum >= ITM_MAX_STIMPORT) {
		command_print(CMD_CTX, "Stimulus port %i is out of bounds", spnum);
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (strcmp(CMD_ARGV[1], "disabled") == 0) {
		if (itm_stimport_network[spnum])
			free((char *)itm_stimport_network[spnum]);
		itm_stimport_network[spnum] = strdup("disabled");
		return ERROR_OK;
	}

	unsigned npnum;
	COMMAND_PARSE_NUMBER(uint, CMD_ARGV[1], npnum);

	if ((npnum < 1000) || (npnum > 65535)) {
		command_print(CMD_CTX, "Stimulus port %i network port %i is out of bounds", spnum, npnum);
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	/* We could check if npnum is already in use by other stimulus port, tcl
	 * or telnet configurations. However, if a clash is configured then a
	 * "port in use" error will be raised so we can just leave that code to
	 * catch duplication errors. */
	itm_stimport_network[spnum] = malloc(5 + 1);
	sprintf((char *)itm_stimport_network[spnum], "%i", npnum);

	if (CMD_ARGC > 2) {
		if (strcmp(CMD_ARGV[2], "capture") == 0) {
			itm_stimport_capture[spnum / 32] |= (1 << (spnum % 32));
		} else {
			command_print(CMD_CTX, "Stimulus port %i network port %i unrecognised modifier", spnum, npnum);
			return ERROR_COMMAND_SYNTAX_ERROR;
		}
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_cortex_m_itm_packets_command)
{
	return CALL_COMMAND_HANDLER(server_pipe_command, &itm_packet_port);
}

#define TPIU_BOOL_WRAPPER(name, print_name)	\
	COMMAND_HANDLER(handle_cortex_m_itm_ ## name) \
	{ \
		struct target *target = get_current_target(CMD_CTX); \
		assert(target != NULL); \
		struct armv7m_common *armv7m = target_to_armv7m(target); \
		assert(armv7m != NULL); \
		return CALL_COMMAND_HANDLER(handle_command_parse_bool, \
			&armv7m->name, print_name); \
	}

TPIU_BOOL_WRAPPER(tpiusync, "TPIO sync packets")
TPIU_BOOL_WRAPPER(dwt, "DWT packets")

static const struct command_registration cortex_m_itm_any_command_handlers[] = {
	{
		.name = "stimport",
		.handler = handle_cortex_m_itm_stimport_command,
		.mode = COMMAND_ANY,
		.help = "Specify stimulus port to capture "
			"and network TCP port on which to listen "
			"for incoming network connections.",
		.usage = "stimport_num tcp_port|'disabled' ['capture']",
	},
	{
		.name = "packets",
		.handler = handle_cortex_m_itm_packets_command,
		.mode = COMMAND_ANY,
		.help = "Specify network TCP port on which to listen "
			"for incoming network connections. The port "
			"will provide the raw (unprocessed) TPIU packets.",
		.usage = "tcp_port",
	},
	{
		.name = "tpiusync",
		.handler = handle_cortex_m_itm_tpiusync,
		.mode = COMMAND_ANY,
		.help = "Display or modify flag controlling generation "
			"of TPIU sync packets.",
		.usage = "['enable'|'disable']",
	},
	{
		.name = "dwt",
		.handler = handle_cortex_m_itm_dwt,
		.mode = COMMAND_ANY,
		.help = "Display or modify flag controlling generation "
			"of DWT packets.",
		.usage = "['enable'|'disable']",
	},
	COMMAND_REGISTRATION_DONE
};

/* NOTE: Ideally we should NEVER generate info/debug/logging output in these ITM
 * and DWT processing routines, since slow processing of the trace capture can
 * result in OVERFLOW packets being seen if the trace is not emptied quickly
 * enough. */

static void itm_process_data(void *priv, struct armv7m_tpiu_pktstate *ctx)
{
	struct target *target = priv;
	assert(target != NULL);

	struct armv7m_common *armv7m = target_to_armv7m(target);
	assert(armv7m != NULL);

	/* Check if connection for this stimulus port: */
	if (armv7m->itm_stimport_service[ctx->pcode]) {
		unsigned int idx;
		for (idx = 0; (idx < ctx->size); idx++) {
			unsigned char db = ((ctx->pdata >> (idx * 8)) & 0xFF);
			itm_stimport_output(armv7m->itm_stimport_service[ctx->pcode], db);
		}
	}

	return;
}

static void dwt_process_data(void *priv, struct armv7m_tpiu_pktstate *ctx)
{
	/* DWT discriminator IDs:
	 *     0 - Event counter wrapping
	 *         1-byte with bitmask of counter overflow marker bits.
	 *     1 - Exception tracing
	 *         2-byte exception number and event description.
	 *     2 - PC sample
	 *         1-byte for WFI/WFE (CPU asleep) or 4-byte for PC sample.
	 * 8..23 - Data tracing
	 *
	 * NOTE: The DWT_SLEEPCNT counts the number of "power saving" cycles
	 * initiated by WFI or WFE. */

	/* CONSIDER: We provide a mechanism for external tools to get the raw
	 * TPIU stream. However, we could provide "profiling" support within
	 * OpenOCD by processing PC sampling (if enabled).
	 *
	 * Other SWD systems provide profiling by keeping counts of CPU
	 * addresses seen. We should only ever see 16bit aligned PC values when
	 * dealing with ARM/Thumb CPUs and some simple profiles use that to
	 * provide a simple 16-bit profile count. If we wanted to leave the CPU
	 * running for longer then we would ideally want deeper counts
	 * (e.g. 64-bit). We would need command line functionality to allow the
	 * profile dump gathered so far to be output. This could either be via
	 * the gdb "monitor" interface, or directly via the telnet
	 * console. Ideally we would have the option to wrap the data gathered
	 * in a gprof "gmon.out" compatible form for direct processing. We would
	 * need to know "address+length" pairs for areas of the target 32-bit
	 * memory space to track PC samples for. If the PC is outside of the
	 * configured ranges then we should track that count as "other" so that
	 * some information is held (much like we track how many samples were in
	 * "WFI/WFE" state). It should not be an issue for resource rich hosts
	 * (PCs with lots of memory) to set aside buffers for the memory sizes
	 * normally available on Cortex-M SoC designs (e.g. 256K). */

	if (ctx->pcode == 2) {
		if (ctx->size == 1) {
			if (ctx->pdata == 0x00) {
				static uint32_t total_idle;
				total_idle++;
			} /* else DWT PC sample size 1 but non-zero */
		} else if (ctx->size == 4) {
			/* IMPLEMENT: Track sample for PC
			 * "ctx->pdata". Increment count in relevant "pot" for
			 * sampled address. */
		} /* else DWT PC sample size unrecognised */
	}

	return;
}

/* NOTE: Issues were seen receiving trace data when using ST-LINK/V2 firmware
 * reporting JTAG v14. The ST-LINK/V2 JTAG v17 firmware seems to work MUCH
 * better. */

/* NOTE: We do this FSM-driven byte-at-a-time processing since we seem to get
 * partial packets returned by the trace data fetch, which means we need to keep
 * state between data reads from the USB ST-LINK/V2 interface. */

static void itmdwt_process_byte(void *priv, struct armv7m_tpiu_pktstate *ctx, unsigned char db)
{
	struct target *target = priv;
	assert(target != NULL);

	struct armv7m_common *armv7m = target_to_armv7m(target);
	assert(armv7m != NULL);

	switch (ctx->fsm) {
	case PKTFSM_HDR:
		switch (db) {
		case ITMDWTPP_SYNC:
		case ITMDWTPP_OVERFLOW:
			/* stay at PKTFSM_HDR */
			break;

		default:
			{
				unsigned char source = (db & ITMDWTPP_TYPE_MASK);
				unsigned char size = 0;

				switch (source) {
				case ITMDWTPP_TYPE_PROTOCOL:
					/*
					if (db & ITMDWTPP_PROTOCOL_EXTENSION) {
					  EX[2:0] in bits 4..6 with C in bit 7
					  if (db & ITMDWTPP_SOURCE_SELECTION) {
					    DWT EXTENSION "db"
					  } else {
					    ITM EXTENSION page:
					    ((db&ITMDWTPP_PROTOCOL_EXT_ITM_PAGE_MASK)>>ITMDWTPP_PROTOCOL_EXT_ITM_PAGE_SHIFT)
					  }
					} else {
					  if (db & ITMDWTPP_SOURCE_SELECTION) {
					    if ((db & 0x94) == 0x94) {
					      GLOBAL timestamp "db"
					    } else {
					      RESERVED "db"
					    }
					  } else {
					    LOCAL timestamp "db"
					  }
					}
					*/
					break;

				case ITMDWTPP_TYPE_SOURCE1:
					size = 1;
				case ITMDWTPP_TYPE_SOURCE2:
					if (size == 0)
						size = 2;
				case ITMDWTPP_TYPE_SOURCE4:
					if (size == 0)
						size = 4;

					/* Common source processing */
					if (db & ITMDWTPP_SOURCE_SELECTION) {
						unsigned char id = ((db & ITMDWTPP_SOURCE_MASK) >> ITMDWTPP_SOURCE_SHIFT);
						ctx->fsm = PKTFSM_DWT1;
						ctx->pcode = id;
						ctx->pdata = 0;
					} else {
						unsigned char port = ((db & ITMDWTPP_SOURCE_MASK) >> ITMDWTPP_SOURCE_SHIFT);
						ctx->fsm = PKTFSM_ITM1;
						ctx->pcode = port;
						ctx->pdata = 0;
					}
					break;
				}

				ctx->size = size;
			}
			break;
		}
		break;

	case PKTFSM_ITM1:
		ctx->pdata = db;
		if (ctx->size == 1) {
			itm_process_data(priv, ctx);
			ctx->fsm = PKTFSM_HDR;
		} else {
			ctx->fsm = PKTFSM_ITM2;
		}
		break;

	case PKTFSM_ITM2:
		ctx->pdata |= (db << 8);
		if (ctx->size == 2) {
			itm_process_data(priv, ctx);
			ctx->fsm = PKTFSM_HDR;
		} else {
			ctx->fsm = PKTFSM_ITM3;
		}
		break;

	case PKTFSM_ITM3:
		ctx->pdata |= (db << 16);
		if (ctx->size == 3) {
			itm_process_data(priv, ctx);
			ctx->fsm = PKTFSM_HDR;
		} else {
			ctx->fsm = PKTFSM_ITM4;
		}
		break;

	case PKTFSM_ITM4:
		ctx->pdata |= (db << 24);
		if (ctx->size == 4) {
			itm_process_data(priv, ctx);
			ctx->fsm = PKTFSM_HDR;
		} else {
			/* Unexpected size */
			ctx->fsm = PKTFSM_HDR;
		}
		break;

	case PKTFSM_DWT1:
		ctx->pdata = db;
		if (ctx->size == 1) {
			dwt_process_data(priv, ctx);
			ctx->fsm = PKTFSM_HDR;
		} else {
			ctx->fsm = PKTFSM_DWT2;
		}
		break;

	case PKTFSM_DWT2:
		ctx->pdata |= (db << 8);
		if (ctx->size == 2) {
			dwt_process_data(priv, ctx);
			ctx->fsm = PKTFSM_HDR;
		} else {
			ctx->fsm = PKTFSM_DWT3;
		}
		break;

	case PKTFSM_DWT3:
		ctx->pdata |= (db << 16);
		if (ctx->size == 3) {
			dwt_process_data(priv, ctx);
			ctx->fsm = PKTFSM_HDR;
		} else {
			ctx->fsm = PKTFSM_DWT4;
		}
		break;

	case PKTFSM_DWT4:
		ctx->pdata |= (db << 24);
		if (ctx->size == 4) {
			dwt_process_data(priv, ctx);
			ctx->fsm = PKTFSM_HDR;
		} else {
			/* Unexpected size */
			ctx->fsm = PKTFSM_HDR;
		}
		break;
	}

	return;
}

void trace_process(struct target *target, uint8_t *buf, size_t size)
{
	/* If configured for raw packet capture then write raw trace data as
	 * received to the open connection: */
	struct armv7m_common *armv7m = target_to_armv7m(target);
	assert(armv7m != NULL);

	if (armv7m->tpiu_packet_service) {
		struct itm_stimport_service *cs = armv7m->tpiu_packet_service;
		struct itm_stimport_connection *cc = cs->cc;
		/* This can be used by external tools to reconstruct the trace
		 * data (and will have the console and instrumentation
		 * interleaved along with any other enabled trace data). */
		/* NOTE: In reality we could leave all TPIU processing to
		 * external tools if we wanted to avoid OpenOCD having to parse
		 * the packet format; and deal with stimulus ports, PC samples,
		 * etc. However, it is useful having built-in support for at
		 * least diagnostic output when using OpenOCD in a test server
		 * environment. */
		if (cc) {
			assert(cc->connection != NULL);
			itm_stimport_write(cc->connection, buf, (int)size);
		}
	}

	unsigned int idx;
	for (idx = 0; (idx < size); idx++)
		itmdwt_process_byte(target, &(armv7m->tpiu_pktstate), buf[idx]);

	return;
}

static int tpiu_reset(struct target *target)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);
	assert(armv7m != NULL);

	/* Close any active capture files: */
	unsigned int idx;
	for (idx = 0; (idx < ITM_MAX_STIMPORT); idx++) {
		struct itm_stimport_service *cs = armv7m->itm_stimport_service[idx];
		if (cs) {
			if (cs->cFile) {
				(void)fclose(cs->cFile);
				cs->cFile = NULL;
			}
		}
	}

	memset(&(armv7m->tpiu_pktstate), '\0', sizeof(struct armv7m_tpiu_pktstate));

	return ERROR_OK;
}

static int adapter_init_target(struct command_context *cmd_ctx,
				    struct target *target)
{
	LOG_DEBUG("%s", __func__);

	armv7m_build_reg_cache(target);

	/* TODO:CONSIDER: This is not necessarily the best place for this
	 * initialisation, but it will suffice for initial testing. */
	if (itm_packet_port == NULL)
		itm_packet_port = strdup("disabled");

	unsigned int idx;
	for (idx = 0; (idx < ITM_MAX_STIMPORT); idx++) {
		if (itm_stimport_network[idx] == NULL) {
			/* If we wanted we could check for explicit "idx" values and
			 * enable services for those stimulus ports by default by
			 * setting a valid network port value. e.g.
				if (idx == 0) {
					itm_stimport_network[idx] = strdup("4445");
				}
			 * At the moment we provide no default port mappings. */
			itm_stimport_network[idx] = strdup("disabled");
		}
	}
	itm_stimport_init(target);

	return ERROR_OK;
}

static int adapter_target_create(struct target *target,
		Jim_Interp *interp)
{
	LOG_DEBUG("%s", __func__);

	struct cortex_m3_common *cortex_m3 = calloc(1, sizeof(struct cortex_m3_common));

	if (!cortex_m3)
		return ERROR_COMMAND_SYNTAX_ERROR;

	adapter_init_arch_info(target, cortex_m3, target->tap);

	struct armv7m_common *armv7m = target_to_armv7m(target);
	assert(armv7m != NULL);
	armv7m->tpiusync = true;
	armv7m->dwt = false;

	return ERROR_OK;
}

static int adapter_load_context(struct target *target)
{
	struct armv7m_common *armv7m = target_to_armv7m(target);
	int num_regs = armv7m->arm.core_cache->num_regs;

	for (int i = 0; i < num_regs; i++) {

		struct reg *r = &armv7m->arm.core_cache->reg_list[i];
		if (!r->valid)
			armv7m->arm.read_core_reg(target, r, i, ARM_MODE_ANY);
	}

	return ERROR_OK;
}

static int adapter_debug_entry(struct target *target)
{
	struct hl_interface_s *adapter = target_to_adapter(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	struct arm *arm = &armv7m->arm;
	struct reg *r;
	uint32_t xPSR;
	int retval;

	/* preserve the DCRDR across halts */
	retval = target_read_u32(target, DCB_DCRDR, &target->savedDCRDR);
	if (retval != ERROR_OK)
		return retval;

	retval = armv7m->examine_debug_reason(target);
	if (retval != ERROR_OK)
		return retval;

	adapter_load_context(target);

	/* make sure we clear the vector catch bit */
	adapter->layout->api->write_debug_reg(adapter->fd, DCB_DEMCR, TRCENA);

	r = arm->cpsr;
	xPSR = buf_get_u32(r->value, 0, 32);

	/* Are we in an exception handler */
	if (xPSR & 0x1FF) {
		armv7m->exception_number = (xPSR & 0x1FF);

		arm->core_mode = ARM_MODE_HANDLER;
		arm->map = armv7m_msp_reg_map;
	} else {
		unsigned control = buf_get_u32(arm->core_cache
				->reg_list[ARMV7M_CONTROL].value, 0, 2);

		/* is this thread privileged? */
		arm->core_mode = control & 1
				? ARM_MODE_USER_THREAD
				: ARM_MODE_THREAD;

		/* which stack is it using? */
		if (control & 2)
			arm->map = armv7m_psp_reg_map;
		else
			arm->map = armv7m_msp_reg_map;

		armv7m->exception_number = 0;
	}

	LOG_DEBUG("entered debug state in core mode: %s at PC 0x%08" PRIx32 ", target->state: %s",
		arm_mode_name(arm->core_mode),
		*(uint32_t *)(arm->pc->value),
		target_state_name(target));

	return retval;
}

static int adapter_poll(struct target *target)
{
	enum target_state state;
	struct hl_interface_s *adapter = target_to_adapter(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	enum target_state prev_target_state = target->state;

	state = adapter->layout->api->state(adapter->fd);

	if (state == TARGET_UNKNOWN) {
		LOG_ERROR("jtag status contains invalid mode value - communication failure");
		return ERROR_TARGET_FAILURE;
	}

	if (target->state == state)
		return ERROR_OK;

	if (state == TARGET_HALTED) {
		target->state = state;

		int retval = adapter_debug_entry(target);
		if (retval != ERROR_OK)
			return retval;

		if (prev_target_state == TARGET_DEBUG_RUNNING) {
			target_call_event_callbacks(target, TARGET_EVENT_DEBUG_HALTED);
		} else {
			if (arm_semihosting(target, &retval) != 0)
				return retval;

			target_call_event_callbacks(target, TARGET_EVENT_HALTED);
		}

		LOG_DEBUG("halted: PC: 0x%08x", buf_get_u32(armv7m->arm.pc->value, 0, 32));
	}

	return ERROR_OK;
}

static int adapter_assert_reset(struct target *target)
{
	int res = ERROR_OK;
	struct hl_interface_s *adapter = target_to_adapter(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	bool use_srst_fallback = true;

	LOG_DEBUG("%s", __func__);

	enum reset_types jtag_reset_config = jtag_get_reset_config();

	bool srst_asserted = false;

	if ((jtag_reset_config & RESET_HAS_SRST) &&
	    (jtag_reset_config & RESET_SRST_NO_GATING)) {
		jtag_add_reset(0, 1);
		res = adapter->layout->api->assert_srst(adapter->fd, 0);
		srst_asserted = true;
	}

	adapter->layout->api->write_debug_reg(adapter->fd, DCB_DHCSR, DBGKEY|C_DEBUGEN);

	/* only set vector catch if halt is requested */
	if (target->reset_halt)
		adapter->layout->api->write_debug_reg(adapter->fd, DCB_DEMCR, TRCENA|VC_CORERESET);
	else
		adapter->layout->api->write_debug_reg(adapter->fd, DCB_DEMCR, TRCENA);

	if (jtag_reset_config & RESET_HAS_SRST) {
		if (!srst_asserted) {
			jtag_add_reset(0, 1);
			res = adapter->layout->api->assert_srst(adapter->fd, 0);
		}
		if (res == ERROR_COMMAND_NOTFOUND)
			LOG_ERROR("Hardware srst not supported, falling back to software reset");
		else if (res == ERROR_OK) {
			/* hardware srst supported */
			use_srst_fallback = false;
		}
	}

	if (use_srst_fallback) {
		/* stlink v1 api does not support hardware srst, so we use a software reset fallback */
		adapter->layout->api->write_debug_reg(adapter->fd, NVIC_AIRCR, AIRCR_VECTKEY | AIRCR_SYSRESETREQ);
	}

	res = adapter->layout->api->reset(adapter->fd);

	if (res != ERROR_OK)
		return res;

	/* registers are now invalid */
	register_cache_invalidate(armv7m->arm.core_cache);

	if (target->reset_halt) {
		target->state = TARGET_RESET;
		target->debug_reason = DBG_REASON_DBGRQ;
	} else {
		target->state = TARGET_HALTED;
	}

	return ERROR_OK;
}

static int adapter_deassert_reset(struct target *target)
{
	struct hl_interface_s *adapter = target_to_adapter(target);

	enum reset_types jtag_reset_config = jtag_get_reset_config();

	LOG_DEBUG("%s", __func__);

	if (jtag_reset_config & RESET_HAS_SRST)
		adapter->layout->api->assert_srst(adapter->fd, 1);

	/* virtual deassert reset, we need it for the internal
	 * jtag state machine
	 */
	jtag_add_reset(0, 0);

	if (tpiu_reset(target) != ERROR_OK) {
		LOG_ERROR("Failed to initialise ITM state");
		/* continue since we do not treat this as fatal */
	}

	target->savedDCRDR = 0;  /* clear both DCC busy bits on initial resume */

	return target->reset_halt ? ERROR_OK : target_resume(target, 1, 0, 0, 0);
}

static int adapter_halt(struct target *target)
{
	int res;
	struct hl_interface_s *adapter = target_to_adapter(target);

	LOG_DEBUG("%s", __func__);

	if (target->state == TARGET_HALTED) {
		LOG_DEBUG("target was already halted");
		return ERROR_OK;
	}

	if (target->state == TARGET_UNKNOWN)
		LOG_WARNING("target was in unknown state when halt was requested");

	res = adapter->layout->api->halt(adapter->fd);

	if (res != ERROR_OK)
		return res;

	target->debug_reason = DBG_REASON_DBGRQ;

	return ERROR_OK;
}

static int adapter_resume(struct target *target, int current,
		uint32_t address, int handle_breakpoints,
		int debug_execution)
{
	int res;
	struct hl_interface_s *adapter = target_to_adapter(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	uint32_t resume_pc;
	struct breakpoint *breakpoint = NULL;
	struct reg *pc;

	LOG_DEBUG("%s %d 0x%08x %d %d", __func__, current, address,
			handle_breakpoints, debug_execution);

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!debug_execution) {
		target_free_all_working_areas(target);
		cortex_m3_enable_breakpoints(target);
		cortex_m3_enable_watchpoints(target);
	}

	pc = armv7m->arm.pc;
	if (!current) {
		buf_set_u32(pc->value, 0, 32, address);
		pc->dirty = true;
		pc->valid = true;
	}

	if (!breakpoint_find(target, buf_get_u32(pc->value, 0, 32))
			&& !debug_execution) {
		armv7m_maybe_skip_bkpt_inst(target, NULL);
	}

	resume_pc = buf_get_u32(pc->value, 0, 32);

	/* write any user vector flags */
	res = target_write_u32(target, DCB_DEMCR, TRCENA | armv7m->demcr);
	if (res != ERROR_OK)
		return res;

	armv7m_restore_context(target);

	/* restore savedDCRDR */
	res = target_write_u32(target, DCB_DCRDR, target->savedDCRDR);
	if (res != ERROR_OK)
		return res;

	/* registers are now invalid */
	register_cache_invalidate(armv7m->arm.core_cache);

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		/* Single step past breakpoint at current address */
		breakpoint = breakpoint_find(target, resume_pc);
		if (breakpoint) {
			LOG_DEBUG("unset breakpoint at 0x%8.8" PRIx32 " (ID: %d)",
					breakpoint->address,
					breakpoint->unique_id);
			cortex_m3_unset_breakpoint(target, breakpoint);

			res = adapter->layout->api->step(adapter->fd);

			if (res != ERROR_OK)
				return res;

			cortex_m3_set_breakpoint(target, breakpoint);
		}
	}

	res = adapter->layout->api->run(adapter->fd);

	if (res != ERROR_OK)
		return res;

	target->debug_reason = DBG_REASON_NOTHALTED;

	if (!debug_execution) {
		target->state = TARGET_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
	} else {
		target->state = TARGET_DEBUG_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
	}

	return ERROR_OK;
}

static int adapter_step(struct target *target, int current,
		uint32_t address, int handle_breakpoints)
{
	int res;
	struct hl_interface_s *adapter = target_to_adapter(target);
	struct armv7m_common *armv7m = target_to_armv7m(target);
	struct breakpoint *breakpoint = NULL;
	struct reg *pc = armv7m->arm.pc;
	bool bkpt_inst_found = false;

	LOG_DEBUG("%s", __func__);

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!current) {
		buf_set_u32(pc->value, 0, 32, address);
		pc->dirty = true;
		pc->valid = true;
	}

	uint32_t pc_value = buf_get_u32(pc->value, 0, 32);

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		breakpoint = breakpoint_find(target, pc_value);
		if (breakpoint)
			cortex_m3_unset_breakpoint(target, breakpoint);
	}

	armv7m_maybe_skip_bkpt_inst(target, &bkpt_inst_found);

	target->debug_reason = DBG_REASON_SINGLESTEP;

	armv7m_restore_context(target);

	/* restore savedDCRDR */
	res = target_write_u32(target, DCB_DCRDR, target->savedDCRDR);
	if (res != ERROR_OK)
		return res;

	target_call_event_callbacks(target, TARGET_EVENT_RESUMED);

	res = adapter->layout->api->step(adapter->fd);

	if (res != ERROR_OK)
		return res;

	/* registers are now invalid */
	register_cache_invalidate(armv7m->arm.core_cache);

	if (breakpoint)
		cortex_m3_set_breakpoint(target, breakpoint);

	adapter_debug_entry(target);
	target_call_event_callbacks(target, TARGET_EVENT_HALTED);

	LOG_INFO("halted: PC: 0x%08x", buf_get_u32(armv7m->arm.pc->value, 0, 32));

	return ERROR_OK;
}

static int adapter_read_memory(struct target *target, uint32_t address,
		uint32_t size, uint32_t count,
		uint8_t *buffer)
{
	struct hl_interface_s *adapter = target_to_adapter(target);

	if (!count || !buffer)
		return ERROR_COMMAND_SYNTAX_ERROR;

	LOG_DEBUG("%s 0x%08x %d %d", __func__, address, size, count);

	return adapter->layout->api->read_mem(adapter->fd, address, size, count, buffer);
}

static int adapter_write_memory(struct target *target, uint32_t address,
		uint32_t size, uint32_t count,
		const uint8_t *buffer)
{
	struct hl_interface_s *adapter = target_to_adapter(target);

	if (!count || !buffer)
		return ERROR_COMMAND_SYNTAX_ERROR;

	LOG_DEBUG("%s 0x%08x %d %d", __func__, address, size, count);

	return adapter->layout->api->write_mem(adapter->fd, address, size, count, buffer);
}

static const struct command_registration adapter_command_handlers[] = {
	{
		.chain = arm_command_handlers,
	},
	/* This "itm" command support should really be dependant on a Cortex-M
	 * CPU being detected, or the target interface explicitly supporting a
	 * Cortex-M CPU. Se need to find the relevant source tree point for
	 * adding this "extension". */
	{
		.name = "itm",
		.mode = COMMAND_ANY,
		.help = "Cortex-M ITM specific commands",
		.usage = "",
		.chain = cortex_m_itm_any_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct target_type hla_target = {
	.name = "hla_target",
	.deprecated_name = "stm32_stlink",

	.init_target = adapter_init_target,
	.target_create = adapter_target_create,
	.examine = cortex_m3_examine,
	.commands = adapter_command_handlers,

	.poll = adapter_poll,
	.arch_state = armv7m_arch_state,

	.target_request_data = hl_target_request_data,
	.assert_reset = adapter_assert_reset,
	.deassert_reset = adapter_deassert_reset,

	.halt = adapter_halt,
	.resume = adapter_resume,
	.step = adapter_step,

	.get_gdb_reg_list = armv7m_get_gdb_reg_list,

	.read_memory = adapter_read_memory,
	.write_memory = adapter_write_memory,
	.checksum_memory = armv7m_checksum_memory,
	.blank_check_memory = armv7m_blank_check_memory,

	.run_algorithm = armv7m_run_algorithm,
	.start_algorithm = armv7m_start_algorithm,
	.wait_algorithm = armv7m_wait_algorithm,

	.add_breakpoint = cortex_m3_add_breakpoint,
	.remove_breakpoint = cortex_m3_remove_breakpoint,
	.add_watchpoint = cortex_m3_add_watchpoint,
	.remove_watchpoint = cortex_m3_remove_watchpoint,
};
