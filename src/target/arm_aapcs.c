/***************************************************************************
 *									   *
 *   Copyright (C) 2014 by Andrey Smirnov				   *
 *   andrew.smirnov@gmail.com						   *
 *									   *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or	   *
 *   (at your option) any later version.				   *
 *									   *
 *   This program is distributed in the hope that it will be useful,	   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of	   *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the	   *
 *   GNU General Public License for more details.			   *
 *									   *
 *   You should have received a copy of the GNU General Public License	   *
 *   along with this program; see the file COPYING.  If not see		   *
 *   <http://www.gnu.org/licenses/>					   *
 *									   *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <target.h>
#include <helper/log.h>

#include "register.h"

#include "arm.h"
#include "armv7m.h"
#include "armv4_5.h"
#include "algorithm.h"


struct arm_aapcs_call_artifacts {
	struct mem_param *mem_params;
	int num_mem_params;

	struct reg_param *reg_params;
	int num_reg_params;

	struct reg_param *lr, *sp;

	struct working_area *stack;
	struct working_area *exit_pad;

	uint32_t exit_pad_address;
};

enum arm_aapcs_registers {
	AAPCS_R0,
	AAPCS_R1,
	AAPCS_R2,
	AAPCS_R3,
	AAPCS_REG_COUNT,
	AAPCS_LR_SP_COUNT = 2,
	AAPCS_LAST_IN_OUT_REG = AAPCS_R1,
};


static void arm_aapcs_destroy_artifact_params(struct arm_aapcs_call_artifacts *artifacts)
{
	for (int i = 0; i < artifacts->num_reg_params; i++)
		destroy_reg_param(&artifacts->reg_params[i]);

	if (artifacts->lr)
		destroy_reg_param(artifacts->lr);
	if (artifacts->sp)
		destroy_reg_param(artifacts->sp);

	for (int i = 0; i < artifacts->num_mem_params; i++)
		destroy_mem_param(&artifacts->mem_params[i]);
}

void arm_aapcs_destroy_artifacts(struct target *target,
				 arm_aapcs_call_artifacts_t artifacts)
{
	if (artifacts == NULL)
		return;

	arm_aapcs_destroy_artifact_params(artifacts);

	if (artifacts->stack)
		target_free_working_area(target, artifacts->stack);

	if (artifacts->exit_pad)
		target_free_working_area(target, artifacts->exit_pad);

	free(artifacts->mem_params);
	free(artifacts->reg_params);
	free(artifacts);
}

static int arm_aapcs_call_async_v(struct target *target,
				  uint32_t address, size_t stack_size,
				  struct arm_algorithm *algorithm,
				  arm_aapcs_call_artifacts_t *artfs,
				  int argc, va_list args)
{
	assert(algorithm != NULL);
	assert(artfs != NULL);

	struct arm *arm = target_to_arm(target);

	int ret, err1, err2;
	uint32_t argv[argc];

	const char *lr_name, *sp_name;

	err1 = err2 = ERROR_OK;

	algorithm->bp_type = BKPT_SOFT;

	if (is_armv7m(target_to_armv7m(target))) {
		lr_name = "lr";
		sp_name = "sp";
	} else {
		struct reg *r;

		r = &ARMV4_5_CORE_REG_MODE(arm->core_cache,
					   algorithm->core_mode, 13);
		sp_name = r->name;

		r = &ARMV4_5_CORE_REG_MODE(arm->core_cache,
					   algorithm->core_mode, 14);
		lr_name = r->name;
	}

	LOG_DEBUG("Calling helper @0x%08x", address);
	for (int i = 0; i < argc; i++) {
		argv[i] = va_arg(args, uint32_t);
		LOG_DEBUG("argument #%d: 0x%08x", i, argv[i]);
	}

	struct arm_aapcs_call_artifacts *artifacts =
		calloc(1, sizeof(struct arm_aapcs_call_artifacts));
	if (!artifacts)
		return ERROR_FAIL;

	*artfs = artifacts;


	/*
	  AAPCS specifies that first four parameter to a funciton are
	  passed int registers(r0 .. r3)
	 */
	artifacts->num_reg_params = MIN(AAPCS_REG_COUNT, argc);
	/*
	  Rest of the parameters are passed via stack
	 */
	artifacts->num_mem_params = (argc > artifacts->num_reg_params) ?
		argc - artifacts->num_reg_params : 0;

	artifacts->reg_params = calloc(artifacts->num_reg_params,
				       sizeof(struct reg_param));

	if (artifacts->num_reg_params && !artifacts->reg_params) {
		ret = ERROR_FAIL;
		goto free_artifacts;
	}

	artifacts->mem_params = calloc(artifacts->num_mem_params,
				       sizeof(struct mem_param));

	if (artifacts->num_mem_params && !artifacts->mem_params) {
		ret = ERROR_FAIL;
		goto free_reg_params;
	}


	/*
	  We need to allocate memory for software breakpoint that
	  would be used to exit the call. Depending on the CPU mode it
	  may be 4 or 2 bytes in size, so we allocate 8 bytes in order
	  to always be able to provide 4 byte alignment
	 */
	ret = target_alloc_working_area(target, 8,
					&artifacts->exit_pad);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to allocate memory for exit breakpoint");
		goto free_mem_params;
	}

	/* Align by 4 byte boundary */
	artifacts->exit_pad_address = artifacts->exit_pad->address;
	artifacts->exit_pad_address &= 0xFFFFFFFC;
	artifacts->exit_pad_address |= 0x00000002;

	const size_t   parmeter_size = 4;
	const uint32_t stack_params_size = artifacts->num_mem_params * parmeter_size;
	const uint32_t total_stack_size  = stack_size + stack_params_size;

	ret = target_alloc_working_area(target, total_stack_size,
					&artifacts->stack);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to allocate stack for the AAPCS call to 0x%08" PRIx32, address);
		goto free_exit_pad;
	}

	/*
	  Calculate stack bottom and stack top taking into account
	  parameters passing. In cases of functions with less than 4
	  arguments stack_bottom == stack_top.
	*/
	const uint32_t stack_bottom = artifacts->stack->address + total_stack_size;
	const uint32_t stack_top    = stack_bottom - stack_params_size;

	struct reg_param *rparam;
	void *new_block;
	const char *reg_name[] = { "r0", "r1", "r2", "r3" };

	switch (argc) {
	default:
		for (int i = 0; i < artifacts->num_mem_params; i++) {
			init_mem_param(&artifacts->mem_params[i],
				       stack_top + parmeter_size * i,
				       parmeter_size, PARAM_OUT);
			/* We need to take into account endiannes of
			 * the target, hence the use of
			 * target_buffer_set_* */
			target_buffer_set_u32(target,
					      artifacts->mem_params[i].value,
					      argv[i + AAPCS_REG_COUNT]);
		}
	case 4:			/* FALLTHROUGH */
	case 3:			/* FALLTHROUGH */
	case 2:			/* FALLTHROUGH */
	case 1:			/* FALLTHROUGH */
		for (int i = artifacts->num_reg_params - 1; i >= 0; i--) {
			rparam = &artifacts->reg_params[i];
			/*
			 * Mark r0 and r1 as IN_OUT and the rest of
			 * the registers as OUT
			 */
			const enum param_direction direction =
				(i <= AAPCS_LAST_IN_OUT_REG) ? PARAM_IN_OUT : PARAM_OUT;

			init_reg_param(rparam, reg_name[i], 32, direction);
			buf_set_u32(rparam->value, 0, 32, argv[i]);
		}
	case 0:			/* FALLTHROUGH */
		/*
		  LR and SP have to be passed for every call, so to
		  simplify the code dealing with the paramteres we
		  just append them to the end of register parameter
		  array and treat them as some sort of "shadow
		  parameters". This way any code dealing with
		  artifacts->num_reg_params doesn't have to take into
		  account two additional parameters and have the code
		  to deal with it.
		 */
		new_block = realloc(artifacts->reg_params,
				    (artifacts->num_reg_params + AAPCS_LR_SP_COUNT) *
				    sizeof(struct reg_param));
		if (!new_block) {
			LOG_ERROR("Failed to allocate additional space for SP and LR registers");
			ret = ERROR_FAIL;
			goto destroy_params;
		} else {
			artifacts->reg_params = new_block;
			artifacts->lr = &artifacts->reg_params[artifacts->num_reg_params];
			artifacts->sp = &artifacts->reg_params[artifacts->num_reg_params + 1];

			memset(artifacts->lr, 0, sizeof(*artifacts->lr));
			memset(artifacts->sp, 0, sizeof(*artifacts->sp));
		}

		uint32_t __exit_pad_address = artifacts->exit_pad_address;

		ret = arm_fixup_execution_address(algorithm->core_state, &__exit_pad_address);
		if (ret != ERROR_OK)
			goto destroy_params;

		init_reg_param(artifacts->lr, lr_name, 32, PARAM_OUT);
		buf_set_u32(artifacts->lr->value, 0, 32, __exit_pad_address);

		init_reg_param(artifacts->sp, sp_name, 32, PARAM_OUT);
		buf_set_u32(artifacts->sp->value, 0, 32, stack_top);
	}

	ret = target_start_algorithm(target,
				     artifacts->num_mem_params,
				     artifacts->mem_params,
				     artifacts->num_reg_params +
				     AAPCS_LR_SP_COUNT,
				     artifacts->reg_params,
				     address,  artifacts->exit_pad_address,
				     algorithm);
	if (ret == ERROR_OK)
		return ret;

