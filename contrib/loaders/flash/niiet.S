/***************************************************************************
 *   Copyright (C) 2011 by Andreas Fritiofson                              *
 *   andreas.fritiofson@gmail.com                                          *
 *                                                                         *
 *   Copyright (C) 2013 by Paul Fertser                                    *
 *   fercerpav@gmail.com                                                   *
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
 ***************************************************************************/

/* Registers addresses */
#define NIIET_FLASH_BASE 0xA001C000
#define NIIET_FLASH_FMA 0x00		/* Address reg */
#define NIIET_FLASH_FMD1 0x04		/* Data reg 1'st word */
#define NIIET_FLASH_FMC 0x08		/* Command reg */
#define NIIET_FLASH_FCIS 0x0C		/* Operation Status reg */
#define NIIET_FLASH_FCIM 0x10		/* Interrupt Mask reg (not used) */
#define NIIET_FLASH_FCIC 0x14		/* Operation Status Clear reg */
#define NIIET_FLASH_FMD2 0x50		/* Data reg 2'nd word */
#define NIIET_FLASH_FMD3 0x54		/* Data reg 3'd word */
#define NIIET_FLASH_FMD4 0x58		/* Data reg 4'th word*/

/* Magic key*/
#define NIIET_KEY 0xA4420000

/* Bit masks for regs*/
#define NIIET_FLASH_WRITE (1 << 0)							/* Main block write */
#define NIIET_FLASH_PERASE (1 << 1)							/* Main block page erase */
#define NIIET_FLASH_FERASE (1 << 2)							/* Main block full erase */
#define NIIET_FLASH_IFBWRITE (1 << 4)						/* Info block write */
#define NIIET_FLASH_IFBPERASE (1 << 5)						/* Info block erase */

	/* Params:
	 * r0 - flash base (in), status (out)
	 * r1 - count (32bit)
	 * r2 - workarea start
	 * r3 - workarea end
	 * r4 - target address
	 * Clobbered:
	 * r5 - rp
	 * r6 - wp, tmp
	 * r7 - current FLASH_CMD
	 */


flasher:
	ldr r7, [r0, #NIIET_FLASH_FMD1]
wait_fifo:
	ldr 	r6, [r2, #0]							/* read wp */
	cmp 	r6, #0									/* abort if wp == 0 */
	beq 	exit
	ldr 	r5, [r2, #4]							/* read rp */
	cmp 	r5, r6									/* wait until rp != wp */
	beq 	wait_fifo

	 /* "*target_address++ = *rp++"     x4 words*/
	ldr	r6, [r5]
	str	r6, [r0, #NIIET_FLASH_FMD1]
	ldr	r6, [r5, #4]
	str	r6, [r0, #NIIET_FLASH_FMD2]
	ldr	r6, [r5, #8]
	str	r6, [r0, #NIIET_FLASH_FMD3]
	ldr	r6, [r5, #12]
	str	r6, [r0, #NIIET_FLASH_FMD4]
	str	r4, [r0, #NIIET_FLASH_FMA]

	ldr r6, =(NIIET_KEY | NIIET_FLASH_WRITE)

	str r6, [r0, #NIIET_FLASH_FMC]
	bl waitFCIS			/* wait for OperationComplete flag */

	adds r5, #16
	adds r4, #16

	cmp 	r5, r3		/* wrap rp at end of buffer */
	bcc	no_wrap
	mov	r5, r2
	adds	r5, #8


no_wrap:
	str 	r5, [r2, #4]	/* store rp */
	subs	r1, r1, #1		/* decrement word count */
	cmp     r1, #0
	ble     exit			/* loop if not done */
	b	wait_fifo

waitFCIS:
	/* Loop here until OperationComplete flag is not set */
	/* If OperationError flag is set instead - quit */
	ldr r6, [r0, #NIIET_FLASH_FCIS]
	tst r6, #2
	bne exit
	tst r6, #1
	beq waitFCIS
	ldr r6, =1
	str r6, [r0, #NIIET_FLASH_FCIC]
	bx lr

exit:
	mov		r0, r6			/* return status in r0 */
	bkpt	#0				/* breakpoint */

.size  flasher, .-flasher
