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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/jtag.h>
#include "target/target.h"
#include "target/target_type.h"
#include "target/register.h"
#include "rtos.h"
#include "rtos_standard_stackings.h"
#include "server/gdb_server.h"

#define METAL_MAX_THREAD_NAME_STR_SIZE (64)

static bool metal_detect_rtos(struct target *target);
static int metal_create(struct target *target);
static int metal_update_threads(struct rtos *rtos);
static int metal_get_thread_reg_list(struct rtos *rtos, int64_t thread_id,
									struct rtos_reg **reg_list, int *num_regs);
static int metal_os_smp_init(struct target *target);
static int metal_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[]);
static int metal_target_for_threadid(struct connection *connection, int64_t threadid,
									 bool make_thread_current, struct target **t);

/**
 * metal rtos type simply shows one thread per smp core (not numbered by coreid)
 */
struct rtos_type metal_rtos = {
	.name = "metal",
	.detect_rtos = metal_detect_rtos,
	.create = metal_create,
	.update_threads = metal_update_threads,
	.get_thread_reg_list = metal_get_thread_reg_list,
	.smp_init = metal_os_smp_init,
	.get_symbol_list_to_lookup = metal_get_symbol_list_to_lookup,
};

static const char *const metal_symbol_list[] = {
		NULL
};

static bool metal_detect_rtos(struct target *target)
{
	/* no autodetect */
	return 0;
}

static int metal_create(struct target *target)
{
	target->rtos->rtos_specific_params = NULL;
	target->rtos->gdb_target_for_threadid = metal_target_for_threadid;
	return 0;
}

static struct target *metal_target_for_threadid_internal(struct target_list *head, int64_t threadid,
														 bool make_thread_current)
{
	struct target *target = NULL;
	threadid_t target_thread_id = 1;
	while (head != (struct target_list *) NULL) {
		if (threadid == target_thread_id) {
			target = head->target;
			if (make_thread_current)
				target->rtos->current_thread = threadid;
			break;
		}
		target_thread_id++;
		head = head->next;
	}

	return target;
}

static int metal_target_for_threadid(struct connection *connection, int64_t threadid,
									 bool make_thread_current, struct target **t)
{
	struct target *curr = get_target_from_connection(connection);
	struct target *match = 0;
	if (curr)
		match = metal_target_for_threadid_internal(curr->head, threadid, make_thread_current);

	if (match)
		*t = match;
	else
		*t = curr; /* need to return something, caller does not check return code */
	return ERROR_OK;
}


static int metal_get_target_thread_details(struct target *target, int threadid, struct thread_detail *details)
{
	int retval = ERROR_OK;
	details->threadid = threadid;
	details->exists = true;
	details->thread_name_str = malloc(METAL_MAX_THREAD_NAME_STR_SIZE);
	snprintf(details->thread_name_str, METAL_MAX_THREAD_NAME_STR_SIZE, "%s", target->cmd_name);
	details->extra_info_str = NULL;
	details->extra_info_str = malloc(METAL_MAX_THREAD_NAME_STR_SIZE);
	/*
	 * note" status is nearly always "halted" as you'd expect. I'm leaving it in, because it is potentially useful
	 * in gdb if something is wrong
	 */
	snprintf(details->extra_info_str, METAL_MAX_THREAD_NAME_STR_SIZE, "%s", target_state_name(target));
	return retval;
}

