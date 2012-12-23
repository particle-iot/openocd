/***************************************************************************
 *   Copyright (C) 2013 by Franck Jullien                                  *
 *   elec4fun@gmail.com                                                    *
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "log.h"
#include "register.h"
#include "target.h"
#include "target_type.h"
#include "fileio.h"

/* Get the target registers list and write a tdesc feature section with
 * all registers matching feature_name. If feature_name is NULL or empty,
 * create a "nogroup" feature with all registers without feature definition.
 */
int generate_feature_section(struct target *target, struct fileio *fileio,
			     const char *arch_name, const char *feature_name)
{
	struct reg **reg_list;
	int reg_list_size;
	bool nogroup = false;

	int retval = target_get_gdb_reg_list(target, &reg_list, &reg_list_size);
	if (retval != ERROR_OK)
		return retval;

	/* If the feature name passed to the function is NULL or empty
	 * it means we want to create a "nogroup" feature section.
	 */
	if ((feature_name != NULL && !strcmp(feature_name, "")) || feature_name == NULL)
		nogroup = true;

	retval = fileio_fprintf(fileio, "  <feature name=\"org.gnu.gdb.%s.%s\">\n",
				arch_name, nogroup ? "nogroup" : feature_name);
	if (retval != ERROR_OK)
		goto out;

	for (int i = 0; i < reg_list_size; i++) {

		bool add_reg_to_group = false;

		if (nogroup) {
			if ((reg_list[i]->feature != NULL && !strcmp(reg_list[i]->feature, ""))
			     || reg_list[i]->feature == NULL) {
				add_reg_to_group = true;
			}
		} else {
			if (reg_list[i]->feature != NULL && strcmp(reg_list[i]->feature, "")) {
				if (!strcmp(reg_list[i]->feature, feature_name))
					add_reg_to_group = true;
			}
		}

		if (add_reg_to_group) {
			retval = fileio_fprintf(fileio, "    <reg name=\"%s\"   "
				       "        bitsize=\"%d\" regnum=\"%d\"",
				       reg_list[i]->name, reg_list[i]->size, i);
			if (retval != ERROR_OK)
				goto out;

			if (reg_list[i]->group != NULL && strcmp(reg_list[i]->group, "")) {
				retval = fileio_fprintf(fileio, " group=\"%s\"", reg_list[i]->group);
				if (retval != ERROR_OK)
					goto out;
			}

			retval = fileio_fprintf(fileio, "/>\n");
			if (retval != ERROR_OK)
				goto out;
		}
	}

	retval = fileio_fprintf(fileio, "  </feature>\n");

out:
	free(reg_list);

	return retval;
}

/* Get a list of available target registers features. feature_list must
 * be freed by caller.
 */
int get_reg_features_list(struct target *target, char **feature_list[])
{
	struct reg **reg_list;
	int reg_list_size;
	int tbl_sz = 0;

	int retval = target_get_gdb_reg_list(target, &reg_list, &reg_list_size);
	if (retval != ERROR_OK) {
		*feature_list = NULL;
		return retval;
	}

	/* Start with only one element */
	*feature_list = calloc(1, sizeof(char *));

	for (int i = 0; i < reg_list_size; i++) {
		if (reg_list[i]->feature != NULL && strcmp(reg_list[i]->feature, "")) {
			/* We found a feature, check if the feature is already in the
			 * table. If not, allocate a new entry for the table and
			 * put the new feature in it.
			 */
			for (int j = 0; j < (tbl_sz + 1); j++) {
					if (!((*feature_list)[j])) {
						(*feature_list)[tbl_sz++] = strdup(reg_list[i]->feature);
						*feature_list = realloc(*feature_list, sizeof(char *) * (tbl_sz + 1));
						(*feature_list)[tbl_sz] = NULL;
						break;
					} else {
						if (!strcmp((*feature_list)[j], reg_list[i]->feature))
							break;
					}
			}
		}
	}

	free(reg_list);

	return tbl_sz;
}

/* Returns how many registers don't have a feature specified */
int count_reg_without_group(struct target *target)
{
	struct reg **reg_list;
	int reg_list_size;
	int reg_without_group = 0;

	int retval = target_get_gdb_reg_list(target, &reg_list, &reg_list_size);
	if (retval != ERROR_OK)
		return retval;

	for (int i = 0; i < reg_list_size; i++) {
			if ((reg_list[i]->feature != NULL &&
			     !strcmp(reg_list[i]->feature, "")) ||
			     reg_list[i]->feature == NULL) {
				reg_without_group++;
			}
	}

	free(reg_list);

	return reg_without_group;
}

/* Open a file for write, set the header of the file according to the
 * gdb target description format and configure the architecture element with
 * the given arch_name.
 */
int open_and_init_tdesc_file(struct fileio *fileio, const char *filename,
			     const char *arch_name)
{
	int retval = fileio_open(fileio, filename, FILEIO_WRITE, FILEIO_TEXT);
	if (retval != ERROR_OK)
		return retval;

	retval = fileio_fprintf(fileio, "<?xml version=\"1.0\"?>\n"
					"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
					"<target>\n"
					"  <architecture>%s</architecture>\n\n", arch_name);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

/* Close a target descriptor file */
int close_tdesc_file(struct fileio *fileio)
{
	int retval = fileio_fprintf(fileio, "</target>\n");
	if (retval != ERROR_OK)
		return retval;

	fileio_close(fileio);

	return ERROR_OK;
}
