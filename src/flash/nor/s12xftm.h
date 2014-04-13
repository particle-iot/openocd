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

#ifndef __S12XFTM_H__
#define __S12XFTM_H__

enum s12xftm_helper_commands {
	S12XFTM_HELPER_CMD_PERF = 0,
};


struct s12xftm_helper_context {
	uint8_t command;
	volatile struct {
		int8_t retcode;
	} out;

	volatile struct {
		union {
			struct {
				uint32_t counter;
			} perf;
		};
	} in;
} __attribute__((__packed__));

#endif
