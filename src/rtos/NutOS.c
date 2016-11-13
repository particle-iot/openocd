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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "target/armv7m.h"
#include "target/target.h"
#include "target/target_type.h"
#include "rtos.h"
#include "rtos_standard_stackings.h"

static const struct stack_register_offset
rtos_NutOS_Cortex_M0_stack_offsets[ARMV7M_NUM_CORE_REGS] = {
	{   -1, 32 },		/* r0   */
	{   -1, 32 },		/* r1   */
	{   -1, 32 },		/* r2   */
	{   -1, 32 },		/* r3   */
	{ 0x14, 32 },		/* r4   */
	{ 0x18, 32 },		/* r5   */
	{ 0x1c, 32 },		/* r6   */
	{ 0x20, 32 },		/* r7   */
	{ 0x04, 32 },		/* r8   */
	{ 0x08, 32 },		/* r9   */
	{ 0x0c, 32 },		/* r10  */
	{ 0x10, 32 },		/* r11  */
	{   -1, 32 },		/* r12  */
	{   -2, 32 },		/* sp   */
	{   -1, 32 },		/* lr   */
	{ 0x24, 32 },		/* pc   */
	{ 0x00, 32 },		/* xPSR */
};

static const struct stack_register_offset
rtos_NutOS_Cortex_M3_stack_offsets[ARMV7M_NUM_CORE_REGS] = {
	{   -1, 32 },		/* r0   */
	{   -1, 32 },		/* r1   */
	{   -1, 32 },		/* r2   */
	{   -1, 32 },		/* r3   */
	{ 0x04, 32 },		/* r4   */
	{ 0x08, 32 },		/* r5   */
	{ 0x0c, 32 },		/* r6   */
	{ 0x10, 32 },		/* r7   */
	{ 0x14, 32 },		/* r8   */
	{ 0x18, 32 },		/* r9   */
	{ 0x1c, 32 },		/* r10  */
	{ 0x20, 32 },		/* r11  */
	{   -1, 32 },		/* r12  */
	{   -2, 32 },		/* sp   */
	{   -1, 32 },		/* lr   */
	{ 0x24, 32 },		/* pc   */
	{ 0x00, 32 },		/* xPSR */
};

const struct rtos_register_stacking rtos_NutOS_Cortex_M0_stacking = {
	0x28,								/* stack_registers_size */
	-1,									/* stack_growth_direction */
	ARMV7M_NUM_CORE_REGS,				/* num_output_registers */
	rtos_generic_stack_align8,			/* stack_alignment */
	rtos_NutOS_Cortex_M0_stack_offsets	/* register_offsets */
};

const struct rtos_register_stacking rtos_NutOS_Cortex_M3_stacking = {
	0x28,								/* stack_registers_size */
	-1,									/* stack_growth_direction */
	ARMV7M_NUM_CORE_REGS,				/* num_output_registers */
	rtos_generic_stack_align8,			/* stack_alignment */
	rtos_NutOS_Cortex_M3_stack_offsets	/* register_offsets */
};

static int NutOS_detect_rtos(struct target *target);
static int NutOS_create(struct target *target);
static int NutOS_update_threads(struct rtos *rtos);
static int NutOS_get_thread_reg_list(
	struct rtos *rtos, int64_t thread_id, char **hex_reg_list);
static int NutOS_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[]);

struct rtos_type NutOS_rtos = {
		.name = "NutOS",
		.detect_rtos = NutOS_detect_rtos,
		.create = NutOS_create,
		.update_threads = NutOS_update_threads,
		.get_thread_reg_list = NutOS_get_thread_reg_list,
		.get_symbol_list_to_lookup = NutOS_get_symbol_list_to_lookup,
};

/**
 * @brief NutOS thread states.
 */
static const char * const NutOS_thread_states[] = {
	"TERMINATED", "RUNNING", "READY", "SLEEPING"
};

