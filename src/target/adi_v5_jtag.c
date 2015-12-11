/***************************************************************************
 *   Copyright (C) 2006 by Magnus Lundin
 *   lundin@mlu.mine.nu
 *
 *   Copyright (C) 2008 by Spencer Oliver
 *   spen@spen-soft.co.uk
 *
 *   Copyright (C) 2009 by Oyvind Harboe
 *   oyvind.harboe@zylin.com
 *
 *   Copyright (C) 2009-2010 by David Brownell
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 ***************************************************************************/

/**
 * @file
 * This file implements JTAG transport support for cores implementing
 the ARM Debug Interface version 5 (ADIv5).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arm.h"
#include "arm_adi_v5.h"
#include <helper/time_support.h>
#include <helper/list.h>

#define DEBUG_WAIT

/* JTAG instructions/registers for JTAG-DP and SWJ-DP */
#define JTAG_DP_ABORT		0x8
#define JTAG_DP_DPACC		0xA
#define JTAG_DP_APACC		0xB
#define JTAG_DP_IDCODE		0xE

/* three-bit ACK values for DPACC and APACC reads */
#define JTAG_ACK_OK_FAULT	0x2
#define JTAG_ACK_WAIT		0x1

static int jtag_ap_q_abort(struct adiv5_dap *dap, uint8_t *ack);

#ifdef DEBUG_WAIT
static const char *dap_reg_name(int instr, int reg_addr)
{
	char *reg_name = "UNK";

	if (instr == JTAG_DP_DPACC) {
		switch (reg_addr) {
		case DP_ABORT:
			reg_name =  "ABORT";
			break;
		case DP_CTRL_STAT:
			reg_name =  "CTRL/STAT";
			break;
		case DP_SELECT:
			reg_name = "SELECT";
			break;
		case DP_RDBUFF:
			reg_name =  "RDBUFF";
			break;
		case DP_WCR:
			reg_name =  "WCR";
			break;
		default:
			reg_name = "UNK";
			break;
		}
	}

	if (instr == JTAG_DP_APACC) {
		switch (reg_addr) {
		case MEM_AP_REG_CSW:
			reg_name = "CSW";
			break;
		case MEM_AP_REG_TAR:
			reg_name = "TAR";
			break;
		case MEM_AP_REG_DRW:
			reg_name = "DRW";
			break;
		case MEM_AP_REG_BD0:
			reg_name = "BD0";
			break;
		case MEM_AP_REG_BD1:
			reg_name = "BD1";
			break;
		case MEM_AP_REG_BD2:
			reg_name = "BD2";
			break;
		case MEM_AP_REG_BD3:
			reg_name = "BD3";
			break;
		case MEM_AP_REG_CFG:
			reg_name = "CFG";
			break;
		case MEM_AP_REG_BASE:
			reg_name = "BASE";
			break;
		case AP_REG_IDR:
			reg_name = "IDR";
			break;
		default:
			reg_name = "UNK";
			break;
		}
	}

	return reg_name;
}
#endif

struct dap_cmd {
	struct list_head lh;
	uint8_t instr;
	uint8_t reg_addr;
	uint8_t RnW;
	uint8_t *invalue;
	uint8_t outvalue[4];
	uint8_t ack;
	uint32_t memaccess_tck;
	uint32_t dp_select;

	struct scan_field fields[2];
	uint8_t out_addr_buf;
};

static void log_dap_cmd(const char *header, struct dap_cmd *el)
{
#ifdef DEBUG_WAIT
	LOG_DEBUG("%s: %2s %6s %5s 0x%08x %2s", header,
		el->instr == JTAG_DP_APACC ? "AP" : "DP",
		dap_reg_name(el->instr, el->reg_addr),
		el->RnW == DPAP_READ ? "READ" : "WRITE",
		el->RnW == DPAP_WRITE ? buf_get_u32(el->outvalue, 0, 32) :
					(el->invalue ? buf_get_u32(el->invalue, 0, 32) :
						(uint32_t)-1),
		el->ack == JTAG_ACK_OK_FAULT ? "OK" : "WAIT");
#endif
}

