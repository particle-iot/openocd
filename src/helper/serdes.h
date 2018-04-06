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
	const char * name;
};

typedef void * (* serdes_check_fn_t)(const char *text, size_t len);
typedef int (* serdes_value_fn_t)(void *result);

struct serdes_wordset {
	serdes_check_fn_t check;
	serdes_value_fn_t value;
};

#define SERDES_FIELD(type, base_type, name) \
	{ (type), offsetof(base_type, name), (#name) }

int serdes_read_struct(Jim_Interp *interp, Jim_Obj **objects,
		const struct serdes_field *fields, const struct serdes_wordset *wordset,
		void *storage);

int serdes_print_struct(const struct serdes_field *fields, void *storage);

#endif /* OPENOCD_HELPER_SERDES_H */
