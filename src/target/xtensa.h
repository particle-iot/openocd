/***************************************************************************
 *   Copyright (C) 2015 by Angus Gratton                                   *
 *   gus@projectgus.com                                                    *
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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
***************************************************************************/

#ifndef OPENOCD_TARGET_XTENSA_H
#define OPENOCD_TARGET_XTENSA_H

#include <jtag/jtag.h>
#include "breakpoints.h"

enum xtensa_state {
	XT_NORMAL,
	XT_OCD_RUN,
	XT_OCD_HALT,
};

struct xtensa_common {
	struct jtag_tap *tap;
	enum xtensa_state state;
	struct reg_cache *core_cache;

	uint32_t num_brps;  /* Number of breakpoints available */
	struct breakpoint **hw_brps;
};

struct xtensa_tap_instr {
	const uint8_t idx;
	const uint8_t inst;
	const uint8_t data_len;
	const char *name;
};

static inline struct xtensa_common *target_to_xtensa(struct target *target)
{
	return (struct xtensa_common *)target->arch_info;
}

enum xt_reg {
	XT_REG_GENERAL = 0,
	XT_REG_ALIASED = 1,
	XT_REG_SPECIAL = 2,
};

struct xtensa_core_reg {
	uint32_t idx; /* gdb server access index */
	const char *name;
	uint8_t reg_num; /* ISA register num (meaning depends on register type) */
	enum xt_reg type;
	struct target *target;
};

#endif /* OPENOCD_TARGET_XTENSA_H */
