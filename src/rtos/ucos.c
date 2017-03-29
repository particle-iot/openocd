/***************************************************************************
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
#include "target/armv7m.h"
#include "rtos_standard_stackings.h"

#define THREAD_DEFAULT_ID 0xff	/* when ucos is not running, this is the thread id */

static bool ucos_detect_rtos(struct target *target);
static int ucos_create(struct target *target);
static int ucos_update_threads(struct rtos *rtos);
static int ucos_get_thread_reg_list(struct rtos *rtos, int64_t thread_id,
				    char **hex_reg_list);
static int ucos_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[]);

struct ucos_thread_state {
	int value;
	char *desc;
};

struct ucos_thread_state ucos_thread_states[] = {
	{0, "Ready"},
	{1, "WaitSemaphore"},
	{2, "WaitMailbox"},
	{4, "WaitQ"},
	{8, "Suspended"},
	{0x10, "WaitMutex"},
	{0x20, "WaitFlag"},
	{0x40, "Sleep"},
	{0x80, "WaitEvent"},
};

#define UCOS_NUM_STATES (sizeof(ucos_thread_states)/sizeof(struct ucos_thread_state))

struct ucos_params {
	char *target_name;
	unsigned char pointer_width;
	const struct rtos_register_stacking *stacking_info;
};

static const struct stack_register_offset
	rtos_ucos_Cortex_M3_stack_offsets[ARMV7M_NUM_CORE_REGS] = {
	{0x04, 32},		/* r0   */
	{0x08, 32},		/* r1   */
	{0x0c, 32},		/* r2   */
	{0x10, 32},		/* r3   */
	{0x14, 32},		/* r4   */
	{0x18, 32},		/* r5   */
	{0x1c, 32},		/* r6   */
	{0x20, 32},		/* r7   */
	{0x24, 32},		/* r8   */
	{0x28, 32},		/* r9   */
	{0x2c, 32},		/* r10  */
	{0x30, 32},		/* r11  */
	{0x34, 32},		/* r12  */
	{-2, 32},		/* sp    new_stack_ptr */
	{-1, 32},		/* lr    zero */
	{0x3c, 32},		/* pc   */
	{0x00, 32},		/* xPSR */
};

const struct rtos_register_stacking rtos_ucos_Cortex_M3_stacking = {
	0x44,			/* stack_registers_size */
	-1,			/* stack_growth_direction */
	ARMV7M_NUM_CORE_REGS,	/* num_output_registers */
	rtos_generic_stack_align8,	/* stack_alignment */
	rtos_ucos_Cortex_M3_stack_offsets	/* register_offsets */
};

const struct ucos_params ucos_params_list[] = {
	{
	 "arm7tdmi",		/* target_name */
	 4,			/* pointer_width; */
	 &rtos_ucos_Cortex_M3_stacking	/* stacking_info */
	 },
	{
	 "arm926ejs",		/* target_name */
	 4,					/* pointer_width; */
	 &rtos_ucos_Cortex_M3_stacking	/* stacking_info */
	 }
};

#define UCOS_NUM_PARAMS ((int)(sizeof(ucos_params_list)/sizeof(struct ucos_params)))

enum ucos_symbol_values {
	ucos_VAL_OSTCBPrioTbl,
	ucos_VAL_OSTCBCur,
	ucos_VAL_idle_stack,
	ucos_VAL_name_offset,
	ucos_VAL_size_tcb,
	ucos_VAL_num_threads,
	ucos_VAL_status_offset
};

static char *ucos_symbol_list[] = {
	"OSTCBPrioTbl",
	"OSTCBCur",
	"OSTaskIdleStk",
	"OSGdbHelpOffsetName",
	"OSGdbHelpSizeTcb",
	"OSGdbHelpMaxTcb",
	"OSGdbHelpOffsetStat",
	NULL
};

const struct rtos_type ucos_rtos = {
	.name = "ucos",

	.detect_rtos = ucos_detect_rtos,
	.create = ucos_create,
	.update_threads = ucos_update_threads,
	.get_thread_reg_list = ucos_get_thread_reg_list,
	.get_symbol_list_to_lookup = ucos_get_symbol_list_to_lookup,

};

