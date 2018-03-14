/***************************************************************************
 *   Copyright (C) 2016 by Matthias Welwarsky                              *
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
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include "target/arm_adi_v5.h"
#include "helper/list.h"
#include "helper/command.h"
#include "transport/transport.h"

static LIST_HEAD(all_dap);

static void dap_instance_init(struct adiv5_dap *dap)
{
	int i;
	/* Set up with safe defaults */
	for (i = 0; i <= 255; i++) {
		dap->ap[i].dap = dap;
		dap->ap[i].ap_num = i;
		/* memaccess_tck max is 255 */
		dap->ap[i].memaccess_tck = 255;
		/* Number of bits for tar autoincrement, impl. dep. at least 10 */
		dap->ap[i].tar_autoincr_block = (1<<10);
	}
	INIT_LIST_HEAD(&dap->cmd_journal);
}

const char *adiv5_dap_name(struct adiv5_dap *self)
{
	struct arm_dap_object *obj = container_of(self, struct arm_dap_object, dap);
	return obj->name;
}

struct adiv5_dap *dap_instance_by_jim_obj(Jim_Interp *interp, Jim_Obj *o)
{
	struct arm_dap_object *obj = NULL;
	const char *name;
	bool found = false;

	name = Jim_GetString(o, NULL);

	list_for_each_entry(obj, &all_dap, lh) {
		if (!strcmp(name, obj->name)) {
			found = true;
			break;
		}
	}

	if (found)
		return &obj->dap;
	return NULL;
}

static int dap_init_all(void)
{
	struct arm_dap_object *obj;
	int retval;

	LOG_DEBUG("Initializing all DAPs ...");

	list_for_each_entry(obj, &all_dap, lh) {
		struct adiv5_dap *dap = &obj->dap;
		/* skip taps that are disabled */
		if (!dap->tap->enabled)
			continue;

		if (transport_is_swd()) {
			dap->ops = &swd_dap_ops;
			retval = dap->ops->connect(dap);
			if (retval != ERROR_OK)
				return retval;
		} else
		if (!transport_is_hla()) {
			retval = dap_dp_init(dap);
			if (retval != ERROR_OK)
				return retval;
		}
	}

	return ERROR_OK;
}

int dap_cleanup_all(void)
{
	struct arm_dap_object *obj, *tmp;

	list_for_each_entry_safe(obj, tmp, &all_dap, lh) {
		free(obj->name);
		free(obj);
	}

	return ERROR_OK;
}

enum dap_cfg_param {
	CFG_CHAIN_POSITION,
};

static const Jim_Nvp nvp_config_opts[] = {
	{ .name = "-chain-position",   .value = CFG_CHAIN_POSITION },
	{ .name = NULL, .value = -1 }
};

static int dap_configure(Jim_GetOptInfo *goi, struct arm_dap_object *dap)
{
	struct jtag_tap *tap = NULL;
	Jim_Nvp *n;
	int e;

	/* parse config or cget options ... */
	while (goi->argc > 0) {
		Jim_SetEmptyResult(goi->interp);

		e = Jim_GetOpt_Nvp(goi, nvp_config_opts, &n);
		if (e != JIM_OK) {
			Jim_GetOpt_NvpUnknown(goi, nvp_config_opts, 0);
			return e;
		}
		switch (n->value) {
		case CFG_CHAIN_POSITION: {
			Jim_Obj *o_t;
			e = Jim_GetOpt_Obj(goi, &o_t);
			if (e != JIM_OK)
				return e;
			tap = jtag_tap_by_jim_obj(goi->interp, o_t);
			if (tap == NULL) {
				Jim_SetResultString(goi->interp, "-chain-position is invalid", -1);
				return JIM_ERR;
			}
			/* loop for more */
			break;
		}
		default:
			break;
		}
	}

	if (tap == NULL) {
		Jim_SetResultString(goi->interp, "-chain-position required when creating DAP", -1);
		return JIM_ERR;
	}

	dap_instance_init(&dap->dap);
	dap->dap.tap = tap;

	return JIM_OK;
}

