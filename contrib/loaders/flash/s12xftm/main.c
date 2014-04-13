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

/*
  WARNING! WARNING! WARNING!

  When making changes to this code, please make sure to inspect the
  resulting assembly and verify that all generated code is position
  independent. When allocating memory space on the target OpenOCD does
  not give any guaranteees that the start address of it would have any
  specific value, so it is important to us to be able to function when
  placed at an arbitrary offset.

  WARNING! WARNING! WARNING!
 */
#include <stdint.h>
#include <stdbool.h>

#include <helper/log.h>
#include <flash/nor/s12xftm.h>

static inline void bgnd(void) __attribute__((naked, noreturn));
static inline void bgnd(void)
{
	__asm__ __volatile__ ("bgnd");
}

int main(void) __attribute__((naked, noreturn));
int main(void)
{
	register uint16_t x __asm__("x");

	struct s12xftm_helper_context *context = (struct s12xftm_helper_context *)x;

	switch (context->command) {
	case S12XFTM_HELPER_CMD_PERF:
		while (--context->in.perf.counter)
			;

		context->out.retcode = ERROR_OK;
		break;

	default:
		break;
	}

	bgnd();
}
