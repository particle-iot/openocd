/***************************************************************************
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by David T.L. Wong                                 *
 *                                                                         *
 *   Copyright (C) 2009 by David N. Claffey <dnclaffey@gmail.com>          *
 *                                                                         *
 *   Copyright (C) 2011 by Drasko DRASKOVIC                                *
 *   drasko.draskovic@gmail.com                                            *
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

/*
 * This version has optimized assembly routines for 32 bit operations:
 * - read word
 * - write word
 * - write array of words
 *
 * One thing to be aware of is that the MIPS32 cpu will execute the
 * instruction after a branch instruction (one delay slot).
 *
 * For example:
 *  LW $2, ($5 +10)
 *  B foo
 *  LW $1, ($2 +100)
 *
 * The LW $1, ($2 +100) instruction is also executed. If this is
 * not wanted a NOP can be inserted:
 *
 *  LW $2, ($5 +10)
 *  B foo
 *  NOP
 *  LW $1, ($2 +100)
 *
 * or the code can be changed to:
 *
 *  B foo
 *  LW $2, ($5 +10)
 *  LW $1, ($2 +100)
 *
 * The original code contained NOPs. I have removed these and moved
 * the branches.
 *
 * These changes result in a 35% speed increase when programming an
 * external flash.
 *
 * More improvement could be gained if the registers do no need
 * to be preserved but in that case the routines should be aware
 * OpenOCD is used as a flash programmer or as a debug tool.
 *
 * Nico Coesel
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/time_support.h>

#include "mips32.h"
#include "mips32_pracc.h"

const char *excep_code[] = {EXCEPTION_CODE_LIST};

static int wait_for_pracc_rw(struct mips_ejtag *ejtag_info, bool read_addr)
{
	int64_t then = timeval_ms();
	jtag_add_clocks(ejtag_info->clocks);
	while (1) {
		mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
		ejtag_info->pa_ctrl = ejtag_info->ejtag_ctrl;
		uint8_t ctrl_scan[4];
		mips_ejtag_drscan_32_queued(ejtag_info, ejtag_info->pa_ctrl, ctrl_scan);	/* queued */

		int retval;
		if (read_addr) {
			mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ADDRESS);
			ejtag_info->pa_addr = 0;
			retval = mips_ejtag_drscan_32(ejtag_info, &ejtag_info->pa_addr); /* request execution */
		} else
			retval = jtag_execute_queue();		/* request execution */
		if (retval != ERROR_OK)
			return retval;

		ejtag_info->pa_ctrl = buf_get_u32(ctrl_scan, 0, 32);
		if (ejtag_info->pa_ctrl & EJTAG_CTRL_PRACC)		/* if pracc bit set, done */
			break;

		int64_t timeout = timeval_ms() - then;
		if (timeout > 1000) {
			LOG_DEBUG("DEBUGMODULE: No memory access in progress!");
			return ERROR_JTAG_DEVICE_ERROR;
		}
	}
	return ERROR_OK;
}

/* Finish processor access */
static void mips32_pracc_finish(struct mips_ejtag *ejtag_info)
{
	uint32_t ctrl = ejtag_info->ejtag_ctrl & ~EJTAG_CTRL_PRACC;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	mips_ejtag_drscan_32_out(ejtag_info, ctrl);
}

int mips32_pracc_clean_text_jump(struct mips_ejtag *ejtag_info)
{
	uint32_t jt_code = MIPS32_J(ejtag_info->isa, MIPS32_PRACC_TEXT);
	pracc_swap16_array(ejtag_info, &jt_code, 1);
	/* do 3 0/nops to clean pipeline before a jump to pracc text, NOP in delay slot */
	for (int i = 0; i != 5; i++) {
		/* Wait for pracc */
		int retval = wait_for_pracc_rw(ejtag_info, 0);
		if (retval != ERROR_OK)
			return retval;

		/* Data or instruction out */
		mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
		uint32_t data = (i == 3) ? jt_code : MIPS32_NOP;
		mips_ejtag_drscan_32_out(ejtag_info, data);

		/* finish pa */
		mips32_pracc_finish(ejtag_info);
	}

	if (ejtag_info->mode[pa_mode] == opt_async)	/* async mode support only for MIPS ... */
		return ERROR_OK;

	for (int i = 0; i != 2; i++) {
		int retval = wait_for_pracc_rw(ejtag_info, READ_ADDR);
		if (retval != ERROR_OK)
			return retval;

		if (ejtag_info->pa_addr != MIPS32_PRACC_TEXT) {	/* LEXRA/BMIPS ?, shift out another NOP, max 2 */
			mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
			mips_ejtag_drscan_32_out(ejtag_info, MIPS32_NOP);
			mips32_pracc_finish(ejtag_info);
		} else
			break;
	}

	return ERROR_OK;
}

static void pracc_log_debug_mode_exception_info(struct mips_ejtag *ejtag_info, bool restore_regs)
{
	if (ejtag_info->exception_check)	/* avoid recursive calls */
		return;

	ejtag_info->exception_check = 1;

	uint32_t val;
	int retval = mips32_cp0_read(ejtag_info, &val, 23, 0);	/* read Cp0 Debug register */
	uint32_t exc_addr;
	if (retval == ERROR_OK)
		retval = mips32_cp0_read(ejtag_info, &exc_addr, 24, 0);	/* read Cp0 DEPC register */

	if (retval == ERROR_OK && (val & EJTAG_DEBUG_CAUSE_MASK) == 0)
		LOG_WARNING("Debug mode exception, code: %s, triggered at address: %"PRIx32,
			excep_code[(val & EJTAG_DEBUG_EXCEPCODE_MASK) >> EJTAG_DEBUG_EXCEPCODE_SHIFT], exc_addr);

	if (restore_regs)	/* current pracc functions only take 3 registers at most */
		mips32_pracc_restore_working_regs(ejtag_info, 8, 9);	/* restore $8, $9 and $15 */

	ejtag_info->exception_check = 0;
}

