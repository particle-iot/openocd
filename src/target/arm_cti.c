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
#include "target/arm_cti.h"
#include "target/target.h"
#include "helper/time_support.h"
#include "helper/list.h"

struct arm_cti {
	uint32_t base;
	struct adiv5_ap *ap;
};

struct arm_cti_object {
	struct list_head lh;
	struct arm_cti cti;
	int ap_num;
	struct jtag_tap *tap;
	char *name;
};

static LIST_HEAD(all_cti);

void arm_cti_dump_info(struct arm_cti *self)
{
	LOG_DEBUG("cti.self %p", self);
	LOG_DEBUG("cti.base 0x%08" PRIx32, self->base);
}

const char *arm_cti_name(struct arm_cti *self)
{
	struct arm_cti_object *obj = container_of(self, struct arm_cti_object, cti);
	return obj->name;
}

struct arm_cti *cti_instance_by_jim_obj(Jim_Interp *interp, Jim_Obj *o)
{
	struct arm_cti_object *obj = NULL;
	const char *name;
	bool found = false;

	name = Jim_GetString(o, NULL);

	list_for_each_entry(obj, &all_cti, lh) {
		if (!strcmp(name, obj->name)) {
			found = true;
			break;
		}
	}

	if (found)
		return &obj->cti;
	return NULL;
}

static int arm_cti_mod_reg_bits(struct arm_cti *self, unsigned int reg, uint32_t mask, uint32_t value)
{
	uint32_t tmp;

	/* Read register */
	int retval = mem_ap_read_atomic_u32(self->ap, self->base + reg, &tmp);
	if (ERROR_OK != retval)
		return retval;

	/* clear bitfield */
	tmp &= ~mask;
	/* put new value */
	tmp |= value & mask;

	/* write new value */
	return mem_ap_write_atomic_u32(self->ap, self->base + reg, tmp);
}

int arm_cti_enable(struct arm_cti *self, bool enable)
{
	uint32_t val = enable ? 1 : 0;

	return mem_ap_write_atomic_u32(self->ap, self->base + CTI_CTR, val);
}

int arm_cti_ack_events(struct arm_cti *self, uint32_t event)
{
	int retval;
	uint32_t tmp;

	retval = mem_ap_write_atomic_u32(self->ap, self->base + CTI_INACK, event);
	if (retval == ERROR_OK) {
		int64_t then = timeval_ms();
		for (;;) {
			retval = mem_ap_read_atomic_u32(self->ap, self->base + CTI_TROUT_STATUS, &tmp);
			if (retval != ERROR_OK)
				break;
			if ((tmp & event) == 0)
				break;
			if (timeval_ms() > then + 1000) {
				LOG_ERROR("timeout waiting for target");
				retval = ERROR_TARGET_TIMEOUT;
				break;
			}
		}
	}

	return retval;
}

int arm_cti_gate_channel(struct arm_cti *self, uint32_t channel)
{
	if (channel > 31)
		return ERROR_COMMAND_ARGUMENT_INVALID;

	return arm_cti_mod_reg_bits(self, CTI_GATE, CTI_CHNL(channel), 0);
}

int arm_cti_ungate_channel(struct arm_cti *self, uint32_t channel)
{
	if (channel > 31)
		return ERROR_COMMAND_ARGUMENT_INVALID;

	return arm_cti_mod_reg_bits(self, CTI_GATE, CTI_CHNL(channel), 0xFFFFFFFF);
}

int arm_cti_write_reg(struct arm_cti *self, unsigned int reg, uint32_t value)
{
	return mem_ap_write_atomic_u32(self->ap, self->base + reg, value);
}

int arm_cti_read_reg(struct arm_cti *self, unsigned int reg, uint32_t *p_value)
{
	if (p_value == NULL)
		return ERROR_COMMAND_ARGUMENT_INVALID;

	return mem_ap_read_atomic_u32(self->ap, self->base + reg, p_value);
}

int arm_cti_pulse_channel(struct arm_cti *self, uint32_t channel)
{
	if (channel > 31)
		return ERROR_COMMAND_ARGUMENT_INVALID;

	return arm_cti_write_reg(self, CTI_APPPULSE, CTI_CHNL(channel));
}

int arm_cti_set_channel(struct arm_cti *self, uint32_t channel)
{
	if (channel > 31)
		return ERROR_COMMAND_ARGUMENT_INVALID;

	return arm_cti_write_reg(self, CTI_APPSET, CTI_CHNL(channel));
}

