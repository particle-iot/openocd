/***************************************************************************
 *   Copyright (C) 2011 by Broadcom Corporation                            *
 *   Evan Hunter - ehunter@broadcom.com                                    *
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
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/time_support.h>
#include <jtag/jtag.h>
#include "target/target.h"
#include "target/target_type.h"
#include "rtos.h"
#include "helper/log.h"
#include "helper/types.h"
#include "rtos_standard_stackings.h"

static int ThreadX_detect_rtos(struct target *target);
static int ThreadX_create(struct target *target);
static int ThreadX_update_threads(struct rtos *rtos);
static int ThreadX_get_thread_reg_list(struct rtos *rtos, int64_t thread_id, char **hex_reg_list);
static int ThreadX_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[]);

static const struct rtos_register_stacking *get_stacking_info(struct rtos *rtos, int64_t stack_ptr);
static const struct rtos_register_stacking *get_stacking_info_arm926ejs(struct rtos *rtos, int64_t stack_ptr);

struct ThreadX_thread_state {
	int value;
	const char *desc;
};

static const struct ThreadX_thread_state ThreadX_thread_states[] = {
	{ 0,  "Ready" },
	{ 1,  "Completed" },
	{ 2,  "Terminated" },
	{ 3,  "Suspended" },
	{ 4,  "Sleeping" },
	{ 5,  "Waiting - Queue" },
	{ 6,  "Waiting - Semaphore" },
	{ 7,  "Waiting - Event flag" },
	{ 8,  "Waiting - Memory" },
	{ 9,  "Waiting - Memory" },
	{ 10, "Waiting - I/O" },
	{ 11, "Waiting - Filesystem" },
	{ 12, "Waiting - Network" },
	{ 13, "Waiting - Mutex" },
};

#define THREADX_NUM_STATES (sizeof(ThreadX_thread_states)/sizeof(struct ThreadX_thread_state))

struct ThreadX_params {
	const char *target_name;
	unsigned char pointer_width;
	unsigned char thread_stack_offset;
	unsigned char thread_name_offset;
	unsigned char thread_state_offset;
	unsigned char thread_next_offset;
	const struct rtos_register_stacking *stacking_info;
	const struct rtos_register_stacking *(*get_stacking_info)(struct rtos *rtos, int64_t stack_ptr);
};

static const struct ThreadX_params ThreadX_params_list[] = {
	{
	"cortex_m",				/* target_name */
	4,							/* pointer_width; */
	8,							/* thread_stack_offset; */
	40,							/* thread_name_offset; */
	48,							/* thread_state_offset; */
	136,						/* thread_next_offset */
	&rtos_standard_Cortex_M3_stacking,	/* stacking_info */
	NULL,						/* get_stacking_info */
	},
	{
	"cortex_r4",				/* target_name */
	4,							/* pointer_width; */
	8,							/* thread_stack_offset; */
	40,							/* thread_name_offset; */
	48,							/* thread_state_offset; */
	136,						/* thread_next_offset */
	&rtos_standard_Cortex_R4_stacking,	/* stacking_info */
	NULL,						/* get_stacking_info */
	},
	{
	"arm926ejs",				/* target_name */
	4,							/* pointer_width; */
	8,							/* thread_stack_offset; */
	40,							/* thread_name_offset; */
	48,							/* thread_state_offset; */
	136,						/* thread_next_offset */
	rtos_standard_arm926ejs_stackings,	/* stacking_info */
	get_stacking_info_arm926ejs,	/* get_stacking_info */
	},
};

#define THREADX_NUM_PARAMS ((int)(sizeof(ThreadX_params_list)/sizeof(struct ThreadX_params)))

enum ThreadX_symbol_values {
	ThreadX_VAL_tx_thread_current_ptr = 0,
	ThreadX_VAL_tx_thread_created_ptr = 1,
	ThreadX_VAL_tx_thread_created_count = 2,
};

static const char * const ThreadX_symbol_list[] = {
	"_tx_thread_current_ptr",
	"_tx_thread_created_ptr",
	"_tx_thread_created_count",
	NULL
};

