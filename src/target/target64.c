/***************************************************************************
 *   Copyright (C) 2015 by Alamy Liu                                       *
 *   alamy.liu@gmail.com                                                   *
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

#include <flash/common.h>
#include <helper/time_support.h>
#include <helper/binarybuffer.h>

#include "register.h"

#include "target.h"
#include "target_type.h"
#include "target_type64.h"
//#include "target_request.h"
//#include "breakpoints.h"


/* 64-bit targets */
extern struct target_type_64 aarch64_target;

static struct target_type_64 *target_types_64[] = {
	&aarch64_target,
	NULL,
};

/**
 * Executes a target-specific native code algorithm in the target.
 * It differs from target_run_algorithm in that the algorithm is asynchronous.
 * Because of this it requires an compliant algorithm:
 * see contrib/loaders/flash/stm32f1x.S for example.
 *
 * @param target used to run the algorithm
 */

int target_run_flash_async_algorithm_64(struct target *target,
		const uint8_t *buffer, uint32_t count, int block_size,
		int num_mem_params, struct mem_param *mem_params,
		int num_reg_params, struct reg_param *reg_params,
		uintmax_t buffer_start, uintmax_t buffer_size,
		uintmax_t entry_point, uintmax_t exit_point, void *arch_info)
{
	int retval;
	int timeout = 0;

	const uint8_t *buffer_orig = buffer;

	/* Set up working area. First word is write pointer, second word is read pointer,
	 * rest is fifo data area. */
	uintmax_t wp_addr = buffer_start;
	uintmax_t rp_addr = buffer_start + 4;			/* Alamy: 4/8 ? */
	uintmax_t fifo_start_addr = buffer_start + 8;	/* Alamy: ? */
	uintmax_t fifo_end_addr = buffer_start + buffer_size;

	uintmax_t wp = fifo_start_addr;
	uintmax_t rp = fifo_start_addr;

	/* validate block_size is 2^n */
	assert(!block_size || !(block_size & (block_size - 1)));

	retval = target_write_u32(target, wp_addr, wp);	/* Alamy: write_u64 ? */
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, rp_addr, rp);	/* Alamy: write_u64 ? */
	if (retval != ERROR_OK)
		return retval;

	/* Start up algorithm on target and let it idle while writing the first chunk */
	retval = target_start_algorithm(target, num_mem_params, mem_params,
			num_reg_params, reg_params,
			entry_point,
			exit_point,
			arch_info);

	if (retval != ERROR_OK) {
		LOG_ERROR("error starting target flash write algorithm");
		return retval;
	}

	while (count > 0) {

		retval = target_read_u64(target, rp_addr, &rp);
		if (retval != ERROR_OK) {
			LOG_ERROR("failed to get read pointer");
			break;
		}

		LOG_DEBUG("offs 0x%zx count 0x%" PRIx32 " wp 0x%.*" PRIXMAX " rp 0x%.*" PRIXMAX,
			(size_t) (buffer - buffer_orig), count,
			addr_fmt_width(target), wp,
			addr_fmt_width(target), rp);

		if (rp == 0) {
			LOG_ERROR("flash write algorithm aborted by target");
			retval = ERROR_FLASH_OPERATION_FAILED;
			break;
		}

		if (((rp - fifo_start_addr) & (block_size - 1)) || rp < fifo_start_addr || rp >= fifo_end_addr) {
			LOG_ERROR("corrupted fifo read pointer 0x%.*" PRIXMAX,
				addr_fmt_width(target), rp);
			break;
		}

		/* Count the number of bytes available in the fifo without
		 * crossing the wrap around. Make sure to not fill it completely,
		 * because that would make wp == rp and that's the empty condition. */
		uint32_t thisrun_bytes;
		if (rp > wp)
			thisrun_bytes = rp - wp - block_size;
		else if (rp > fifo_start_addr)
			thisrun_bytes = fifo_end_addr - wp;
		else
			thisrun_bytes = fifo_end_addr - wp - block_size;

		if (thisrun_bytes == 0) {
			/* Throttle polling a bit if transfer is (much) faster than flash
			 * programming. The exact delay shouldn't matter as long as it's
			 * less than buffer size / flash speed. This is very unlikely to
			 * run when using high latency connections such as USB. */
			alive_sleep(10);

			/* to stop an infinite loop on some targets check and increment a timeout
			 * this issue was observed on a stellaris using the new ICDI interface */
			if (timeout++ >= 500) {
				LOG_ERROR("timeout waiting for algorithm, a target reset is recommended");
				return ERROR_FLASH_OPERATION_FAILED;
			}
			continue;
		}

		/* reset our timeout */
		timeout = 0;

		/* Limit to the amount of data we actually want to write */
		if (thisrun_bytes > count * block_size)
			thisrun_bytes = count * block_size;

		/* Write data to fifo */
		retval = target_write_buffer(target, wp, thisrun_bytes, buffer);
		if (retval != ERROR_OK)
			break;

		/* Update counters and wrap write pointer */
		buffer += thisrun_bytes;
		count -= thisrun_bytes / block_size;
		wp += thisrun_bytes;
		if (wp >= fifo_end_addr)
			wp = fifo_start_addr;

		/* Store updated write pointer to target */
		retval = target_write_u64(target, wp_addr, wp);
		if (retval != ERROR_OK)
			break;
	}

	if (retval != ERROR_OK) {
		/* abort flash write algorithm on target */
		target_write_u64(target, wp_addr, 0);
	}

	int retval2 = target_wait_algorithm(target, num_mem_params, mem_params,
			num_reg_params, reg_params,
			exit_point,
			10000,
			arch_info);

	if (retval2 != ERROR_OK) {
		LOG_ERROR("error waiting for target flash write algorithm");
		retval = retval2;
	}

	return retval;
}


