/***************************************************************************
 *   Copyright (C) 2016 by Andreas Bolsch                                  *
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
 * arm-none-eabi-gcc -Wa,-adlmn -c contrib/loaders/flash/stmqspi.S > stmqspi.lst
 *
 * To generate binary file:
 * arm-none-eabi-objcopy -O binary stmqspi.o stmqspi_flash_write_code.bin
 *
 * To generate C array definition:
 * xxd -i stmqspi_flash_write_code.bin
 */

/* Params:
 * r0 - total count (bytes), status (out)
 * r1 - flash page size
 * r2 - address offset into flash
 * r3 - QSPI io_base
 * r8 - fifo start
 * r9 - fifo end + 1

 * Clobbered:
 * r4 - rp
 * r5 - offset of QSPI_DR
 * r6 - single 0x0 / dual 0x1
 * r7 - tmp
 */

#include "../../../../src/flash/nor/stmqspi.h"

	.macro	wait_busy
0:
	ldr		r7, [r3, #QSPI_SR]			/* load status */
	lsrs	r7, r7, #(QSPI_BUSY+1)		/* shift BUSY into C */
	bcs		0b							/* loop till BUSY cleared */
	movs	r7, #(1<<QSPI_TCF)			/* TCF bitmask */
	str		r7, [r3, #QSPI_FCR]			/* clear TCF flag */
	.endm

start:
	subs	r0, #1						/* decrement count for DLR */
	subs	r1, #1						/* page size mask and for DLR */
	ldr		r4, rp						/* load rp */
	ldr		r6, [r3, #QSPI_CR]			/* get QSPI_CR register */
	lsls	r6, r6, #(31-QSPI_DUAL_FLASH)	/* clear higher order bits */
	lsrs	r6, r6, #31					/* DUAL_FLASH bit into bit 0 */
abort:
	movs	r5, #(1<<QSPI_ABORT)		/* abort bit mask */
	ldr		r7, [r3, #QSPI_CR]			/* get QSPI_CR register */
	orrs	r7,	r5						/* set abort bit */
	str		r7, [r3, #QSPI_CR]			/* store new CR register */
	movs	r5, #QSPI_DR				/* load QSPI_DR address offset */
wip_loop:
	wait_busy
	str		r6, [r3, #QSPI_DLR]			/* one or two (for dual) bytes */
	ldr		r7, ccr_read_status			/* CCR for status read */
	str		r7, [r3, #QSPI_CCR]			/* initiate status read */
	ldr		r7, [r3, #QSPI_SR]			/* delay for fifo startup */
	ldrb	r7, [r3, r5]				/* get first status register */
	lsrs	r7, r7, #(SPIFLASH_BSY+1)	/* if first flash busy, */
	bcs		wip_loop					/* then poll again */
	tst		r6, r6						/* dual mode ? */
	beq		write_enable				/* not dual, then ok */
	ldrb	r7, [r3, r5]				/* get second status register */
	lsrs	r7, r7, #(SPIFLASH_BSY+1)	/* if second flash busy, */
	bcs		wip_loop					/* then poll again */
write_enable:
	tst		r0, r0						/* test residual count */
	bmi		exit						/* if negative, then finished */
	wait_busy
	ldr		r7, ccr_write_enable		/* CCR for write enable */
	str		r7, [r3, #QSPI_CCR]			/* initiate write enable */
	wait_busy
	str		r6, [r3, #QSPI_DLR]			/* one or two (for dual) bytes */
	ldr		r7, ccr_read_status			/* CCR for status read */
	str		r7, [r3, #QSPI_CCR]			/* initiate status read */
	ldr		r7, [r3, #QSPI_SR]			/* delay for fifo startup */
	ldrb	r7, [r3, r5]				/* get first status register */
	lsrs	r7, r7, #(SPIFLASH_WE+1)	/* if first flash not */
	bcc		error						/* write enabled, then error */
	tst		r6, r6						/* dual mode ? */
	beq		start_write					/* not dual, then ok */
	ldrb	r7, [r3, r5]				/* get second status register */
	lsrs	r7, r7, #(SPIFLASH_WE+1)	/* if second flash not */
	bcc		error						/* write enabled, then error */
start_write:
	mov		r7, r0						/* get remaining count */
	cmp		r0, r1						/* if count <= page size */
	bls		write_dlr					/* then write all remaining */
	mov		r7, r1						/* else exactly one page */
write_dlr:
	str		r7, [r3, #QSPI_DLR]			/* size-1 in DLR register */
	ldr		r7, ccr_page_write			/* CCR for page write */
	str		r7, [r3, #QSPI_CCR]			/* initiate transfer */
	str		r2, [r3, #QSPI_AR]			/* store SPI start address */
write_loop:
	ldr		r7, wp						/* get wp */
	cmp		r7, #0						/* if wp equals 0 */
	beq		exit						/* then abort */
	cmp		r4, r7						/* check if fifo empty */
	beq		write_loop					/* wait till not empty */
	ldrb	r7, [r4, #0]				/* read next byte */
	strb	r7, [r3, r5]				/* write next byte to DR */
	adds	r2, #1						/* increment address */
	adds	r4, #1						/* increment internal rp */
	cmp		r4, r9						/* internal rp beyond end? */
	blo		write_loop1					/* if no, the ok */
	mov		r4, r8						/* else wrap around */
write_loop1:
	adr		r7, rp						/* get address of rp */
	str		r4, [r7, #0]				/* store updated rp */
	subs	r0, #1						/* decrement count */
	bmi		page_end					/* stop if no data left */
	tst		r2, r1						/* page end ? */
	bne		write_loop					/* if not, then next byte */
page_end:
	ldr		r7, [r3, #QSPI_SR]			/* load status */
	lsrs	r7, r7, #(QSPI_TCF+1)		/* shift TCF into C */
	bcc		page_end					/* wait till TCF set */
	b		wip_loop					/* then next page */

error:
	movs	r0, #0						/* return 0xFFFFFFFF */
	subs	r0, #2						/* for error */
exit:
	adds	r0, #1						/* correct count */
	movs	r6, #(1<<QSPI_ABORT)		/* abort bit mask */
	ldr		r7, [r3, #QSPI_CR]			/* get QSPI CR register */
	orrs	r7,	r6						/* set abort bit */
	str		r7, [r3, #QSPI_CR]			/* store new CR register */

	.align	2							/* align to word, bkpt is 4 words */
	bkpt	#0							/* before code end for exit_point */
	.align	2							/* align to word */

ccr_read_status:
	.space	4							/* QSPI_CCR value for READ_STATUS command */
ccr_write_enable:
	.space	4							/* QSPI_CCR value for WRITE_ENABLE command */
ccr_page_write:
	.space	4							/* QSPI_CCR value for PAGE_WRITE command */

	.equ wp, .							/* wp, uint32_t */
	.equ rp, wp + 4						/* rp, uint32_t */
	.equ buffer, rp + 4					/* buffer follows right away */
