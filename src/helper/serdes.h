/***************************************************************************
 *   Copyright (C) 2018 by Vlad Ivanov                                     *
 *   vlad@ivanov.email                                                     *
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

#ifndef OPENOCD_HELPER_SERDES_H
#define OPENOCD_HELPER_SERDES_H

#include <stdlib.h>
#include <stddef.h>

enum serdes_type {
	serdes_int8,
	serdes_uint8,
	serdes_int32,
	serdes_uint32,
	serdes_string,
};

struct serdes_field {
	enum serdes_type type;
	off_t offset;
};

int serdes_read_struct(Jim_Interp *interp, Jim_Obj **objects,
		const struct serdes_field *fields, void *storage);

#endif /* OPENOCD_HELPER_SERDES_H */
