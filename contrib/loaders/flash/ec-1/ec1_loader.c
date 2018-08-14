/***************************************************************************
 *
 *   Copyright (C) 2018 by Konstantin Kraskovskiy <kraskovski@otsl.jp>                *
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

/*
 To compile: arm-none-eabi-gcc -march=armv7-r -marm -c ./ec1_loader.c
 To strip: arm-none-eabi-objcopy -O binary ec1_loader.o ec1.bin
 To hex: xxd -c4 -i ./ec1.h
 */

#define BASE 0xA0005000
#define SMCR (BASE + 0x20)
#define SMCMR (BASE + 0x24)
#define SMADR (BASE + 0x28)
#define SMENR (BASE + 0x30)
#define SMRDR0 (BASE + 0x38)
#define SMWDR0 (BASE + 0x40)
#define CMNSR (BASE + 0x48)

#define CDE (1 << 14)
#define SPIE (1)
#define SPIWE (2)
#define SPIRE (4)
#define SSLKP (1 << 8)
#define TEND (1)
#define ADE_24 (7 << 8)
#define BUSY (1)
#define SPIDE_BYTE (8)
#define SPIDE_HALF (12)
#define SPIDE_WORD (15)

#define SPIFLASH_PAGE_PROGRAM (2 << 16)
#define SPIFLASH_READ_STATUS (5 << 16)
#define SPIFLASH_WRITE_ENABLE (6 << 16)

#define PAGE_MASK (0xFF)


__attribute__((noreturn, naked, pcs("aapcs"), aligned(4)))
unsigned ec1_write(
			unsigned r0_count,
			unsigned r1_dummy,
			unsigned r2_offset,
			unsigned r3_p_buffer)
{
	register unsigned v_smenr = CDE | ADE_24;

	/* write enable */
	*(unsigned *)SMENR = CDE;
	*(unsigned *)SMCMR = SPIFLASH_WRITE_ENABLE;
	*(unsigned *)SMCR = SPIE;
	while (!(*((volatile unsigned *)CMNSR) & TEND));

	/* writing */
	*(unsigned *)SMCMR = SPIFLASH_PAGE_PROGRAM;
	*(unsigned *)SMADR = r2_offset;

	while (r0_count) {
		switch (PAGE_MASK - (r2_offset & PAGE_MASK)) {
			case 0:
				*(unsigned *)SMENR = v_smenr | SPIDE_BYTE;
				*(unsigned char *)SMWDR0 = *(unsigned char *)r3_p_buffer;
				*(unsigned *)SMCR = SPIWE | SPIE;
				r0_count--;
				r3_p_buffer++;
				r2_offset++;
				break;
			case 1:
			case 2:
				switch (r0_count) {
					case 1:
						*(unsigned *)SMENR = v_smenr | SPIDE_BYTE;
						*(unsigned char *)SMWDR0 = *(unsigned char *)r3_p_buffer;
						*(unsigned *)SMCR = SPIWE | SPIE;
						r0_count--;
						r3_p_buffer++;
						r2_offset++;
						break;
					default:
						*(unsigned *)SMENR = v_smenr | SPIDE_HALF;
						*(unsigned short *)SMWDR0 = *(unsigned short *)r3_p_buffer;
						if (r0_count == 2)
							*(unsigned *)SMCR = SPIWE | SPIE;
						else
							*(unsigned *)SMCR = SSLKP | SPIWE | SPIE;
						r0_count -= 2;
						r3_p_buffer += 2;
						r2_offset += 2;
				}
				break;
			default:
				switch (r0_count) {
					case 1:
						*(unsigned *)SMENR = v_smenr | SPIDE_BYTE;
						*(unsigned char *)SMWDR0 = *(unsigned char *)r3_p_buffer;
						*(unsigned *)SMCR = SPIWE | SPIE;
						r0_count--;
						r3_p_buffer++;
						r2_offset++;
						break;
					case 2:
					case 3:
						*(unsigned *)SMENR = v_smenr | SPIDE_HALF;
						*(unsigned short *)SMWDR0 = *(unsigned short *)r3_p_buffer;
						if (r0_count == 2)
							*(unsigned *)SMCR = SPIWE | SPIE;
						else
							*(unsigned *)SMCR = SSLKP | SPIWE | SPIE;
						r0_count -= 2;
						r3_p_buffer += 2;
						r2_offset += 2;
						break;
					default:
						*(unsigned *)SMENR = v_smenr | SPIDE_WORD;
						*(unsigned *)SMWDR0 = *(unsigned *)r3_p_buffer;
						if (r0_count == 4)
							*(unsigned *)SMCR = SPIWE | SPIE;
						else
							*(unsigned *)SMCR = SSLKP | SPIWE | SPIE;
						r0_count -= 4;
						r3_p_buffer += 4;
						r2_offset += 4;
				}
		}

		while (!(*((volatile unsigned *)CMNSR) & TEND));
		v_smenr = 0;
		if (!(r2_offset & PAGE_MASK))
			break;
	}

	/* wait for page commit */
	*(unsigned *)SMENR = CDE | SPIDE_BYTE;
	*(unsigned *)SMCMR = SPIFLASH_READ_STATUS;
	*(unsigned *)SMCR = SSLKP | SPIRE | SPIE;
	while (!(*((volatile unsigned *)CMNSR) & TEND));

	*(unsigned *)SMENR = SPIDE_BYTE;
	while (*((volatile unsigned char *)SMRDR0) & BUSY) {
		*(volatile unsigned *)SMCR = SSLKP | SPIRE | SPIE;
		while (!(*((volatile unsigned *)CMNSR) & TEND));
	}

	*(volatile unsigned *)SMCR = SPIRE | SPIE;
	while (!(*((volatile unsigned *)CMNSR) & TEND));

	__asm__ __volatile__ ("bkpt"); /* return */
}
