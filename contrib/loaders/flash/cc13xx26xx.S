/***************************************************************************
 *   Copyright (C) 2016 by Peter Andersson                                 *
 *   pelleplutt1976@gmail.com                                              *
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

	.text
	.syntax unified
	.cpu cortex-m3
	.thumb
	.thumb_func

	/* Simple trampoline code for calling TI's ROM library functions
	 * and return gracefully to OpenOCD
	 * Params:
	 * r0 - first argument to cc13xx/26xx ROM library function
	 * r1 - second argument to cc13xx/26xx ROM library function
	 * r2 - third argument to cc13xx/26xx ROM library function
	 * r3 - address of cc13xx/26xx ROM library function
	 * Clobbered:
	 * unknown, depends on TI's rommed api
	 */
	blx		r3
	bkpt    #0
