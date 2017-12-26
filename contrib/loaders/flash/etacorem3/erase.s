@
@ Eta Compute ECM35xx flash sector write algorithm.
@
@ Copyright (C) 2017-2018 Rick Foos <rfoos@solengtech.com>.
@
@ Copyright (C) 2017-2018 Eta Compute www.etacompute.com.
@
@ All rights reserved.
@
	.syntax unified
	.cpu cortex-m3
	.fpu softvfp
	.eabi_attribute 20, 1
	.eabi_attribute 21, 1
	.eabi_attribute 23, 3
	.eabi_attribute 24, 1
	.eabi_attribute 25, 1
	.eabi_attribute 26, 1
	.eabi_attribute 30, 6
	.eabi_attribute 34, 1
	.eabi_attribute 18, 4
	.thumb
	.syntax unified
	.file	"erase.c"
	.comm	BootROM_flash_erase,4,4
	.section	.text.main,"ax",%progbits
	.align	2
	.global	main
	.thumb
	.thumb_func
	.type	main, %function
main:
	@ args = 0, pretend = 0, frame = 24
	@ frame_needed = 1, uses_anonymous_args = 0
	push	{r4, r7, lr}
	sub	sp, sp, #36
	add	r7, sp, #8
	mov	r3, #268439552
	str	r3, [r7, #16]
	ldr	r3, [r7, #16]
	ldr	r3, [r3]
	str	r3, [r7, #20]
	ldr	r3, [r7, #16]
	ldr	r3, [r3, #4]
	str	r3, [r7, #12]
	ldr	r2, [r7, #20]
	ldr	r3, [r7, #12]
	add	r3, r3, r2
	str	r3, [r7, #8]
	ldr	r3, [r7, #20]
	cmp	r3, #16777216
	bcs	.L2
	ldr	r3, [r7, #16]
	movs	r2, #1
	str	r2, [r3, #16]
	b	.L3
.L2:
	ldr	r3, [r7, #20]
	cmp	r3, #17301504
	bcc	.L4
	ldr	r3, [r7, #16]
	movs	r2, #2
	str	r2, [r3, #16]
	b	.L3
.L4:
	ldr	r3, [r7, #8]
	cmp	r3, #17301504
	bls	.L5
	ldr	r3, [r7, #16]
	movs	r2, #3
	str	r2, [r3, #16]
	b	.L3
.L5:
	ldr	r3, [r7, #16]
	ldr	r3, [r3, #12]
	cmp	r3, #0
	beq	.L6
	ldr	r3, [r7, #16]
	ldr	r3, [r3, #12]
	mov	r2, r3
	ldr	r3, .L14
	str	r2, [r3]
	ldr	r3, [r7, #16]
	ldr	r3, [r3, #8]
	cmp	r3, #1
	bne	.L10
	b	.L13
.L6:
	ldr	r3, [r7, #16]
	movs	r2, #4
	str	r2, [r3, #16]
	b	.L3
.L13:
	ldr	r3, .L14
	ldr	r4, [r3]
	movs	r3, #48
	str	r3, [sp, #4]
	mov	r3, #768
	str	r3, [sp]
	movs	r3, #48
	movs	r2, #16
	movs	r1, #1
	mov	r0, #16777216
	blx	r4
	b	.L9
.L11:
	ldr	r3, [r7, #20]
	bic	r3, r3, #4080
	bic	r3, r3, #15
	str	r3, [r7, #4]
	ldr	r3, .L14
	ldr	r4, [r3]
	movs	r3, #48
	str	r3, [sp, #4]
	mov	r3, #768
	str	r3, [sp]
	movs	r3, #48
	movs	r2, #16
	movs	r1, #0
	ldr	r0, [r7, #4]
	blx	r4
	ldr	r3, [r7, #20]
	add	r3, r3, #4096
	str	r3, [r7, #20]
.L10:
	ldr	r2, [r7, #20]
	ldr	r3, [r7, #8]
	cmp	r2, r3
	bcc	.L11
.L9:
	ldr	r3, [r7, #16]
	movs	r2, #0
	str	r2, [r3, #16]
.L3:
	.syntax unified
@ 110 "../src/erase.c" 1
	    BKPT      #0
@ 0 "" 2
	.thumb
	.syntax unified
	movs	r3, #0
	mov	r0, r3
	adds	r7, r7, #28
	mov	sp, r7
	@ sp needed
	pop	{r4, r7, pc}
.L15:
	.align	2
.L14:
	.word	BootROM_flash_erase
	.size	main, .-main
	.ident	"GCC: (15:5.4.1+svn241155-1) 5.4.1 20160919"