int arm_cti_clear_channel(struct arm_cti *self, uint32_t channel)
{
	if (channel > 31)
		return ERROR_COMMAND_ARGUMENT_INVALID;

	return arm_cti_write_reg(self, CTI_APPCLEAR, CTI_CHNL(channel));
}

static uint32_t cti_regs[22];

static const struct {
	uint32_t offset;
	const char *label;
	uint32_t *p_val;
} cti_names[] = {
	{ CTI_CTR,  "CTI_CTR", &cti_regs[0] },
	{ CTI_GATE, "CTI_GATE", &cti_regs[1] },
	{ CTI_INEN0, "CTI_INEN0", &cti_regs[2] },
	{ CTI_INEN1, "CTI_INEN1", &cti_regs[3] },
	{ CTI_INEN2, "CTI_INEN2", &cti_regs[4] },
	{ CTI_INEN3, "CTI_INEN3", &cti_regs[5] },
	{ CTI_INEN4, "CTI_INEN4", &cti_regs[6] },
	{ CTI_INEN5, "CTI_INEN5", &cti_regs[7] },
	{ CTI_INEN6, "CTI_INEN6", &cti_regs[8] },
	{ CTI_INEN7, "CTI_INEN7", &cti_regs[9] },
	{ CTI_OUTEN0, "CTI_OUTEN0", &cti_regs[10] },
	{ CTI_OUTEN1, "CTI_OUTEN1", &cti_regs[11] },
	{ CTI_OUTEN2, "CTI_OUTEN2", &cti_regs[12] },
	{ CTI_OUTEN3, "CTI_OUTEN3", &cti_regs[13] },
	{ CTI_OUTEN4, "CTI_OUTEN4", &cti_regs[14] },
	{ CTI_OUTEN5, "CTI_OUTEN5", &cti_regs[15] },
	{ CTI_OUTEN6, "CTI_OUTEN6", &cti_regs[16] },
	{ CTI_OUTEN7, "CTI_OUTEN7", &cti_regs[17] },
	{ CTI_TRIN_STATUS, "CTI_TRIN_STATUS", &cti_regs[18] },
	{ CTI_TROUT_STATUS, "CTI_TROUT_STATUS", &cti_regs[19] },
	{ CTI_CHIN_STATUS, "CTI_CHIN_STATUS", &cti_regs[20] },
	{ CTI_CHOU_STATUS, "CTI_CHOUT_STATUS", &cti_regs[21] },
};

static int jim_cti_dump(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
	struct arm_cti_object *obj = Jim_CmdPrivData(interp);
	struct command_context *cmd_ctx = current_command_context(interp);
	struct arm_cti *cti = &obj->cti;

	for (int i = 0; i < (int)ARRAY_SIZE(cti_names); i++)
		mem_ap_read_u32(cti->ap, cti->base + cti_names[i].offset, cti_names[i].p_val);

	dap_run(cti->ap->dap);

	for (int i = 0; i < (int)ARRAY_SIZE(cti_names); i++)
		command_print(cmd_ctx, "%16.16s 0x%08" PRIx32, cti_names[i].label, *cti_names[i].p_val);

	return JIM_OK;
}

static const struct command_registration cti_instance_command_handlers[] = {
	{
		.name  = "dump",
		.mode  = COMMAND_EXEC,
		.jim_handler = jim_cti_dump,
		.help  = "dump CTI registers",
		.usage = "",
	},
	COMMAND_REGISTRATION_DONE
};

enum cti_cfg_param {
	CFG_CHAIN_POSITION,
	CFG_AP_NUM,
	CFG_CTIBASE
};

static const Jim_Nvp nvp_config_opts[] = {
	{ .name = "-chain-position",   .value = CFG_CHAIN_POSITION },
	{ .name = "-ctibase",          .value = CFG_CTIBASE },
	{ .name = "-ap-num",           .value = CFG_AP_NUM },
	{ .name = NULL, .value = -1 }
};