const struct rtos_type ThreadX_rtos = {
	.name = "ThreadX",

	.detect_rtos = ThreadX_detect_rtos,
	.create = ThreadX_create,
	.update_threads = ThreadX_update_threads,
	.get_thread_reg_list = ThreadX_get_thread_reg_list,
	.get_symbol_list_to_lookup = ThreadX_get_symbol_list_to_lookup,
};

static const struct rtos_register_stacking *get_stacking_info(struct rtos *rtos, int64_t stack_ptr)
{
	const struct ThreadX_params *param = rtos->rtos_specific_params;

	if (param->get_stacking_info != NULL)
		return param->get_stacking_info(rtos, stack_ptr);
	else
		return param->stacking_info;
}

static const struct rtos_register_stacking *get_stacking_info_arm926ejs(struct rtos *rtos, int64_t stack_ptr)
{
	const struct ThreadX_params *param = rtos->rtos_specific_params;
	int retval;
	uint32_t flag;

	retval = target_read_buffer(rtos->target, stack_ptr,
								sizeof(flag), (uint8_t *)&flag);
	if (retval != ERROR_OK)
		return NULL;

	if (flag == 0)
		return &param->stacking_info[0];
	else
		return &param->stacking_info[1];
}

static int ThreadX_update_threads(struct rtos *rtos)
{
	int retval;
	int tasks_found = 0;
	int thread_list_size = 0;
	const struct ThreadX_params *param;

	if (rtos == NULL)
		return -1;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct ThreadX_params *) rtos->rtos_specific_params;

	if (rtos->symbols == NULL) {
		LOG_ERROR("No symbols for ThreadX");
		return -4;
	}

	if (rtos->symbols[ThreadX_VAL_tx_thread_created_count].address == 0) {
		LOG_ERROR("Don't have the number of threads in ThreadX");
		return -2;
	}

	/* read the number of threads */
	retval = target_read_buffer(rtos->target,
			rtos->symbols[ThreadX_VAL_tx_thread_created_count].address,
			4,
			(uint8_t *)&thread_list_size);

	if (retval != ERROR_OK) {
		LOG_ERROR("Could not read ThreadX thread count from target");
		return retval;
	}

	/* wipe out previous thread details if any */
	rtos_free_threadlist(rtos);

	/* read the current thread id */
	retval = target_read_buffer(rtos->target,
			rtos->symbols[ThreadX_VAL_tx_thread_current_ptr].address,
			4,
			(uint8_t *)&rtos->current_thread);

	if (retval != ERROR_OK) {
		LOG_ERROR("Could not read ThreadX current thread from target");
		return retval;
	}

	if ((thread_list_size  == 0) || (rtos->current_thread == 0)) {
		/* Either : No RTOS threads - there is always at least the current execution though */
		/* OR     : No current thread - all threads suspended - show the current execution
		 * of idling */
		char tmp_str[] = "Current Execution";
		thread_list_size++;
		tasks_found++;
		rtos->thread_details = malloc(
				sizeof(struct thread_detail) * thread_list_size);
		rtos->thread_details->threadid = 1;
		rtos->thread_details->exists = true;
		rtos->thread_details->display_str = NULL;
		rtos->thread_details->extra_info_str = NULL;
		rtos->thread_details->thread_name_str = malloc(sizeof(tmp_str));
		strcpy(rtos->thread_details->thread_name_str, tmp_str);

		if (thread_list_size == 0) {
			rtos->thread_count = 1;
			return ERROR_OK;
		}
	} else {
		/* create space for new thread details */
		rtos->thread_details = malloc(
				sizeof(struct thread_detail) * thread_list_size);
	}

	/* Read the pointer to the first thread */
	int64_t thread_ptr = 0;
	retval = target_read_buffer(rtos->target,
			rtos->symbols[ThreadX_VAL_tx_thread_created_ptr].address,
			param->pointer_width,
			(uint8_t *)&thread_ptr);
	if (retval != ERROR_OK) {
		LOG_ERROR("Could not read ThreadX thread location from target");
		return retval;
	}

	/* loop over all threads */
	int64_t prev_thread_ptr = 0;
	while ((thread_ptr != prev_thread_ptr) && (tasks_found < thread_list_size)) {

		#define THREADX_THREAD_NAME_STR_SIZE (200)
		char tmp_str[THREADX_THREAD_NAME_STR_SIZE];
		unsigned int i = 0;
		int64_t name_ptr = 0;

		/* Save the thread pointer */
		rtos->thread_details[tasks_found].threadid = thread_ptr;

		/* read the name pointer */
		retval = target_read_buffer(rtos->target,
				thread_ptr + param->thread_name_offset,
				param->pointer_width,
				(uint8_t *)&name_ptr);
		if (retval != ERROR_OK) {
			LOG_ERROR("Could not read ThreadX thread name pointer from target");
			return retval;
		}

		/* Read the thread name */
		retval =
			target_read_buffer(rtos->target,
				name_ptr,
				THREADX_THREAD_NAME_STR_SIZE,
				(uint8_t *)&tmp_str);
		if (retval != ERROR_OK) {
			LOG_ERROR("Error reading thread name from ThreadX target");
			return retval;
		}
		tmp_str[THREADX_THREAD_NAME_STR_SIZE-1] = '\x00';

		if (tmp_str[0] == '\x00')
			strcpy(tmp_str, "No Name");

		rtos->thread_details[tasks_found].thread_name_str =
			malloc(strlen(tmp_str)+1);
		strcpy(rtos->thread_details[tasks_found].thread_name_str, tmp_str);

		/* Read the thread status */
		int64_t thread_status = 0;
		retval = target_read_buffer(rtos->target,
				thread_ptr + param->thread_state_offset,
				4,
				(uint8_t *)&thread_status);
		if (retval != ERROR_OK) {
			LOG_ERROR("Error reading thread state from ThreadX target");
			return retval;
		}

		for (i = 0; (i < THREADX_NUM_STATES) &&
				(ThreadX_thread_states[i].value != thread_status); i++) {
			/* empty */
		}

		const char *state_desc;
		if  (i < THREADX_NUM_STATES)
			state_desc = ThreadX_thread_states[i].desc;
		else
			state_desc = "Unknown state";

		rtos->thread_details[tasks_found].extra_info_str = malloc(strlen(
					state_desc)+1);
		strcpy(rtos->thread_details[tasks_found].extra_info_str, state_desc);

		rtos->thread_details[tasks_found].exists = true;

		rtos->thread_details[tasks_found].display_str = NULL;

		tasks_found++;
		prev_thread_ptr = thread_ptr;

		/* Get the location of the next thread structure. */
		thread_ptr = 0;
		retval = target_read_buffer(rtos->target,
				prev_thread_ptr + param->thread_next_offset,
				param->pointer_width,
				(uint8_t *) &thread_ptr);
		if (retval != ERROR_OK) {
			LOG_ERROR("Error reading next thread pointer in ThreadX thread list");
			return retval;
		}
	}

	rtos->thread_count = tasks_found;

	return 0;
}