int mips32_pracc_exec(struct mips_ejtag *ejtag_info, struct pracc_queue_info *ctx,
					uint32_t *param_out, bool check_last)
{
	int code_count = 0;
	int store_pending = 0;		/* increases with every store instr at dmseg, decreases with every store pa */
	uint32_t max_store_addr = 0;	/* for store pa address testing */
	bool restart = 0;		/* restarting control */
	int restart_count = 0;
	uint32_t instr = 0;
	bool final_check = 0;		/* set to 1 if in final checks after function code shifted out */
	bool pass = 0;			/* to check the pass through pracc text after function code sent */
	int retval;
	bool data_read_pending = 0;	/* to check if data read is pending from previous pracc access */
	uint8_t data_scan_pending[4];	/* buffer to scan in ejtag data register */
	uint32_t addr_of_data_pending;

	while (1) {
		if (restart) {
			if (restart_count < 3) {					/* max 3 restarts allowed */
				retval = mips32_pracc_clean_text_jump(ejtag_info);
				if (retval != ERROR_OK)
					return retval;
			} else
				return ERROR_JTAG_DEVICE_ERROR;
			restart_count++;
			restart = 0;
			code_count = 0;
			LOG_DEBUG("restarting code");
		}

		retval = wait_for_pracc_rw(ejtag_info, READ_ADDR); /* update current pa info: control and address */
		if (retval != ERROR_OK)
			return retval;

		if (data_read_pending) {	/* data is now available */
			/* store data at param out, address based offset */
			param_out[(addr_of_data_pending - MIPS32_PRACC_PARAM_OUT) / 4] =
								buf_get_u32(data_scan_pending, 0, 32);
			data_read_pending = 0;
		}

		/* Check for read or write access */
		if (ejtag_info->pa_ctrl & EJTAG_CTRL_PRNW) {				/* write/store access */
			/* Check for pending store from a previous store instruction at dmseg */
			if (store_pending == 0) {
				LOG_DEBUG("unexpected write at address %" PRIx32, ejtag_info->pa_addr);
				if (code_count < 2 && !final_check) {	/* allow for restart */
					restart = 1;
					continue;
				} else
					return ERROR_JTAG_DEVICE_ERROR;
			} else {
				/* check address */
				if (ejtag_info->pa_addr < MIPS32_PRACC_PARAM_OUT ||
						ejtag_info->pa_addr > max_store_addr) {
					LOG_DEBUG("writing at unexpected address %" PRIx32, ejtag_info->pa_addr);
					return ERROR_JTAG_DEVICE_ERROR;
				}
			}
			/* add data scan queued, read after queue executed */
			mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
			mips_ejtag_drscan_32_queued(ejtag_info, 0, data_scan_pending);
			data_read_pending = 1;
			addr_of_data_pending = ejtag_info->pa_addr;

			store_pending--;

		} else {					/* read/fetch access */
			 if (!final_check) {			/* executing function code */
				/* check address */
				if (ejtag_info->pa_addr != (MIPS32_PRACC_TEXT + code_count * 4)) {
					LOG_DEBUG("reading at unexpected address %" PRIx32 ", expected %x",
							ejtag_info->pa_addr, MIPS32_PRACC_TEXT + code_count * 4);

					/* restart code execution only in some cases */
					if (code_count == 1 && ejtag_info->pa_addr == MIPS32_PRACC_TEXT &&
										restart_count == 0) {
						LOG_DEBUG("restarting, without clean jump");
						restart_count++;
						code_count = 0;
						continue;
					} else if (code_count < 2) {
						restart = 1;
						continue;
					}

					if (ejtag_info->pa_addr == MIPS32_PRACC_TEXT)
						return ERROR_PRACC_TEXT_JUMP;
					else
						return ERROR_JTAG_DEVICE_ERROR;
				}
				/* check for store instruction at dmseg */
				uint32_t store_addr = ctx->pracc_list[code_count].addr;
				if (store_addr != 0) {
					if (store_addr > max_store_addr)
						max_store_addr = store_addr;
					store_pending++;
				}

				instr = ctx->pracc_list[code_count++].instr;
				if (code_count == ctx->code_count)	/* last instruction, start final check */
					final_check = 1;

			 } else {	/* final check after function code shifted out */
					/* check address */
				if (ejtag_info->pa_addr == MIPS32_PRACC_TEXT) {
					if (!pass) {	/* first pass through pracc text */
						if (store_pending == 0)		/* done, normal exit */
							return ERROR_OK;
						pass = 1;		/* pracc text passed */
						code_count = 0;		/* restart code count */
					} else {
						LOG_DEBUG("unexpected second pass through pracc text");
						return ERROR_JTAG_DEVICE_ERROR;
					}
				} else {
					if (ejtag_info->pa_addr != (MIPS32_PRACC_TEXT + code_count * 4)) {
						LOG_DEBUG("unexpected read address in final check: %"
							PRIx32 ", expected: %x", ejtag_info->pa_addr,
							MIPS32_PRACC_TEXT + code_count * 4);
						return ERROR_JTAG_DEVICE_ERROR;
					}
				}
				if (!pass) {
					if ((code_count - ctx->code_count) > 1) { /* allow max 2 instr delay slot */
						LOG_DEBUG("failed to jump back to pracc text");
						return ERROR_JTAG_DEVICE_ERROR;
					}
				} else
					if (code_count > 10) {		/* enough, abandone */
						LOG_DEBUG("execution abandoned, store pending: %d", store_pending);
						return ERROR_JTAG_DEVICE_ERROR;
					}
				instr = MIPS32_NOP;	/* shift out NOPs instructions */
				code_count++;
			 }

			/* Send instruction out */
			mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
			mips_ejtag_drscan_32_out(ejtag_info, instr);
		}
		/* finish processor access, let the processor eat! */
		mips32_pracc_finish(ejtag_info);

		if (final_check && !check_last)			/* last instr, don't check, execute and exit */
			return jtag_execute_queue();

		if (store_pending == 0 &&  data_read_pending == 0 && pass) {
			/* store access done, but after passing pracc text */
			LOG_DEBUG("warning: store access pass pracc text");
			return ERROR_OK;
		}
	}
}

inline void pracc_queue_init(struct pracc_queue_info *ctx)
{
	ctx->retval = ERROR_OK;
	ctx->code_count = 0;
	ctx->store_count = 0;
	ctx->max_code = 0;
	ctx->pracc_list = NULL;
	ctx->isa = ctx->ejtag_info->isa ? 1 : 0;
}

