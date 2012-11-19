/*
 * Copyright (c) 2010 by David Brownell
 *
 * Copyright (C) 2011-2012 Tomasz Boleslaw CEDRO
 * cederom@tlen.pl, http://www.tomek.cedro.info
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** @file
 * Infrastructure for specifying and managing the transport protocol
 * used in a given debug or programming session.
 *
 * Examples of "debug-capable" transports are JTAG or SWD.
 * Additionally, JTAG supports boundary scan testing.
 *
 * Examples of "programming-capable" transports include SPI or UART;
 * those are used (often mediated by a ROM bootloader) for ISP style
 * programming, to perform an initial load of code into flash, or
 * sometimes into SRAM.  Target code could use "variant" options to
 * decide how to use such protocols.  For example, Cortex-M3 cores
 * from TI/Luminary and from NXP use different protocols for for
 * UART or SPI based firmware loading.
 *
 * As a rule, there are protocols layered on top of the transport.
 * For example, different chip families use JTAG in different ways
 * for debugging.  Also, each family that supports programming over
 * a UART link for initial firmware loading tends to define its own
 * messaging and error handling.
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
#include <jtag/jtag.h>

extern struct command_context *global_cmd_ctx;

/*-----------------------------------------------------------------------*/

extern struct jtag_interface *jtag_interface;

/*
 * Infrastructure internals
 */

/** List of transports known to OpenOCD. */
oocd_transport_t *oocd_transport_list_all;

/**
 * NULL-terminated Vector of names of transports which the
 * currently selected debug adapter supports.  This is declared
 * by the time that adapter is fully set up.
 */
static const char **oocd_transport_list_allowed;

/** * The transport being used for the current OpenOCD session.  */
oocd_transport_t *session;

/**
 * Register all supported transport types with this one function at startup.
 * This needs to be done like this in a function, not using contructors,
 * because linking killed contructor function attributes.
 * Registering them here gives more predictable behavior.
 */
int oocd_transport_register_all(void){
	if (oocd_transport_register(&jtag_transport)!=ERROR_OK)
		return ERROR_FAIL;
	if (oocd_transport_register(&swd_transport)!=ERROR_OK)            
		return ERROR_FAIL;
	if (oocd_transport_register(&oocd_transport_swd)!=ERROR_OK)
		return ERROR_FAIL;
	return ERROR_OK;
}

/**
 * Set and setup transport for current session.
 * This will find transport by given name, verify its configuration,
 * then call driver specific select() routine.
 * Note that select() sets up the internals, init() talks to hardware. 
 */
int oocd_transport_select(struct command_context *ctx, const char *name)
{
	for (oocd_transport_t *t = oocd_transport_list_all; t; t = t->next) {
		if (strcmp(t->name, name) == 0) {
			if (t->setup(ctx) == ERROR_FAIL) return ERROR_FAIL;
			session = t;
			return ERROR_OK;
		}
	}

	LOG_ERROR("Transport - '%s' is not available!", name);
	return ERROR_FAIL;
}

/**
 * Called by debug adapter drivers, or affiliated Tcl config scripts,
 * to declare the set of transports supported by an adapter.  When
 * there is only one member of that set, it is automatically selected.
 */
int oocd_transport_allow(struct command_context *ctx, const char **vector)
{
	/* NOTE:  caller is required to provide only a list
	 * of *valid* transport names
	 *
	 * REVISIT should we validate that?  and insist there's
	 * at least one non-NULL element in that list?
	 *
	 * ... allow removals, e.g. external strapping prevents use
	 * of one transport; C code should be definitive about what
	 * can be used when all goes well.
	 */
	if (oocd_transport_list_allowed != NULL || session) {
		LOG_ERROR("Transport - can't modify the set of allowed transports.");
		return ERROR_FAIL;
	}

	oocd_transport_list_allowed = vector;

	/* autoselect if there's no choice ... */
	if (!vector[1]) {
		LOG_INFO("Transport - autoselecting '%s' as the only option for '%s' interface.", vector[0], jtag_interface->name);
		return oocd_transport_select(ctx, vector[0]);
	}

	return ERROR_OK;
}

/**
 * Used to verify corrrect adapter driver initialization.
 *
 * @returns true if the adapter declared one or more transports.
 */
bool oocd_transport_declared(void)
{
	return oocd_transport_list_allowed != NULL;
}

/**
 * Registers a transport.  There are general purpose transports
 * (such as JTAG), as well as relatively proprietary ones which are
 * specific to a given chip (or chip family).
 *
 * Code implementing a transport needs to register it before it can
 * be selected and then activated.  This is a dynamic process, so
 * that chips (and families) can define transports as needed (without
 * nneeding error-prone static tables).
 *
 * @param new_transport the transport being registered.  On a
 * successful return, this memory is owned by the transport framework.
 *
 * @returns ERROR_OK on success, else a fault code.
 */
int oocd_transport_register(oocd_transport_t *new_transport)
{
	oocd_transport_t *t;

	for (t = oocd_transport_list_all; t; t = t->next) {
		if (strcmp(t->name, new_transport->name) == 0) {
			LOG_ERROR("Transport - '%s' name already used!", new_transport->name);
			return ERROR_FAIL;
		}
	}

	if (!new_transport->setup)
		LOG_ERROR("Transport - '%s' has not setup procedure!", new_transport->name);

	/* splice this into the list */
	new_transport->next = oocd_transport_list_all;
	oocd_transport_list_all = new_transport;
	LOG_DEBUG("Transport - registered '%s'.", new_transport->name);

	return ERROR_OK;
}

/**
 * Returns the transport currently being used by this debug or
 * programming session.
 *
 * @returns handle to the read-only transport entity.
 */
