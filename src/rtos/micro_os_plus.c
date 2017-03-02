/***************************************************************************
 *   Copyright (C) 2011 by Broadcom Corporation                            *
 *   Evan Hunter - ehunter@broadcom.com                                    *
 *                                                                         *
 *   Copyright (C) 2017 by OtherUse                                        *
 *   Armin van der Togt - armin@otheruse.nl                                *
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

#include <helper/time_support.h>
#include <jtag/jtag.h>
#include "target/target.h"
#include "target/target_type.h"
#include "rtos.h"
#include "helper/log.h"
#include "helper/types.h"
#include "rtos_standard_stackings.h"
#include "target/armv7m.h"
#include "target/cortex_m.h"

static int micro_os_plus_detect_rtos(struct target *target);
static int micro_os_plus_create(struct target *target);
static int micro_os_plus_update_threads(struct rtos *rtos);
static int micro_os_plus_get_thread_reg_list(struct rtos *rtos, int64_t thread_id, char **hex_reg_list);
static int micro_os_plus_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[]);

struct micro_os_plus_thread_state {
	int value;
	const char *desc;
};

#define MICRO_OS_PLUS_THREAD_NAME_STR_SIZE (200)

static const struct micro_os_plus_thread_state micro_os_plus_thread_states[] = {
		{ 0, "Undefined" },
		{ 1, "Ready" },
		{ 2, "Running" },
		{ 3, "Suspended" },
		{ 4, "Terminated" },
		{ 5, "Destroyed" },
};

#define MICRO_OS_PLUS_NUM_STATES (sizeof(micro_os_plus_thread_states)/sizeof(struct micro_os_plus_thread_state))

struct micro_os_plus_params {
	const char *target_name;
	unsigned char pointer_width;
	unsigned char thread_name_offset;
	unsigned char thread_parent_offset;
	unsigned char thread_list_node_offset;
	unsigned char thread_children_node_offset;
	unsigned char thread_state_offset;
	unsigned char thread_stack_bottom_offset;
	unsigned char thread_stack_offset;
	const struct rtos_register_stacking *stacking_info_default;
	const struct rtos_register_stacking *stacking_info_with_fpu;
};

static const struct micro_os_plus_params micro_os_plus_params_list[] = {
	{
	"cortex_m", /* target_name */
	4, /* pointer_width; */
	0x04, /* thread_name_offset; */
	0x24, /* thread_parent_offset; */
	0x28, /* thread_list_node_offset; */
	0x30, /* thread_children_node_offset; */
	0x60, /* thread_state_offset; */
	0x70, /* thread_stack_bottom_offset; */
	0x80, /* thread_stack_offset; */
	&rtos_standard_Cortex_M4F_stacking, /* stacking_info */
	&rtos_standard_Cortex_M4F_FPU_stacking, /* stacking_info */
	},
	{
	"hla_target", /* target_name */
	4, /* pointer_width; */
	0x04, /* thread_name_offset; */
	0x24, /* thread_parent_offset; */
	0x28, /* thread_list_node_offset; */
	0x30, /* thread_children_node_offset; */
	0x60, /* thread_state_offset; */
	0x70, /* thread_stack_bottom_offset; */
	0x80, /* thread_stack_offset; */
	&rtos_standard_Cortex_M4F_stacking, /* stacking_info */
	&rtos_standard_Cortex_M4F_FPU_stacking, /* stacking_info */
	},
};

#define MICRO_OS_PLUS_NUM_PARAMS ((int)(sizeof(micro_os_plus_params_list)/sizeof(struct micro_os_plus_params)))

enum micro_os_plus_symbol_values {
	micro_os_plus_VAL_thread_list = 0,
	micro_os_plus_VAL_current_thread_ptr = 1,
	micro_os_plus_VAL_scheduler_is_started = 2,
};

static const char * const micro_os_plus_symbol_list[] = {
		"os::rtos::scheduler::top_threads_list_",
		"os::rtos::scheduler::current_thread_",
		"os::rtos::scheduler::is_started_",
		NULL };

const struct rtos_type micro_os_plus_rtos = {
		.name = "µOS++ IIIe",
		.detect_rtos = micro_os_plus_detect_rtos,
		.create = micro_os_plus_create,
		.update_threads =
		micro_os_plus_update_threads,
		.get_thread_reg_list = micro_os_plus_get_thread_reg_list,
		.get_symbol_list_to_lookup = micro_os_plus_get_symbol_list_to_lookup,
};

static int micro_os_plus_get_thread_info(struct rtos *rtos, uint32_t thread_index,
		struct thread_detail *thread_details)
{
	const struct micro_os_plus_params *param = (const struct micro_os_plus_params *) rtos->rtos_specific_params;
	char tmp_str[MICRO_OS_PLUS_THREAD_NAME_STR_SIZE];
	int retval;
	/* Thread id is thread address */
	thread_details->threadid = thread_index;
	uint32_t name_ptr = 0;
	/* read the name pointer */
	retval = target_read_buffer(rtos->target, thread_index + param->thread_name_offset, param->pointer_width,
			(uint8_t *) &name_ptr);
	if (retval != ERROR_OK) {
		LOG_ERROR("Could not read µOS++ IIIe thread name pointer from target");
		return retval;
	}