static int default_examine_64(struct target *target)
{
	target_set_examined(target);
	return ERROR_OK;
}

/* no check by default */
static int default_check_reset_64(struct target *target)
{
	return ERROR_OK;
}

static int identity_virt2phys_64(struct target *target,
		uint64_t virtual, uint64_t *physical)
{
	*physical = virtual;
	return ERROR_OK;
}

static int no_mmu_64(struct target *target, int *enabled)
{
	*enabled = 0;
	return ERROR_OK;
}

static int target_read_buffer_default_64(
	struct target *target,
	uint64_t address,
	uint32_t count,
	uint8_t *buffer)
{
	uint32_t size;

	/* Align up to maximum 4 bytes. The loop condition makes sure the next pass
	 * will have something to do with the size we leave to it. */
	for (size = 1; size < 4 && count >= size * 2 + (address & size); size *= 2) {
		if (address & size) {
			int retval = target_read_memory(target, address, size, 1, buffer);
			if (retval != ERROR_OK)
				return retval;
			address += size;
			count -= size;
			buffer += size;
		}
	}

	/* Read the data with as large access size as possible. */
	for (; size > 0; size /= 2) {
		uint32_t aligned = count - count % size;
		if (aligned > 0) {
			int retval = target_read_memory(target, address, size, aligned / size, buffer);
			if (retval != ERROR_OK)
				return retval;
			address += aligned;
			count -= aligned;
			buffer += aligned;
		}
	}

	return ERROR_OK;
}

static int target_write_buffer_default_64(
	struct target *target,
	uint64_t address,
	uint32_t count,
	const uint8_t *buffer)
{
	uint32_t size;

	/* Align up to maximum 4 bytes. The loop condition makes sure the next pass
	 * will have something to do with the size we leave to it. */
	for (size = 1; size < 4 && count >= size * 2 + (address & size); size *= 2) {
		if (address & size) {
			int retval = target_write_memory(target, address, size, 1, buffer);
			if (retval != ERROR_OK)
				return retval;
			address += size;
			count -= size;
			buffer += size;
		}
	}

	/* Write the data with as large access size as possible. */
	for (; size > 0; size /= 2) {
		uint32_t aligned = count - count % size;
		if (aligned > 0) {
			int retval = target_write_memory(target, address, size, aligned / size, buffer);
			if (retval != ERROR_OK)
				return retval;
			address += aligned;
			count -= aligned;
			buffer += aligned;
		}
	}

	return ERROR_OK;
}

