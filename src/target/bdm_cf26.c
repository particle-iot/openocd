/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2017 by Angelo Dureghello                               *
 *   angelo@sysam.it                                                       *
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
 *   along with this program; see the file COPYING.  If not see            *
 *   <http://www.gnu.org/licenses/>                                        *
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <transport/transport.h>
#include <jtag/jtag.h>
#include <jtag/interface.h>

#include "target.h"
#include "bdm_cf26.h"
#include "log.h"


/*
 * bdm 26 pin versions are A B B+ C D D+ D+PSTB
 */

static const struct bdm_cf26_driver *target_to_bdm_cf26(struct target *target)
{
	return target->tap->priv;
}

int_least32_t bdm_cf26_read_dm_reg(struct target *target,
					enum bdm_cf26_registers reg)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->read_dm_reg)
		return bdm->read_dm_reg(bdm, reg);
	else
		return ERROR_FAIL;
}

int_least32_t bdm_cf26_read_ad_reg(struct target *target,
					enum bdm_cf26_registers reg)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->read_ad_reg)
		return bdm->read_ad_reg(bdm, reg);
	else
		return ERROR_FAIL;
}

int_least32_t bdm_cf26_read_sc_reg(struct target *target,
					enum cf_sysctl_registers reg)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->read_sc_reg)
		return bdm->read_sc_reg(bdm, reg);
	else
		return ERROR_FAIL;
}

int_least8_t bdm_cf26_read_mem_byte(struct target *target, uint32_t address)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->read_mem_byte)
		return bdm->read_mem_byte(bdm, address);

	else
		return ERROR_FAIL;
}

int_least16_t bdm_cf26_read_mem_word(struct target *target, uint32_t address)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->read_mem_word)
		return bdm->read_mem_word(bdm, address);
	else
		return ERROR_FAIL;
}

int_least32_t bdm_cf26_read_mem_long(struct target *target, uint32_t address)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->read_mem_long)
		return bdm->read_mem_long(bdm, address);
	else
		return ERROR_FAIL;
}

int bdm_cf26_write_dm_reg(struct target *target,
				enum bdm_cf26_registers reg, uint32_t value)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->write_dm_reg)
		return bdm->write_dm_reg(bdm, reg, value);
	else
		return ERROR_FAIL;
}

int bdm_cf26_write_ad_reg(struct target *target,
				enum bdm_cf26_registers reg, uint32_t value)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->write_ad_reg)
		return bdm->write_ad_reg(bdm, reg, value);
	else
		return ERROR_FAIL;
}

int bdm_cf26_write_sc_reg(struct target *target,
				enum cf_sysctl_registers reg, uint32_t value)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->write_sc_reg)
		return bdm->write_sc_reg(bdm, reg, value);
	else
		return ERROR_FAIL;
}

int bdm_cf26_write_mem_byte(struct target *target, uint32_t address,
				uint8_t value)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->write_mem_byte)
		return bdm->write_mem_byte(bdm, address, value);

	else
		return ERROR_FAIL;
}

int bdm_cf26_write_mem_word(struct target *target, uint32_t address,
				uint16_t value)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->write_mem_word)
		return bdm->write_mem_word(bdm, address, value);

	else
		return ERROR_FAIL;
}

int bdm_cf26_write_mem_long(struct target *target, uint32_t address,
				uint32_t value)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->write_mem_long)
		return bdm->write_mem_long(bdm, address, value);

	else
		return ERROR_FAIL;
}

int bdm_cf26_assert_reset(struct target *target)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->assert_reset)
		return bdm->assert_reset(bdm);
	else
		return ERROR_FAIL;
}

int bdm_cf26_deassert_reset(struct target *target)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->deassert_reset)
		return bdm->deassert_reset(bdm);
	else
		return ERROR_FAIL;
}

int bdm_cf26_halt(struct target *target)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->halt)
		return bdm->halt(bdm);
	else
		return ERROR_FAIL;
}

int bdm_cf26_go(struct target *target)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->go)
		return bdm->go(bdm);
	else
		return ERROR_FAIL;
}

int bdm_cf26_get_all_cpu_regs(struct target *target, uint8_t **reg_buff)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->get_all_cpu_regs)
		return bdm->get_all_cpu_regs(bdm, reg_buff);
	else
		return ERROR_FAIL;
}

int bdm_cf26_read_memory(struct target *target, uint32_t address,
		    uint32_t size, uint32_t count, uint8_t *buffer)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->read_memory)
		return bdm->read_memory(bdm, address, size, count, buffer);
	else
		return ERROR_FAIL;
}

int bdm_cf26_write_memory(struct target *target, uint32_t address,
		     uint32_t size, uint32_t count, const uint8_t *buffer)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->write_memory)
		return bdm->write_memory(bdm, address, size, count, buffer);
	else
		return ERROR_FAIL;
}

int32_t bdm_cf26_read_pc(struct target *target)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->read_pc)
		return bdm->read_pc(bdm);
	else
		return ERROR_FAIL;
}

int bdm_cf26_write_pc(struct target *target, uint32_t value)
{
	const struct bdm_cf26_driver *bdm = target_to_bdm_cf26(target);

	if (bdm->write_pc)
		return bdm->write_pc(bdm, value);
	else
		return ERROR_FAIL;
}

static const struct command_registration bdm_cf26_commands[] = {
	{
		.name = "newdap",
		.jim_handler = jim_jtag_newtap,
		.mode = COMMAND_CONFIG,
		.help = "declare a new BDM DAP"
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration bdm_cf26_handlers[] = {
	{
		.name = "bdm_cf26",
		.mode = COMMAND_ANY,
		.help = "bdm_cf26 command group",
		.chain = bdm_cf26_commands,
	},
	COMMAND_REGISTRATION_DONE
};

extern struct jtag_interface *jtag_interface;

static int bdm_cf26_transport_init(struct command_context *cmd_ctx)
{
	const struct bdm_cf26_driver *bdm = jtag_interface->bdm_cf26;

	struct target *target = get_current_target(cmd_ctx);
	if (!target) {
		LOG_ERROR("no current target");
		return ERROR_FAIL;
	}

	target->tap->priv = (void *)bdm;
	target->tap->hasidcode = 0;

	return ERROR_OK;
}

static int bdm_cf26_transport_select(struct command_context *cmd_ctx)
{
	return register_commands(cmd_ctx, NULL, bdm_cf26_handlers);
}

static struct transport bdm_cf26_transport = {
	.name	= "bdm_cf26",
	.select = bdm_cf26_transport_select,
	.init   = bdm_cf26_transport_init,
};

static void bdm_cf26_transport_constructor(void) __attribute__ ((constructor, used));
static void bdm_cf26_transport_constructor(void)
{
	transport_register(&bdm_cf26_transport);
}

/*
 * Returns true if the current debug session
 * is using BDM 26 pins as its transport.
 */
bool transport_is_bdm_cf26(void)
{
	return get_current_transport() == &bdm_cf26_transport;
}
