/***************************************************************************
 *   Copyright (C) 2012 by George Harris                                   *
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

	.text
	.syntax unified
	.cpu cortex-m3
	.thumb
	.thumb_func

/*
 * Params :
 * r0 = workarea start, status (out)
 * r1 = workarea end
 * r2 = target address (offset from flash base)
 * r3 = count (bytes)
 * r4 = page size
 * Clobbered:
 * r7 - rp
 * r8 - wp, tmp
 * r9 - send/receive data
 * r10 - temp
 * r11 - current page end address
 */

setup: /* Initialize SSP pins and module */
	mov.w	r10, #0x6000
	movt	r10, #0x4008
	mov.w	r8, #0xea
	str.w	r8, [r10, #0x18c]	/* Configure SCK pin function */
	mov.w	r8, #0x40
	str.w	r8, [r10, #0x190]	/* Configure /HOLD pin function */
	mov.w	r8, #0x40
	str.w	r8, [r10, #0x194]	/* Configure /WP pin function */
	mov.w	r8, #0xed
	str.w	r8, [r10, #0x198]	/* Configure MISO pin function */
	mov.w	r8, #0xed
	str.w	r8, [r10, #0x19c]	/* Configure MOSI pin function */
	mov.w	r8, #0x44
	str.w	r8, [r10, #0x1a0]	/* Configure CS pin function */

	mov.w	r10, #0x6000
	movt	r10, #0x400f
	mov.w	r8, #0x800
	str 	r8, [r10, #0x14]	/* Set CS as output */
	mov.w	r10, #0x5000
	movt	r10, #0x400f
	mov.w	r8, #0xff
	str.w	r8, [r10, #0x2ac]	/* Set CS high */

	mov.w 	r10, #0x0000
	movt 	r10, #0x4005
	mov.w 	r8, #0x0000
	movt 	r8, #0x0100
	str.w 	r8, [r10, #0x94] 	/* Configure SSP0 base clock (use 12 MHz IRC) */

	mov.w 	r10, #0x2000
	movt 	r10, #0x4005
	mov.w 	r8, #0x01
	str.w 	r8, [r10, #0x700] 	/* Configure (enable) SSP0 branch clock */

	mov.w 	r10, #0x3000
	movt	r10, #0x4008
	mov.w 	r8, #0x07
	str.w 	r8, [r10] 			/* Set clock postscale */
	mov.w 	r8, #0x02
	str.w 	r8, [r10, #0x10] 	/* Set clock prescale */
	str.w 	r8, [r10, #0x04] 	/* Enable SSP in SPI mode */

	mov.w 	r11, #0x00
find_next_page_boundary:
	add 	r11, r4			/* Increment to the next page */
	cmp 	r11, r2
	/* If we have not reached the next page boundary after the target address, keep going */
	bls 	find_next_page_boundary
write_enable:
	bl 		cs_down
	mov.w 	r9, #0x06 		/* Send the write enable command */
	bl 		write_data
	bl 		cs_up

	bl 		cs_down
	mov.w 	r9, #0x05 		/* Get status register */
	bl 		write_data
	mov.w 	r9, #0x00 		/* Dummy data to clock in status */
	bl 		write_data
	bl 		cs_up

	tst 	r9, #0x02 		/* If the WE bit isn't set, we have a problem. */
	beq 	error
page_program:
	bl 		cs_down
	mov.w 	r9, #0x02 		/* Send the page program command */
	bl 		write_data
write_address:
	lsr 	r9, r2, #16 	/* Send the current 24-bit write address, MSB first */
	bl 		write_data
	lsr 	r9, r2, #8
	bl 		write_data
	mov.w 	r9, r2
	bl 		write_data
wait_fifo:
	ldr 	r8, [r0]  		/* read the write pointer */
	cmp 	r8, #0 			/* if it's zero, we're gonzo */
	beq 	exit
	ldr 	r7, [r0, #4] 	/* read the read pointer */
	cmp 	r7, r8 			/* wait until they are not equal */
	beq 	wait_fifo
write:
	ldrb 	r9, [r7], #0x01 /* Load one byte from the FIFO, increment the read pointer by 1 */
	bl 		write_data 		/* send the byte to the flash chip */

	cmp 	r7, r1			/* wrap the read pointer if it is at the end */
	it  	cs
	addcs	r7, r0, #8		/* skip loader args */
	str 	r7, [r0, #4]	/* store the new read pointer */
	subs	r3, r3, #1		/* decrement count */
	cbz		r3, exit 		/* Exit if we have written everything */

	add 	r2, #1 			/* Increment flash address by 1 */
	cmp 	r11, r2   		/* See if we have reached the end of a page */
	bne 	wait_fifo 		/* If not, keep writing bytes */
	bl 		cs_up			/* Otherwise, end the command and keep going w/ the next page */
	add 	r11, r4 		/* Move up the end-of-page address by the page size*/
wait_flash_busy:			/* Wait for the flash to finish the previous page write */
	bl 		cs_down
	mov.w 	r9, #0x05 		/* Get status register */
	bl 		write_data
	mov.w 	r9, #0x00 		/* Dummy data to clock in status */
	bl 		write_data
	bl 		cs_up
	tst 	r9, #0x01 		/* If it isn't done, keep waiting */
	bne 	wait_flash_busy
	b 		write_enable 	/* If it is done, start a new page write */
write_data: 				/* Send/receive 1 byte of data over SSP */
	mov.w	r10, #0x3000
	movt	r10, #0x4008
	str.w 	r9, [r10, #0x008] 	/* Write supplied data to the SSP data reg */
wait_transmit:
	ldr 	r9, [r10, #0x00c] 	/* Check SSP status */
	tst 	r9, #0x0010
	bne 	wait_transmit 		/* If still transmitting, keep waiting */
	ldr 	r9, [r10, #0x008]  	/* Load received data */
	bx 		lr 					/* Exit subroutine */
cs_up:
	mov.w 	r8, #0xff
	b 		cs_write
cs_down:
	mov.w 	r8, #0x0000
cs_write:
	mov.w 	r10, #0x4000
	movt	r10, #0x400f
	str.w 	r8, [r10, #0xab]
	bx 		lr
error:
	movs	r0, #0
	str 	r0, [r2, #4]	/* set rp = 0 on error */
exit:
	mov 	r0, r6
	bkpt 	#0x00

	.end
