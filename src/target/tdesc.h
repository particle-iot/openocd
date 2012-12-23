/***************************************************************************
 *   Copyright (C) 2012 by Franck Jullien                                  *
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

#ifndef TDESC_H
#define TDESC_H

#include "register.h"
#include "target.h"
#include "target_type.h"
#include "fileio.h"

int generate_feature_section(struct target *target, struct fileio *fileio,
			     const char *arch_name, const char *feature_name)

int get_reg_features_list(struct target *target, char **feature_list[]);

int count_reg_without_group(struct target *target);

int open_and_init_tdesc_file(struct fileio *fileio, const char *filename,
			     const char *arch_name);

int close_tdesc_file(struct fileio *fileio);

#endif