void pracc_add(struct pracc_queue_info *ctx, uint32_t addr, uint32_t instr)
{
	if (ctx->retval != ERROR_OK)	/* On previous out of memory, return */
		return;
	if (ctx->code_count == ctx->max_code) {
		void *p = realloc(ctx->pracc_list, sizeof(pa_list) * (ctx->max_code + PRACC_BLOCK));
		if (p) {
			ctx->max_code += PRACC_BLOCK;
			ctx->pracc_list = p;
		} else {
			ctx->retval = ERROR_FAIL;	/* Out of memory */
			return;
		}
	}
	ctx->pracc_list[ctx->code_count].instr = instr;
	ctx->pracc_list[ctx->code_count++].addr = addr;
	if (addr)
		ctx->store_count++;
}

void pracc_add_li32(struct pracc_queue_info *ctx, uint32_t reg_num, uint32_t data, bool optimize)
{
	if (LOWER16(data) == 0 && optimize)
		pracc_add(ctx, 0, MIPS32_LUI(ctx->isa, reg_num, UPPER16(data)));	/* load only upper value */
	else if (UPPER16(data) == 0 && optimize)
		pracc_add(ctx, 0, MIPS32_ORI(ctx->isa, reg_num, 0, LOWER16(data)));	/* load only lower */
	else {
		pracc_add(ctx, 0, MIPS32_LUI(ctx->isa, reg_num, UPPER16(data)));	/* load upper and lower */
		pracc_add(ctx, 0, MIPS32_ORI(ctx->isa, reg_num, reg_num, LOWER16(data)));
	}
}

inline void pracc_queue_free(struct pracc_queue_info *ctx)
{
	if (ctx->pracc_list != NULL)
		free(ctx->pracc_list);

	if (ctx->retval == ERROR_PRACC_TEXT_JUMP) {
		ctx->retval = ERROR_JTAG_DEVICE_ERROR;	/* change to standard error code */
		pracc_log_debug_mode_exception_info(ctx->ejtag_info, 1);	/* also restore working regs */
	}
}

int mips32_pracc_queue_exec(struct mips_ejtag *ejtag_info, struct pracc_queue_info *ctx,
					uint32_t *buf, bool check_last)
{
	if (ctx->retval != ERROR_OK) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}

	if (ejtag_info->isa && ejtag_info->endianness)
		for (int i = 0; i != ctx->code_count; i++)
			ctx->pracc_list[i].instr = SWAP16(ctx->pracc_list[i].instr);

	mips_ejtag_update_clocks(ejtag_info);

	if (ejtag_info->mode[pa_mode] == opt_sync || ejtag_info->mode[core_mode] == opt_bmips32)
		return mips32_pracc_exec(ejtag_info, ctx, buf, check_last);

	union scan_in {
		uint8_t scan_96[12];
		struct {
			uint8_t ctrl[4];
			uint8_t data[4];
			uint8_t addr[4];
		} scan_32;

	} *scan_in = malloc(sizeof(union scan_in) * (ctx->code_count + ctx->store_count));
	if (scan_in == NULL) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}

	uint32_t ejtag_ctrl = ejtag_info->ejtag_ctrl & ~EJTAG_CTRL_PRACC;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ALL);

	int scan_count = 0;
	for (int i = 0; i != ctx->code_count; i++) {
		jtag_add_clocks(ejtag_info->clocks);
		mips_ejtag_add_scan_96(ejtag_info, ejtag_ctrl, ctx->pracc_list[i].instr,
				       scan_in[scan_count++].scan_96);

		/* Check store address from previous instruction, if not the first */
		if (i > 0 && ctx->pracc_list[i - 1].addr) {
			jtag_add_clocks(ejtag_info->clocks);
			mips_ejtag_add_scan_96(ejtag_info, ejtag_ctrl, 0, scan_in[scan_count++].scan_96);
		}
	}

	int retval = jtag_execute_queue();		/* execute queued scans */
	if (retval != ERROR_OK)
		goto exit_jtag_error;

	uint32_t fetch_addr = MIPS32_PRACC_TEXT;		/* start address */
	scan_count = 0;
	for (int i = 0; i != ctx->code_count; i++) {				/* verify every pracc access */
		/* check pracc bit */
		ejtag_ctrl = buf_get_u32(scan_in[scan_count].scan_32.ctrl, 0, 32);
		uint32_t addr = buf_get_u32(scan_in[scan_count].scan_32.addr, 0, 32);
		if (!(ejtag_ctrl & EJTAG_CTRL_PRACC)) {
			LOG_ERROR("Error: access not pending  count: %d", scan_count);
			retval = ERROR_FAIL;
			goto exit;
		}
		if (ejtag_ctrl & EJTAG_CTRL_PRNW) {
			LOG_ERROR("Not a fetch/read access, count: %d", scan_count);
			retval = ERROR_FAIL;
			goto exit;
		}
		if (addr != fetch_addr) {
			LOG_ERROR("Fetch addr mismatch, read: %" PRIx32 " expected: %" PRIx32 " count: %d",
					  addr, fetch_addr, scan_count);
			if (addr == MIPS32_PRACC_TEXT)
				retval = ERROR_PRACC_TEXT_JUMP;
			else
				retval = ERROR_FAIL;
			goto exit;
		}
		fetch_addr += 4;
		scan_count++;

		/* check if previous intrucction is a store instruction at dmesg */
		if (i > 0 && ctx->pracc_list[i - 1].addr) {
			uint32_t store_addr = ctx->pracc_list[i - 1].addr;
			ejtag_ctrl = buf_get_u32(scan_in[scan_count].scan_32.ctrl, 0, 32);
			addr = buf_get_u32(scan_in[scan_count].scan_32.addr, 0, 32);

			if (!(ejtag_ctrl & EJTAG_CTRL_PRNW)) {
				LOG_ERROR("Not a store/write access, count: %d", scan_count);
				if (addr == MIPS32_PRACC_TEXT)
					retval = ERROR_PRACC_TEXT_JUMP;
				else
					retval = ERROR_FAIL;
				goto exit;
			}
			if (addr != store_addr) {
				LOG_ERROR("Store address mismatch, read: %" PRIx32 " expected: %" PRIx32 " count: %d",
							      addr, store_addr, scan_count);
				retval = ERROR_FAIL;
				goto exit;
			}
			int buf_index = (addr - MIPS32_PRACC_PARAM_OUT) / 4;
			buf[buf_index] = buf_get_u32(scan_in[scan_count].scan_32.data, 0, 32);
			scan_count++;
		}
	}
