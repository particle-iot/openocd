/***************************************************************************
 *   Copyright (C) 2013 by Brandon Warhurst                                *
 *   roboknight+openocd@gmail.com                                          *
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
 ***************************************************************************/

#ifndef JEM_T_H
#define JEM_T_H

#include <jtag/jtag.h>

#define MAX_HARD_BREAKS 2

struct mcu_jtag {
	struct jtag_tap *tap;
	uint8_t dr[48],
			tdr[48],
			ir[4];
};

struct jem_common {
	struct mcu_jtag jtag_info;
	int target_started;
	int num_hw_bkpts_avail;
};

#endif /* JEM_T_H */