static int target_get_gdb_fileio_info_default_64(struct target *target,
		struct gdb_fileio_info *fileio_info)
{
	/* If target does not support semi-hosting function, target
	   has no need to provide .get_gdb_fileio_info callback.
	   It just return ERROR_FAIL and gdb_server will return "Txx"
	   as target halted every time.  */
	return ERROR_FAIL;
}

static int target_gdb_fileio_end_default_64(struct target *target,
		int retcode, int fileio_errno, bool ctrl_c)
{
	return ERROR_OK;
}

static int target_profiling_default_64(struct target *target, uint64_t *samples,
		uint32_t max_num_samples, uint32_t *num_samples, uint32_t seconds)
{
	struct timeval timeout, now;

	gettimeofday(&timeout, NULL);
	timeval_add_time(&timeout, seconds, 0);

	LOG_INFO("Starting profiling. Halting and resuming the"
			" target as often as we can...");

	uint32_t sample_count = 0;
	/* hopefully it is safe to cache! We want to stop/restart as quickly as possible. */
	struct reg *reg = register_get_by_name(target->reg_cache, "pc", 1);	/* ERROR */

	int retval = ERROR_OK;
	for (;;) {
		target_poll(target);
		if (target->state == TARGET_HALTED) {
			uint32_t t = buf_get_u32(reg->value, 0, 32);	/* ERROR */
			samples[sample_count++] = t;
			/* current pc, addr = 0, do not handle breakpoints, not debugging */
			retval = target_resume(target, 1, 0, 0, 0);
			target_poll(target);
			alive_sleep(10); /* sleep 10ms, i.e. <100 samples/second. */
		} else if (target->state == TARGET_RUNNING) {
			/* We want to quickly sample the PC. */
			retval = target_halt(target);
		} else {
			LOG_INFO("Target not halted or running");
			retval = ERROR_OK;
			break;
		}

		if (retval != ERROR_OK)
			break;

		gettimeofday(&now, NULL);
		if ((sample_count >= max_num_samples) ||
			((now.tv_sec >= timeout.tv_sec) && (now.tv_usec >= timeout.tv_usec))) {
			LOG_INFO("Profiling completed. %" PRIu32 " samples.", sample_count);
			break;
		}
	}

	*num_samples = sample_count;
	return retval;
}

/* Duplicated from target.c */
static inline void target_reset_examined(struct target *target)
{
	target->examined = false;
}

