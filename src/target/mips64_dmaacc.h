/***************************************************************************
 *   Copyright (C) 2008 by John McCarthy                                   *
 *   jgmcc@magma.ca                                                        *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by David T.L. Wong                                 *
 *                                                                         *
 *   Copyright (C) 2013 by FengGao                                         *
 *   gf91597@gmail.com                                                     *
 *                                                                         *
 *   Copyright (C) 2013 by Jia Liu                                         *
 *   proljc@gmail.com                                                      *
 *                                                                         *
 *   Copyright (C) 2013 by Chunning Ha                                     *
 *   dev@hcn.name                                                          *
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
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef MIPS32_DMAACC_H
#define MIPS32_DMAACC_H

#include "mips_ejtag.h"

#define EJTAG_CTRL_DMA_BYTE			0x00000000
#define EJTAG_CTRL_DMA_HALFWORD		0x00000080
#define EJTAG_CTRL_DMA_WORD			0x00000100
#define EJTAG_CTRL_DMA_TRIPLEBYTE	0x00000180

#define RETRY_ATTEMPTS	0

int mips64_dmaacc_read_mem(struct mips_ejtag *ejtag_info,
		uint64_t addr, int size, int count, void *buf);
int mips64_dmaacc_write_mem(struct mips_ejtag *ejtag_info,
		uint64_t addr, int size, int count, void *buf);

#endif
