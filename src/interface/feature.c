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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <interface/feature.h>
#include <helper/log.h>

/**
 @file oocd_feature_core Interface specific features core definitions.
 To keep Interface API small, compact and readable, but versatile
 and extensible at the same time 'Interface Features' are introduced.
 Interface Features is a dynamic list of user definable driver extensions
 that can provide additional functionalities to the interface driver.
 Intelligent drivers can expose their hardware specific operations this way,
 while generic drivers can get additional applications with use of external
 libraries that will generate additional features (ie. LibSWD for SWD support).
 Interface Features API was invented and introduced in 2012 by Tomasz Boleslaw
 CEDRO (http://www.tomek.cedro.info) as a part of generic SWD implementation
 for OpenOCD with use of LibSWD (http://libswd.sf.net).
 */

/** Add feature new_feature to existing list of features. */
int oocd_feature_add(oocd_feature_t *features, oocd_feature_t *new_feature){
	oocd_feature_t *last_feature;

	if (features==NULL){
		LOG_ERROR("Invalid features list selected (no features)!");
		return ERROR_FAIL;
	}

	if (new_feature==NULL || new_feature->name==NULL || new_feature->body==NULL){
		LOG_ERROR("Cannot add this invalid feature!");
		return ERROR_FAIL;
	}

	if (oocd_feature_find(features, new_feature->name) != NULL){
		LOG_ERROR("Feature %s already on the list!", new_feature->name);
		return ERROR_FAIL;
	}

	for (last_feature=features; last_feature->next!=NULL; last_feature=last_feature->next){};
	new_feature->next=NULL;
	last_feature->next=new_feature;

	return ERROR_OK;
}

/** Add new_feature or replace existing feature on the list. */
int oocd_feature_update(oocd_feature_t *features, oocd_feature_t *new_feature){
	oocd_feature_t *feature=features;

	if (features==NULL){
		LOG_ERROR("Invalid features list selected (no features)!");
		return ERROR_FAIL;
	}

	if (new_feature==NULL || new_feature->name==NULL || new_feature->body==NULL){
		LOG_ERROR("Cannot add this invalid feature!");
		return ERROR_FAIL;
	}

	if (oocd_feature_find(features, new_feature->name) == NULL){
		for (feature=features; feature->next!=NULL; feature=feature->next){};
		new_feature->next=NULL;
		feature->next=new_feature;
		return ERROR_OK;
	} else {
		feature=features;
		if (feature->next == NULL){
			if (strncmp(new_feature->name, feature->name, OOCD_FEATURE_NAME_MAXLEN)==0){
				feature = new_feature;
				return ERROR_OK;
			}
		}
		while (feature != NULL){
			if (feature->next == NULL) break;
			if (strncmp(new_feature->name, feature->next->name, OOCD_FEATURE_NAME_MAXLEN)==0){ 
				if (feature->next->next == NULL){
					feature->next = new_feature;
				} else {
					new_feature->next = feature->next->next;
					feature->next = new_feature;
				}
				return ERROR_OK;
			}
			feature = feature->next;
		}
	}
	return ERROR_FAIL;
}

/** Remove feature by given name from a features list. */
int oocd_feature_del(oocd_feature_t *features, char *name){
	oocd_feature_t *feature=features;

	if (features==NULL){
		LOG_ERROR("Invalid features list selected (no features)!");
		return ERROR_FAIL;
	}

	if (feature->next == NULL){
		if (strncmp(name, feature->name, OOCD_FEATURE_NAME_MAXLEN)==0){
			features = NULL;
			LOG_DEBUG("Removed feature: %s", name);
			return ERROR_OK;
		}
	}

	while (feature != NULL){
		if (feature->next == NULL) break;
		if (strncmp(name, feature->next->name, OOCD_FEATURE_NAME_MAXLEN)==0){ 
			if (feature->next->next == NULL){
				feature->next = NULL;
			} else feature->next = feature->next->next;
			LOG_DEBUG("Removed feature: %s", name);
			return ERROR_OK;
		}
		feature = feature->next;
	}
	return ERROR_OK;
}

/** Find feature by a name from list of features. */
oocd_feature_t *oocd_feature_find(oocd_feature_t *features, char *name){
	oocd_feature_t *feature=features;

     if (features==NULL){
          LOG_ERROR("Invalid features list selected (no features)!");
          return NULL;
     }

	while (feature != NULL){
          if (strncmp(name, feature->name, OOCD_FEATURE_NAME_MAXLEN)==0) return feature;
		feature = feature->next;	
	}
	return NULL;
}

