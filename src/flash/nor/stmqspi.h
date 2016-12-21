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

/* QSPI register offsets */
#define QSPI_CR			(0x00)	/* Control register */
#define QSPI_DCR		(0x04)	/* Device configuration register */
#define QSPI_SR			(0x08)	/* Status register */
#define QSPI_FCR		(0x0C)	/* Flag clear register */
#define QSPI_DLR		(0x10)	/* Data length register */
#define QSPI_CCR		(0x14)	/* Communication configuration register */
#define QSPI_AR			(0x18)	/* Address register */
#define QSPI_DR			(0x20)	/* Data register */

/* bits in QSPI_CR */
#define QSPI_FSEL_FLASH		7	/* Select flash 2 */
#define QSPI_DUAL_FLASH		6	/* Dual flash mode */
#define QSPI_ABORT			1	/* Abort bit */

/* bits in QSPI_DCR */
#define QSPI_FSIZE_POS		16	/* bit position of FSIZE */
#define QSPI_FSIZE_LEN		5	/* width of FSIZE field */

/* bits in QSPI_SR */
#define QSPI_BUSY			5	/* Busy flag */
#define QSPI_FTF			2	/* FIFO threshold flag */
#define QSPI_TCF			1	/* Transfer complete flag */

/* fields in QSPI_CCR */
#define QSPI_DCYC_POS		18					/* bit position of DCYC */
#define QSPI_DCYC_LEN		5					/* width of DCYC field */
#define QSPI_DCYC_MASK		(((1U<<QSPI_DCYC_LEN) - 1)<<QSPI_DCYC_POS)
#define QSPI_ADSIZE_POS		12					/* bit position of ADSIZE */

#define QSPI_WRITE_MODE		0x00000000U			/* indirect write mode */
#define QSPI_READ_MODE		0x04000000U			/* indirect read mode */
#define QSPI_MM_MODE		0x0C000000U			/* memory mapped mode */
#define QSPI_ALTB_MODE		0x0003C000U			/* alternate byte mode */
#define QSPI_4LINE_MODE		0x03000F00U			/* 4 lines for address, data */
#define QSPI_NO_DATA		(~0x03000000U)		/* no data */
#define QSPI_NO_ALTB		(~QSPI_ALTB_MODE)	/* no alternate */
#define QSPI_NO_ADDR		(~0x00000C00U)		/* no address */
#define QSPI_ADDR4			(0x3U<<QSPI_ADSIZE_POS)	/* 4 byte address */

/* OCTOSPI register offsets */
#define OCTOSPI_CR		(0x000)	/* Control register */
#define OCTOSPI_DCR1	(0x008)	/* Device configuration register 1 */
#define OCTOSPI_DCR2	(0x00C)	/* Device configuration register 2 */
#define OCTOSPI_DCR3	(0x010)	/* Device configuration register 3 */
#define	OCTOSPI_SR		(0x020)	/* Status register */
#define OCTOSPI_FCR		(0x024)	/* Flag clear register */
#define OCTOSPI_DLR		(0x040)	/* Data length register */
#define OCTOSPI_AR		(0x048)	/* Address register */
#define OCTOSPI_DR		(0x050)	/* Data register */
#define OCTOSPI_CCR		(0x100)	/* Communication configuration register */
#define OCTOSPI_IR		(0x110)	/* Instruction register */
#define OCTOSPI_WCCR	(0x180)	/* Write communication configuration register */
#define OCTOSPI_WIR		(0x190)	/* Write instruction register */
#define OCTOSPI_MAGIC	(0x3FC)	/* Magic ID register */

#define OCTO_MAGIC_ID	0xA3C5DD01

#endif /* OPENOCD_FLASH_NOR_STMQSPI_H */
