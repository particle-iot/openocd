/***************************************************************************
 *   Copyright (C) 2016 by Andreas Bolsch                                  *
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

#ifndef OPENOCD_FLASH_NOR_STMQSPI_H
#define OPENOCD_FLASH_NOR_STMQSPI_H

#include "spi.h"

/* register offsets */
#define QSPI_CR			(0x00)	/* Control register */
#define QSPI_DCR		(0x04)	/* Configuration register */
#define QSPI_SR			(0x08)	/* Status register */
#define QSPI_FCR		(0x0C)	/* Flag clear register */
#define QSPI_DLR		(0x10)	/* Data length register */
#define QSPI_CCR		(0x14)	/* Communication configuration register */
#define QSPI_AR			(0x18)	/* Address register */
#define QSPI_DR			(0x20)	/* Data register */
#define QSPI_PSMKR		(0x24)	/* Polling status mask register */

/* bits in QSPI_CR */
#define QSPI_FSEL_FLASH		7	/* Select flash 2 */
#define QSPI_DUAL_FLASH		6	/* Dual flash mode */
#define QSPI_ABORT			1	/* Abort bit */

/* bits in QSPI_DCR */
#define QSPI_FSIZE0			16	/* bottom of FSIZE field */
#define QSPI_FSIZE_LEN		5	/* width of FSIZE field */

/* bits in QSPI_SR */
#define QSPI_BUSY			5	/* Busy flag */
#define QSPI_FTF			2	/* FIFO threshold flag */
#define QSPI_TCF			1	/* Transfer complete flag */

/* fields in QSPI_CCR */
#define QSPI_WRITE_MODE		0x00000000		/* indirect write mode */
#define QSPI_READ_MODE		0x04000000		/* indirect read mode */
#define QSPI_MM_MODE		0x0C000000		/* memory mapped mode */
#define QSPI_1LINE_MODE		0x01000500		/* 1 line for address, data */
#define QSPI_2LINE_MODE		0x02000A00		/* 2 lines for address, data */
#define QSPI_4LINE_MODE		0x03000F00		/* 4 lines for address, data */
#define QSPI_NO_DATA		(~0x03000000)	/* no data */
#define QSPI_NO_ADDR		(~0x00000C00)	/* no address */
#define QSPI_ADDR3			0x00002000		/* 3 byte address */
#define QSPI_ADDR4			0x00003000		/* 4 byte address */

#define QSPI_DCYC_POS		18				/* bit position of DCYC */
#define QSPI_DCYC_LEN		5				/* length of DCYC field */
#define QSPI_DCYC_MASK		(((1<<QSPI_DCYC_LEN) - 1)<<QSPI_DCYC_POS)
#define QSPI_DCYCLES		0				/* for single data line mode */

#endif /* OPENOCD_FLASH_NOR_STMQSPI_H */
