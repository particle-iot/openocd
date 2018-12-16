/***************************************************************************
 *   Copyright (C) 2010 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2011 Øyvind Harboe                                      *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2015 Uwe Bonnes                                         *
 *   bon@elektron.ikp.physik.tu-darmstadt.de                               *
 *                                                                         *
 *   Copyright (C) 2018 Andreas Bolsch                                     *
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
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.                                        *
 ***************************************************************************/

	.text
	.syntax unified
	.cpu cortex-m0
	.thumb

/*
 * Params :
 * r0 = workarea start, status (out)
 * r1 = workarea end + 1
 * r2 = target address
 * r3 = count (64bit words)
 * r4 = flash base
 *
 * Clobbered:
 * r5   - rp
 * r6/7 - temp (64-bit)
 */

#include "../../../../src/flash/nor/stm32l4x.h"

	.thumb_func
	.global _start
_start:
wait_fifo:
	ldr		r6, [r0, #0]				/* read wp */
	cmp		r6, #0						/* if wp == 0,  */
	beq		exit						/* then abort */
	ldr		r5, [r0, #4]				/* read rp */
	subs	r6, r6, r5					/* number of bytes available for read in r6 */
	bpl		fifo_stat					/* if not wrapped around, skip */
	adds	r6, r1						/* add end of buffer */
	subs	r6, r0						/* sub start of buffer */
fifo_stat:
	cmp		r6, #8						/* wait until *more* than 8 bytes available */
	bcc		wait_fifo

	movs	r6, #(1 << FLASH_PG)		/* flash program enable */
	str		r6, [r4, #FLASH_CR_OFFS]	/* start operation */
	ldmia	r5!, {r6, r7}				/* read one dword from src, increment ptr */
	stmia	r2!, {r6, r7}				/* write one dword to dst, increment ptr */
	dsb
busy:
	ldr 	r6, [r4, #FLASH_SR_OFFS]	/* get status register */
	lsrs	r7, r6, #FLASH_BSY+1		/* BSY == 1 => operation in progress */
	bne 	busy						/* wait more ... */
	movs	r7, #FLASH_ERROR_MASK		/* PGSERR | SIZERR | PGPERR | PGAERR | WRPERR | OPERR */
	uxtb	r7, r7						/* discard upper 24 bits */
	tst		r6, r7						/* check for any error bit */
	bne		error						/* fail... */

	cmp		r5, r1						/* rp at end of buffer? */
	bcc		upd_rp						/* if no, then skip */
	subs	r5, r1						/* sub end of buffer */
	adds	r5, r0						/* add start of buffer */
	adds	r5, #8						/* skip wp and rp */
upd_rp:
	str		r5, [r0, #4]				/* store rp */
	subs	r3, r3, #1					/* decrement dword count */
	beq		exit						/* exit if done */
	b		wait_fifo
error:
	movs	r1, #0
	str		r1, [r0, #4]				/* set rp = 0 on error */
exit:
	movs	r6, #(0 << FLASH_PG)		/* flash program disable */
	str		r6, [r4, #FLASH_CR_OFFS]	/* start operation */
	mov		r0, r6						/* return status in r0 */
	bkpt	#0x00

