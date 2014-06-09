/*
 * show_progress.c - simple progress bar functions
 *
 * Copyright (c) 2010 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix
 *
 * Copyright (c) 2014 Franck Jullien <franck.jullien@gmail.com>
 *    - Made it work with OpenOCD
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/log.h>

#define HASHES_PER_LINE	65

static int printed;
static int progress_max;
static int now;
static int spin;

int show_progress(int val)
{
	long long tmp;
	char spinchr[] = "\\|/-";

	if (!val) {
		printf("%c\b", spinchr[spin++ % (sizeof(spinchr) - 1)]);
		return ERROR_OK;
	}

	now += val;

	if (progress_max) {
		tmp = (long long)now * HASHES_PER_LINE;
		tmp = tmp / progress_max;

		if (progress_max < now)
			tmp = HASHES_PER_LINE;
	} else {
		tmp = printed + 1;
	}

	while (printed < tmp) {
		if (!(printed % HASHES_PER_LINE) && printed)
			LOG_USER_N("\n\t");
		LOG_USER_N("#");
		printed++;
	}

	return ERROR_OK;
}

void init_progression_bar(int max)
{
	printed = 0;
	progress_max = max;
	now = 0;

	if (progress_max) {
		LOG_USER_N("\t[%"stringify(HASHES_PER_LINE)"s]\r\t[", "");
	} else {
		LOG_USER_N("\t");
	}
}

void stop_progression_bar(void)
{
	if (progress_max) {
		/* If progress val doesn't end exactly where it should, finish the job */
		while (printed < HASHES_PER_LINE) {
			if (!(printed % HASHES_PER_LINE) && printed)
				LOG_USER_N("\n\t");
			LOG_USER_N("#");
			printed++;
		}
	}

	LOG_USER_N("\n");
}
