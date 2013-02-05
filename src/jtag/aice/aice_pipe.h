/***************************************************************************
 *   Copyright (C) 2012 by Hsiangkai Wang                                  *
 *   Hsiangkai Wang <hkwang@andestech.com>                                 *
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
#ifndef _AICE_PIPE_H_
#define _AICE_PIPE_H_

static inline void set_u32(void *_buffer, uint32_t value)
{
	uint8_t *buffer = (uint8_t *)_buffer;

	buffer[3] = (value >> 24) & 0xff;
	buffer[2] = (value >> 16) & 0xff;
	buffer[1] = (value >> 8) & 0xff;
	buffer[0] = (value >> 0) & 0xff;
}

static inline uint32_t get_u32(const void *_buffer)
{
	uint8_t *buffer = (uint8_t *)_buffer;
	uint32_t data = (((uint32_t)buffer[3]) << 24) |
		(((uint32_t)buffer[2]) << 16) |
		(((uint32_t)buffer[1]) << 8) |
		(((uint32_t)buffer[0]) << 0);

	return data;
}

static inline void set_u16(void *_buffer, uint16_t value)
{
	uint8_t *buffer = (uint8_t *)_buffer;

	buffer[1] = (value >> 8) & 0xff;
	buffer[0] = (value >> 0) & 0xff;
}

static inline uint16_t get_u16(const void *_buffer)
{
	uint8_t *buffer = (uint8_t *)_buffer;
	uint16_t data = (((uint16_t)buffer[1]) << 8) |
		(((uint16_t)buffer[0]) << 0);

	return data;
}

extern struct aice_port_api_s aice_pipe;

#endif