static struct dap_cmd *dap_cmd_new(void)
{
	struct dap_cmd *cmd;

	cmd = (struct dap_cmd *)calloc(1, sizeof(struct dap_cmd));
	if (cmd != NULL)
		INIT_LIST_HEAD(&cmd->lh);

	return cmd;
}

static void flush_journal(struct list_head *lh)
{
	struct dap_cmd *el, *tmp;

	list_for_each_entry_safe(el, tmp, lh, lh) {
		list_del(&el->lh);
		free(el);
	}
}

/***************************************************************************
 *
 * DPACC and APACC scanchain access through JTAG-DP (or SWJ-DP)
 *
***************************************************************************/

static int adi_jtag_dp_scan_cmd(struct adiv5_dap *dap, struct dap_cmd *cmd)
{
	struct jtag_tap *tap = dap->tap;
	int retval;

	retval = arm_jtag_set_instr(tap, cmd->instr, NULL, TAP_IDLE);
	if (retval != ERROR_OK)
		return retval;

	/* Scan out a read or write operation using some DP or AP register.
	 * For APACC access with any sticky error flag set, this is discarded.
	 */
	cmd->fields[0].num_bits = 3;
	buf_set_u32(&cmd->out_addr_buf, 0, 3, ((cmd->reg_addr >> 1) & 0x6) | (cmd->RnW & 0x1));
	cmd->fields[0].out_value = &cmd->out_addr_buf;
	cmd->fields[0].in_value = &cmd->ack;

	/* NOTE: if we receive JTAG_ACK_WAIT, the previous operation did not
	 * complete; data we write is discarded, data we read is unpredictable.
	 * When overrun detect is active, STICKYORUN is set.
	 */

	cmd->fields[1].num_bits = 32;
	cmd->fields[1].out_value = cmd->outvalue;
	cmd->fields[1].in_value = cmd->invalue;

	jtag_add_dr_scan(tap, 2, cmd->fields, TAP_IDLE);

	/* Add specified number of tck clocks after starting memory bus
	 * access, giving the hardware time to complete the access.
	 * They provide more time for the (MEM) AP to complete the read ...
	 * See "Minimum Response Time" for JTAG-DP, in the ADIv5 spec.
	 */
	if ((cmd->instr == JTAG_DP_APACC)
			&& ((cmd->reg_addr == MEM_AP_REG_DRW)
				|| ((cmd->reg_addr & 0xF0) == MEM_AP_REG_BD0))
			&& (cmd->memaccess_tck != 0))
		jtag_add_runtest(cmd->memaccess_tck,
				TAP_IDLE);

	return ERROR_OK;
}

/**
 * Scan DPACC or APACC using target ordered uint8_t buffers.  No endianness
 * conversions are performed.  See section 4.4.3 of the ADIv5 spec, which
 * discusses operations which access these registers.
 *
 * Note that only one scan is performed.  If RnW is set, a separate scan
 * will be needed to collect the data which was read; the "invalue" collects
 * the posted result of a preceding operation, not the current one.
 *
 * @param dap the DAP
 * @param instr JTAG_DP_APACC (AP access) or JTAG_DP_DPACC (DP access)
 * @param reg_addr two significant bits; A[3:2]; for APACC access, the
 *	SELECT register has more addressing bits.
 * @param RnW false iff outvalue will be written to the DP or AP
 * @param outvalue points to a 32-bit (little-endian) integer
 * @param invalue NULL, or points to a 32-bit (little-endian) integer
 * @param ack points to where the three bit JTAG_ACK_* code will be stored
 * @param memaccess_tck number of idle cycles to add after AP access
 */