static int dap_create(Jim_GetOptInfo *goi)
{
	struct command_context *cmd_ctx;
	static struct arm_dap_object *dap;
	Jim_Obj *new_cmd;
	Jim_Cmd *cmd;
	const char *cp;
	int e;

	cmd_ctx = current_command_context(goi->interp);
	assert(cmd_ctx != NULL);

	if (goi->argc < 3) {
		Jim_WrongNumArgs(goi->interp, 1, goi->argv, "?name? ..options...");
		return JIM_ERR;
	}
	/* COMMAND */
	Jim_GetOpt_Obj(goi, &new_cmd);
	/* does this command exist? */
	cmd = Jim_GetCommand(goi->interp, new_cmd, JIM_ERRMSG);
	if (cmd) {
		cp = Jim_GetString(new_cmd, NULL);
		Jim_SetResultFormatted(goi->interp, "Command: %s Exists", cp);
		return JIM_ERR;
	}

	/* Create it */
	dap = calloc(1, sizeof(struct arm_dap_object));
	if (dap == NULL)
		return JIM_ERR;

	e = dap_configure(goi, dap);
	if (e != JIM_OK) {
		free(dap);
		return e;
	}

	cp = Jim_GetString(new_cmd, NULL);
	dap->name = strdup(cp);

	/* now - create the new dap name command */
	const struct command_registration dap_subcommands[] = {
		{
			.chain = dap_instance_commands,
		},
		COMMAND_REGISTRATION_DONE
	};
	const struct command_registration dap_commands[] = {
		{
			.name = cp,
			.mode = COMMAND_ANY,
			.help = "dap instance command group",
			.usage = "",
			.chain = dap_subcommands,
		},
		COMMAND_REGISTRATION_DONE
	};
	e = register_commands(cmd_ctx, NULL, dap_commands);
	if (ERROR_OK != e)
		return JIM_ERR;

	struct command *c = command_find_in_context(cmd_ctx, cp);
	assert(c);
	command_set_handler_data(c, dap);

	list_add_tail(&dap->lh, &all_dap);

	return (ERROR_OK == e) ? JIM_OK : JIM_ERR;
}

static int jim_dap_create(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
	Jim_GetOptInfo goi;
	Jim_GetOpt_Setup(&goi, interp, argc - 1, argv + 1);
	if (goi.argc < 2) {
		Jim_WrongNumArgs(goi.interp, goi.argc, goi.argv,
			"<name> [<dap_options> ...]");
		return JIM_ERR;
	}
	return dap_create(&goi);
}

static int jim_dap_names(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
	struct arm_dap_object *obj;

	if (argc != 1) {
		Jim_WrongNumArgs(interp, 1, argv, "Too many parameters");
		return JIM_ERR;
	}
	Jim_SetResult(interp, Jim_NewListObj(interp, NULL, 0));
	list_for_each_entry(obj, &all_dap, lh) {
		Jim_ListAppendElement(interp, Jim_GetResult(interp),
			Jim_NewStringObj(interp, obj->name, -1));
	}
	return JIM_OK;
}

COMMAND_HANDLER(handle_dap_init)
{
	return dap_init_all();
}

static const struct command_registration dap_subcommand_handlers[] = {
	{
		.name = "create",
		.mode = COMMAND_ANY,
		.jim_handler = jim_dap_create,
		.usage = "name '-chain-position' name",
		.help = "Creates a new DAP object",
	},
	{
		.name = "names",
		.mode = COMMAND_ANY,
		.jim_handler = jim_dap_names,
		.usage = "",
		.help = "Lists all registered DAP objects by name",
	},
	{
		.name = "init",
		.mode = COMMAND_ANY,
		.handler = handle_dap_init,
		.usage = "",
		.help = "Initialize all registered DAPs"
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration dap_commands[] = {
	{
		.name = "dap",
		.mode = COMMAND_CONFIG,
		.help = "DAP commands",
		.chain = dap_subcommand_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

int dap_register_commands(struct command_context *cmd_ctx)
{
	return register_commands(cmd_ctx, NULL, dap_commands);
}
