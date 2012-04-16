.syntax unified
.cpu cortex-m3
.thumb

.global _start

_start:
	ldr sp, =_estack
	push {r0, r1, r2}

/*------------------------------------------------------------------------------
 * Clear .bss
 */
clear_bss:
	ldr r3, =0
	ldr r4, =__bss_start__
	ldr r5, =__bss_end__
clear_bss_loop:
	cmp r4, r5
	it ne
	strne r3, [r4], #4
	bne clear_bss_loop

/*------------------------------------------------------------------------------
 * branch to main.
 */
	pop {r0, r1, r2}
	bl main
	bkpt #0

ExitLoop:
	B ExitLoop
