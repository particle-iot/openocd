
/***************************************************************************
 *   Copyright (C) 2015 by Andreas Bolsch                                  *
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
 * arm-none-eabi-gcc -Wa,-adln -c contrib/loaders/flash/stmqspi.S > stmqspi.lst
 *
 * To generate binary file:
 * arm-none-eabi-objcopy -O binary stmqspi.o stmqspi_flash_write_code.bin
 *
 * To generate C array definition:
 * xxd -i stmqspi_flash_write_code.bin
 */

/* Params:
 * r0 - total count (bytes)
 * r1 - flash page size
 * r2 - adress offset into flash
 * r3 - QSPI io_base
 * Clobbered:
 * r4 - QSPI DR address
 * r5 - ptr into buffer
 * r6 - single / dual
 * r7 - tmp
 */

#include "../../../src/flash/nor/stmqspi.h"

	.macro	wait_busy
0:
	ldr		r7, [r3, #QSPI_SR]		/* load status */
	lsrs	r7, r7, #(QSPI_BUSY+1)	/* shift BUSY into C */
	bcs		0b						/* loop till BUSY cleared */
	movs	r7, #(1<<QSPI_TCF)		/* TCF bitmask */
	str		r7, [r3, #QSPI_FCR]		/* clear TCF flag */
	.endm

	.macro	wait_fifo
1:
	ldr		r7,	[r3, #QSPI_SR]		/* load status */
	lsrs	r7, #(QSPI_FTF+1)		/* wait for FTF to be set */
	bcc		1b						/* if not, then test again */
	.endm

start:
	subs	r0, #1					/* decrement count for DLR */
	subs	r1, #1					/* page size mask and for DLR */
	movs	r4, r3					/* copy base address */
	adds	r4, #QSPI_DR			/* QSPI DR address */
	adr		r5, buffer				/* buffer start address */
	movs	r6, #0					/* one byte per default */
	ldr		r7, [r3, #QSPI_CR]		/* get QSPI CR register */
	lsrs	r7, #(QSPI_DUAL_FLASH+1)	/* dual bit into carry */
	bcc		wip_loop				/* skip if single */
	adds	r6, #1					/* two bytes for dual */

wip_loop:
	wait_busy
	str		r6, [r3, #QSPI_DLR]		/* one or two bytes */
	ldr		r7, ccr_read_status		/* CCR for status read */
	str		r7, [r3, #QSPI_CCR]		/* initiate status read */
	wait_fifo
	ldrb	r7, [r4, #0]			/* get first status register */
	lsrs	r7, #(SPIFLASH_WIP+1)	/* if first flash busy, */
	bcs		wip_loop				/* then poll again */
	tst		r6, r6					/* dual flash? */
	beq		write_enable			/* no, then ok */
	wait_fifo
	ldrb	r7, [r4, #0]			/* get second status register */
	lsrs	r7, #(SPIFLASH_WIP+1)	/* if second flash busy, */
	bcs		wip_loop				/* then poll again */

write_enable:
	wait_busy
	ldr		r7, ccr_write_enable	/* CCR for write enable */
	str		r7, [r3, #QSPI_CCR]		/* initiate write enable */

is_enabled:
	wait_busy
	str		r6, [r3, #QSPI_DLR]		/* one or two bytes */
	ldr		r7, ccr_read_status		/* CCR for status read */
	str		r7, [r3, #QSPI_CCR]		/* initiate status read */
	wait_fifo
	ldrb	r7, [r4, #0]			/* get first status register */
	lsrs	r7, #(SPIFLASH_WEL+1)	/* if first flash */
	bcc		error					/* not enabled, then error */
	tst		r6, r6					/* dual flash? */
	beq		busy_loop				/* no, then ok */
	wait_fifo
	ldrb	r7, [r4, #0]			/* get second status register */
	lsrs	r7, #(SPIFLASH_WEL+1)	/* if second flash */
	bcc		error					/* not enabled, then error */

busy_loop:
	wait_busy
	cmp		r0, r1					/* if count > page size */
	bhi		page_write				/* then write one page only */
	str		r0, [r3, #QSPI_DLR]		/* remaining count in DLR register */
	b		start_write

page_write:
	str		r1, [r3, #QSPI_DLR]		/* page size in DLR register */

start_write:
	ldr		r7, ccr_page_write		/* CCR for page write */
	str		r7, [r3, #QSPI_CCR]		/* initiate transfer */
	str		r2, [r3, #QSPI_AR]		/* store SPI start address */

write_loop:
	wait_fifo
	ldrb	r7,[r5, #0]				/* load and */
	strb	r7,[r4, #0]				/* store next byte */
	adds	r5, #1					/* increment buffer ptr */
	adds	r2, #1					/* increment address */
	subs	r0, #1					/* decrement count */
	bmi		page_end				/* stop if no data left */
	tst		r2, r1					/* page end ? */
	bne		write_loop				/* if not, then next byte */

page_end:
	ldr		r7, [r3, #QSPI_SR]		/* load status */
	lsrs	r7, r7, #(QSPI_TCF+1)	/* shift TCF into C */
	bcc		page_end				/* wait till TCF set */
	movs	r0, r0					/* test residual count */
	bpl		wip_loop				/* if non-negative, then next page */
	adds	r0, #1					/* correct count */
	b		exit

error:
	movs	r0, #0
	subs	r0, #1					/* return 0xFFFFFFFF for error */

exit:
	.align	2						/* align to word, bkpt is 4 words */
	bkpt	#0						/* before code end for exit_point */
	.align	2						/* align to word */

ccr_read_status:
	.space	4						/* QSPI_CCR value for READ_STATUS command */

ccr_write_enable:
	.space	4						/* QSPI_CCR value for WRITE_ENABLE command */

ccr_page_write:
	.space	4						/* QSPI_CCR value for PAGE_WRITE command */

buffer:								/* buffer follows right away */
