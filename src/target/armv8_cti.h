/***************************************************************************
 *   Copyright (C) 2015 by Alamy Liu                                       *
 *   alamy.liu@gmail.com                                                   *
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

#ifndef	_ARMV8_CTI_
#define	_ARMV8_CTI_

#include "target.h"
#include "target_type64.h"


/* CTI Channel events
 * CTI_APPSET, CTI_APPCLEAR, CTI_APPPULSE
 * CTI_GATE, CTI_CHINSTATUS, CTICHOUTSTATUS, CTI_OUTEN
 */
#define ARMV8_CTI_CHANNEL_DEBUG			(0b1 << 0)
#define ARMV8_CTI_CHANNEL_RESTART		(0b1 << 1)
#define ARMV8_CTI_CHANNEL_CROSS_HALT	(0b1 << 2)


/* Fields of CTI_CONTROL (H9.3.14 CTICONTROL) */
#define ARMV8_CTI_CONTROL_GLBEN		(0b1 << 0)	/* Enable/Disable CTI */



int armv8_cti_reset(void);
int armv8_cti_init(struct target *target);

int armv8_cti_generate_events(struct target *target, int channel_events);
int armv8_cti_clear_trigger_events(struct target *target, int out_trigger_events);

int armv8_cti_halt_single(struct target *target);
int armv8_cti_enable_cross_halt(struct target *target);
int armv8_cti_restart_smp(struct target *target);

/* Just for the words */
#define armv8_cti_enable_halt_smp	armv8_cti_enable_cross_halt

#endif	// _ARMV8_CTI_
