#include "../../../src/flash/nor/spi.h"

/* set NCS */
	.macro deselect
deselect:
	ldr		r6, port_pin_ncs			/* NCS port address */
	ldr		r7, [r6]					/* load port data */
	ldr		r6, port_pin_ncs+4			/* NCS pin bitmask */
	orrs	r7, r7, r6					/* set NCS bit */
	ldr		r6, port_pin_ncs			/* NCS port address */
	str		r7, [r6]					/* store new contents */
	nop									/* switch to output */
	bx		lr							/* return */
	.endm

/* entry point, initialize registers */
	.macro	setup_regs
	subs	r0, #1						/* decrement count */
	mov		r11, r0						/* save count */
	mov		r12, r2						/* save address offset */
	subs	r1, #1						/* create page size mask */
	lsls	r3, r3, #30					/* flags into bit 30 and 31 */
	orrs	r1, r1, r3					/* copy flags in page size mask */
	mov		r10, r1						/* save mask and flags */
	ldr		r0, port_pin_sclk			/* load SCLK port address */
	ldr		r1, port_pin_sclk+4			/* load SCLK pin bitmask */
	bl		deselect					/* for a clean start */
	.endm

/* send cmd and following 2-, 3- or 4-byte address to flash */
	.macro	send_cmd_addr
	bl		shift_out_byte				/* send cmd */
	mov		r7, r10						/* get 3/4-byte flags */
	lsls	r7, r7, #1					/* test for 4-byte address */
	bcc		addr_0_23					/* skip if 3-byte address */
	mov		r4, r12						/* get address offset */
	lsrs	r4, r4, #24					/* addr bits 31-24 */
	bl		shift_out_next				/* send addr byte */
addr_0_23:
	mov		r7, r10						/* get 3/4-byte flags */
	lsls	r7, r7, #2					/* test for 3-byte address */
	bcc		addr_0_15					/* skip if 2-byte address */
	mov		r4, r12						/* get address offset */
	lsrs	r4, r4, #16					/* addr bits 23-16 */
	bl		shift_out_next				/* send addr byte */
addr_0_15:
	mov		r4, r12						/* get address offset */
	lsrs	r4, r4, #8					/* addr bits 15-8 */
	bl		shift_out_next				/* send addr byte */
	mov		r4, r12						/* get address offset */
	lsrs	r4, r4, #0					/* addr bits 7-0 */
	bl		shift_out_next				/* send addr byte */
	.endm

/* shift in one byte from MISO pin
 * r4: data byte (out)
 * r5: bit no of MISO pin (lowest byte)
 * clobbered: r2, r3, r6, r7 */
	.macro	shift_in_byte
shift_in_byte:
	movs	r4, #1						/* set bit 0 only */
	lsls	r4, r4, #24					/* into bit 24 */
	movs	r7, #0						/* clear temp result */
	ldr		r6, [r0]					/* load SCLK port data */
shift_in_loop:
	orrs	r4, r4, r7					/* insert new bit into result */
	orrs	r6, r6, r1					/* set SCLK bit */
	str		r6, [r0]					/* store new SCLK port data */
	ldr		r7, [r2]					/* load MISO port data */
	ands	r7, r7, r3					/* mask all but MISO bit */
	rors	r7, r7, r5					/* shift new bit into bit 0 */
	bics	r6, r6, r1					/* clear SCLK bit */
	str		r6, [r0]					/* store new SCLK port data */
	lsls	r4, r4, #1					/* shift result left one */
	bcc		shift_in_loop				/* again if not finished */
	orrs	r4, r4, r7					/* insert last bit into result */
	bx		lr							/* return */
	.endm

/* tail of shift_out_byte, MOSI port address parametrized */
	.macro	shift_tail	mosi
	lsls	r4, r4, #1					/* original bit 7 into C */
shift_out_loop_\@:
	bics	r6, r6, r1					/* clear SCLK bit */
	sbcs	r5, r5, r5					/* fill all bits with ~C */
	ands	r5, r5, r3					/* set/clear bit at MOSI position */
	str		r6, [r0]					/* store new SCLK port data */
	orrs	\mosi, \mosi, r3			/* set MOSI bit */
	bics	\mosi, \mosi, r5			/* insert new MOSI bit */
	str		\mosi, [r2]					/* store new MOSI port data */
	orrs	r6, r6, r1					/* set SCLK bit */
	lsls	r4, r4, #1					/* shift next bit into C */
	str		r6, [r0]					/* store new SCLK port data */
	bne		shift_out_loop_\@			/* again if not finished */
	str		r6, [r0]					/* store new SCLK port data */
	bics	r6, r6, r1					/* clear SCLK bit */
	str		r6, [r0]					/* store new SCLK port data */
	bx		lr							/* return */
	.endm

/* clear NCS, shift lowest byte of word out via MOSI
 * depending on whether MOSI and SCLK are located on same port,
 * algorithm is slightly different
 *
 * r4: data word (in)
 * clobbered: r2, r3, r5, r6, r7 */
	.macro	shift_out_byte
shift_out_byte:
	ldr		r2, port_pin_ncs			/* NCS port address */
	ldr		r3, port_pin_ncs+4			/* NCS pin bitmask */
	ldr		r7, [r2]					/* load port data */
	bics	r7, r7, r3					/* clear NCS bit */
	str		r7, [r2]					/* store new contents */
	ldr		r2, port_pin_mosi			/* MOSI port address */
	ldr		r3, port_pin_mosi+4			/* MOSI pin bitmask */
shift_out_next:
	lsls	r4, r4, #1					/* insert '1' bit just */
	adds	r4, r4, #1					/* right of data byte */
	lsls	r4, r4, #23					/* original bit 7 into bit 31 */
	ldr		r6, [r0]					/* load SCLK port data */
	bics	r6, r6, r1					/* clear SCLK bit */
	cmp		r0, r2						/* SCLK and MOSI on same port? */
	beq		shift_out_eq
shift_out_ne:
	ldr		r7, [r2]					/* load MOSI port data */
	shift_tail	mosi=r7					/* SCLK, MOSI on different ports */
shift_out_eq:
	shift_tail	mosi=r6					/* SCLk, MOSI on same port */
	.endm

/* parameter and buffer allocation */
	.macro	param_block
port_pin_ncs:
	.space	8							/* port address (output reg) and mask for NCS pin */
port_pin_sclk:
	.space	8							/* port address (output reg) and mask for SCLK pin */
port_pin_miso:
	.space	8							/* port address (input reg) and mask for MISO pin  */
port_pin_mosi:
	.space	8							/* port address (output reg) and mask for MOSI pin */
bits_no:
	.space	4							/* bit numbers of NCS, SCLK, MISO, MOSI pins */
	.endm
