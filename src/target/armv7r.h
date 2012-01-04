/***************************************************************************
 *    Copyright (C) 2009 by David Brownell                                 *
 *                                                                         *
 *    Copyright (C) 2011 by Google Inc.                                    *
 *    aschultz@google.com - Copy from armv7a                               *
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
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifndef ARMV7R_H
#define ARMV7R_H

#include "arm_adi_v5.h"
#include "arm.h"
#include "armv7a.h"
#include "arm_dpm.h"

/* We are very similar to the ARMv7A, the main difference is they have an
 * MMU and we have a PMU.  We'll leverage as much ARMV7A code as possible
 */

/* TODO: May need to implement support for MPU
 *  See B4 in ARM DDI 0406B for more information.
 */

struct armv7r_common {
	struct arm armv4_5_common;
	int common_magic;
	struct reg_cache *core_cache;

	struct adiv5_dap dap;

	/* Core Debug Unit */
	struct arm_dpm dpm;
	uint32_t debug_base;
	uint8_t debug_ap;
	uint8_t memory_ap;
	/* mdir */
	uint8_t multi_processor_system;
	uint8_t cluster_id;
	uint8_t cpu_id;

	/* TODO: May need to implement PMU cache */
	struct armv7a_cache_common armv7r_cache;

	int (*examine_debug_reason)(struct target *target);
	int (*post_debug_entry)(struct target *target);

	void (*pre_restore_context)(struct target *target);
};

static inline struct armv7r_common *
target_to_armv7r(struct target *target)
{
	return container_of(target->arch_info, struct armv7r_common,
			armv4_5_common);
}

int armv7r_arch_state(struct target *target);
int armv7r_identify_cache(struct target *target);
int armv7r_init_arch_info(struct target *target, struct armv7r_common *armv7r);

int armv7r_handle_cache_info_command(struct command_context *cmd_ctx,
		struct armv7a_cache_common *armv7r_cache);

extern const struct command_registration armv7r_command_handlers[];

#endif /* ARMV7R_H */