exit:
	if (retval != ERROR_OK) {
		LOG_USER("Warning! unknown sequence of code executed");
		ejtag_info->mode[pa_mode] = opt_sync;				/* force sync pa mode */
		if (retval == ERROR_PRACC_TEXT_JUMP)
			pracc_log_debug_mode_exception_info(ejtag_info, 0);	/* log info, if any */

		/* restore working registers, if no error the core fetches at pracc text again */
		retval = mips32_pracc_restore_working_regs(ejtag_info, 8, 9);
		if (retval == ERROR_OK)						/* works in sync mode ? */
			ejtag_info->mode[pa_mode] = opt_async;			/* continue in async mode */
		retval = ERROR_FAIL;						/* return standard error */
	}

exit_jtag_error:

	free(scan_in);
	return retval;
}

int mips32_pracc_restore_working_regs(struct mips_ejtag *ejtag_info, unsigned first, unsigned last)
{
	if (last > 15 || first > last)
		return ERROR_FAIL;

	struct pracc_queue_info ctx = {.ejtag_info = ejtag_info};
	pracc_queue_init(&ctx);

	for (unsigned i = first; i <= last; i++)
		pracc_add_li32(&ctx, i, ejtag_info->regs[i], 1);

	pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));
	/* restore reg 15 from DeSave except if last == 15 */
	last == 15 ? pracc_add(&ctx, 0, MIPS32_MTC0(ctx.isa, 15, 31, 0)) :	/* load $15 in DeSave */
		pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 15, 31, 0));		/* restore $15 from DeSave */

	ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, NULL, 1);
	pracc_queue_free(&ctx);

	if (ctx.retval != ERROR_OK)
		LOG_ERROR("Failed to restore working registers");
	return ctx.retval;
}

int mips32_pracc_read_u32(struct mips_ejtag *ejtag_info, uint32_t addr, uint32_t *buf)
{
	struct pracc_queue_info ctx = {.ejtag_info = ejtag_info};
	pracc_queue_init(&ctx);

	pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 15, PRACC_UPPER_BASE_ADDR));	/* $15 = MIPS32_PRACC_BASE_ADDR */
	pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 8, UPPER16((addr + 0x8000)))); /* load  $8 with modified upper addr */
	pracc_add(&ctx, 0, MIPS32_LW(ctx.isa, 8, LOWER16(addr), 8));			/* lw $8, LOWER16(addr)($8) */
	pracc_add(&ctx, MIPS32_PRACC_PARAM_OUT,
				MIPS32_SW(ctx.isa, 8, PRACC_OUT_OFFSET, 15));	/* sw $8,PRACC_OUT_OFFSET($15) */
	pracc_add_li32(&ctx, 8, ejtag_info->regs[8], 0);				/* restore $8 */
	pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));		/* jump to start */
	pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 15, 31, 0));				/* move COP0 DeSave to $15 */

	ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, buf, 1);
	pracc_queue_free(&ctx);
	return ctx.retval;
}

int mips32_pracc_read_mem(struct mips_ejtag *ejtag_info, uint32_t addr, int size, int count, void *buf)
{
	if (count == 1 && size == 4)
		return mips32_pracc_read_u32(ejtag_info, addr, (uint32_t *)buf);

	struct pracc_queue_info ctx = {.ejtag_info = ejtag_info};
	pracc_queue_init(&ctx);

	uint32_t *data = NULL;
	if (size != 4) {
		data = malloc(256 * sizeof(uint32_t));
		if (data == NULL) {
			LOG_ERROR("Out of memory");
			goto exit;
		}
	}

	uint32_t *buf32 = buf;
	uint16_t *buf16 = buf;
	uint8_t *buf8 = buf;

	while (count) {
		ctx.code_count = 0;
		ctx.store_count = 0;

		int this_round_count = (count > 256) ? 256 : count;
		uint32_t last_upper_base_addr = UPPER16((addr + 0x8000));

		pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 15, PRACC_UPPER_BASE_ADDR)); /* $15 = MIPS32_PRACC_BASE_ADDR */
		pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 9, last_upper_base_addr));	/* upper memory addr to $9 */

		for (int i = 0; i != this_round_count; i++) {			/* Main code loop */
			uint32_t upper_base_addr = UPPER16((addr + 0x8000));
			if (last_upper_base_addr != upper_base_addr) {	/* if needed, change upper addr in $9 */
				pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 9, upper_base_addr));
				last_upper_base_addr = upper_base_addr;
			}

			if (size == 4)				/* load from memory to $8 */
				pracc_add(&ctx, 0, MIPS32_LW(ctx.isa, 8, LOWER16(addr), 9));
			else if (size == 2)
				pracc_add(&ctx, 0, MIPS32_LHU(ctx.isa, 8, LOWER16(addr), 9));
			else
				pracc_add(&ctx, 0, MIPS32_LBU(ctx.isa, 8, LOWER16(addr), 9));

			pracc_add(&ctx, MIPS32_PRACC_PARAM_OUT + i * 4,			/* store $8 at param out */
					  MIPS32_SW(ctx.isa, 8, PRACC_OUT_OFFSET + i * 4, 15));
			addr += size;
		}
		pracc_add_li32(&ctx, 8, ejtag_info->regs[8], 0);				/* restore $8 */
		pracc_add_li32(&ctx, 9, ejtag_info->regs[9], 0);				/* restore $9 */

		pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));	/* jump to start */
		pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 15, 31, 0));			/* restore $15 from DeSave */

		if (size == 4) {
			ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, buf32, 1);
			if (ctx.retval != ERROR_OK)
				goto exit;
			buf32 += this_round_count;
		} else {
			ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, data, 1);
			if (ctx.retval != ERROR_OK)
				goto exit;

			uint32_t *data_p = data;
			for (int i = 0; i != this_round_count; i++) {
				if (size == 2)
					*buf16++ = *data_p++;
				else
					*buf8++ = *data_p++;
			}
		}
		count -= this_round_count;
	}
exit:
	pracc_queue_free(&ctx);
	if (data != NULL)
		free(data);
	return ctx.retval;
}

