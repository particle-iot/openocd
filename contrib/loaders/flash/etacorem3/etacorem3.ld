/****************************************************************************
*  Copyright (C) 2017 by Rick Foos
*  rfoos@solengtech.com
*
*  Copyright (C) 2017 by ETA Compute
*  www.etacompute.com
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*  1. Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*  2. Redistributions in binary form must reproduce the above copyright
*     notice, this list of conditions and the following disclaimer in the
*     documentation and/or other materials provided with the distribution.
*  3. Neither the name of the author nor the names of its contributors may
*     be used to endorse or promote products derived from this software
*     without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
*  THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
*  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
*  AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
*  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
*  THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
*  SUCH DAMAGE.
*
****************************************************************************/


ENTRY(ResetISR)
SEARCH_DIR(.)

/*
 *   SRAM is at 0x10000000 of length 0x00020000
 */
MEMORY
{
    FLASH(RX) : ORIGIN = 0x01000000, LENGTH = 0x00100000
    SRAM (RWX) : ORIGIN = 0x10000000, LENGTH = 0x00020000
}

SECTIONS
{
    .text :
    {
        _text = .;
        KEEP(*(.vectors))
        . = ALIGN(0x4);
        *(.text*)
        . = ALIGN(0x4);
        *(.rodata*)
        . = ALIGN(0x4);
        _etext = .;
    } > SRAM= 0

    .data :
    {
        . = ALIGN(0x4);
        _data = .;
        *(.data*)
        KEEP(*(.mainHeap))
        _edata = .;
    } > SRAM

    .datax :
    {
        _datax = .;
        KEEP(*(.mainStack))
        _edatax = .;
    } > SRAM
    .bss :
    {
        . = ALIGN(0x4);
        _bss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > SRAM

    .magic  0x1001FFF0 :
    {
        KEEP(*(.magic))
        magic_numbers[0]=0xc001c0de;
        magic_numbers[1]=0xc001c0de;
        magic_numbers[2]=0xdeadbeef;
        magic_numbers[3]=0xc369a517;
    } > SRAM


}
