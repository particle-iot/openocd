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

#ifndef OOCD_FEATURE_H
#define OOCD_FEATURE_H

#define OOCD_FEATURE_NAME_MAXLEN 50
#define OOCD_FEATURE_DESCRIPTION_MAXLEN 80
#define OOCD_FEATURE_ARM_DAP "oocd_feature_arm_dap"

typedef struct oocd_feature {
	/** Feature name. */
	char name[OOCD_FEATURE_NAME_MAXLEN];

	/** Description is a one line feature summary. */
	char description[OOCD_FEATURE_DESCRIPTION_MAXLEN];

	/** Body points to feature contents. */
	void *body;

	/** Next points to next feature on the list. */
	struct oocd_feature *next;

} oocd_feature_t;

int oocd_feature_add(oocd_feature_t *features, oocd_feature_t *feature);
int oocd_feature_del(oocd_feature_t *features, char *name);
int oocd_feature_update(oocd_feature_t *features, oocd_feature_t *feature);
oocd_feature_t *oocd_feature_find(oocd_feature_t *features, char *name);

#endif