static int adi_jtag_dp_scan(struct adiv5_dap *dap,
		uint8_t instr, uint8_t reg_addr, uint8_t RnW,
		uint8_t *outvalue, uint8_t *invalue,
		uint32_t memaccess_tck)
{
	struct dap_cmd *cmd;
	int retval;

	cmd = dap_cmd_new();
	if (cmd != NULL) {
		cmd->instr = instr;
		cmd->reg_addr = reg_addr;
		cmd->RnW = RnW;
		memcpy(cmd->outvalue, outvalue, 4);
		cmd->invalue = invalue;
		cmd->memaccess_tck = memaccess_tck;
		cmd->dp_select = dap->select;
	} else
		return ERROR_JTAG_DEVICE_ERROR;

	retval = adi_jtag_dp_scan_cmd(dap, cmd);
	if (retval == ERROR_OK)
		list_add_tail(&cmd->lh,	&dap->cmd_journal);

	return retval;
}

/**
 * Scan DPACC or APACC out and in from host ordered uint32_t buffers.
 * This is exactly like adi_jtag_dp_scan(), except that endianness
 * conversions are performed (so the types of invalue and outvalue
 * must be different).
 */
static int adi_jtag_dp_scan_u32(struct adiv5_dap *dap,
		uint8_t instr, uint8_t reg_addr, uint8_t RnW,
		uint32_t outvalue, uint32_t *invalue,
		uint32_t memaccess_tck)
{
	uint8_t out_value_buf[4];
	int retval;

	buf_set_u32(out_value_buf, 0, 32, outvalue);

	retval = adi_jtag_dp_scan(dap, instr, reg_addr, RnW,
			out_value_buf, (uint8_t *)invalue, memaccess_tck);
	if (retval != ERROR_OK)
		return retval;

	if (invalue)
		jtag_add_callback(arm_le_to_h_u32,
				(jtag_callback_data_t) invalue);

	return retval;
}

/**
 * Utility to write AP registers.
 */
static inline int adi_jtag_ap_write_check(struct adiv5_ap *ap,
		uint8_t reg_addr, uint8_t *outvalue)
{
	return adi_jtag_dp_scan(ap->dap, JTAG_DP_APACC, reg_addr, DPAP_WRITE,
			outvalue, NULL, ap->memaccess_tck);
}

static int adi_jtag_scan_inout_check_u32(struct adiv5_dap *dap,
		uint8_t instr, uint8_t reg_addr, uint8_t RnW,
		uint32_t outvalue, uint32_t *invalue, uint32_t memaccess_tck)
{
	int retval;

	/* Issue the read or write */
	retval = adi_jtag_dp_scan_u32(dap, instr, reg_addr,
			RnW, outvalue, NULL, memaccess_tck);
	if (retval != ERROR_OK)
		return retval;

	/* For reads,  collect posted value; RDBUFF has no other effect.
	 * Assumes read gets acked with OK/FAULT, and CTRL_STAT says "OK".
	 */
	if ((RnW == DPAP_READ) && (invalue != NULL))
		retval = adi_jtag_dp_scan_u32(dap, JTAG_DP_DPACC,
				DP_RDBUFF, DPAP_READ, 0, invalue, 0);
	return retval;
}