	/* Read the thread name */
	retval = target_read_buffer(rtos->target, name_ptr,
	MICRO_OS_PLUS_THREAD_NAME_STR_SIZE, (uint8_t *) &tmp_str);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error reading thread name from µOS++ IIIe target");
		return retval;
	}
	tmp_str[MICRO_OS_PLUS_THREAD_NAME_STR_SIZE - 1] = '\x00';

	if (tmp_str[0] == '\x00')
		strcpy(tmp_str, "No Name");

	thread_details->thread_name_str = malloc(strlen(tmp_str) + 1);
	strcpy(thread_details->thread_name_str, tmp_str);

	/* Read the thread status */
	uint8_t thread_status = 0;
	retval = target_read_buffer(rtos->target, thread_index + param->thread_state_offset, 1, &thread_status);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error reading thread state from µOS++ IIIe target");
		return retval;
	}
	/* Read the parent id */
	uint32_t thread_parent;
	retval = target_read_buffer(rtos->target, thread_index + param->thread_parent_offset, param->pointer_width,
			(uint8_t *) &thread_parent);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error reading parent id from µOS++ IIIe target");
		return retval;
	}

	if (thread_status >= MICRO_OS_PLUS_NUM_STATES)
		thread_status = 0;
	const char *state_desc;
	state_desc = micro_os_plus_thread_states[thread_status].desc;

	thread_details->extra_info_str = malloc(strlen(state_desc) + 30);
	sprintf(thread_details->extra_info_str, "State: %s, parent: %u", state_desc, thread_parent);

	thread_details->exists = true;
	return 0;

}

static int micro_os_plus_get_thread_info_recursive(struct rtos *rtos, uint32_t list_head, uint32_t list_prev,
		int *thread_list_index,
		bool count_only)
{
	const struct micro_os_plus_params *param = (const struct micro_os_plus_params *) rtos->rtos_specific_params;
	int retval = 0;
	if (list_prev == list_head)
		return retval;
	if (!count_only) {
		micro_os_plus_get_thread_info(rtos, list_prev - param->thread_list_node_offset,
				&(rtos->thread_details[*thread_list_index]));
	}
	(*thread_list_index)++;
	uint32_t children_head = list_prev - param->thread_list_node_offset + param->thread_children_node_offset;
	uint32_t children_prev;
	retval = target_read_buffer(rtos->target, children_head, param->pointer_width, (uint8_t *) &children_prev);
	if (retval != ERROR_OK)
		return retval;
	retval = micro_os_plus_get_thread_info_recursive(rtos, children_head, children_prev, thread_list_index,
			count_only);
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_buffer(rtos->target, list_prev, param->pointer_width, (uint8_t *) &list_prev);
	if (retval != ERROR_OK)
		return retval;
	retval = micro_os_plus_get_thread_info_recursive(rtos, list_head, list_prev, thread_list_index, count_only);
	return retval;
}

static int micro_os_plus_create_single_thread(struct rtos *rtos)
{
	/* No RTOS threads - there is always at least the current execution though */
	char tmp_str[] = "Current Execution";
	rtos->thread_details = malloc(sizeof(struct thread_detail));
	if (!rtos->thread_details) {
		LOG_ERROR("Error allocating memory for 1 thread");
		return ERROR_FAIL;
	}
	rtos->thread_details->threadid = 1;
	rtos->thread_details->exists = true;
	rtos->thread_details->extra_info_str = NULL;
	rtos->thread_details->thread_name_str = malloc(sizeof(tmp_str));
	strcpy(rtos->thread_details->thread_name_str, tmp_str);
	rtos->thread_count = 1;
	rtos->current_thread = 1;
	return ERROR_OK;
}