struct NutOS_params {
	const char *target_name;
	unsigned char pointer_width;
	unsigned char thread_stack_offset;
	unsigned char thread_name_offset;
	unsigned char thread_state_offset;
	unsigned char thread_next_offset;
	const struct rtos_register_stacking *stacking_info_M0;
	const struct rtos_register_stacking *stacking_info_M3;
};

static const struct NutOS_params NutOS_params_list[] = {
	{
		"cortex_m",								/* target_name */
		4,										/* pointer_width; */
		0x18,									/* thread_stack_offset; */
		0x0c,									/* thread_name_offset; */
		0x15,									/* thread_state_offset; */
		0,										/* thread_next_offset */
		&rtos_NutOS_Cortex_M0_stacking,			/* stacking_info for M0 */
		&rtos_NutOS_Cortex_M3_stacking,			/* stacking_info for M3/M4*/
	},
	{
		"hla_target",							/* target_name */
		4,										/* pointer_width; */
		0x18,									/* thread_stack_offset; */
		0x0c,									/* thread_name_offset; */
		0x15,									/* thread_state_offset; */
		0,										/* thread_next_offset */
		&rtos_NutOS_Cortex_M0_stacking,			/* stacking_info for M0 */
		&rtos_NutOS_Cortex_M3_stacking,			/* stacking_info for M3/M4*/
	}
};

#define NUTOS_NUM_PARAMS ((int)(sizeof(NutOS_params_list)/sizeof(struct NutOS_params)))

enum NutOS_symbol_values {
	NutOS_VAL_thread_list = 0,
	NutOS_VAL_current_thread_ptr = 1
};

static const char * const NutOS_symbol_list[] = {
	"nutThreadList",
	"runningThread",
	NULL
};

static int NutOS_update_threads(struct rtos *rtos)
{
	int retval;
	const struct NutOS_params *param;

	if (rtos == NULL)
		return -1;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct NutOS_params *) rtos->rtos_specific_params;

	if (rtos->symbols == NULL) {
		LOG_ERROR("No symbols for NutOS");
		return -4;
	}

	if (rtos->symbols[NutOS_VAL_thread_list].address == 0) {
		LOG_ERROR("Don't have the thread list head");
		return -2;
	}

	/* determine the number of current threads.*/
	rtos->thread_count = 0;
	int threadIdx = 0;
	uint32_t thread_list_head = rtos->symbols[NutOS_VAL_thread_list].address;
	/* Read nutThreadList pointer. */
	retval = target_read_buffer(
		rtos->target, thread_list_head + param->thread_next_offset,
		param->pointer_width, (uint8_t *) &thread_list_head);
	if (retval != ERROR_OK)
			return retval;
	uint32_t thread_link = thread_list_head;
	do {
		retval = target_read_buffer(
			rtos->target, thread_link + param->thread_next_offset,
			param->pointer_width, (uint8_t *) &thread_link);
		if (retval != ERROR_OK)
			return retval;
		threadIdx++;
	} while (thread_link);

	/* wipe out previous thread details if any */
	rtos_free_threadlist(rtos);

	/* read the current thread id */
	retval = target_read_buffer(rtos->target,
			rtos->symbols[NutOS_VAL_current_thread_ptr].address,
			4,
			(uint8_t *)&rtos->current_thread);
	if (retval != ERROR_OK) {
		LOG_ERROR("Could not read NutOS current thread from target");
		return retval;
	}
	if ((threadIdx  == 0) || (rtos->current_thread == 0))
		LOG_WARNING("Handle threadIdx  == 0) || (rtos->current_thread == 0)\n");
	rtos->thread_count = threadIdx;
	/* Store the NUTTHREADINFO pointer and name for each thread*/
	rtos->thread_details = calloc(rtos->thread_count,
								  sizeof(struct thread_detail));
	if (NULL == rtos->thread_details)
		return ERROR_FAIL;
	int i;
	thread_link = thread_list_head;
	for (i = 0; i < rtos->thread_count; i++) {
		uint8_t state;
		rtos->thread_details[i].threadid = thread_link;
		rtos->thread_details[i].exists = true;
		rtos->thread_details[i].thread_name_str =  calloc(9, sizeof(uint8_t));
		if (rtos->thread_details[i].thread_name_str) {
			retval = target_read_buffer(
				rtos->target, thread_link + param->thread_name_offset,
				9, (uint8_t *) rtos->thread_details[i].thread_name_str);
			if (retval != ERROR_OK)
				return retval;
		} else {
			LOG_ERROR("Calloc failed\n");
		}
		retval = target_read_buffer(
			rtos->target, thread_link + param->thread_state_offset,
			1, &state);
		retval = target_read_buffer(
			rtos->target, thread_link + param->thread_next_offset,
			param->pointer_width, (uint8_t *) &thread_link);
		if (retval != ERROR_OK)
			return retval;
	}
	return 0;
}