int mips32_cp0_read(struct mips_ejtag *ejtag_info, uint32_t *val, uint32_t cp0_reg, uint32_t cp0_sel)
{
	struct pracc_queue_info ctx = {.ejtag_info = ejtag_info};
	pracc_queue_init(&ctx);

	pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 15, PRACC_UPPER_BASE_ADDR));	/* $15 = MIPS32_PRACC_BASE_ADDR */
	pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 8, cp0_reg, cp0_sel));		/* move cp0 reg / sel to $8 */
	pracc_add(&ctx, MIPS32_PRACC_PARAM_OUT,
				MIPS32_SW(ctx.isa, 8, PRACC_OUT_OFFSET, 15));	/* store $8 to pracc_out */
	pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 15, 31, 0));				/* restore $15 from DeSave */
	pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 8, UPPER16(ejtag_info->regs[8]))); /* restore upper 16 bits  of $8 */
	pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));		/* jump to start */
	pracc_add(&ctx, 0, MIPS32_ORI(ctx.isa, 8, 8, LOWER16(ejtag_info->regs[8]))); /* restore lower 16 bits of $8 */

	ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, val, 1);
	pracc_queue_free(&ctx);
	return ctx.retval;
}

int mips32_cp0_write(struct mips_ejtag *ejtag_info, uint32_t val, uint32_t cp0_reg, uint32_t cp0_sel)
{
	struct pracc_queue_info ctx = {.ejtag_info = ejtag_info};
	pracc_queue_init(&ctx);

	pracc_add_li32(&ctx, 15, val, 0);				/* Load val to $15 */

	pracc_add(&ctx, 0, MIPS32_MTC0(ctx.isa, 15, cp0_reg, cp0_sel));		/* write $15 to cp0 reg / sel */
	pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));		/* jump to start */
	pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 15, 31, 0));			/* restore $15 from DeSave */

	ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, NULL, 1);
	pracc_queue_free(&ctx);
	return ctx.retval;
}

/**
 * \b mips32_pracc_sync_cache
 *
 * Synchronize Caches to Make Instruction Writes Effective
 * (ref. doc. MIPS32 Architecture For Programmers Volume II: The MIPS32 Instruction Set,
 *  Document Number: MD00086, Revision 2.00, June 9, 2003)
 *
 * When the instruction stream is written, the SYNCI instruction should be used
 * in conjunction with other instructions to make the newly-written instructions effective.
 *
 * Explanation :
 * A program that loads another program into memory is actually writing the D- side cache.
 * The instructions it has loaded can't be executed until they reach the I-cache.
 *
 * After the instructions have been written, the loader should arrange
 * to write back any containing D-cache line and invalidate any locations
 * already in the I-cache.
 *
 * If the cache coherency attribute (CCA) is set to zero, it's a write through cache, there is no need
 * to write back.
 *
 * In the latest MIPS32/64 CPUs, MIPS provides the synci instruction,
 * which does the whole job for a cache-line-sized chunk of the memory you just loaded:
 * That is, it arranges a D-cache write-back (if CCA = 3) and an I-cache invalidate.
 *
 * The line size is obtained with the rdhwr SYNCI_Step in release 2 or from cp0 config 1 register in release 1.
 */
static int mips32_pracc_synchronize_cache(struct mips_ejtag *ejtag_info,
					 uint32_t start_addr, uint32_t end_addr, int cached, int rel)
{
	struct pracc_queue_info ctx = {.ejtag_info = ejtag_info};
	pracc_queue_init(&ctx);

	/** Find cache line size in bytes */
	uint32_t clsiz;
	if (rel) {	/* Release 2 (rel = 1) */
		pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 15, PRACC_UPPER_BASE_ADDR)); /* $15 = MIPS32_PRACC_BASE_ADDR */

		pracc_add(&ctx, 0, MIPS32_RDHWR(ctx.isa, 8, MIPS32_SYNCI_STEP)); /* load synci_step value to $8 */

		pracc_add(&ctx, MIPS32_PRACC_PARAM_OUT,
				MIPS32_SW(ctx.isa, 8, PRACC_OUT_OFFSET, 15));		/* store $8 to pracc_out */

		pracc_add_li32(&ctx, 8, ejtag_info->regs[8], 0);				/* restore $8 */

		pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));	/* jump to start */
		pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 15, 31, 0));			/* restore $15 from DeSave */

		ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, &clsiz, 1);
		if (ctx.retval != ERROR_OK)
			goto exit;

	} else {			/* Release 1 (rel = 0) */
		uint32_t conf;
		ctx.retval = mips32_cp0_read(ejtag_info, &conf, 16, 1);
		if (ctx.retval != ERROR_OK)
			goto exit;

		uint32_t dl = (conf & MIPS32_CONFIG1_DL_MASK) >> MIPS32_CONFIG1_DL_SHIFT;

		/* dl encoding : dl=1 => 4 bytes, dl=2 => 8 bytes, etc... max dl=6 => 128 bytes cache line size */
		clsiz = 0x2 << dl;
		if (dl == 0)
			clsiz = 0;
	}

	if (clsiz == 0)
		goto exit;  /* Nothing to do */

	/* make sure clsiz is power of 2 */
	if (clsiz & (clsiz - 1)) {
		LOG_DEBUG("clsiz must be power of 2");
		ctx.retval = ERROR_FAIL;
		goto exit;
	}

	/* make sure start_addr and end_addr have the same offset inside de cache line */
	start_addr |= clsiz - 1;
	end_addr |= clsiz - 1;

	ctx.code_count = 0;
	ctx.store_count = 0;

	int count = 0;
	uint32_t last_upper_base_addr = UPPER16((start_addr + 0x8000));

	pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 15, last_upper_base_addr)); /* load upper memory base addr to $15 */

	while (start_addr <= end_addr) {						/* main loop */
		uint32_t upper_base_addr = UPPER16((start_addr + 0x8000));
		if (last_upper_base_addr != upper_base_addr) {		/* if needed, change upper addr in $15 */
			pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 15, upper_base_addr));
			last_upper_base_addr = upper_base_addr;
		}
		if (rel)			/* synci instruction, offset($15) */
			pracc_add(&ctx, 0, MIPS32_SYNCI(ctx.isa, LOWER16(start_addr), 15));

		else {
			if (cached == 3)	/* cache Hit_Writeback_D, offset($15) */
				pracc_add(&ctx, 0, MIPS32_CACHE(ctx.isa, MIPS32_CACHE_D_HIT_WRITEBACK,
							LOWER16(start_addr), 15));
			/* cache Hit_Invalidate_I, offset($15) */
			pracc_add(&ctx, 0, MIPS32_CACHE(ctx.isa, MIPS32_CACHE_I_HIT_INVALIDATE,
							LOWER16(start_addr), 15));
		}
		start_addr += clsiz;
		count++;
		if (count == 256 && start_addr <= end_addr) {			/* more ?, then execute code list */
			pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));	/* to start */
			pracc_add(&ctx, 0, MIPS32_NOP);					/* nop in delay slot */

			ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, NULL, 1);
			if (ctx.retval != ERROR_OK)
				goto exit;

			ctx.code_count = 0;	/* reset counters for another loop */
			ctx.store_count = 0;
			count = 0;
		}
	}
	pracc_add(&ctx, 0, MIPS32_SYNC(ctx.isa));
	pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));		/* jump to start */
	pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 15, 31, 0));				/* restore $15 from DeSave*/

	ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, NULL, 1);
