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

#include "target.h"
#include "bdm.h"
#include "log.h"


static struct bdm *target_to_bdm(struct target *target)
{
	return target->tap->priv;
}

int bdm_background(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->background)
		return bdm->background(bdm);
	else
		return ERROR_FAIL;
}

int bdm_assert_reset(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->assert_reset)
		return bdm->assert_reset(bdm);
	else
		return ERROR_FAIL;
}

int bdm_assert_bknd(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->assert_bknd)
		return bdm->assert_bknd(bdm);
	else
		return ERROR_FAIL;
}

int bdm_deassert_reset(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->deassert_reset)
		return bdm->deassert_reset(bdm);
	else
		return ERROR_FAIL;
}

int bdm_deassert_bknd(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->deassert_bknd)
		return bdm->deassert_bknd(bdm);
	else
		return ERROR_FAIL;
}

int bdm_ack_enable(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->ack_enable)
		return bdm->ack_enable(bdm);
	else
		return ERROR_FAIL;
}

int_least16_t bdm_read_bd_byte(struct target *target, enum bdm_registers reg)
{
	struct bdm *bdm = target_to_bdm(target);

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
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->read_byte)
		return bdm->read_byte(bdm, address);
	else
		return ERROR_FAIL;

}

int_least32_t bdm_read_word(struct target *target, uint32_t address)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->write_bd_byte)
		return bdm->read_word(bdm, address);
	else
		return ERROR_FAIL;
}

int bdm_write_bd_byte(struct target *target, enum bdm_registers reg, uint8_t value)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->write_bd_byte)
		return bdm->write_bd_byte(bdm, reg, value);
	else
		return ERROR_FAIL;
}

int bdm_write_bd_word(struct target *target, uint32_t address, uint16_t value)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->write_bd_byte)
		return bdm->write_bd_word(bdm, address, value);
	else
		return ERROR_FAIL;
}

int bdm_write_byte(struct target *target, uint32_t address, uint8_t value)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->write_byte)
		return bdm->write_byte(bdm, address, value);
	else
		return ERROR_FAIL;
}

int bdm_write_word(struct target *target, uint32_t address, uint16_t value)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->write_word)
		return bdm->write_word(bdm, address, value);
	else
		return ERROR_FAIL;
}

int bdm_go(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->go)
		return bdm->go(bdm);
	else
		return ERROR_FAIL;
}

int bdm_trace1(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->trace1)
		return bdm->trace1(bdm);
	else
		return ERROR_FAIL;
}

int bdm_read_memory(struct target *target, uint32_t address,
		    uint32_t size, uint32_t count, uint8_t *buffer)
{
	struct bdm *bdm = target_to_bdm(target);

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
	struct bdm *bdm = target_to_bdm(target);

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
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->read_pc)
		return bdm->read_pc(bdm);
	else
		return ERROR_FAIL;
}

int32_t bdm_read_sp(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->read_sp)
		return bdm->read_sp(bdm);
	else
		return ERROR_FAIL;
}

int32_t bdm_read_d(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->read_d)
		return bdm->read_d(bdm);
	else
		return ERROR_FAIL;
}

int32_t bdm_read_x(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->read_x)
		return bdm->read_x(bdm);
	else
		return ERROR_FAIL;
}

int32_t bdm_read_y(struct target *target)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->read_y)
		return bdm->read_y(bdm);
	else
		return ERROR_FAIL;
}

int bdm_write_pc(struct target *target, uint16_t value)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->write_pc)
		return bdm->write_pc(bdm, value);
	else
		return ERROR_FAIL;
}

int bdm_write_sp(struct target *target, uint16_t value)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->write_sp)
		return bdm->write_sp(bdm, value);
	else
		return ERROR_FAIL;
}

int bdm_write_d(struct target *target, uint16_t value)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->write_d)
		return bdm->write_d(bdm, value);
	else
		return ERROR_FAIL;
}

int bdm_write_x(struct target *target, uint16_t value)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->write_x)
		return bdm->write_x(bdm, value);
	else
		return ERROR_FAIL;
}

int bdm_write_y(struct target *target, uint16_t value)
{
	struct bdm *bdm = target_to_bdm(target);

	if (bdm->write_y)
		return bdm->write_y(bdm, value);
	else
		return ERROR_FAIL;
}