static int ThreadX_get_thread_reg_list(struct rtos *rtos, int64_t thread_id, char **hex_reg_list)
{
	int retval;
	const struct ThreadX_params *param;
	const struct rtos_register_stacking *stacking_info;

	*hex_reg_list = NULL;

	if (rtos == NULL)
		return -1;

	if (thread_id == 0 || thread_id == 1)
		return -2;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct ThreadX_params *) rtos->rtos_specific_params;

	/* Read the stack pointer */
	int64_t stack_ptr = 0;
	retval = target_read_buffer(rtos->target,
			thread_id + param->thread_stack_offset,
			param->pointer_width,
			(uint8_t *)&stack_ptr);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error reading stack frame from ThreadX thread");
		return retval;
	}
	if (stack_ptr == 0) {
		LOG_ERROR("Null stack pointer in ThreadX thread");
		return -5;
	}

	/* Get corresponding stacking info */
	stacking_info = get_stacking_info(rtos, stack_ptr);
	if (stacking_info == NULL) {
		LOG_ERROR("Unknown stacking info for ThreadX thread");
		return -6;
	}

	return rtos_generic_stack_read(rtos->target, stacking_info, stack_ptr, hex_reg_list);
}

static int ThreadX_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[])
{
	unsigned int i;
	*symbol_list = calloc(
			ARRAY_SIZE(ThreadX_symbol_list), sizeof(symbol_table_elem_t));

	for (i = 0; i < ARRAY_SIZE(ThreadX_symbol_list); i++)
		(*symbol_list)[i].symbol_name = ThreadX_symbol_list[i];

	return 0;
}

