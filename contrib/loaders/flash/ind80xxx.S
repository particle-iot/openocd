/***************************************************************************
 *   Copyright (C) 2017 IndieSemi                                          *
 *   Artur Troian <troian.ap@gmail.com>                                    *
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

    .syntax unified
    .cpu cortex-m0
    .text
    .thumb
    .thumb_func

/* To assemble:
 * arm-none-eabi-gcc -c ind80xxx.S
 *
 * To disassemble:
 * arm-none-eabi-objdump -o ind80xxx.o
 *
 * To generate binary file:
 * arm-none-eabi-objcopy -O binary ind80xxx.o ind80xxx_flash_write_code.bin
 *
 * To generate include file:
 * xxd -i ind80xxx_flash_write_code.bin
 */

/*
 * Params :
 * r0 = workarea start, status (out)
 * r1 = workarea end
 * r2 = target address
 * r3 = count (64bit words)
 *
 * Clobbered:
 * r4   - rp
 * r5   - wp
 * r6   - tmp
 * r7   - tmp
 */

wait_fifo:
    ldr     r5, [r0, #0]                    /* read wp */
    cmp     r5, #0                          /* abort if wp == 0 */
    beq     exit
    ldr     r4, [r0, #4]                    /* read rp */
    cmp     r4, r5                          /* wait until rp != wp */
    beq     wait_fifo


    // set target address
    ldr     r6, FLASHCTRL_SFR_ADDR
    str     r2, [r6]

    // load unlock pattern
    ldr     r6, FLASHCTRL_SFR_UNLOCKWRITE
    ldr     r7, E_FLASH_WRITE_UNLOCK_PAT
    str     r7, [r6]

    // Check that the address written in the FLASH_ADDR register is still the expected address
    ldr     r6, FLASHCTRL_SFR_ADDR
    ldr     r7, [r6]
    cmp     r2, r7
    bne     error

    ldr     r6, [r4]        // read one word from src
    ldr     r7, FLASHCTRL_SFR_WRITEDATA
    str     r6, [r7]        // write to dst
    adds    r4, #4          // increment source address
    adds    r2, #4          // increment target address

    // enable write
    ldr     r6, FLASHCTRL_SFR_STARTWRITE
    ldr     r7, E_FLASH_WRITE_START_PAT
    str     r7, [r6]

    cmp     r4, r1  // wrap rp at end of work area buffer
    bcc     no_wrap
    mov     r4, r0
    adds    r4, #8  // skip rp, wp at start of work area

no_wrap:
    str     r4, [r0, #4]    // write back rp
    subs    r3, #4          // decrement word count
    bne     wait_fifo       // loop if not done
    b       exit

error:
    movs    r6, #1
    movs    r1, #0
    str     r1, [r0, #4]    /* set rp = 0 on error */
exit:
    mov     r0, r6
    bkpt    #0x00

.align 4
FLASHCTRL_SFR_CTRL:         .word 0xA0000000
FLASHCTRL_SFR_ADDR:         .word 0xA0000004
FLASHCTRL_SFR_WRITEDATA:    .word 0xA0000008
FLASHCTRL_SFR_UNLOCKWRITE:  .word 0xA000000C
FLASHCTRL_SFR_STARTWRITE:   .word 0xA0000010
FLASHCTRL_SFR_UNLOCKERASE:  .word 0xA0000014
FLASHCTRL_SFR_STARTERASE:   .word 0xA0000018
FLASHCTRL_SFR_UNLOCKCTRLOP: .word 0xA000001C
FLASHCTRL_SFR_CTRLOP:       .word 0xA0000020

E_FLASH_CTRL_UNLOCK_PAT:    .word 0xACDC1972
E_FLASH_WRITE_UNLOCK_PAT:   .word 0x55555555
E_FLASH_WRITE_START_PAT:    .word 0xAAAAAAAA
E_FLASH_ERASE_UNLOCK_PAT:   .word 0x66666666
E_FLASH_ERASE_START_PAT:    .word 0xEEEEEEEE
