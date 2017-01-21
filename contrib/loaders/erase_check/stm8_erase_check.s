/*
    Copyright (C) 2017 Ake Rehnman
    ake.rehnman(at)gmail.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
;;
;; erase check memory code
;;
 .org 0x0
;; start address
 VAR0: .byte 0x00
       .word 0x9000
;; byte count
 VAR1: .byte 0x01
       .word 0x0000
;
; SP must point to VAR0 on entry
; first relocate VAR0 to the location
; we are running at
start:
	ldw X,SP
	ldw .cont+2,X
	ldw X,(VAR0+1,SP)	;start addr
	ldw Y,(VAR1+1,SP)	;count
	ld A,#0xff
;
; if count == 0 return
.L1:
	tnzw Y
	jrne .decrcnt	;continue if low word != 0
	tnz (VAR1,SP)	;high byte
	jreq .exit	;goto exit
;
; decrement count (VAR1)
.decrcnt:
	tnzw Y	;low word count
	jrne .decr1
	dec (VAR1,SP)	;high byte
.decr1:
	decw Y;	decr low word
;
; first check if [VAR0] is 0xff
.cont:
	ldf A, [VAR0.e]
	cp A,#0xff
	jrne .exit ;exit if not 0xff
;
; increment VAR0 (addr)
	incw X
	jrne .L1
	inc (VAR0,SP)	;increment high byte
	jra .L1
;
.exit:
	ldw (VAR0+1,SP),X	;start addr
	ldw (VAR1+1,SP),Y	;count
	break