static int NutOS_get_thread_reg_list(
	struct rtos *rtos, int64_t thread_id, char **hex_reg_list)
{
	int retval;
	const struct NutOS_params *param;
	struct armv7m_common *armv7m = target_to_armv7m(rtos->target);
	const struct rtos_register_stacking *stacking_info;
	int64_t stack_ptr = 0;

	*hex_reg_list = NULL;

	if (rtos == NULL)
		return -1;

	if (thread_id == 0)
		return -2;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct NutOS_params *) rtos->rtos_specific_params;
	if (armv7m->arm.is_armv6m == true)
		stacking_info = param->stacking_info_M0;
	else
		stacking_info = param->stacking_info_M3;
	uint32_t thread_list_ptr = rtos->symbols[NutOS_VAL_thread_list].address;
	uint32_t thread_list_head;
	retval = target_read_buffer(
		rtos->target, thread_list_ptr,
		param->pointer_width, (uint8_t *) &thread_list_head);
	if (retval != ERROR_OK)
		return retval;
	do {
		if (thread_list_head == thread_id)
			break;
		/* Read next linked NUTTHREADINFO */
		retval = target_read_buffer(
			rtos->target, thread_list_head + param->thread_next_offset,
			param->pointer_width, (uint8_t *) &thread_list_head);
		if (retval != ERROR_OK)
			return retval;
	} while (thread_list_head);
	if (!thread_list_head) {
		LOG_ERROR("Could not find Thread");
		return -1;
	}
	/* Read the stack pointer */
	retval = target_read_buffer(
		rtos->target, thread_list_head + param->thread_stack_offset,
		param->pointer_width, (uint8_t *) &stack_ptr);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error reading stack frame from NutOS thread");
		return retval;
	}

	return rtos_generic_stack_read(
		rtos->target, stacking_info, stack_ptr, hex_reg_list);
}

static int NutOS_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[])
{
	unsigned int i;
	*symbol_list = calloc(
			ARRAY_SIZE(NutOS_symbol_list), sizeof(symbol_table_elem_t));

	for (i = 0; i < ARRAY_SIZE(NutOS_symbol_list); i++)
		(*symbol_list)[i].symbol_name = NutOS_symbol_list[i];

	return 0;
}

static int NutOS_detect_rtos(struct target *target)
{
	if ((target->rtos->symbols != NULL) &&
			(target->rtos->symbols[NutOS_VAL_thread_list].address != 0)) {
		/* looks like NutOS */
		return 1;
	}
	return 0;
}

static int NutOS_create(struct target *target)
{
	int i = 0;
	while ((i < NUTOS_NUM_PARAMS) &&
		(0 != strcmp(NutOS_params_list[i].target_name, target->type->name))) {
		i++;
	}
	if (i >= NUTOS_NUM_PARAMS) {
		LOG_ERROR("Could not find target in NutOS compatibility list");
		return -1;
	}

	target->rtos->rtos_specific_params = (void *) &NutOS_params_list[i];
	return 0;
}
