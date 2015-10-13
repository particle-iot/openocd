/***************************************************************************                                                  *
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

	.text
	.syntax unified
	.cpu cortex-m4
	.thumb
	.thumb_func
	.global write

	/* Params:
	 * r0 - write cmd (in), status (out)
	 * r1 - count
	 * r2 - workarea start
	 * r3 - workarea end
	 * r4 - target address
	 * Clobbered:
	 * r5 - rp
	 * r6 - wp, tmp
	 * r7 - flash base
	 */

	ldr		r7, =#0x0A001C000	/* Flash reg base*/
	bl		wait_fifo
	b		load_data


wrap_check:
	cmp		r5, r3			/* wrap rp at end of buffer */
	bcc		no_wrap
	mov		r5, r2
	adds	r5, #8
no_wrap:
	str		r5, [r2, #4]	/* store rp */
	subs	r1, r1, #1		/* decrement word count */
	cmp		r1, #0
	beq		exit			/* loop if not done */
	bx		LR


wait_fifo:
	ldr		r6, [r2, #0]	/* read wp */
	cmp		r6, #0			/* abort if wp == 0 */
	beq		exit
	ldr		r5, [r2, #4]	/* read rp */
	cmp		r5, r6			/* wait until rp != wp */
	beq		wait_fifo
	bx		LR


load_data:
	ldr r6, [r5]			/* read data1 */
	str r6, [r7, #0x04]		/* (FMD1) */
	adds	r5, #4
	bl		wrap_check
	bl		wait_fifo

	ldr r6, [r5]			/* read data2 */
	str r6, [r7, #0x50]		/* (FMD2) */
	adds	r5, #4
	bl		wrap_check
	bl		wait_fifo

	ldr r6, [r5]			/* read data3 */
	str r6, [r7, #0x54]		/* (FMD3) */
	adds	r5, #4
	bl		wrap_check
	bl		wait_fifo

	ldr r6, [r5]			/* read data4 */
	str r6, [r7, #0x58]		/* (FMD4) */
	adds	r5, #4

start_write:
	str r4, [r7, #0x00]		/* set addr (FMA) */
	adds	r4, #16
	str r0, [r7, #0x08]		/* write cmd (FMC) */

busy:
	ldr		r6, [r7, #0x0C]	/* wait until flag set (FCIS) */
	cmp		r6, #0x0
	beq		busy

	cmp		r6, #2			/* check the error bit */
	beq		error

	movs	r6, #1			/* clear flags (FCIC) */
	str r6, [r7, #0x14]

	bl		wrap_check
	bl		wait_fifo
	b		load_data

error:
	movs	r0, #0
	str		r0, [r2, #4]	/* set rp = 0 on error */
exit:
	mov		r0, r6			/* return status in r0 */
	bkpt	#0