exit:
	pracc_queue_free(&ctx);
	return ctx.retval;
}

static int mips32_pracc_write_mem_generic(struct mips_ejtag *ejtag_info,
		uint32_t addr, int size, int count, const void *buf)
{
	struct pracc_queue_info ctx = {.ejtag_info = ejtag_info};
	pracc_queue_init(&ctx);

	const uint32_t *buf32 = buf;
	const uint16_t *buf16 = buf;
	const uint8_t *buf8 = buf;

	while (count) {
		ctx.code_count = 0;
		ctx.store_count = 0;

		int this_round_count = (count > 128) ? 128 : count;
		uint32_t last_upper_base_addr = UPPER16((addr + 0x8000));
			      /* load $15 with memory base address */
		pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 15, last_upper_base_addr));

		for (int i = 0; i != this_round_count; i++) {
			uint32_t upper_base_addr = UPPER16((addr + 0x8000));
			if (last_upper_base_addr != upper_base_addr) {	/* if needed, change upper address in $15*/
				pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 15, upper_base_addr));
				last_upper_base_addr = upper_base_addr;
			}

			if (size == 4) {
				pracc_add_li32(&ctx, 8, *buf32, 1);		/* load with li32, optimize */
				pracc_add(&ctx, 0, MIPS32_SW(ctx.isa, 8, LOWER16(addr), 15)); /* store word to mem */
				buf32++;

			} else if (size == 2) {
				pracc_add(&ctx, 0, MIPS32_ORI(ctx.isa, 8, 0, *buf16));		/* load lower value */
				pracc_add(&ctx, 0, MIPS32_SH(ctx.isa, 8, LOWER16(addr), 15)); /* store half word */
				buf16++;

			} else {
				pracc_add(&ctx, 0, MIPS32_ORI(ctx.isa, 8, 0, *buf8));		/* load lower value */
				pracc_add(&ctx, 0, MIPS32_SB(ctx.isa, 8, LOWER16(addr), 15));	/* store byte */
				buf8++;
			}
			addr += size;
		}

		pracc_add_li32(&ctx, 8, ejtag_info->regs[8], 0);				/* restore $8 */

		pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));	/* jump to start */
		pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 15, 31, 0));			/* restore $15 from DeSave */

		ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, NULL, 1);
		if (ctx.retval != ERROR_OK)
			goto exit;
		count -= this_round_count;
	}
exit:
	pracc_queue_free(&ctx);
	return ctx.retval;
}

int mips32_pracc_write_mem(struct mips_ejtag *ejtag_info, uint32_t addr, int size, int count, const void *buf)
{
	int retval = mips32_pracc_write_mem_generic(ejtag_info, addr, size, count, buf);
	if (retval != ERROR_OK)
		return retval;

	/**
	 * If we are in the cacheable region and cache is activated,
	 * we must clean D$ (if Cache Coherency Attribute is set to 3) + invalidate I$ after we did the write,
	 * so that changes do not continue to live only in D$ (if CCA = 3), but to be
	 * replicated in I$ also (maybe we wrote the istructions)
	 */
	uint32_t conf = 0;
	int cached = 0;

	if ((KSEGX(addr) == KSEG1) || ((addr >= 0xff200000) && (addr <= 0xff3fffff)))
		return retval; /*Nothing to do*/

	mips32_cp0_read(ejtag_info, &conf, 16, 0);

	switch (KSEGX(addr)) {
		case KUSEG:
			cached = (conf & MIPS32_CONFIG0_KU_MASK) >> MIPS32_CONFIG0_KU_SHIFT;
			break;
		case KSEG0:
			cached = (conf & MIPS32_CONFIG0_K0_MASK) >> MIPS32_CONFIG0_K0_SHIFT;
			break;
		case KSEG2:
		case KSEG3:
			cached = (conf & MIPS32_CONFIG0_K23_MASK) >> MIPS32_CONFIG0_K23_SHIFT;
			break;
		default:
			/* what ? */
			break;
	}

	/**
	 * Check cachablitiy bits coherency algorithm
	 * is the region cacheable or uncached.
	 * If cacheable we have to synchronize the cache
	 */
	if (cached == 3 || cached == 0) {		/* Write back cache or write through cache */
		uint32_t start_addr = addr;
		uint32_t end_addr = addr + count * size;
		uint32_t rel = (conf & MIPS32_CONFIG0_AR_MASK) >> MIPS32_CONFIG0_AR_SHIFT;
		if (rel > 1) {
			LOG_DEBUG("Unknown release in cache code");
			return ERROR_FAIL;
		}
		retval = mips32_pracc_synchronize_cache(ejtag_info, start_addr, end_addr, cached, rel);
	}

	return retval;
}

