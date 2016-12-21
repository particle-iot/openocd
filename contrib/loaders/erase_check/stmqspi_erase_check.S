/***************************************************************************
 *   Copyright (C) 2017 by Andreas Bolsch                                  *
 *   andreas.bolsch@mni.thm.de                                             *
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
	.cpu cortex-m0
	.thumb
	.thumb_func

/* To assemble:
 * arm-none-eabi-gcc -Wa,-adlmn contrib/loaders/erase_check/stmqspi_erase_check.S > stmqspi_erase_check.lst
 *
 * To generate binary file:
 * arm-none-eabi-objcopy -O binary stmqspi_erase_check.o stmqspi_erase_check.bin
 *
 * To generate C array definition:
 * xxd -i stmqspi_erase_check.bin
 */

/* Params:
 * r0 - sector count
 *
 * Clobbered:
 * r1 - r7 tmp */

start:
	adr		r1, buffer					/* pointer to start of buffer */
sector_start:
	ldr		r2, [r1, #0]				/* start address of sector */
	ldr		r3, [r1, #4]				/* length of sector */
	ldr		r4, [r1, #8]				/* initial result 0x00FF */
read_loop:
	ldrb	r5, [r2]					/* read byte from flash */
	movs	r6, #0xFF					/* fill bits 8-15 */
	lsls	r6, #8						/* with ones */
	orrs	r6, r6, r5					/* copy ones to left of read byte */
	ands	r4, r4, r6					/* and read byte to result */
	lsls	r5, r5, #8					/* shift result into higher byte */
	orrs	r4, r4, r5					/* or read byte to result */
	adds	r2, r2, #1					/* increment address */
	subs	r3, r3, #1					/* decrement count */
	bne		read_loop					/* if not done, then next byte */
sector_end:
	str		r3, [r1, #4]				/* store remaining (zero) count */
	str		r4, [r1, #8]				/* save updated result */
	adds	r1, r1, #12					/* three words */
	subs	r0, r0, #1					/* decrement count */
	bne		sector_start				/* next sector if not finished */
exit:
	.align	2							/* align to word, bkpt is 4 words */
	bkpt	#0							/* before code end for exit_point */
	.align	2							/* align to word */

	.equ	buffer, .
