/***************************************************************************
 *   Copyright (C) 2012 by George Harris  		                           *
 *   george@luminairecoffee.com                                            *
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

/***************************************************************************
*	This is an algorithm for the LPC43xx family (and probably the LPC18xx  *
*	family as well, though they have not been tested) that will initialize *
*	memory-mapped SPI flash accesses. Unfortunately NXP has published      *
*	neither the ROM source code that performs this initialization nor the  *
*	register descriptions necessary to do so, so this code is necessary to *
*	call into the ROM SPIFI API.                                           *
***************************************************************************/

	.text
	.syntax unified
	.arch armv7-m
	.thumb
	.thumb_func

	.align 2

/*
 * Params :
 * r0 = spifi clock speed
 */

code:
	mov.w 	r8, r0
	sub		sp, #132
	add		r7, sp, #0
	/* Initialize SPIFI pins */
	mov.w	r3, #24576
	movt	r3, #16392
	mov.w	r2, #243
	str.w 	r2, [r3, #396]
	mov.w	r3, #24576
   	movt	r3, #16392
   	mov.w	r2, #24576
   	movt	r2, #16392
   	mov.w	r1, #24576
   	movt	r1, #16392
   	mov.w	r0, #24576
   	movt	r0, #16392
   	mov.w	r4, #211
   	str.w	r4, [r0, #412]
   	mov	r0, r4
   	str.w	r0, [r1, #408]
   	mov	r1, r0
   	str.w	r1, [r2, #404]
   	str.w	r1, [r3, #400]
   	mov.w	r3, #24576
   	movt	r3, #16392
   	mov.w	r2, #19
   	str.w	r2, [r3, #416]

   	/* Perform SPIFI init. See spifi_rom_api.h (in NXP's lpc43xx driver package) for details */
   	/* on initialization arguments. */
   	movw 	r3, #280			/* The ROM API table is located @ 0x10400118, and			*/
   	movt 	r3, #4160			/* the first pointer in the struct is to the init function. */
   	ldr 	r3, [r3, #0]
   	ldr 	r4, [r3, #0]		/* Grab the init function pointer from the table */
   	/* Set up function arguments */
   	movw 	r0, #948
   	movt 	r0, #4096			/* Pointer to a SPIFI data struct that we don't care about */
   	mov.w 	r1, #3 				/* "csHigh". Not 100% sure what this does. */
   	mov.w 	r2, #192 			/* The configuration word: S_RCVCLOCK | S_FULLCLK */
   	mov.w 	r3, r8 				/* SPIFI clock speed (12MHz) */
   	blx 	r4					/* Call the init function */
   	b 		done

done:
	bkpt 	#0

	.end