int mips32_pracc_write_regs(struct mips_ejtag *ejtag_info, uint32_t *regs)
{
	struct pracc_queue_info ctx = {.ejtag_info = ejtag_info};
	pracc_queue_init(&ctx);

	uint32_t cp0_write_code[] = {
		MIPS32_MTC0(ctx.isa, 1, 12, 0),					/* move $1 to status */
		MIPS32_MTLO(ctx.isa, 1),						/* move $1 to lo */
		MIPS32_MTHI(ctx.isa, 1),						/* move $1 to hi */
		MIPS32_MTC0(ctx.isa, 1, 8, 0),					/* move $1 to badvaddr */
		MIPS32_MTC0(ctx.isa, 1, 13, 0),					/* move $1 to cause*/
		MIPS32_MTC0(ctx.isa, 1, 24, 0),					/* move $1 to depc (pc) */
	};

	/* load registers 2 to 31 with li32, optimize */
	for (int i = 2; i < 32; i++)
		pracc_add_li32(&ctx, i, regs[i], 1);

	for (int i = 0; i != 6; i++) {
		pracc_add_li32(&ctx, 1, regs[i + 32], 0);	/* load CPO value in $1 */
		pracc_add(&ctx, 0, cp0_write_code[i]);			/* write value from $1 to CPO register */
	}
	pracc_add(&ctx, 0, MIPS32_MTC0(ctx.isa, 15, 31, 0));				/* load $15 in DeSave */
	pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 1, UPPER16((regs[1]))));		/* load upper half word in $1 */
	pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));		/* jump to start */
	pracc_add(&ctx, 0, MIPS32_ORI(ctx.isa, 1, 1, LOWER16((regs[1]))));	/* load lower half word in $1 */

	ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, NULL, 1);

	for (int i = 1; i != 16; i++)
		ejtag_info->regs[i] = regs[i];

	pracc_queue_free(&ctx);
	return ctx.retval;
}

int mips32_pracc_read_regs(struct mips_ejtag *ejtag_info, uint32_t *regs)
{
	struct pracc_queue_info ctx = {.ejtag_info = ejtag_info};
	pracc_queue_init(&ctx);

	uint32_t cp0_read_code[] = {
		MIPS32_MFC0(ctx.isa, 8, 12, 0),					/* move status to $8 */
		MIPS32_MFLO(ctx.isa, 8),						/* move lo to $8 */
		MIPS32_MFHI(ctx.isa, 8),						/* move hi to $8 */
		MIPS32_MFC0(ctx.isa, 8, 8, 0),					/* move badvaddr to $8 */
		MIPS32_MFC0(ctx.isa, 8, 13, 0),					/* move cause to $8 */
		MIPS32_MFC0(ctx.isa, 8, 24, 0),					/* move depc (pc) to $8 */
	};

	pracc_add(&ctx, 0, MIPS32_MTC0(ctx.isa, 1, 31, 0));				/* move $1 to COP0 DeSave */
	pracc_add(&ctx, 0, MIPS32_LUI(ctx.isa, 1, PRACC_UPPER_BASE_ADDR));	/* $1 = MIP32_PRACC_BASE_ADDR */

	for (int i = 2; i != 32; i++)					/* store GPR's 2 to 31 */
		pracc_add(&ctx, MIPS32_PRACC_PARAM_OUT + (i * 4),
				  MIPS32_SW(ctx.isa, i, PRACC_OUT_OFFSET + (i * 4), 1));

	for (int i = 0; i != 6; i++) {
		pracc_add(&ctx, 0, cp0_read_code[i]);				/* load COP0 needed registers to $8 */
		pracc_add(&ctx, MIPS32_PRACC_PARAM_OUT + (i + 32) * 4,			/* store $8 at PARAM OUT */
				  MIPS32_SW(ctx.isa, 8, PRACC_OUT_OFFSET + (i + 32) * 4, 1));
	}
	pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 8, 31, 0));			/* move DeSave to $8, reg1 value */
	pracc_add(&ctx, MIPS32_PRACC_PARAM_OUT + 4,			/* store reg1 value from $8 to param out */
			  MIPS32_SW(ctx.isa, 8, PRACC_OUT_OFFSET + 4, 1));

	pracc_add(&ctx, 0, MIPS32_MFC0(ctx.isa, 1, 31, 0));		/* move COP0 DeSave to $1, restore reg1 */
	pracc_add(&ctx, 0, MIPS32_B(ctx.isa, NEG16((ctx.code_count + 1) << ctx.isa)));		/* jump to start */
	pracc_add(&ctx, 0, MIPS32_MTC0(ctx.isa, 15, 31, 0));				/* load $15 in DeSave */

	ctx.retval = mips32_pracc_queue_exec(ejtag_info, &ctx, regs, 1);

	/* reg8 is saved but not restored, next called function should restore it */
	for (int i = 1; i != 16; i++)
		ejtag_info->regs[i] = regs[i];

	pracc_queue_free(&ctx);
	return ctx.retval;
}

static int mips32_pracc_xfer_check(struct mips_ejtag *ejtag_info, int *fail)
{
	int retval = wait_for_pracc_rw(ejtag_info, READ_ADDR);
	if (retval != ERROR_OK)
		return retval;

	/* high casual latency ? */
	if (ejtag_info->pa_addr == MIPS32_PRACC_FASTDATA_AREA) {	/* handler running, continue */
		*fail = 0;
		return ERROR_OK;
	}

	if (ejtag_info->pa_addr == MIPS32_PRACC_TEXT) {			/* handler not running */
		unsigned mode = ejtag_info->mode[pa_mode];
		ejtag_info->mode[pa_mode] = opt_sync;			/* force sync pa mode */

		pracc_log_debug_mode_exception_info(ejtag_info, 0);
		retval = mips32_pracc_restore_working_regs(ejtag_info, 8, 10);

		ejtag_info->mode[pa_mode] = mode;			/* restore pa mode */
		if (retval != ERROR_OK)
			return retval;
		*fail = 1;
	} else
		*fail = 2;	/* other addr than fastdata area or pracc text ? */

	return ERROR_OK;
}

/* fastdata upload/download requires an initialized working area
 * to load the download code; it should not be called otherwise
 * fetch order from the fastdata area
 * 1. start addr
 * 2. end addr
 * 3. data ...
 */