static int micro_os_plus_update_threads(struct rtos *rtos)
{
	int retval;
	int thread_count = 0;
	const struct micro_os_plus_params *param;

	if (rtos == NULL)
		return -1;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct micro_os_plus_params *) rtos->rtos_specific_params;

	if (rtos->symbols == NULL) {
		LOG_ERROR("No symbols for µOS++ IIIe");
		return -4;
	}

	if (rtos->symbols[micro_os_plus_VAL_thread_list].address == 0) {
		LOG_ERROR("Don't have the thread list head");
		return -2;
	}

	/* wipe out previous thread details if any */
	rtos_free_threadlist(rtos);

	/* read os::rtos::scheduler::is_started_ to see if the scheduler has been started */
	uint8_t is_started;
	retval = target_read_buffer(rtos->target, rtos->symbols[micro_os_plus_VAL_scheduler_is_started].address, 1,
			(uint8_t *) &is_started);
	if (retval != ERROR_OK)
		return retval;

	if (is_started == 0) {
		/* scheduler not yet started, return single thread */
		return micro_os_plus_create_single_thread(rtos);
	}
	/* read the top_thread list head  */
	uint32_t thread_list_head = rtos->symbols[micro_os_plus_VAL_thread_list].address;
	uint32_t first_thread;
	retval = target_read_buffer(rtos->target, thread_list_head, param->pointer_width, (uint8_t *) &first_thread);
	if (retval != ERROR_OK)
		return retval;

	if (first_thread == 0 || first_thread == thread_list_head) {
		/* this should only be possible during initialization */
		return micro_os_plus_create_single_thread(rtos);
	} else {
		/* calculate the number of threads */
		retval = micro_os_plus_get_thread_info_recursive(rtos, thread_list_head, first_thread, &thread_count, true);
		if (retval != 0)
			return retval;
		/* read the current thread id */
		uint32_t current_thread_addr;
		retval = target_read_buffer(rtos->target, rtos->symbols[micro_os_plus_VAL_current_thread_ptr].address, 4,
				(uint8_t *) &current_thread_addr);
		if (retval != ERROR_OK)
			return retval;
		if (thread_count == 0) {
			/* this should never happen */
			LOG_ERROR("Thread list is empty!");
			return micro_os_plus_create_single_thread(rtos);
		}
		rtos->thread_count = thread_count;
		/* Use thread address as thread id */
		rtos->current_thread = current_thread_addr;
		/* create space for new thread details */
		rtos->thread_details = malloc(sizeof(struct thread_detail) * thread_count);
		int thread_list_index = 0;
		/* read information about the available threads */
		micro_os_plus_get_thread_info_recursive(rtos, thread_list_head, first_thread, &thread_list_index, false);
	}
	return 0;
}

static int micro_os_plus_get_thread_reg_list(struct rtos *rtos, int64_t thread_id, char **hex_reg_list)
{
	int retval;
	const struct micro_os_plus_params *param;

	*hex_reg_list = NULL;

	if (rtos == NULL)
		return -1;

	if (thread_id == 0)
		return -2;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct micro_os_plus_params *) rtos->rtos_specific_params;

	/* Read the stack pointer */
	int64_t stack_ptr = 0;
	retval = target_read_buffer(rtos->target, thread_id + param->thread_stack_offset, param->pointer_width,
			(uint8_t *) &stack_ptr);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error reading stack frame from µOS++ IIIe thread");
		return retval;
	}

	bool fpu_enabled = false;
	struct armv7m_common *armv7m_target = target_to_armv7m(rtos->target);
	if (is_armv7m(armv7m_target)) {
		if (armv7m_target->fp_feature == FPv4_SP) {
			/* Found ARM v7m target which includes a FPU */
			uint32_t cpacr;

			retval = target_read_u32(rtos->target, FPU_CPACR, &cpacr);
			if (retval != ERROR_OK) {
				LOG_ERROR("Could not read CPACR register to check FPU state");
				return -1;
			}

			/* Check if CP10 and CP11 are set to full access. */
			if (cpacr & 0x00F00000) {
				/* Found target with enabled FPU */
				fpu_enabled = true;
			}
		}
	}

	if (fpu_enabled) {
		/* Read the LR to decide between stacking with or without FPU */
		uint32_t LR_svc = 0;
		retval = target_read_buffer(rtos->target, stack_ptr + 0x20, param->pointer_width, (uint8_t *) &LR_svc);
		if (retval != ERROR_OK) {
			LOG_OUTPUT("Error reading stack frame from FreeRTOS thread\r\n");
			return retval;
		}
		if ((LR_svc & 0x10) == 0)
			return rtos_generic_stack_read(rtos->target, param->stacking_info_with_fpu, stack_ptr, hex_reg_list);
	}

	return rtos_generic_stack_read(rtos->target, param->stacking_info_default, stack_ptr, hex_reg_list);
}

static int micro_os_plus_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[])
{
	unsigned int i;
	*symbol_list = calloc(ARRAY_SIZE(micro_os_plus_symbol_list), sizeof(symbol_table_elem_t));

	for (i = 0; i < ARRAY_SIZE(micro_os_plus_symbol_list); i++)
		(*symbol_list)[i].symbol_name = micro_os_plus_symbol_list[i];

	return 0;
}

static int micro_os_plus_detect_rtos(struct target *target)
{
	if ((target->rtos->symbols != NULL) && (target->rtos->symbols[micro_os_plus_VAL_thread_list].address != 0)) {
		/* looks like µOS++ IIIe */
		return 1;
	}
	return 0;
}

static int micro_os_plus_create(struct target *target)
{
	int i = 0;
	while ((i < MICRO_OS_PLUS_NUM_PARAMS)
			&& (0 != strcmp(micro_os_plus_params_list[i].target_name, target->type->name))) {
		i++;
	}
	if (i >= MICRO_OS_PLUS_NUM_PARAMS) {
		LOG_ERROR("Could not find target in micro_os_plus compatibility list");
		return -1;
	}

	target->rtos->rtos_specific_params = (void *) &micro_os_plus_params_list[i];
	target->rtos->current_thread = 0;
	target->rtos->thread_details = NULL;
	return 0;
}