static int jtagdp_transaction_endcheck(struct adiv5_dap *dap)
{
	int retval;
	uint32_t ctrlstat;
	struct dap_cmd *el, *tmp;
	LIST_HEAD(replay_list);

	/* too expensive to call keep_alive() here */

	/* make sure all queued transactions are complete */
	retval = jtag_execute_queue();
	if (retval != ERROR_OK)
		return retval;

	/* move all transactions over to the replay list */
	list_for_each_entry_safe(el, tmp, &dap->cmd_journal, lh) {
		log_dap_cmd("LOG", el);
		list_move_tail(&el->lh, &replay_list);
	}

	/* Post CTRL/STAT read; discard any previous posted read value
	 * but collect its ACK status.
	 */
	retval = adi_jtag_scan_inout_check_u32(dap, JTAG_DP_DPACC,
			DP_CTRL_STAT, DPAP_READ, 0, &ctrlstat, 0);
	if (retval != ERROR_OK)
		return retval;
	retval = jtag_execute_queue();
	if (retval != ERROR_OK)
		return retval;

	LOG_DEBUG("CTRL/STAT %" PRIx32, ctrlstat);

	/* REVISIT also STICKYCMP, for pushed comparisons (nyet used) */

	/* Check for STICKYERR */
	if (ctrlstat & SSTICKYERR) {
		LOG_DEBUG("jtag-dp: CTRL/STAT error, 0x%" PRIx32, ctrlstat);
		/* Check power to debug regions */
		if ((ctrlstat & 0xf0000000) != 0xf0000000) {
			LOG_ERROR("Debug regions are unpowered, an unexpected reset might have happened");
			retval = ERROR_JTAG_DEVICE_ERROR;
			goto done;
		}

		if (ctrlstat & SSTICKYERR)
			LOG_ERROR("JTAG-DP STICKY ERROR");

		/* Clear Sticky Error Bits */
		retval = adi_jtag_scan_inout_check_u32(dap, JTAG_DP_DPACC,
				DP_CTRL_STAT, DPAP_WRITE,
				dap->dp_ctrl_stat | SSTICKYERR, NULL, 0);
		if (retval != ERROR_OK)
			goto done;

		retval = adi_jtag_scan_inout_check_u32(dap, JTAG_DP_DPACC,
				DP_CTRL_STAT, DPAP_READ, 0, &ctrlstat, 0);
		if (retval != ERROR_OK)
			goto done;

		retval = jtag_execute_queue();
		if (retval != ERROR_OK)
			goto done;

		LOG_DEBUG("jtag-dp: CTRL/STAT 0x%" PRIx32, ctrlstat);

		retval = ERROR_JTAG_DEVICE_ERROR;
		goto done;
	}

	/* check for overrun condition in the last batch of transactions */
	if (ctrlstat & SSTICKYORUN) {
		/* Clear STICKYORUN bit in order to replay the data */
		ctrlstat |= SSTICKYORUN;
		retval = adi_jtag_scan_inout_check_u32(dap, JTAG_DP_DPACC,
				DP_CTRL_STAT, DPAP_WRITE, ctrlstat, NULL, 0);
		if (retval != ERROR_OK)
			goto done;
		retval = adi_jtag_scan_inout_check_u32(dap, JTAG_DP_DPACC,
				DP_CTRL_STAT, DPAP_READ, 0, &ctrlstat, 0);
		if (retval != ERROR_OK)
			goto done;
		retval = jtag_execute_queue();
		if (retval != ERROR_OK)
			goto done;

		LOG_DEBUG("Recover: CTRL/STAT %" PRIx32, ctrlstat);

		list_for_each_entry_safe(el, tmp, &replay_list, lh) {
			do {
				adi_jtag_dp_scan_cmd(dap, el);
				jtag_execute_queue();
				log_dap_cmd("REC", el);
			} while (el->ack == JTAG_ACK_WAIT);

			list_del(&el->lh);
			free(el);
		}
	}

 done:
	dap->ack = 0;
	flush_journal(&replay_list);
	flush_journal(&dap->cmd_journal);
	return retval;
}

/*--------------------------------------------------------------------------*/

static int jtag_dp_q_read(struct adiv5_dap *dap, unsigned reg,
		uint32_t *data)
{
	return adi_jtag_scan_inout_check_u32(dap, JTAG_DP_DPACC,
			reg, DPAP_READ, 0, data, 0);
}

static int jtag_dp_q_write(struct adiv5_dap *dap, unsigned reg,
		uint32_t data)
{
	return adi_jtag_scan_inout_check_u32(dap, JTAG_DP_DPACC,
			reg, DPAP_WRITE, data, NULL, 0);
}