static int metal_update_threads(struct rtos *rtos)
{
	if (rtos == NULL)
		return ERROR_FAIL;

	threadid_t old_current_thread = rtos->current_thread;

	/* wipe out previous thread details if any (includes current_thread) */
	rtos_free_threadlist(rtos);

	/*
	 * determine number of threads, and choose the new current thread:
	 *
	 * we choose
	 *    a) the old current_thread if it is at a breakpoint/watchpoint/singlestep
	 *    b) the first encountered other thread at a breakpoing/watchpoint/singlestep
	 *    c) the old current thread if valid
	 *    d) the first thread
	 *
	 * this handles common cases including hitting a breakpoint on a different core
	 */
	int thread_list_size = 0;
	struct target_list *head = rtos->target->head;
	threadid_t new_current_thread = -1;
	while (head != (struct target_list *) NULL) {
		/* include a thread for any target... just figure out its state later */
		threadid_t threadid = ++thread_list_size;
		struct target *target = head->target;
		if (target->state == TARGET_HALTED) {
			switch (target->debug_reason) {
				case DBG_REASON_BREAKPOINT:
				case DBG_REASON_WATCHPOINT:
				case DBG_REASON_WPTANDBKPT:
				case DBG_REASON_SINGLESTEP:
					if (new_current_thread == -1 || threadid == old_current_thread)
						new_current_thread = threadid;
					break;
				default:
					/* nothing to do */
					break;
			}
		}
		head = head->next;
	}

	/* if we didn't pick a new current thread, try the old one */
	if (new_current_thread == -1)
		new_current_thread = old_current_thread;

	if (new_current_thread <= 0 || new_current_thread > thread_list_size) {
		/* gdb likes a valid thread number! */
		new_current_thread = 1;
		assert(thread_list_size);
	}

	rtos->current_thread = new_current_thread;

	/* create space for new thread details */
	rtos->thread_details = malloc(sizeof(struct thread_detail) * thread_list_size);
	if (!rtos->thread_details) {
		LOG_ERROR("Error allocating memory for %d threads", thread_list_size);
		return ERROR_FAIL;
	}

	head = rtos->target->head;

	int threadIdx = 0;
	while (head != (struct target_list *) NULL) {
		struct thread_detail *details = &rtos->thread_details[threadIdx++];
		metal_get_target_thread_details(head->target, threadIdx, details);
		head = head->next;
	}

	rtos->thread_count = threadIdx;
	return ERROR_OK;
}

static int metal_get_thread_reg_list(struct rtos *rtos, int64_t thread_id,
									 struct rtos_reg **reg_list, int *num_regs)
{
	struct target *target = metal_target_for_threadid_internal(rtos->target->head, thread_id, false);
	if (target) {
		struct reg **gdb_reg_list;

		int retval = target_get_gdb_reg_list(target, &gdb_reg_list, num_regs, REG_CLASS_GENERAL);
		if (retval != ERROR_OK)
			return retval;
		*reg_list = calloc(*num_regs, sizeof(struct rtos_reg));

		for (int i = 0; i < *num_regs; ++i) {
			if (!gdb_reg_list[i]->valid)
				gdb_reg_list[i]->type->get(gdb_reg_list[i]);

			(*reg_list)[i].number = gdb_reg_list[i]->number;
			(*reg_list)[i].size = gdb_reg_list[i]->size;

			buf_cpy(gdb_reg_list[i]->value, (*reg_list)[i].value, (*reg_list)[i].size);
		}
		free(gdb_reg_list);

		return ERROR_OK;
	}
	LOG_ERROR("current thread %" PRIx64 ": no target to perform access" PRIx32, thread_id);

	return ERROR_FAIL;
}

static int metal_os_smp_init(struct target *target)
{
	struct target_list *head;
	struct rtos *rtos = target->rtos;
	head = target->head;

	while (head != (struct target_list *) NULL) {
		if (head->target->rtos != rtos) {
			/*  remap smp target on rtos  */
			free(head->target->rtos);
			head->target->rtos = rtos;
		}
		head = head->next;
	}

	return ERROR_OK;
}

static int metal_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[])
{
	unsigned int i;
	*symbol_list = calloc(ARRAY_SIZE(metal_symbol_list), sizeof(symbol_table_elem_t));

	for (i = 0; i < ARRAY_SIZE(metal_symbol_list); i++)
		(*symbol_list)[i].symbol_name = metal_symbol_list[i];

	return 0;
}
