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

union value_target {
	void *data;
	uint8_t *data_uint8;
	int8_t *data_int8;
	uint32_t *data_uint32;
	int32_t *data_int32;
};

static char *string_trim(const char *str, size_t in_length,
		size_t *out_length) {
	size_t pos = 0;
	size_t last_char = 0;
	size_t result_length = strnlen(str, in_length);
	char *result = calloc(result_length, 1);
	bool started = false;

	for (size_t i = 0; i < result_length; i++) {
		if (!started) {
			if (str[i] != ' ') {
				started = true;
				result[pos] = str[i];
				last_char = pos;
				pos++;
			}
		} else {
			result[pos] = str[i];
			pos++;

			if (str[i] != ' ') {
				last_char = pos;
			}
		}
	}

	result[last_char] = '\0';

	if (out_length) {
		*out_length = last_char;
	}

	return result;
}

static int read_value(Jim_Interp *interp, Jim_Obj *value,
		enum serdes_type type, void *storage)
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

	switch (type) {
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

			upper_limit = number_limits[type].upper;
			lower_limit = number_limits[type].lower;

			if (wide > upper_limit || wide < lower_limit) {
				LOG_ERROR("serdes: value %" JIM_WIDE_MODIFIER " is out of range", wide);
				return ERROR_FAIL;
			}

			union value_target target = {
				.data = storage
			};

			switch (type) {
			case serdes_uint8:
				*target.data_uint8 = wide;
				break;
			case serdes_int8:
				*target.data_int8 = wide;
				break;
			case serdes_uint32:
				*target.data_uint32 = wide;
				break;
			case serdes_int32:
				*target.data_int32 = wide;
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

static int print_value(const struct serdes_field *field, void *storage)
{
	union value_target target = {
		.data = storage
	};

	fprintf(stderr, "\t.%s = ", field->name);

	switch (field->type) {
	case serdes_int8:
		fprintf(stderr, "%" PRId8 ",", *target.data_int8);
		break;
	case serdes_uint8:
		fprintf(stderr, "%" PRIu8 ",", *target.data_uint8);
		break;
	case serdes_int32:
		fprintf(stderr, "%" PRId32 ",", *target.data_int32);
		break;
	case serdes_uint32:
		fprintf(stderr, "%" PRIu32 ",", *target.data_uint32);
		break;
	case serdes_string:
		fprintf(stderr, "%s", *(const char **) target.data);
		break;
	default:
		return ERROR_FAIL;
	}

	fprintf(stderr, "\n");

	return ERROR_OK;
}

int serdes_read_struct(Jim_Interp *interp, Jim_Obj **objects,
		const struct serdes_field *fields, const struct serdes_wordset *wordset,
		void *storage)
{
	int result = ERROR_FAIL;

	if (fields == NULL || objects == NULL) {
		goto error;
	}

	const struct serdes_field *field;
	Jim_Obj **object = objects;
	char *key_trimmed = NULL;

	field = fields;

	for (; field->offset != -1; field++) {
		int len = 0;
		size_t key_trimmed_length = 0;
		const char *key_string = Jim_GetString(*object, &len);

		key_trimmed = string_trim(key_string, len, &key_trimmed_length);
		void *key = wordset->check(key_trimmed, key_trimmed_length);

		if (!key) {
			LOG_ERROR("Unknown struct field: %s", key_trimmed);
			goto error;
		}

		object++;

		size_t index = wordset->value(key);
		void *target = (void *) ((uintptr_t) storage +
				(uintptr_t) fields[index].offset);

		if (read_value(interp, *object,
				fields[index].type, target) != ERROR_OK) {
			goto error;
		}

		object++;
	}

	result = ERROR_OK;
error:
	if (key_trimmed) {
		free(key_trimmed);
	}

	return result;
}

int serdes_print_struct(const struct serdes_field *fields, void *storage)
{
	int result = ERROR_FAIL;

	if (fields == NULL) {
		goto error;
	}

	const struct serdes_field *field = fields;

	fprintf(stderr, "{\n");

	for (; field->offset != -1; field++) {
		void *target = (void *) ((uintptr_t) storage + (uintptr_t) field->offset);
		print_value(field, target);
	}

	fprintf(stderr, "}\n");

	result = ERROR_OK;
error:
	return result;
}