#if 0
static int target_create_64(Jim_GetOptInfo *goi)
{
	Jim_Obj *new_cmd;
	Jim_Cmd *cmd;
	const char *cp;
	char *cp2;
	int e;
	int x;
	struct target *target;
	struct command_context *cmd_ctx;

	cmd_ctx = current_command_context(goi->interp);
	assert(cmd_ctx != NULL);

	if (goi->argc < 3) {
		Jim_WrongNumArgs(goi->interp, 1, goi->argv, "?name? ?type? ..options...");
		return JIM_ERR;
	}

	/* COMMAND */
	Jim_GetOpt_Obj(goi, &new_cmd);
	/* does this command exist? */
	cmd = Jim_GetCommand(goi->interp, new_cmd, JIM_ERRMSG);
	if (cmd) {
		cp = Jim_GetString(new_cmd, NULL);
		Jim_SetResultFormatted(goi->interp, "Command/target: %s Exists", cp);
		return JIM_ERR;
	}

	/* TYPE */
	e = Jim_GetOpt_String(goi, &cp2, NULL);
	if (e != JIM_OK)
		return e;
	cp = cp2;
	struct transport *tr = get_current_transport();
	if (tr->override_target) {
		e = tr->override_target(&cp);
		if (e != ERROR_OK) {
			LOG_ERROR("The selected transport doesn't support this target");
			return JIM_ERR;
		}
		LOG_INFO("The selected transport took over low-level target control. The results might differ compared to plain JTAG/SWD");
	}
	/* now does target type exist */
	for (x = 0 ; target_types[x] ; x++) {
		if (0 == strcmp(cp, target_types[x]->name)) {
			/* found */
			break;
		}

		/* check for deprecated name */
		if (target_types[x]->deprecated_name) {
			if (0 == strcmp(cp, target_types[x]->deprecated_name)) {
				/* found */
				LOG_WARNING("target name is deprecated use: \'%s\'", target_types[x]->name);
				break;
			}
		}
	}
	if (target_types[x] == NULL) {
		Jim_SetResultFormatted(goi->interp, "Unknown target type %s, try one of ", cp);
		for (x = 0 ; target_types[x] ; x++) {
			if (target_types[x + 1]) {
				Jim_AppendStrings(goi->interp,
								   Jim_GetResult(goi->interp),
								   target_types[x]->name,
								   ", ", NULL);
			} else {
				Jim_AppendStrings(goi->interp,
								   Jim_GetResult(goi->interp),
								   " or ",
								   target_types[x]->name, NULL);
			}
		}
		return JIM_ERR;
	}

	/* Create it */
	target = calloc(1, sizeof(struct target));
	/* set target number */
	target->target_number = new_target_number();
	cmd_ctx->current_target = target->target_number;

	/* allocate memory for each unique target type */
	target->type = calloc(1, sizeof(struct target_type));
	/* target->type64 = calloc(1, sizeof(struct target_type64)); */	/* Alamy */

	memcpy(target->type, target_types[x], sizeof(struct target_type));

	/* will be set by "-endian" */
	target->endianness = TARGET_ENDIAN_UNKNOWN;

	/* default to first core, override with -coreid */
	target->coreid = 0;

	target->working_area        = 0x0;
	target->working_area_size   = 0x0;
	target->working_areas       = NULL;
	target->backup_working_area = 0;

	target->state               = TARGET_UNKNOWN;
	target->debug_reason        = DBG_REASON_UNDEFINED;
	target->reg_cache           = NULL;
	target->breakpoints         = NULL;
	target->watchpoints         = NULL;
	target->next                = NULL;
	target->arch_info           = NULL;

	target->display             = 1;

	target->halt_issued			= false;

	/* initialize trace information */
	target->trace_info = malloc(sizeof(struct trace));
	target->trace_info->num_trace_points         = 0;
	target->trace_info->trace_points_size        = 0;
	target->trace_info->trace_points             = NULL;
	target->trace_info->trace_history_size       = 0;
	target->trace_info->trace_history            = NULL;
	target->trace_info->trace_history_pos        = 0;
	target->trace_info->trace_history_overflowed = 0;

	target->dbgmsg          = NULL;
	target->dbg_msg_enabled = 0;

	target->endianness = TARGET_ENDIAN_UNKNOWN;

	target->rtos = NULL;
	target->rtos_auto_detect = false;

	/* Do the rest as "configure" options */
	goi->isconfigure = 1;
	e = target_configure(goi, target);

	if (target->tap == NULL) {
		Jim_SetResultString(goi->interp, "-chain-position required when creating target", -1);
		e = JIM_ERR;
	}

	if (e != JIM_OK) {
		free(target->type);
		free(target);
		return e;
	}

	if (target->endianness == TARGET_ENDIAN_UNKNOWN) {
		/* default endian to little if not specified */
		target->endianness = TARGET_LITTLE_ENDIAN;
	}

	cp = Jim_GetString(new_cmd, NULL);
	target->cmd_name = strdup(cp);

	/* create the target specific commands */
	if (target->type->commands) {
		e = register_commands(cmd_ctx, NULL, target->type->commands);
		if (ERROR_OK != e)
			LOG_ERROR("unable to register '%s' commands", cp);
	}
	if (target->type->target_create)
		(*(target->type->target_create))(target, goi->interp);

	/* append to end of list */
	{
		struct target **tpp;
		tpp = &(all_targets);
		while (*tpp)
			tpp = &((*tpp)->next);
		*tpp = target;
	}

	/* now - create the new target name command */
	const struct command_registration target_subcommands[] = {
		{
			.chain = target_instance_command_handlers,
		},
		{
			.chain = target->type->commands,
		},
		COMMAND_REGISTRATION_DONE
	};
	const struct command_registration target_commands[] = {
		{
			.name = cp,
			.mode = COMMAND_ANY,
			.help = "target command group",
			.usage = "",
			.chain = target_subcommands,
		},
		COMMAND_REGISTRATION_DONE
	};
	e = register_commands(cmd_ctx, NULL, target_commands);
	if (ERROR_OK != e)
		return JIM_ERR;

	struct command *c = command_find_in_context(cmd_ctx, cp);
	assert(c);
	command_set_handler_data(c, target);

	return (ERROR_OK == e) ? JIM_OK : JIM_ERR;
}
#endif

