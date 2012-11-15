/*
 * Copyright (C) 2012 Tomasz Boleslaw CEDRO
 * cederom@tlen.pl, http://www.tomek.cedro.info
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef FEATURE_H
#define FEATURE_H

#define FEATURE_NAME_MAXLEN 50
#define FEATURE_DESCRIPTION_MAXLEN 80
#define FEATURE_ARM_DAP "feature_arm_dap"

struct feature {
	/** Feature name. */
	char name[FEATURE_NAME_MAXLEN];

	/** Description is a one line feature summary. */
	char description[FEATURE_DESCRIPTION_MAXLEN];

	/** Body points to feature contents. */
	void *body;

	/** Next points to next feature on the list. */
	struct feature *next;

};

int feature_add(struct feature *features, struct feature *feature);
int feature_del(struct feature *features, char *name);
int feature_update(struct feature *features, struct feature *feature);
struct feature *feature_find(struct feature *features, char *name);

#endif