destroy_params:
	/* Failure cleanup */
	arm_aapcs_destroy_artifact_params(artifacts);

	err1 = target_free_working_area(target, artifacts->stack);
	if (err1 != ERROR_OK)
		LOG_ERROR("Faield to free stack working area (%d)", err1);

free_exit_pad:
	err2 = target_free_working_area(target, artifacts->exit_pad);
	if (err2 != ERROR_OK)
		LOG_ERROR("Faield to free 'exit pad' working area (%d)", err2);

free_mem_params:
	free(artifacts->mem_params);
free_reg_params:
	free(artifacts->reg_params);
free_artifacts:
	free(artifacts);
	*artfs = NULL;

	return ret ?: err1 ?: err2;
}

int arm_aapcs_call_async(struct target *target,
			 uint32_t address, size_t stack_size,
			 struct arm_algorithm *algorithm,
			 arm_aapcs_call_artifacts_t *artfs,
			 int argc, ...)
{
	int ret;
	va_list args;

	va_start(args, argc);
	ret = arm_aapcs_call_async_v(target, address, stack_size,
				     algorithm,
				     artfs, argc, args);
	va_end(args);

	return ret;
}

int arm_aapcs_call_wait(struct target *target,
			arm_aapcs_call_artifacts_t artifacts,
			int timeout_ms,
			uint32_t *result1, uint32_t *result2)
{
	const int ret = target_wait_algorithm(target,
					      artifacts->num_mem_params,
					      artifacts->mem_params,
					      artifacts->num_reg_params +
					      AAPCS_LR_SP_COUNT,
					      artifacts->reg_params,
					      artifacts->exit_pad_address,
					      timeout_ms,
					      NULL);
	if (ret == ERROR_OK) {
		if (result1)
			*result1 = buf_get_u32(artifacts->reg_params[AAPCS_R0].value, 0, 32);
		if (result2)
			*result2 = buf_get_u32(artifacts->reg_params[AAPCS_R1].value, 0, 32);
	}

	arm_aapcs_destroy_artifacts(target, artifacts);

	return ret;
}

int arm_aapcs_call(struct target *target,
		   uint32_t address, size_t stack_size,
		   struct arm_algorithm *algorithm,
		   int timeout_ms,
		   uint32_t *result1, uint32_t *result2,
		   int argc, ...)
{

	int ret;
	va_list args;

	va_start(args, argc);
	ret = arm_aapcs_call_v(target, address, stack_size,
			       algorithm, timeout_ms,
			       result1, result2,
			       argc, args);
	va_end(args);


	return ret;
}


int arm_aapcs_call_v(struct target *target,
		     uint32_t address, size_t stack_size,
		     struct arm_algorithm *algorithm,
		     int timeout_ms,
		     uint32_t *result1, uint32_t *result2,
		     int argc, va_list args)
{

	int ret;
	arm_aapcs_call_artifacts_t artifacts;

	ret = arm_aapcs_call_async_v(target, address, stack_size,
				     algorithm,
				     &artifacts, argc, args);

	if (ret == ERROR_OK)
		ret = arm_aapcs_call_wait(target, artifacts, timeout_ms,
					  result1, result2);

	return ret;
}