static int ucos_update_threads(struct rtos *rtos)
{
	int retval;
	int tasks_found = 0;
	const struct ucos_params *param;

	if (rtos == NULL)
		return -1;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct ucos_params *)rtos->rtos_specific_params;

	if (rtos->symbols == NULL) {
		LOG_ERROR("No symbols for ucos");
		return -4;
	}

	if (rtos->symbols[ucos_VAL_OSTCBPrioTbl].address == 0) {
		LOG_ERROR("Don't have the thread list head");
		return -2;
	}

	/* wipe out previous thread details if any */
	rtos_free_threadlist(rtos);

	/* determine the number of current threads */
	uint32_t num_threads;
	uint32_t tcb_size;
	uint32_t name_offset;
	uint32_t active_threads = 0;
	uint32_t OSTCBCur;
	uint32_t status_offset;
	uint32_t i;

	target_read_buffer(rtos->target,
			   rtos->symbols[ucos_VAL_num_threads].address,
			   param->pointer_width, (uint8_t *) &num_threads);
	target_read_buffer(rtos->target,
			   rtos->symbols[ucos_VAL_name_offset].address,
			   param->pointer_width, (uint8_t *) &name_offset);
	target_read_buffer(rtos->target,
			   rtos->symbols[ucos_VAL_OSTCBCur].address,
			   param->pointer_width, (uint8_t *) &OSTCBCur);
	target_read_buffer(rtos->target,
			   rtos->symbols[ucos_VAL_status_offset].address,
			   param->pointer_width, (uint8_t *) &status_offset);
	target_read_buffer(rtos->target,
			   rtos->symbols[ucos_VAL_size_tcb].address,
			   param->pointer_width, (uint8_t *) &tcb_size);

	LOG_INFO
	    ("CurTask: %u TCB size: %u num threads: %u status off: %u name off:%u",
	     OSTCBCur, tcb_size, num_threads, status_offset, name_offset);

	num_threads &= 63;
	uint32_t *OSTCBPrioTbl = malloc(num_threads * param->pointer_width);

	/* sanity check */
	if (tcb_size < 0x1000) {
		target_read_buffer(rtos->target,
				   rtos->symbols[ucos_VAL_OSTCBPrioTbl].address,
				   param->pointer_width * num_threads,
				   (uint8_t *) OSTCBPrioTbl);
		rtos->current_thread = THREAD_DEFAULT_ID;

		for (i = 0; i < num_threads; ++i) {

			if (OSTCBPrioTbl[i])
				++active_threads;

			if (OSTCBCur && OSTCBPrioTbl[i] == OSTCBCur)
				rtos->current_thread = i;


		}
		LOG_INFO("Found %u active tasks, %u is current thread",
			 active_threads, (unsigned)rtos->current_thread);
	} else {
		LOG_WARNING
		    ("ucos TCB size out of range.  Maybe the code is not loade yet??");
		rtos->current_thread = THREAD_DEFAULT_ID;
	}

	tasks_found = 0;
	if ((active_threads == 0)
	    || (rtos->current_thread == THREAD_DEFAULT_ID)) {
		/* Either : No RTOS threads - there is always at least the current execution though */
		/* OR     : No current thread - all threads suspended - show the current execution
		 * of idling */
		LOG_INFO("ucos not running, defaulting to current execution");
		char tmp_str[] = "Current Execution";
		active_threads++;
		tasks_found++;
		rtos->thread_details =
		    malloc(sizeof(struct thread_detail) * active_threads);
		rtos->thread_details->threadid = THREAD_DEFAULT_ID;
		rtos->thread_details->exists = true;
		rtos->thread_details->extra_info_str = NULL;
		rtos->thread_details->thread_name_str = malloc(sizeof(tmp_str));
		strcpy(rtos->thread_details->thread_name_str, tmp_str);
		rtos->current_thread = 0;

		if (active_threads == 1) {
			rtos->thread_count = 1;
			retval = ERROR_OK;
			goto exit;
		}
	} else {
		/* create space for new thread details */
		rtos->thread_details =
		    malloc(sizeof(struct thread_detail) * active_threads);
	}

	for (i = 0; i < num_threads; ++i) {
		/* get the trhead status */
		uint8_t thread_state;
		uint8_t thread_prio;
		uint8_t both[2];

		if (OSTCBPrioTbl[i]) {
			char tmp_str[tcb_size - name_offset + 1];
			retval = target_read_buffer(rtos->target,
						    OSTCBPrioTbl[i] +
						    status_offset, 2,
						    (uint8_t *) both);
			if (retval != ERROR_OK) {
				LOG_ERROR
				    ("Could not read ucos thread status from target");
				goto exit;
			}
			thread_state = both[0];
			thread_prio = both[1];
			rtos->thread_details[tasks_found].threadid =
			    thread_prio;
			rtos->thread_details->exists = true;
			memset(tmp_str, 0, sizeof(tmp_str));
			retval = target_read_buffer(rtos->target,
						    OSTCBPrioTbl[i] +
						    name_offset,
						    sizeof(tmp_str) - 1,
						    (uint8_t *) tmp_str);

			if (retval != ERROR_OK) {
				LOG_ERROR
				    ("Could not read ucos thread name from target");
				goto exit;
			}

			LOG_INFO("Task %x: id: %d: name: %s detected",
				 OSTCBPrioTbl[i], thread_prio, tmp_str);

			if (tmp_str[0] == '\x00')
				strcpy(tmp_str, "No Name");

			rtos->thread_details[tasks_found].thread_name_str =
			    malloc(strlen(tmp_str) + 1);
			strcpy(rtos->thread_details[tasks_found].
			       thread_name_str, tmp_str);

			uint32_t j;
			for (j = 0;
			     (j < UCOS_NUM_STATES)
			     && (ucos_thread_states[j].value != thread_state);
			     j++) {
				/*
				 * empty
				 */
			}

			char *state_desc;
			if (j < UCOS_NUM_STATES)
				state_desc = ucos_thread_states[j].desc;
			else {
				sprintf(tmp_str, "state:%x", thread_state);
				state_desc = tmp_str;
			}
			if (OSTCBPrioTbl[i] == OSTCBCur)
				state_desc = "Running";


			rtos->thread_details[tasks_found].extra_info_str =
			    malloc(strlen(state_desc) + 1);
			strcpy(rtos->thread_details[tasks_found].extra_info_str,
			       state_desc);

			rtos->thread_details[tasks_found].exists = true;

			++tasks_found;
		}
	}

	LOG_INFO("Tasks found: %u", tasks_found);
	retval = 0;

 exit:
	rtos->thread_count = tasks_found;

	free(OSTCBPrioTbl);
	return retval;
}