int target_init_one_64(
	struct command_context *cmd_ctx,
	struct target *target)
{
	target_reset_examined(target);

	struct target_type_64 *type64 = target->type64;
	if (type64->examine == NULL)
		type64->examine = default_examine_64;

	if (type64->check_reset == NULL)
		type64->check_reset = default_check_reset_64;

	assert(type64->init_target != NULL);

	int retval = type64->init_target(cmd_ctx, target);
	if (ERROR_OK != retval) {
		LOG_ERROR("target '%s' init failed", target_name(target));
		return retval;
	}

	/* Sanity-check MMU support ... stub in what we must, to help
	 * implement it in stages, but warn if we need to do so.
	 */
	if (type64->mmu) {
		if (type64->virt2phys == NULL) {
			LOG_ERROR("type '%s' is missing virt2phys", type64->name);
			type64->virt2phys = identity_virt2phys_64;
		}
	} else {
		/* Make sure no-MMU targets all behave the same:  make no
		 * distinction between physical and virtual addresses, and
		 * ensure that virt2phys() is always an identity mapping.
		 */
		if (type64->write_phys_memory || type64->read_phys_memory || type64->virt2phys)
			LOG_WARNING("type '%s' has bad MMU hooks", type64->name);

		type64->mmu = no_mmu_64;
		type64->write_phys_memory = type64->write_memory;
		type64->read_phys_memory = type64->read_memory;
		type64->virt2phys = identity_virt2phys_64;
	}

	if (type64->read_buffer == NULL)
		type64->read_buffer = target_read_buffer_default_64;

	if (type64->write_buffer == NULL)
		type64->write_buffer = target_write_buffer_default_64;

	if (type64->get_gdb_fileio_info == NULL)
		type64->get_gdb_fileio_info = target_get_gdb_fileio_info_default_64;

	if (type64->gdb_fileio_end == NULL)
		type64->gdb_fileio_end = target_gdb_fileio_end_default_64;

	if (type64->profiling == NULL)
		type64->profiling = target_profiling_default_64;

	return ERROR_OK;
}

/* Looking for target in 64-bit target type table */
struct target_type_64 *scan_target_type_64(Jim_GetOptInfo *goi, const char *cp)
{
	int i;

	for (i = 0; target_types_64[i]; i++) {
		if (0 == strcmp(cp, target_types_64[i]->name))
			return target_types_64[i];	/* found */
	}

	/* Not found with target name */
	Jim_SetResultFormatted(goi->interp, "Unknown target type %s, try one of ", cp);
	for (i = 0; target_types_64[i]; i++) {
		if (target_types_64[i + 1]) {
			Jim_AppendStrings(goi->interp,
							Jim_GetResult(goi->interp),
							target_types_64[i]->name,
							", ", NULL);
		} else {
			Jim_AppendStrings(goi->interp,
							Jim_GetResult(goi->interp),
							" or ",
							target_types_64[i]->name, NULL);
		}
	}

	return NULL;
}