static int cti_configure(Jim_GetOptInfo *goi, struct arm_cti_object *cti)
{
	struct jtag_tap *tap = NULL;
	Jim_Nvp *n;
	jim_wide w;
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
		case CFG_CHAIN_POSITION:
			if (goi->isconfigure) {
				Jim_Obj *o_t;
				e = Jim_GetOpt_Obj(goi, &o_t);
				if (e != JIM_OK)
					return e;
				tap = jtag_tap_by_jim_obj(goi->interp, o_t);
				if (tap == NULL)
					return JIM_ERR;
				/* make this exactly 1 or 0 */
				cti->tap = tap;
			} else {
				if (goi->argc != 0)
					goto no_params;
			}
			Jim_SetResultString(goi->interp, cti->tap->dotted_name, -1);
			/* loop for more e*/
			break;
		case CFG_CTIBASE:
			if (goi->isconfigure) {
				e = Jim_GetOpt_Wide(goi, &w);
				if (e != JIM_OK)
					return e;
				cti->cti.base = (uint32_t)w;
			} else {
				if (goi->argc != 0)
					goto no_params;
			}
			Jim_SetResult(goi->interp, Jim_NewIntObj(goi->interp, cti->cti.base));
			/* loop for more */
			break;
		case CFG_AP_NUM:
			if (goi->isconfigure) {
				e = Jim_GetOpt_Wide(goi, &w);
				if (e != JIM_OK)
					return e;
				cti->ap_num = (uint32_t)w;
			} else {
				if (goi->argc != 0)
					goto no_params;
			}
		}
	}

	if (tap == NULL)
		goto no_params;

	if (tap->dap == NULL) {
		tap->dap = dap_init();
		tap->dap->tap = tap;
	}

	cti->cti.ap = dap_ap(tap->dap, cti->ap_num);

	return JIM_OK;

no_params:
	Jim_WrongNumArgs(goi->interp,
			goi->argc, goi->argv,
			"NO PARAMS");
	return JIM_ERR;
}

static int cti_create(Jim_GetOptInfo *goi)
{
	struct command_context *cmd_ctx;
	static struct arm_cti_object *cti;
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
	cti = calloc(1, sizeof(struct arm_cti_object));

	/* Do the rest as "configure" options */
	goi->isconfigure = 1;
	e = cti_configure(goi, cti);

	if (cti->tap == NULL) {
		Jim_SetResultString(goi->interp, "-chain-position required when creating CTI", -1);
		e = JIM_ERR;
	}

	if (e != JIM_OK) {
		free(cti);
		return e;
	}

	cp = Jim_GetString(new_cmd, NULL);
	cti->name = strdup(cp);

	/* now - create the new cti name command */
	const struct command_registration cti_subcommands[] = {
		{
			.chain = cti_instance_command_handlers,
		},
		COMMAND_REGISTRATION_DONE
	};
	const struct command_registration cti_commands[] = {
		{
			.name = cp,
			.mode = COMMAND_ANY,
			.help = "cti instance command group",
			.usage = "",
			.chain = cti_subcommands,
		},
		COMMAND_REGISTRATION_DONE
	};
	e = register_commands(cmd_ctx, NULL, cti_commands);
	if (ERROR_OK != e)
		return JIM_ERR;

	struct command *c = command_find_in_context(cmd_ctx, cp);
	assert(c);
	command_set_handler_data(c, cti);

	list_add_tail(&cti->lh, &all_cti);

	return (ERROR_OK == e) ? JIM_OK : JIM_ERR;
}

static int jim_cti_create(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
	Jim_GetOptInfo goi;
	Jim_GetOpt_Setup(&goi, interp, argc - 1, argv + 1);
	if (goi.argc < 2) {
		Jim_WrongNumArgs(goi.interp, goi.argc, goi.argv,
			"<name> [<cti_options> ...]");
		return JIM_ERR;
	}
	return cti_create(&goi);
}

static int jim_cti_names(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
	struct arm_cti_object *obj;

	if (argc != 1) {
		Jim_WrongNumArgs(interp, 1, argv, "Too many parameters");
		return JIM_ERR;
	}
	Jim_SetResult(interp, Jim_NewListObj(interp, NULL, 0));
	list_for_each_entry(obj, &all_cti, lh) {
		Jim_ListAppendElement(interp, Jim_GetResult(interp),
			Jim_NewStringObj(interp, obj->name, -1));
	}
	return JIM_OK;
}


static const struct command_registration cti_subcommand_handlers[] = {
	{
		.name = "create",
		.mode = COMMAND_CONFIG,
		.jim_handler = jim_cti_create,
		.usage = "name '-chain-position' name [options ...]",
		.help = "Creates a new CTI object",
	},
	{
		.name = "names",
		.mode = COMMAND_ANY,
		.jim_handler = jim_cti_names,
		.usage = "",
		.help = "Lists all registered CTI objects by name",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration cti_command_handlers[] = {
	{
		.name = "cti",
		.mode = COMMAND_CONFIG,
		.help = "CTI commands",
		.chain = cti_subcommand_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

int cti_register_commands(struct command_context *cmd_ctx)
{
	return register_commands(cmd_ctx, NULL, cti_command_handlers);
}