static int ThreadX_detect_rtos(struct target *target)
{
	if ((target->rtos->symbols != NULL) &&
			(target->rtos->symbols[ThreadX_VAL_tx_thread_created_ptr].address != 0)) {
		/* looks like ThreadX */
		return 1;
	}
	return 0;
}

#if 0

static int ThreadX_set_current_thread(struct rtos *rtos, threadid_t thread_id)
{
	return 0;
}

static int ThreadX_get_thread_detail(struct rtos *rtos,
	threadid_t thread_id,
	struct thread_detail *detail)
{
	unsigned int i = 0;
	int retval;

#define THREADX_THREAD_NAME_STR_SIZE (200)
	char tmp_str[THREADX_THREAD_NAME_STR_SIZE];

	const struct ThreadX_params *param;

	if (rtos == NULL)
		return -1;

	if (thread_id == 0)
		return -2;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct ThreadX_params *) rtos->rtos_specific_params;

	if (rtos->symbols == NULL) {
		LOG_ERROR("No symbols for ThreadX");
		return -3;
	}

	detail->threadid = thread_id;

	int64_t name_ptr = 0;
	/* read the name pointer */
	retval = target_read_buffer(rtos->target,
			thread_id + param->thread_name_offset,
			param->pointer_width,
			(uint8_t *)&name_ptr);
	if (retval != ERROR_OK) {
		LOG_ERROR("Could not read ThreadX thread name pointer from target");
		return retval;
	}

	/* Read the thread name */
	retval = target_read_buffer(rtos->target,
			name_ptr,
			THREADX_THREAD_NAME_STR_SIZE,
			(uint8_t *)&tmp_str);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error reading thread name from ThreadX target");
		return retval;
	}
	tmp_str[THREADX_THREAD_NAME_STR_SIZE-1] = '\x00';

	if (tmp_str[0] == '\x00')
		strcpy(tmp_str, "No Name");

	detail->thread_name_str = malloc(strlen(tmp_str)+1);

	/* Read the thread status */
	int64_t thread_status = 0;
	retval =
		target_read_buffer(rtos->target,
			thread_id + param->thread_state_offset,
			4,
			(uint8_t *)&thread_status);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error reading thread state from ThreadX target");
		return retval;
	}

	for (i = 0; (i < THREADX_NUM_STATES) &&
			(ThreadX_thread_states[i].value != thread_status); i++) {
		/* empty */
	}

	char *state_desc;
	if  (i < THREADX_NUM_STATES)
		state_desc = ThreadX_thread_states[i].desc;
	else
		state_desc = "Unknown state";

	detail->extra_info_str = malloc(strlen(state_desc)+1);

	detail->exists = true;

	detail->display_str = NULL;

	return 0;
}

#endif

static int ThreadX_create(struct target *target)
{
	int i = 0;
	while ((i < THREADX_NUM_PARAMS) &&
			(0 != strcmp(ThreadX_params_list[i].target_name, target->type->name))) {
		i++;
	}
	if (i >= THREADX_NUM_PARAMS) {
		LOG_ERROR("Could not find target in ThreadX compatibility list");
		return -1;
	}

	target->rtos->rtos_specific_params = (void *) &ThreadX_params_list[i];
	target->rtos->current_thread = 0;
	target->rtos->thread_details = NULL;
	return 0;
}
