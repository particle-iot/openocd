/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2014 by Andrey Smirnov                                  *
 *   andrew.smirnov@gmail.com                                              *
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
#include "bdm.h"
#include "log.h"


static const struct bdm_driver *target_to_bdm(struct target *target)
{
	return target->tap->priv;
}

int bdm_background(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->background)
		return bdm->background(bdm);
	else
		return ERROR_FAIL;
}

int bdm_assert_reset(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->assert_reset)
		return bdm->assert_reset(bdm);
	else
		return ERROR_FAIL;
}

int bdm_assert_bknd(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->assert_bknd)
		return bdm->assert_bknd(bdm);
	else
		return ERROR_FAIL;
}

int bdm_deassert_reset(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->deassert_reset)
		return bdm->deassert_reset(bdm);
	else
		return ERROR_FAIL;
}

int bdm_deassert_bknd(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->deassert_bknd)
		return bdm->deassert_bknd(bdm);
	else
		return ERROR_FAIL;
}

int bdm_ack_enable(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->ack_enable)
		return bdm->ack_enable(bdm);
	else
		return ERROR_FAIL;
}

int_least16_t bdm_read_bd_byte(struct target *target, enum bdm_registers reg)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->read_bd_byte)
		return bdm->read_bd_byte(bdm, reg);
	else
		return ERROR_FAIL;
}

int_least32_t bdm_read_bd_word(struct target *target, enum bdm_registers reg)
{
	return ERROR_FAIL;
}

int_least16_t bdm_read_byte(struct target *target, uint32_t address)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->read_byte)
		return bdm->read_byte(bdm, address);
	else
		return ERROR_FAIL;

}

int_least32_t bdm_read_word(struct target *target, uint32_t address)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_bd_byte)
		return bdm->read_word(bdm, address);
	else
		return ERROR_FAIL;
}

int bdm_write_bd_byte(struct target *target, enum bdm_registers reg, uint8_t value)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_bd_byte)
		return bdm->write_bd_byte(bdm, reg, value);
	else
		return ERROR_FAIL;
}

int bdm_write_bd_word(struct target *target, uint32_t address, uint16_t value)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_bd_byte)
		return bdm->write_bd_word(bdm, address, value);
	else
		return ERROR_FAIL;
}

int bdm_write_byte(struct target *target, uint32_t address, uint8_t value)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_byte)
		return bdm->write_byte(bdm, address, value);
	else
		return ERROR_FAIL;
}

int bdm_write_word(struct target *target, uint32_t address, uint16_t value)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_word)
		return bdm->write_word(bdm, address, value);
	else
		return ERROR_FAIL;
}

int bdm_go(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->go)
		return bdm->go(bdm);
	else
		return ERROR_FAIL;
}

int bdm_trace1(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->trace1)
		return bdm->trace1(bdm);
	else
		return ERROR_FAIL;
}

int bdm_read_memory(struct target *target, uint32_t address,
		    uint32_t size, uint32_t count, uint8_t *buffer)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->read_memory) {
		return bdm->read_memory(bdm, address, size, count, buffer);
	} else {
		/* FIXME: Implement a fallback to
		 * bdb_read_byte/bdm_read_word in case the adapter
		 * does not provide bulk write functionality */

		return ERROR_FAIL;
	}
}

int bdm_write_memory(struct target *target, uint32_t address,
		     uint32_t size, uint32_t count, const uint8_t *buffer)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_memory) {
		return bdm->write_memory(bdm, address, size, count, buffer);
	} else {
		/* FIXME: Implement a fallback to
		 * bdb_write_byte/bdm_write_word in case the adapter
		 * does not provide bulk write functionality */
		return ERROR_FAIL;
	}
}

int32_t bdm_read_pc(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->read_pc)
		return bdm->read_pc(bdm);
	else
		return ERROR_FAIL;
}

int32_t bdm_read_sp(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->read_sp)
		return bdm->read_sp(bdm);
	else
		return ERROR_FAIL;
}

int32_t bdm_read_d(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->read_d)
		return bdm->read_d(bdm);
	else
		return ERROR_FAIL;
}

int32_t bdm_read_x(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->read_x)
		return bdm->read_x(bdm);
	else
		return ERROR_FAIL;
}

int32_t bdm_read_y(struct target *target)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->read_y)
		return bdm->read_y(bdm);
	else
		return ERROR_FAIL;
}

int bdm_write_pc(struct target *target, uint16_t value)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_pc)
		return bdm->write_pc(bdm, value);
	else
		return ERROR_FAIL;
}

int bdm_write_sp(struct target *target, uint16_t value)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_sp)
		return bdm->write_sp(bdm, value);
	else
		return ERROR_FAIL;
}

int bdm_write_d(struct target *target, uint16_t value)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_d)
		return bdm->write_d(bdm, value);
	else
		return ERROR_FAIL;
}

int bdm_write_x(struct target *target, uint16_t value)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_x)
		return bdm->write_x(bdm, value);
	else
		return ERROR_FAIL;
}

int bdm_write_y(struct target *target, uint16_t value)
{
	const struct bdm_driver *bdm = target_to_bdm(target);

	if (bdm->write_y)
		return bdm->write_y(bdm, value);
	else
		return ERROR_FAIL;
}

static const struct command_registration bdm_commands[] = {
	{
		.name = "newdap",
		.jim_handler = jim_jtag_newtap,
		.mode = COMMAND_CONFIG,
		.help = "declare a new BDM DAP"
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration bdm_handlers[] = {
	{
		.name = "bdm",
		.mode = COMMAND_ANY,
		.help = "bdm command group",
		.chain = bdm_commands,
	},
	COMMAND_REGISTRATION_DONE
};

extern struct jtag_interface *jtag_interface;

static int bdm_transport_init(struct command_context *cmd_ctx)
{
	const struct bdm_driver *bdm = jtag_interface->driver.bdm;

	struct target *target = get_current_target(cmd_ctx);
	if (!target) {
		LOG_ERROR("no current target");
		return ERROR_FAIL;
	}

	target->tap->priv = (void *)bdm;
	target->tap->hasidcode = 0;

	return ERROR_OK;
}

static int bdm_transport_select(struct command_context *cmd_ctx)
{
	return register_commands(cmd_ctx, NULL, bdm_handlers);
}

static struct transport bdm_transport = {
	.name	= "bdm",
	.select = bdm_transport_select,
	.init   = bdm_transport_init,
};

static void bdm_transport_constructor(void) __attribute__ ((constructor, used));
static void bdm_transport_constructor(void)
{
	transport_register(&bdm_transport);
}

/** Returns true if the current debug session
 * is using BDM as its transport.
 */
bool transport_is_bdm(void)
{
	return get_current_transport() == &bdm_transport;
}