static int ucos_get_thread_reg_list(struct rtos *rtos, int64_t thread_id,
				    char **hex_reg_list)
{
	int retval;
	const struct ucos_params *param;

	*hex_reg_list = NULL;

	if (rtos == NULL)
		return -1;

	if (thread_id == THREAD_DEFAULT_ID)
		return -1;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct ucos_params *)rtos->rtos_specific_params;

	/* Find the thread with that thread id */
	uint32_t tcb_ptr = 0;
	uint32_t thread_list_head =
	    rtos->symbols[ucos_VAL_OSTCBPrioTbl].address;

	retval = target_read_buffer(rtos->target,
				    thread_list_head +
				    ((thread_id) * param->pointer_width),
				    param->pointer_width,
				    (uint8_t *) &tcb_ptr);

	if (retval != ERROR_OK || tcb_ptr == 0) {
		LOG_ERROR
		    ("Error reading tcb from ucos thread (err: %d id: %u ptr: %u:%u",
		     retval, (unsigned)thread_id,
		     thread_list_head +
		     ((unsigned)thread_id * param->pointer_width), tcb_ptr);
		return retval;
	}

	uint32_t stack_ptr;
	/* read the stack pointer */
	target_read_buffer(rtos->target, tcb_ptr,	/* stack pointer is the first thing in the TCB */
			   param->pointer_width, (uint8_t *) &stack_ptr);

	LOG_INFO("Query regs for thread id %u, stack: %x", (int)thread_id,
		 stack_ptr);

	return rtos_generic_stack_read(rtos->target,
				       param->stacking_info,
				       stack_ptr, hex_reg_list);

}

static int ucos_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[])
{
	unsigned int i;
	*symbol_list =
	    malloc(sizeof(symbol_table_elem_t) * ARRAY_SIZE(ucos_symbol_list));

	for (i = 0; i < ARRAY_SIZE(ucos_symbol_list); i++)
		(*symbol_list)[i].symbol_name = ucos_symbol_list[i];

	return 0;
}

static bool ucos_detect_rtos(struct target *target)
{

	if ((target->rtos->symbols != NULL) &&
	    (target->rtos->symbols[ucos_VAL_OSTCBPrioTbl].address != 0)) {
		/* looks like ucos */
		return 1;
	}
	return 0;
}

static int ucos_create(struct target *target)
{
	int i = 0;
	while ((i < UCOS_NUM_PARAMS) &&
	       (0 !=
		strcmp(ucos_params_list[i].target_name, target->type->name))) {
		i++;
	}
	if (i >= UCOS_NUM_PARAMS) {
		LOG_ERROR("Could not find target in ucos compatibility list");
		return -1;
	}

	target->rtos->rtos_specific_params = (void *)&ucos_params_list[i];
	target->rtos->current_thread = 0;
	target->rtos->thread_details = NULL;
	return 0;
}