oocd_transport_t *oocd_transport_current_get(void)
{
	/* REVISIT -- constify */
	return session;
}

/*-----------------------------------------------------------------------*/

/*
 * Infrastructure for Tcl interface to transports.
 */

/**
 * Makes and stores a copy of a set of transports passed as
 * parameters to a command.
 *
 * @param vector where the resulting copy is stored, as an argv-style
 *	NULL-terminated vector.
 */
COMMAND_HELPER(oocd_transport_list_parse, char ***vector)
{
	char **argv;
	unsigned n = CMD_ARGC;
	unsigned j = 0;

	*vector = NULL;

	if (n < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	/* our return vector must be NULL terminated */
	argv = (char **) calloc(n + 1, sizeof(char *));
	if (argv == NULL)
		return ERROR_FAIL;

	for (unsigned i = 0; i < n; i++) {
		oocd_transport_t *t;

		for (t = oocd_transport_list_all; t; t = t->next) {
			if (strcmp(t->name, CMD_ARGV[i]) != 0)
				continue;
			argv[j++] = strdup(CMD_ARGV[i]);
			break;
		}
		if (!t) {
			LOG_ERROR("Transport - not found '%s'!", CMD_ARGV[i]);
			goto fail;
		}
	}

	*vector = argv;
	return ERROR_OK;

fail:
	for (unsigned i = 0; i < n; i++)
		free(argv[i]);
	free(argv);
	return ERROR_FAIL;
}

COMMAND_HANDLER(handle_oocd_transport_init)
{
	LOG_DEBUG("%s", __func__);
	if (!session) {
		LOG_ERROR("Transport - session's transport is not selected!");

		/* no session transport configured, print transports then fail */
		const char **vector = oocd_transport_list_allowed;
		while (*vector) {
			LOG_ERROR("Transport - '%s' is allowed.", *vector);
			vector++;
		}
		return ERROR_FAIL;
	}

	oocd_feature_t *arm_dap_ops;
	arm_dap_ops=oocd_feature_find(jtag_interface->features, OOCD_FEATURE_ARM_DAP); 
	if (arm_dap_ops == NULL){
		LOG_ERROR("Transport - features '%s' not found on '%s' interface!", OOCD_FEATURE_ARM_DAP, jtag_interface->name);
		return ERROR_FAIL;
	}
	struct dap_ops *dap = (struct dap_ops*) arm_dap_ops->body;
	return dap->init(CMD_CTX);
}

COMMAND_HANDLER(handle_oocd_transport_list)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	command_print(CMD_CTX, "The following transports are available:");

	for (oocd_transport_t *t = oocd_transport_list_all; t; t = t->next)
		command_print(CMD_CTX, "\t%s", t->name);

	return ERROR_OK;
}

/**
 * Implements the Tcl "transport select" command, choosing the
 * transport to be used in this debug session from among the
 * set supported by the debug adapter being used.  Return value
 * is scriptable (allowing "if swd then..." etc).
 */
int oocd_transport_select_jim(Jim_Interp *interp, int argc, Jim_Obj * const *argv)
{
	switch (argc) {
		case 1:		/* return/display */
			if (!session) {
				LOG_ERROR("Transport - session's transport is not selected!");
				return JIM_ERR;
			} else {
				Jim_SetResultString(interp, session->name, -1);
				return JIM_OK;
			}
			break;
		case 2:		/* assign */
			if (session) {
				/* can't change session's transport after-the-fact */
				LOG_ERROR("Transport - session's transport is already selected!");
				return JIM_ERR;
			}

			/* Is this transport supported by our debug adapter?
			 * Example, "JTAG-only" means SWD is not supported.
			 *
			 * NOTE:  requires adapter to have been set up, with
			 * transports declared via C.
			 */
			if (!oocd_transport_list_allowed) {
				LOG_ERROR("Transport - interface does not support any transports!");
				return JIM_ERR;
			}

			for (unsigned i = 0; oocd_transport_list_allowed[i]; i++) {

				if (strcmp(oocd_transport_list_allowed[i], argv[1]->bytes) == 0)
					return oocd_transport_select(global_cmd_ctx, argv[1]->bytes);
			}

			LOG_ERROR("Transport - '%s' not supported by '%s' interface!", argv[1]->bytes, jtag_interface->name);
			return JIM_ERR;
			break;
		default:
			Jim_WrongNumArgs(interp, 1, argv, "[too many parameters]");
			return JIM_ERR;
	}
}

static const struct command_registration oocd_transport_commands[] = {
	{
		.name = "init",
		.handler = handle_oocd_transport_init,
		/* this would be COMMAND_CONFIG ... except that
		 * it needs to trigger event handlers that may
		 * require COMMAND_EXEC ...
		 */
		.mode = COMMAND_ANY,
		.help = "Initialize this session's transport",
		.usage = ""
	},
	{
		.name = "list",
		.handler = handle_oocd_transport_list,
		.mode = COMMAND_ANY,
		.help = "list all built-in transports",
		.usage = ""
	},
	{
		.name = "select",
		.jim_handler = oocd_transport_select_jim,
		.mode = COMMAND_ANY,
		.help = "Select this session's transport",
		.usage = "[transport_name]",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration oocd_transport_group[] = {
	{
		.name = "transport",
		.mode = COMMAND_ANY,
		.help = "Transport command group",
		.chain = oocd_transport_commands,
		.usage = ""
	},
	COMMAND_REGISTRATION_DONE
};

int oocd_transport_register_commands(struct command_context *ctx)
{
	return register_commands(ctx, NULL, oocd_transport_group);
}
