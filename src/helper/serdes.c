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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jim.h>

#include "log.h"
#include "serdes.h"

static int read_value(Jim_Interp *interp, Jim_Obj *value,
		const struct serdes_field *field, void *storage)
{
	static struct {
		jim_wide lower;
		jim_wide upper;
	} number_limits[] = {
		[serdes_uint8]  = { 0, UINT8_MAX },
		[serdes_int8]   = { INT8_MIN, INT8_MAX },
		[serdes_uint32] = { 0, UINT32_MAX },
		[serdes_int32]  = { INT32_MIN, INT32_MAX },
	};

	switch (field->type) {
		case serdes_uint8:
		case serdes_int8:
		case serdes_uint32:
		case serdes_int32: {
			jim_wide upper_limit;
			jim_wide lower_limit;
			jim_wide wide;

			if (Jim_GetWide(interp, value, &wide) != JIM_OK) {
				return ERROR_FAIL;
			}

			upper_limit = number_limits[field->type].upper;
			lower_limit = number_limits[field->type].lower;

			if (wide > upper_limit || wide < lower_limit) {
				LOG_ERROR("serdes: value %" JIM_WIDE_MODIFIER " is out of range", wide);
				return ERROR_FAIL;
			}

			union {
				void * data;
				uint8_t * data_uint8;
				int8_t * data_int8;
				uint32_t * data_uint32;
				int32_t * data_int32;
			} target = {
				.data = storage
			};

			switch (field->type) {
			case serdes_uint8:
				* target.data_uint8 = wide;
				break;
			case serdes_int8:
				* target.data_int8 = wide;
				break;
			case serdes_uint32:
				* target.data_uint32 = wide;
				break;
			case serdes_int32:
				* target.data_int32 = wide;
				break;
			default:
				return ERROR_FAIL;
			}

			break;
		}
		case serdes_string: {
			const char *string;
			int length;

			length = 0;
			string = Jim_GetString(value, &length);

			if (length == 0) {
				LOG_ERROR("serdes: unable to read string");
				return ERROR_FAIL;
			}

			const char **target = (const char **) storage;
			*target = strndup(string, length);

			break;
		}
	}

	return ERROR_OK;
}

int serdes_read_struct(Jim_Interp *interp, Jim_Obj **objects,
		const struct serdes_field *fields, void *storage)
{
	int result = ERROR_FAIL;

	if (fields == NULL || objects == NULL) {
		goto error;
	}

	const struct serdes_field *field;

	// Skip first key
	objects += 1;
	field = fields;

	for (; field->offset != -1; field++, objects += 2) {
		Jim_Obj *object = objects[0];
		void * target = (void *) ((uintptr_t) storage + (uintptr_t) field->offset);

		if (read_value(interp, object, field, target) != ERROR_OK) {
			goto error;
		}
	}

	result = ERROR_OK;
error:
	return result;
}