/** Select the AP register bank matching bits 7:4 of reg. */
static int jtag_ap_q_bankselect(struct adiv5_ap *ap, unsigned reg)
{
	struct adiv5_dap *dap = ap->dap;
	uint32_t select = ((uint32_t)ap->ap_num << 24) | (reg & 0x000000F0);

	if (select == dap->select)
		return ERROR_OK;

	dap->select = select;

	return jtag_dp_q_write(dap, DP_SELECT, select);
}

static int jtag_ap_q_read(struct adiv5_ap *ap, unsigned reg,
		uint32_t *data)
{
	int retval = jtag_ap_q_bankselect(ap, reg);

	if (retval != ERROR_OK)
		return retval;

	return adi_jtag_scan_inout_check_u32(ap->dap, JTAG_DP_APACC, reg,
			DPAP_READ, 0, data, ap->memaccess_tck);
}

static int jtag_ap_q_write(struct adiv5_ap *ap, unsigned reg,
		uint32_t data)
{
	uint8_t out_value_buf[4];

	int retval = jtag_ap_q_bankselect(ap, reg);
	if (retval != ERROR_OK)
		return retval;

	buf_set_u32(out_value_buf, 0, 32, data);

	return adi_jtag_ap_write_check(ap, reg, out_value_buf);
}

static int jtag_ap_q_abort(struct adiv5_dap *dap, uint8_t *ack)
{
	/* for JTAG, this is the only valid ABORT register operation */
	return adi_jtag_dp_scan_u32(dap, JTAG_DP_ABORT,
			0, DPAP_WRITE, 1, NULL, 0);
}

static int jtag_dp_run(struct adiv5_dap *dap)
{
	return jtagdp_transaction_endcheck(dap);
}

/* FIXME don't export ... just initialize as
 * part of DAP setup
*/
const struct dap_ops jtag_dp_ops = {
	.queue_dp_read       = jtag_dp_q_read,
	.queue_dp_write      = jtag_dp_q_write,
	.queue_ap_read       = jtag_ap_q_read,
	.queue_ap_write      = jtag_ap_q_write,
	.queue_ap_abort      = jtag_ap_q_abort,
	.run                 = jtag_dp_run,
};


static const uint8_t swd2jtag_bitseq[] = {
	/* More than 50 TCK/SWCLK cycles with TMS/SWDIO high,
	 * putting both JTAG and SWD logic into reset state.
	 */
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	/* Switching equence disables SWD and enables JTAG
	 * NOTE: bits in the DP's IDCODE can expose the need for
	 * the old/deprecated sequence (0xae 0xde).
	 */
	0x3c, 0xe7,
	/* At least 50 TCK/SWCLK cycles with TMS/SWDIO high,
	 * putting both JTAG and SWD logic into reset state.
	 * NOTE:  some docs say "at least 5".
	 */
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

/** Put the debug link into JTAG mode, if the target supports it.
 * The link's initial mode may be either SWD or JTAG.
 *
 * @param target Enters JTAG mode (if possible).
 *
 * Note that targets implemented with SW-DP do not support JTAG, and
 * that some targets which could otherwise support it may have been
 * configured to disable JTAG signaling
 *
 * @return ERROR_OK or else a fault code.
 */
int dap_to_jtag(struct target *target)
{
	int retval;

	LOG_DEBUG("Enter JTAG mode");

	/* REVISIT it's nasty to need to make calls to a "jtag"
	 * subsystem if the link isn't in JTAG mode...
	 */

	retval = jtag_add_tms_seq(8 * sizeof(swd2jtag_bitseq),
			swd2jtag_bitseq, TAP_RESET);
	if (retval == ERROR_OK)
		retval = jtag_execute_queue();

	/* REVISIT set up the DAP's ops vector for JTAG mode. */

	return retval;
}