int mips32_pracc_fastdata_xfer(struct mips_ejtag *ejtag_info, struct working_area *source,
		int write_t, uint32_t addr, int count, uint32_t *buf)
{
	/* one byte for spracc, four for data, per fastdata scan */
	uint8_t *in_buf = malloc(5 * XFER_BLOCK  * sizeof(uint8_t));
	if (in_buf == NULL) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}

	uint32_t isa = ejtag_info->isa ? 1 : 0;
	uint32_t handler_code[] = {
		/* $15 already points to xfer area, loaded in jump code */
		MIPS32_LW(isa, 8, 0, 15),				/* start addr in t0 */
		MIPS32_LW(isa, 9, 0, 15),				/* end addr to t1 */

		/* loop: */
		write_t ? MIPS32_LW(isa, 10, 0, 15) : MIPS32_LW(isa, 10, 0, 8),	/* from xfer area : from memory */
		write_t ? MIPS32_SW(isa, 10, 0, 8) : MIPS32_SW(isa, 10, 0, 15),	/* to memory      : to xfer area */
		MIPS32_BNE(isa, 9, 8, NEG16(3 << isa)),				/* bne t1,t0, loop */
		MIPS32_ADDI(isa, 8, 8, 4),					/* addi t0,t0,4, next addr*/

		/* restore regs */
		MIPS32_LUI(isa, 8, UPPER16(ejtag_info->regs[8])),
		MIPS32_ORI(isa, 8, 8, LOWER16(ejtag_info->regs[8])),
		MIPS32_LUI(isa, 9, UPPER16(ejtag_info->regs[9])),
		MIPS32_ORI(isa, 9, 9, LOWER16(ejtag_info->regs[9])),
		MIPS32_LUI(isa, 10, UPPER16(ejtag_info->regs[10])),
		MIPS32_ORI(isa, 10, 10, LOWER16(ejtag_info->regs[10])),

		/* exit code */
		MIPS32_ORI(isa, 15, 15, LOWER16(MIPS32_PRACC_TEXT) | isa),	/* isa bit for JR instr */
		MIPS32_JR(isa, 15),						/* jump to pracc text */
		MIPS32_MFC0(isa, 15, 31, 0),					/* restore $15 from DeSave */
	};

	if (source->size < MIPS32_FASTDATA_HANDLER_SIZE)
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;

	pracc_swap16_array(ejtag_info, handler_code, ARRAY_SIZE(handler_code));
		/* write program into RAM */

	int retval;
	if (write_t != ejtag_info->fast_access_save) {
		retval = mips32_pracc_write_mem(ejtag_info, source->address, 4, ARRAY_SIZE(handler_code),
						handler_code);
		if (retval != ERROR_OK)
			goto exit_free;

		/* save previous operation to speed to any consecutive read/writes */
		ejtag_info->fast_access_save = write_t;
	}

	LOG_DEBUG("%s using 0x%.8" TARGET_PRIxADDR " for r/w handler", __func__, source->address);

	uint32_t jmp_code[] = {
		MIPS32_LUI(isa, 8, UPPER16(source->address)),			/* load addr of jump in $8 */
		MIPS32_ORI(isa, 8, 8, LOWER16(source->address) | isa),		/* isa bit for JR instr */
		MIPS32_JR(isa, 8),						/* jump to ram program */
		MIPS32_LUI(isa, 15, UPPER16(MIPS32_PRACC_FASTDATA_AREA)),	/* $15 points to fasdata area */
	};

	pracc_swap16_array(ejtag_info, jmp_code, ARRAY_SIZE(jmp_code));
	mips_ejtag_update_clocks(ejtag_info);

	/* execute jump code, with no address check */
	for (unsigned i = 0; i < ARRAY_SIZE(jmp_code); i++) {
		retval = wait_for_pracc_rw(ejtag_info, 0);
		if (retval != ERROR_OK)
			goto exit_free;

		mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
		mips_ejtag_drscan_32_out(ejtag_info, jmp_code[i]);

		/* Clear the access pending bit (let the processor eat!) */
		mips32_pracc_finish(ejtag_info);
	}

	/* wait PrAcc pending bit for FASTDATA write, read address */
	retval = wait_for_pracc_rw(ejtag_info, READ_ADDR);
	if (retval != ERROR_OK)
		goto exit_free;

	/* next fetch to dmseg should be in FASTDATA_AREA, check */
	if (ejtag_info->pa_addr != MIPS32_PRACC_FASTDATA_AREA) {
		retval = ERROR_FAIL;
		goto exit;
	}

	/* Send the load start address */
	uint32_t val = addr;
	mips_ejtag_fastdata_scan(ejtag_info, 1, &val, in_buf, 1);

	retval = wait_for_pracc_rw(ejtag_info, 0);
	if (retval != ERROR_OK)
		goto exit_free;

	/* Send the load end address */
	val = addr + (count - 1) * 4;
	mips_ejtag_fastdata_scan(ejtag_info, 1, &val, in_buf, 1);

	int failed_scan = 0;
	int consecutive_fails = 0;
	while (count) {
		int fails = 0;
		int this_round_count = count > XFER_BLOCK ? XFER_BLOCK : count;
		mips_ejtag_fastdata_scan(ejtag_info, write_t, buf, in_buf, this_round_count);

		retval = jtag_execute_queue();
		if (retval != ERROR_OK) {
			LOG_ERROR("fastdata load failed");
			goto exit_free;
		}

		for (int i = 0; i != this_round_count; i++) {
			if (in_buf[i * 5] & 1) {	/* check spracc, 1: successful scan */
				consecutive_fails = 0;
				if (!write_t)
					*buf++ = buf_get_u32(&in_buf[1 + (i * 5)], 0, 32);
			} else {
				fails++;
				consecutive_fails++;
			}
		}
		/* check, consecutive_fails is cleared if no error found */
		if (consecutive_fails > 100) {
			retval = mips32_pracc_xfer_check(ejtag_info, &consecutive_fails);
			if (retval != ERROR_OK || consecutive_fails == 1)
				goto exit_free;
			if (consecutive_fails == 2)
				goto exit;
		}

		if (fails)
			failed_scan++;

		if (write_t) {
			if (failed_scan)
				buf = NULL;	/* afterwards shift out only 0's */
			else
				buf += this_round_count;
		}

		count -= this_round_count - fails;	/* successful scans only */
	}

	if (failed_scan && write_t)
		LOG_USER("failed to download, increase scan delay and/or reduce scan rate");
exit:
	retval = wait_for_pracc_rw(ejtag_info, READ_ADDR);
	if (retval != ERROR_OK)
		goto exit_free;

	if (ejtag_info->pa_addr != MIPS32_PRACC_TEXT)	/* should not occur, but... */
		LOG_ERROR("mini program did not return to start, address: %"PRIx32, ejtag_info->pa_addr);

exit_free:
	free(in_buf);
	return retval;
}
