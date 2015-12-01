/***************************************************************************
 *   Copyright (C) 2006 by Magnus Lundin                                   *
 *   lundin@mlu.mine.nu                                                    *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2009-2010 by Oyvind Harboe                              *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2009-2010 by David Brownell                             *
 *                                                                         *
 *   Copyright (C) 2013 by Andreas Fritiofson                              *
 *   andreas.fritiofson@gmail.com                                          *
 *                                                                         *
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

/**
 * @file
 * This file implements support for the ARM Debug Interface version 5 (ADIv5)
 * debugging architecture.  Compared with previous versions, this includes
 * a low pin-count Serial Wire Debug (SWD) alternative to JTAG for message
 * transport, and focusses on memory mapped resources as defined by the
 * CoreSight architecture.
 *
 * A key concept in ADIv5 is the Debug Access Port, or DAP.  A DAP has two
 * basic components:  a Debug Port (DP) transporting messages to and from a
 * debugger, and an Access Port (AP) accessing resources.  Three types of DP
 * are defined.  One uses only JTAG for communication, and is called JTAG-DP.
 * One uses only SWD for communication, and is called SW-DP.  The third can
 * use either SWD or JTAG, and is called SWJ-DP.  The most common type of AP
 * is used to access memory mapped resources and is called a MEM-AP.  Also a
 * JTAG-AP is also defined, bridging to JTAG resources; those are uncommon.
 *
 * This programming interface allows DAP pipelined operations through a
 * transaction queue.  This primarily affects AP operations (such as using
 * a MEM-AP to access memory or registers).  If the current transaction has
 * not finished by the time the next one must begin, and the ORUNDETECT bit
 * is set in the DP_CTRL_STAT register, the SSTICKYORUN status is set and
 * further AP operations will fail.  There are two basic methods to avoid
 * such overrun errors.  One involves polling for status instead of using
 * transaction piplining.  The other involves adding delays to ensure the
 * AP has enough time to complete one operation before starting the next
 * one.  (For JTAG these delays are controlled by memaccess_tck.)
 */

/*
 * Relevant specifications from ARM include:
 *
 * ARM(tm) Debug Interface v5 Architecture Specification    ARM IHI 0031A
 * ARM(rm) Debug Interface Architecture Specification ADIv5.0 to ADIv5.2
 *                                                          ARM IHI 0031C
 * CoreSight(tm) v1.0 Architecture Specification            ARM IHI 0029B
 * CoreSight(tm) v2.0 Architecture Specification            ARM IHI 0029D
 *
 * CoreSight(tm) DAP-Lite TRM, ARM DDI 0316D
 * Cortex-M3(tm) TRM, ARM DDI 0337G
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "jtag/interface.h"
#include "arm.h"
#include "arm_adi_v5.h"
#include <helper/time_support.h>
#include <helper/list.h>

/* ARM ADI Specification requires at least 10 bits used for TAR autoincrement  */

/*
	uint32_t tar_block_size(uint32_t address)
	Return the largest block starting at address that does not cross a tar block size alignment boundary
*/
static uint32_t max_tar_block_size(uint32_t tar_autoincr_block, uint32_t address)
{
	return tar_autoincr_block - ((tar_autoincr_block - 1) & address);
}

/***************************************************************************
 *                                                                         *
 * DP and MEM-AP  register access  through APACC and DPACC                 *
 *                                                                         *
***************************************************************************/

static int mem_ap_setup_csw(struct adiv5_ap *ap, uint32_t csw)
{
	/* Alamy ***** WARNING *****
	 *	CSW register has different bit/field definition in AHB/APB/AXI bus
	 *  We should
	 *  a) Get rid of CSW_MASTER_DEBUG / CSW_HPROT
	 *  b) Different code for different bus (AHB/APB/AXI)
	 */
	csw = csw | CSW_DBG_SW_ENABLE | CSW_MASTER_DEBUG | CSW_HPROT |
		ap->csw_default;

	if (csw != ap->csw_value) {
		/* LOG_DEBUG("DAP: Set CSW %x",csw); */
		int retval = dap_queue_ap_write(ap, MEM_AP_REG_CSW, csw);
		if (retval != ERROR_OK)
			return retval;
		ap->csw_value = csw;
	}
	return ERROR_OK;
}

static int mem_ap_setup_tar(struct adiv5_ap *ap, uint32_t tar)
{
	if (tar != ap->tar_value ||
			(ap->csw_value & CSW_ADDRINC_MASK)) {
		/* LOG_DEBUG("DAP: Set TAR %x",tar); */
		int retval = dap_queue_ap_write(ap, MEM_AP_REG_TAR, tar);
		if (retval != ERROR_OK)
			return retval;
		ap->tar_value = tar;
	}
	return ERROR_OK;
}

/**
 * Queue transactions setting up transfer parameters for the
 * currently selected MEM-AP.
 *
 * Subsequent transfers using registers like MEM_AP_REG_DRW or MEM_AP_REG_BD2
 * initiate data reads or writes using memory or peripheral addresses.
 * If the CSW is configured for it, the TAR may be automatically
 * incremented after each transfer.
 *
 * @param ap The MEM-AP.
 * @param csw MEM-AP Control/Status Word (CSW) register to assign.  If this
 *	matches the cached value, the register is not changed.
 * @param tar MEM-AP Transfer Address Register (TAR) to assign.  If this
 *	matches the cached address, the register is not changed.
 *
 * @return ERROR_OK if the transaction was properly queued, else a fault code.
 */
static int mem_ap_setup_transfer(struct adiv5_ap *ap, uint32_t csw, uint32_t tar)
{
	int retval;
	retval = mem_ap_setup_csw(ap, csw);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_setup_tar(ap, tar);
	if (retval != ERROR_OK)
		return retval;
	return ERROR_OK;
}

/**
 * Asynchronous (queued) read of a word from memory or a system register.
 *
 * @param ap The MEM-AP to access.
 * @param address Address of the 32-bit word to read; it must be
 *	readable by the currently selected MEM-AP.
 * @param value points to where the word will be stored when the
 *	transaction queue is flushed (assuming no errors).
 *
 * @return ERROR_OK for success.  Otherwise a fault code.
 */
int mem_ap_read_u32(struct adiv5_ap *ap, uint32_t address,
		uint32_t *value)
{
	int retval;

	/* Use banked addressing (REG_BDx) to avoid some link traffic
	 * (updating TAR) when reading several consecutive addresses.
	 */
	retval = mem_ap_setup_transfer(ap, CSW_SIZE_32BIT | CSW_ADDRINC_OFF,
			address & 0xFFFFFFF0);
	if (retval != ERROR_OK)
		return retval;

	return dap_queue_ap_read(ap, MEM_AP_REG_BD0 | (address & 0xC), value);
}

/**
 * Synchronous read of a word from memory or a system register.
 * As a side effect, this flushes any queued transactions.
 *
 * @param ap The MEM-AP to access.
 * @param address Address of the 32-bit word to read; it must be
 *	readable by the currently selected MEM-AP.
 * @param value points to where the result will be stored.
 *
 * @return ERROR_OK for success; *value holds the result.
 * Otherwise a fault code.
 */
int mem_ap_read_atomic_u32(struct adiv5_ap *ap, uint32_t address,
		uint32_t *value)
{
	int retval;

	retval = mem_ap_read_u32(ap, address, value);
	if (retval != ERROR_OK)
		return retval;

	return dap_run(ap->dap);
}

/**
 * Asynchronous (queued) write of a word to memory or a system register.
 *
 * @param ap The MEM-AP to access.
 * @param address Address to be written; it must be writable by
 *	the currently selected MEM-AP.
 * @param value Word that will be written to the address when transaction
 *	queue is flushed (assuming no errors).
 *
 * @return ERROR_OK for success.  Otherwise a fault code.
 */
int mem_ap_write_u32(struct adiv5_ap *ap, uint32_t address,
		uint32_t value)
{
	int retval;

	/* Use banked addressing (REG_BDx) to avoid some link traffic
	 * (updating TAR) when writing several consecutive addresses.
	 */
	retval = mem_ap_setup_transfer(ap, CSW_SIZE_32BIT | CSW_ADDRINC_OFF,
			address & 0xFFFFFFF0);
	if (retval != ERROR_OK)
		return retval;

	return dap_queue_ap_write(ap, MEM_AP_REG_BD0 | (address & 0xC),
			value);
}

/**
 * Synchronous write of a word to memory or a system register.
 * As a side effect, this flushes any queued transactions.
 *
 * @param ap The MEM-AP to access.
 * @param address Address to be written; it must be writable by
 *	the currently selected MEM-AP.
 * @param value Word that will be written.
 *
 * @return ERROR_OK for success; the data was written.  Otherwise a fault code.
 */
int mem_ap_write_atomic_u32(struct adiv5_ap *ap, uint32_t address,
		uint32_t value)
{
	int retval = mem_ap_write_u32(ap, address, value);

	if (retval != ERROR_OK)
		return retval;

	return dap_run(ap->dap);
}

/**
 * Set/Clear bits on a u32 value at 'addr'
 *
 * CAUTION:
 * Use 'atomic' read (mem_ap_read_atomic_u32) instead of
 * 'queued' read (mem_ap_read_u32), which queues the command until dap_run()
 * in the mem_ap_write_atomic_u32().
 * In the case of 'queued' read, the 'value' had not be initiated
 * when modifing it (value |= bit_mask).
 */
int mem_ap_set_bits_u32(
	struct adiv5_dap *dap, uint32_t addr, uint32_t bit_mask)
{
	int rc;
	uint32_t value;

	/* CAUTION: Use 'atomic' read */
	rc = mem_ap_read_atomic_u32(dap, addr, &value);
	if (rc != ERROR_OK)	return rc;

	value |= bit_mask;

	return mem_ap_write_atomic_u32(dap, addr, value);
}

int mem_ap_clear_bits_u32(
	struct adiv5_dap *dap, uint32_t addr, uint32_t bit_mask)
{
	int rc;
	uint32_t value;

	rc = mem_ap_read_atomic_u32(dap, addr, &value);
	if (rc != ERROR_OK)	return rc;

	value &= ~bit_mask;

	return mem_ap_write_atomic_u32(dap, addr, value);
}

/**
 * Synchronous write of a block of memory, using a specific access size.
 *
 * @param ap The MEM-AP to access.
 * @param buffer The data buffer to write. No particular alignment is assumed.
 * @param size Which access size to use, in bytes. 1, 2 or 4.
 * @param count The number of writes to do (in size units, not bytes).
 * @param address Address to be written; it must be writable by the currently selected MEM-AP.
 * @param addrinc Whether the target address should be increased for each write or not. This
 *  should normally be true, except when writing to e.g. a FIFO.
 * @return ERROR_OK on success, otherwise an error code.
 */
static int mem_ap_write(struct adiv5_ap *ap, const uint8_t *buffer, uint32_t size, uint32_t count,
		uint32_t address, bool addrinc)
{
	struct adiv5_dap *dap = ap->dap;
	size_t nbytes = size * count;
	const uint32_t csw_addrincr = addrinc ? CSW_ADDRINC_SINGLE : CSW_ADDRINC_OFF;
	uint32_t csw_size;
	uint32_t addr_xor;
	int retval;

	/* TI BE-32 Quirks mode:
	 * Writes on big-endian TMS570 behave very strangely. Observed behavior:
	 *   size   write address   bytes written in order
	 *   4      TAR ^ 0         (val >> 24), (val >> 16), (val >> 8), (val)
	 *   2      TAR ^ 2         (val >> 8), (val)
	 *   1      TAR ^ 3         (val)
	 * For example, if you attempt to write a single byte to address 0, the processor
	 * will actually write a byte to address 3.
	 *
	 * To make writes of size < 4 work as expected, we xor a value with the address before
	 * setting the TAP, and we set the TAP after every transfer rather then relying on
	 * address increment. */

	if (size == 4) {
		csw_size = CSW_SIZE_32BIT;
		addr_xor = 0;
	} else if (size == 2) {
		csw_size = CSW_SIZE_16BIT;
		addr_xor = dap->ti_be_32_quirks ? 2 : 0;
	} else if (size == 1) {
		csw_size = CSW_SIZE_8BIT;
		addr_xor = dap->ti_be_32_quirks ? 3 : 0;
	} else {
		return ERROR_TARGET_UNALIGNED_ACCESS;
	}

	if (ap->unaligned_access_bad && (address % size != 0))
		return ERROR_TARGET_UNALIGNED_ACCESS;

	retval = mem_ap_setup_tar(ap, address ^ addr_xor);
	if (retval != ERROR_OK)
		return retval;

	while (nbytes > 0) {
		uint32_t this_size = size;

		/* Select packed transfer if possible */
		if (addrinc && ap->packed_transfers && nbytes >= 4
				&& max_tar_block_size(ap->tar_autoincr_block, address) >= 4) {
			this_size = 4;
			retval = mem_ap_setup_csw(ap, csw_size | CSW_ADDRINC_PACKED);
		} else {
			retval = mem_ap_setup_csw(ap, csw_size | csw_addrincr);
		}

		if (retval != ERROR_OK)
			break;

		/* How many source bytes each transfer will consume, and their location in the DRW,
		 * depends on the type of transfer and alignment. See ARM document IHI0031C. */
		uint32_t outvalue = 0;
		if (dap->ti_be_32_quirks) {
			switch (this_size) {
			case 4:
				outvalue |= (uint32_t)*buffer++ << 8 * (3 ^ (address++ & 3) ^ addr_xor);
				outvalue |= (uint32_t)*buffer++ << 8 * (3 ^ (address++ & 3) ^ addr_xor);
				outvalue |= (uint32_t)*buffer++ << 8 * (3 ^ (address++ & 3) ^ addr_xor);
				outvalue |= (uint32_t)*buffer++ << 8 * (3 ^ (address++ & 3) ^ addr_xor);
				break;
			case 2:
				outvalue |= (uint32_t)*buffer++ << 8 * (1 ^ (address++ & 3) ^ addr_xor);
				outvalue |= (uint32_t)*buffer++ << 8 * (1 ^ (address++ & 3) ^ addr_xor);
				break;
			case 1:
				outvalue |= (uint32_t)*buffer++ << 8 * (0 ^ (address++ & 3) ^ addr_xor);
				break;
			}
		} else {
			switch (this_size) {
			case 4:
				outvalue |= (uint32_t)*buffer++ << 8 * (address++ & 3);
				outvalue |= (uint32_t)*buffer++ << 8 * (address++ & 3);
			case 2:
				outvalue |= (uint32_t)*buffer++ << 8 * (address++ & 3);
			case 1:
				outvalue |= (uint32_t)*buffer++ << 8 * (address++ & 3);
			}
		}

		nbytes -= this_size;

		retval = dap_queue_ap_write(ap, MEM_AP_REG_DRW, outvalue);
		if (retval != ERROR_OK)
			break;

		/* Rewrite TAR if it wrapped or we're xoring addresses */
		if (addrinc && (addr_xor || (address % ap->tar_autoincr_block < size && nbytes > 0))) {
			retval = mem_ap_setup_tar(ap, address ^ addr_xor);
			if (retval != ERROR_OK)
				break;
		}
	}

	/* REVISIT: Might want to have a queued version of this function that does not run. */
	if (retval == ERROR_OK)
		retval = dap_run(dap);

	if (retval != ERROR_OK) {
		uint32_t tar;
		if (dap_queue_ap_read(ap, MEM_AP_REG_TAR, &tar) == ERROR_OK
				&& dap_run(dap) == ERROR_OK)
			LOG_ERROR("Failed to write memory at 0x%08"PRIx32, tar);
		else
			LOG_ERROR("Failed to write memory and, additionally, failed to find out where");
	}

	return retval;
}

/**
 * Synchronous read of a block of memory, using a specific access size.
 *
 * @param ap The MEM-AP to access.
 * @param buffer The data buffer to receive the data. No particular alignment is assumed.
 * @param size Which access size to use, in bytes. 1, 2 or 4.
 * @param count The number of reads to do (in size units, not bytes).
 * @param address Address to be read; it must be readable by the currently selected MEM-AP.
 * @param addrinc Whether the target address should be increased after each read or not. This
 *  should normally be true, except when reading from e.g. a FIFO.
 * @return ERROR_OK on success, otherwise an error code.
 */
static int mem_ap_read(struct adiv5_ap *ap, uint8_t *buffer, uint32_t size, uint32_t count,
		uint32_t adr, bool addrinc)
{
	struct adiv5_dap *dap = ap->dap;
	size_t nbytes = size * count;
	const uint32_t csw_addrincr = addrinc ? CSW_ADDRINC_SINGLE : CSW_ADDRINC_OFF;
	uint32_t csw_size;
	uint32_t address = adr;
	int retval;

	/* TI BE-32 Quirks mode:
	 * Reads on big-endian TMS570 behave strangely differently than writes.
	 * They read from the physical address requested, but with DRW byte-reversed.
	 * For example, a byte read from address 0 will place the result in the high bytes of DRW.
	 * Also, packed 8-bit and 16-bit transfers seem to sometimes return garbage in some bytes,
	 * so avoid them. */

	if (size == 4)
		csw_size = CSW_SIZE_32BIT;
	else if (size == 2)
		csw_size = CSW_SIZE_16BIT;
	else if (size == 1)
		csw_size = CSW_SIZE_8BIT;
	else
		return ERROR_TARGET_UNALIGNED_ACCESS;

	if (ap->unaligned_access_bad && (adr % size != 0))
		return ERROR_TARGET_UNALIGNED_ACCESS;

	/* Allocate buffer to hold the sequence of DRW reads that will be made. This is a significant
	 * over-allocation if packed transfers are going to be used, but determining the real need at
	 * this point would be messy. */
	uint32_t *read_buf = malloc(count * sizeof(uint32_t));
	uint32_t *read_ptr = read_buf;
	if (read_buf == NULL) {
		LOG_ERROR("Failed to allocate read buffer");
		return ERROR_FAIL;
	}

	retval = mem_ap_setup_tar(ap, address);
	if (retval != ERROR_OK) {
		free(read_buf);
		return retval;
	}

	/* Queue up all reads. Each read will store the entire DRW word in the read buffer. How many
	 * useful bytes it contains, and their location in the word, depends on the type of transfer
	 * and alignment. */
	while (nbytes > 0) {
		uint32_t this_size = size;

		/* Select packed transfer if possible */
		if (addrinc && ap->packed_transfers && nbytes >= 4
				&& max_tar_block_size(ap->tar_autoincr_block, address) >= 4) {
			this_size = 4;
			retval = mem_ap_setup_csw(ap, csw_size | CSW_ADDRINC_PACKED);
		} else {
			retval = mem_ap_setup_csw(ap, csw_size | csw_addrincr);
		}
		if (retval != ERROR_OK)
			break;

		retval = dap_queue_ap_read(ap, MEM_AP_REG_DRW, read_ptr++);
		if (retval != ERROR_OK)
			break;

		nbytes -= this_size;
		address += this_size;

		/* Rewrite TAR if it wrapped */
		if (addrinc && address % ap->tar_autoincr_block < size && nbytes > 0) {
			retval = mem_ap_setup_tar(ap, address);
			if (retval != ERROR_OK)
				break;
		}
	}

	if (retval == ERROR_OK)
		retval = dap_run(dap);

	/* Restore state */
	address = adr;
	nbytes = size * count;
	read_ptr = read_buf;

	/* If something failed, read TAR to find out how much data was successfully read, so we can
	 * at least give the caller what we have. */
	if (retval != ERROR_OK) {
		uint32_t tar;
		if (dap_queue_ap_read(ap, MEM_AP_REG_TAR, &tar) == ERROR_OK
				&& dap_run(dap) == ERROR_OK) {
			LOG_ERROR("Failed to read memory at 0x%08"PRIx32, tar);
			if (nbytes > tar - address)
				nbytes = tar - address;
		} else {
			LOG_ERROR("Failed to read memory and, additionally, failed to find out where");
			nbytes = 0;
		}
	}

	/* Replay loop to populate caller's buffer from the correct word and byte lane */
	while (nbytes > 0) {
		uint32_t this_size = size;

		if (addrinc && ap->packed_transfers && nbytes >= 4
				&& max_tar_block_size(ap->tar_autoincr_block, address) >= 4) {
			this_size = 4;
		}

		if (dap->ti_be_32_quirks) {
			switch (this_size) {
			case 4:
				*buffer++ = *read_ptr >> 8 * (3 - (address++ & 3));
				*buffer++ = *read_ptr >> 8 * (3 - (address++ & 3));
			case 2:
				*buffer++ = *read_ptr >> 8 * (3 - (address++ & 3));
			case 1:
				*buffer++ = *read_ptr >> 8 * (3 - (address++ & 3));
			}
		} else {
			switch (this_size) {
			case 4:
				*buffer++ = *read_ptr >> 8 * (address++ & 3);
				*buffer++ = *read_ptr >> 8 * (address++ & 3);
			case 2:
				*buffer++ = *read_ptr >> 8 * (address++ & 3);
			case 1:
				*buffer++ = *read_ptr >> 8 * (address++ & 3);
			}
		}

		read_ptr++;
		nbytes -= this_size;
	}

	free(read_buf);
	return retval;
}

int mem_ap_read_buf(struct adiv5_ap *ap,
		uint8_t *buffer, uint32_t size, uint32_t count, uint32_t address)
{
	return mem_ap_read(ap, buffer, size, count, address, true);
}

int mem_ap_write_buf(struct adiv5_ap *ap,
		const uint8_t *buffer, uint32_t size, uint32_t count, uint32_t address)
{
	return mem_ap_write(ap, buffer, size, count, address, true);
}

int mem_ap_read_buf_noincr(struct adiv5_ap *ap,
		uint8_t *buffer, uint32_t size, uint32_t count, uint32_t address)
{
	return mem_ap_read(ap, buffer, size, count, address, false);
}

int mem_ap_write_buf_noincr(struct adiv5_ap *ap,
		const uint8_t *buffer, uint32_t size, uint32_t count, uint32_t address)
{
	return mem_ap_write(ap, buffer, size, count, address, false);
}

/*--------------------------------------------------------------------------*/

/* CID Class description -- see ARM IHI 0031C Table 9-3 */
static const char *class_description[16] = {
	"Generic verification component", "ROM table", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"Reserved", "Debug (CoreSight) component",
	"Reserved", "Peripheral Test Block",
	"Reserved", "OptimoDE DESS",
	"Generic IP component", "PrimeCell or System component"
};

static bool is_pid_valid(uint64_t PID)
{
	return ((PID >> 40) == 0x0);
}

static bool is_cid_valid(uint32_t CID)
{
	return ((CID & 0xFFFF0FFF) == 0xB105000D);
}

static bool is_pid_cid_valid(uint64_t PID, uint32_t CID)
{
	return (is_pid_valid(PID) && is_cid_valid(CID));
}


/* PIDR4.SIZE = PID[39:36]: 4KB count in Log2
 *   0b0000    1 block
 *   0b0001    2 blocks
 *   0b0010    4 blocks
 *   0b0011    8 blocks
 */
uint32_t get_pid_4k_count(uint64_t PID)
{
	uint32_t count_log2;
	uint32_t count_4k;

	count_log2 = (PID >> 36) & 0xF;		/* # in Log2 */
	count_4k   = 0x1 << count_log2;		/* # in 4KB */

	return count_4k;
}

void mem_ap_interpret_pid(uint64_t PID)
{

}

cid_class_t get_cid_class(uint32_t CID)
{
	/* Component Class at Bits[15:12] */
	return ((CID & 0x0000F000) >> 12);
}

void mem_ap_interpret_cid(uint32_t CID)
{
	cid_class_t cid_class = get_cid_class(CID);

	LOG_DEBUG("class (0x%02x): %s", cid_class, class_description[cid_class]);
}


void mem_ap_interpret_pid_cid(uint64_t PID, uint32_t CID)
{
	mem_ap_interpret_pid(PID);
	mem_ap_interpret_cid(CID);
}

int mem_ap_read_memtype(
	struct adiv5_dap *dap, uint8_t ap, uintmax_t base,
	uint32_t *memtype)
{
	int retval;
	uint32_t type;

	/* Read MEMTYPE registers */
	retval = mem_ap_read_atomic_u32(dap, base | 0xFC0, &type);
	if (retval != ERROR_OK) return retval;

	LOG_DEBUG("MEMTYPE=0x%08x: Debug bus %s",
		type,
		(type & 0x1) ? "+ Sys mem present" : "");

	/* return memtype value */
	if (memtype)
		*memtype = type;

	return ERROR_OK;
}

int mem_ap_read_cid(
	struct adiv5_dap *dap, uint8_t ap, uintmax_t base,
	uint32_t *cid)
{
	int retval;
	uint32_t CIDR[4], CID = 0;

	/* Read Component ID registers */
	retval = mem_ap_read_u32(dap, base | 0xFF0, &(CIDR[0]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFF4, &(CIDR[1]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFF8, &(CIDR[2]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFFC, &(CIDR[3]));
	if (retval != ERROR_OK) return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK) return retval;

	/* Only bits[7:0] of each CIDR are used */
	CID = 0;	CID |= (CIDR[3] & 0xFF);
	CID <<= 8;	CID |= (CIDR[2] & 0xFF);
	CID <<= 8;	CID |= (CIDR[1] & 0xFF);
	CID <<= 8;	CID |= (CIDR[0] & 0xFF);

	LOG_DEBUG("CID=%08X", CID);

	/* Verification: Some PIDR/CIDR have fixed values */
	if (is_cid_valid(CID)) {
//		mem_ap_interpret_cid(CID);
	} else {
		/* non-fatal error, just logging */
		LOG_ERROR("PID/CID invalid (non-fatal)");
	}


	/* return CID values */
	if (cid) {
		*cid = CID;
	}

	return ERROR_OK;
}

/*
 * A debugger must handle the following situations as non-fatal errors:
 * - BaseAddress is a faulting location
 * - The four words starting at (BaseAddress + 0xFF0) are not valid
 *   component ID registers
 * - An entry in the ROM table points to a faulting location
 * - An entry in the ROM table points to a memory block that does not
 *   have a set of four valid Component ID registers starting at
 *   offset 0xFF0
 *
 * Caller MUST guarantee *pid has 8 uint32_t spaces, and *cid has 4
 */
int mem_ap_read_pid_cid(
	struct adiv5_dap *dap, uintmax_t base,
	uint64_t *pid, uint32_t *cid)
{
	int retval;
	uint32_t PIDR[8];
	uint32_t CIDR[4], CID = 0;
	uint64_t PID = 0;

	/* Read Peripheral ID registers */
	retval = mem_ap_read_u32(dap, base | 0xFD0, &(PIDR[4]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFD4, &(PIDR[5]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFD8, &(PIDR[6]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFDC, &(PIDR[7]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFE0, &(PIDR[0]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFE4, &(PIDR[1]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFE8, &(PIDR[2]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFEC, &(PIDR[3]));
	if (retval != ERROR_OK) return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK) return retval;

	/* Only bits[7:0] of each PIDR are used */
	PID = 0;	PID |= (PIDR[7] & 0xFF);
	PID <<= 8;	PID |= (PIDR[6] & 0xFF);
	PID <<= 8;	PID |= (PIDR[5] & 0xFF);
	PID <<= 8;	PID |= (PIDR[4] & 0xFF);
	PID <<= 8;	PID |= (PIDR[3] & 0xFF);
	PID <<= 8;	PID |= (PIDR[2] & 0xFF);
	PID <<= 8;	PID |= (PIDR[1] & 0xFF);
	PID <<= 8;	PID |= (PIDR[0] & 0xFF);


	/* Read Component ID registers */
	retval = mem_ap_read_u32(dap, base | 0xFF0, &(CIDR[0]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFF4, &(CIDR[1]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFF8, &(CIDR[2]));
	if (retval != ERROR_OK) return retval;
	retval = mem_ap_read_u32(dap, base | 0xFFC, &(CIDR[3]));
	if (retval != ERROR_OK) return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK) return retval;

	/* Only bits[7:0] of each CIDR are used */
	CID = 0;	CID |= (CIDR[3] & 0xFF);
	CID <<= 8;	CID |= (CIDR[2] & 0xFF);
	CID <<= 8;	CID |= (CIDR[1] & 0xFF);
	CID <<= 8;	CID |= (CIDR[0] & 0xFF);

//	LOG_DEBUG("PID=0x%.*" PRIX64 ", CID=%08X", 16, PID, CID);

	/* Verification: Some PIDR/CIDR have fixed values */
	if (is_pid_cid_valid(PID, CID)) {
//		mem_ap_interpret_pid_cid(PID, CID);
	} else {
		/* non-fatal error, just logging */
		LOG_ERROR("PID/CID invalid (non-fatal)");
	}


	/* return PID/CID values */
	if (pid) {
		*pid = PID;
	}
	if (cid) {
		*cid = CID;
	}

	return ERROR_OK;
}

#if 0 /* good */
int mem_ap_read_registers(struct adiv5_dap *dap, uint8_t ap)
{
	int retval;

	struct mem_ap_regs *regs;
	uint32_t csw, tar, tar_la, mbt, base, base_la, cfg;
	uintmax_t baseaddr;

	mem_ap_regs = malloc( sizeof(struct mem_ap_regs) );
	if (mem_ap_regs == NULL) {
		LOG_ERROR("Failed to allocate mem_ap_regs structure");
		return ERROR_FAIL;
	}


	dap_ap_select(dap, ap);

	retval = dap_queue_ap_read(dap, MEM_AP_REG_CSW, &csw);
	if (retval != ERROR_OK) return retval;

	retval = dap_queue_ap_read(dap, MEM_AP_REG_TAR, &tar);
	if (retval != ERROR_OK) return retval;

	retval = dap_queue_ap_read(dap, MEM_AP_REG_TAR_LA, &tar_la);
	if (retval != ERROR_OK) return retval;

	/* We do NOT dump DRW, BD0-3.
	 * These registers cannot be read until the memory access has completed
	 */

	retval = dap_queue_ap_read(dap, MEM_AP_REG_MBT, &mbt);
	if (retval != ERROR_OK) return retval;

	retval = dap_queue_ap_read(dap, MEM_AP_REG_BASE_LA, &base_la);
	if (retval != ERROR_OK) return retval;

	retval = dap_queue_ap_read(dap, MEM_AP_REG_CFG, &cfg);
	if (retval != ERROR_OK) return retval;

	retval = dap_queue_ap_read(dap, MEM_AP_REG_BASE, &base);
	if (retval != ERROR_OK) return retval;

	retval = dap_run(dap);
	if (retval != ERROR_OK) return retval;

	LOG_DEBUG("AP 0x%02x: csw=%08x tar_la/tar=%08x/%08x mbt=%08x base_la/base=%08x/%08x cfg=%08x",
		ap,
		csw,
		tar_la, tar,
		mbt,
		base_la, base,
		cfg);

	/* ARM Juno r1 has the value of 0xE00FF003 */
	if (base == 0xFFFFFFFF) {
		/* Legacy format when no debug entries are present */
		baseaddr = 0x0;
	} else if ((base & 0x2) == 0) {
		/* bit[1]=0: Legacy format for specifying BASEADDR */

		/* Bits[11:0] should be zero. Not checking, just zero it out */
		baseaddr = base & 0xFFFFF000;
	} else {
		/* bit[1]=1: ARM Debug Interface v5 format */
		baseaddr = base & 0xFFFFF000;

		/* Debug register or ROM table address */
		if (base & 0x1) {	/* bit[0]=1: Debug entry present */
			/* Debug entry present */
		} else {
			/* No debug entry present -> ROM table */
		}
	}

	if (base != 0xFFFFFFFF) {
		retval = mem_ap_read_pid_cid(dap, baseaddr, NULL, NULL);
	}

	return ERROR_OK;
}
#endif

/* CoreSight ID description table */
/* [Major][Sub]
     Major: 0x7-0xF are Reserved
     Sub  : 0x7-0xF are Reserved for all Major type
 */
#define	CS_DEV_MAJOR_MAX	(0x6)
#define	CS_DEV_SUB_MAX		(0x6)

static char *csid_major_descriptions[CS_DEV_MAJOR_MAX+1] = {
	"Miscellaneous",	"Trace Sink",		"Trace Link",
	"Trace Source",		"Debug Control",	"Debug Logic",
	"PMU"
};

static char *csid_sub_descriptions[CS_DEV_MAJOR_MAX+1][CS_DEV_SUB_MAX+1] = {
	/* 0x0: Miscellaneous */
	{"Other",       "Reserved",     "Reserved",     "Reserved",
	                "Validation",   "Reserved",     "Reserved"},
	/* 0x1: Trace Sink */
	{"Other",       "Trace port",   "Buffer (ETB)", "Router",
	                "Reserved",     "Reserved",     "Reserved"},
	/* 0x2: Trace Link */
	{"Other",       "Funnel/Router","Filter",       "FIFO/Large Buffer",
	                "Reserved",     "Reserved",     "Reserved"},
	/* 0x3: Trace Source */
	{"Other",       "Processor",    "DSP",          "Engine/Coprocessor",
	                "Bus",          "Reserved",     "Software"},
	/* 0x4: Debug Control */
	{"Other",       "Trig Matrix",  "Auth Module",  "Power Req",
	                "Reserved",     "Reserved",     "Reserved"},
	/* 0x5: Debug Logic */
	{"Other",       "Processor",    "DSP",          "Engine/Coprocessor",
	                "Bus",          "Mem(BIST)",    "Reserved"},
	/* 0x6: Performance Monitor */
	{"Other",       "Processor",    "DSP",          "Engine/Coprocessor",
	                "Bus",          "Mem(MMU)",     "Reserved"}

	/*
	 * CTI: Cross Trigger Interface
	 * PMU: Performance Monitor Unit
	 * ETM: Embedded Trace marcrocell
	 */
};

int mem_ap_examine_coresight(
	struct adiv5_dap *dap,
	uintmax_t base)
{
	int retval;
	uint32_t devtype;
	uint8_t dev_sub, dev_major;

	retval = mem_ap_read_atomic_u32(dap, base | 0xFCC, &devtype);
	if (retval != ERROR_OK) return retval;

	dev_sub   = (devtype & 0xF0) >> 4;	/* bit 7:4 */
	dev_major = (devtype & 0x0F);		/* bit 3:0 */

	if (dev_major > CS_DEV_MAJOR_MAX)
		LOG_INFO("Unknown CoreSight major devtype (major=0x%x,sub=0x%x)",
			dev_major, dev_sub);
	else {
		LOG_INFO("CoreSight devtype(0x%x,0x%x) = %s - %s",
			dev_major, dev_sub,
			csid_major_descriptions[dev_major],
			(dev_sub <= CS_DEV_SUB_MAX)
				? csid_sub_descriptions[dev_major][dev_sub]
				: "Unknown"
		);
	}

	return ERROR_OK;
}


int mem_ap_read_registers(
	struct adiv5_dap *dap,
	uint8_t ap,
	struct _mem_ap_regs *regs)
{
	int retval;

//	struct mem_ap_regs *regs;
//	uint32_t csw, tar, tar_la, mbt, base, base_la, cfg;
//	uintmax_t baseaddr;
	if (regs == NULL) return ERROR_FAIL;


	dap_ap_select(dap, ap);

	retval = dap_queue_ap_read(dap, MEM_AP_REG_CSW, &(regs->csw));
	if (retval != ERROR_OK) return retval;

	retval = dap_queue_ap_read(dap, MEM_AP_REG_TAR, &(regs->tar));
	if (retval != ERROR_OK) return retval;

	retval = dap_queue_ap_read(dap, MEM_AP_REG_TAR_LA, &(regs->tar_la));
	if (retval != ERROR_OK) return retval;

	/* We do NOT read DRW, BD0-3.
	 * These registers cannot be read until the memory access has completed
	 */

	retval = dap_queue_ap_read(dap, MEM_AP_REG_MBT, &(regs->mbt));
	if (retval != ERROR_OK) return retval;

	retval = dap_queue_ap_read(dap, MEM_AP_REG_BASE_LA, &(regs->base_la));
	if (retval != ERROR_OK) return retval;

	retval = dap_queue_ap_read(dap, MEM_AP_REG_CFG, &(regs->cfg));
	if (retval != ERROR_OK) return retval;

	retval = dap_queue_ap_read(dap, MEM_AP_REG_BASE, &(regs->base));
	if (retval != ERROR_OK) return retval;

	retval = dap_run(dap);
	if (retval != ERROR_OK) return retval;

	LOG_DEBUG("AP 0x%02x: csw=%08x tar_la/tar=%08x/%08x "
		"mbt=%08x base_la/base=%08x/%08x cfg=%08x",
		ap,
		regs->csw,
		regs->tar_la, regs->tar,
		regs->mbt,
		regs->base_la, regs->base,
		regs->cfg);

	return ERROR_OK;
}

int dp_scan_romtable(
	struct adiv5_dap *dap,
	uintmax_t baseaddr,
	uint8_t tbl_level)
//	struct _mem_ap_regs *regs)
{
	int retval;
	uint32_t entry_offset;
	uint32_t romentry;
	uintmax_t component_base;
	struct _rom_entry entry;

	if (dap == NULL)	return ERROR_FAIL;



	/*
	 * Now we read ROM table entries until we
	 *   reach the end of the ROM Table (0xF00)
	 *   or hit a blank entry (0x00000000)
	 */
	for (entry_offset = 0; entry_offset < 0xF00 ; entry_offset += 4) {
		retval = mem_ap_read_atomic_u32(dap,
				baseaddr | entry_offset,
				&romentry);
		if (retval != ERROR_OK)
			continue;

		LOG_DEBUG("---- Entry[0x%03X/0x%" PRIXMAX "] = 0x%08x %s",
			entry_offset, baseaddr, romentry,
			(romentry==0x0 ? "(end of table)" : ""));

		if (romentry == 0x0) {	/* Table end marker */
			break;
		}


		/* decode entry fields
		 * IHI0031C: Table 10-2 Format of a ROM Table entry
		 */
		entry.addr_ofst			= romentry & (0xFFFFF << 12);	/* signed */
		entry.power_domain_id		= (romentry & (0x1F << 4)) >> 4;
		entry.power_domain_id_valid	= (romentry & ( 0x1 << 2)) >> 2;
		entry.format			= (romentry & ( 0x1 << 1)) >> 1;
		entry.present			= (romentry & ( 0x1 << 0)) >> 0;

		/* 10.2.2 Empty entries and the end of the ROM Table
		 *   When scanning the ROM Table, an entry marked as
		 * not present must be skipped. However you must not assume
		 * that an entry that is marked as not present represents
		 * the end of the ROM Table
		 */
		if (! entry.present) {	/* Empty entry */
//			command_print(cmd_ctx, "\t\tComponent not present");
			LOG_DEBUG("component not present at 0x%x", entry_offset);
			continue;
		}

		if (! entry.format) {	/* Not a 32-bit ROM entry format */
			continue;
		}



/*
		LOG_DEBUG("ofst=0x%08x, power id=0x%x (valid=%d), 32-bit ROM table format=%d, entry present=%d",
			entry.addr_ofst,
			entry.power_domain_id, entry.power_domain_id_valid,
			entry.format, entry.present);
*/
		LOG_DEBUG("ofst=0x%08x, pwrID=0x%x(valid=%d)",
			entry.addr_ofst,
			entry.power_domain_id, entry.power_domain_id_valid);


		/* 10.2.1 Component descriptions and the component base address */
		/* Component_Base = ROM_Base + Address offset */
		component_base = baseaddr + entry.addr_ofst;
//		LOG_DEBUG("component_base(last 4KB)=0x%.16" PRIxMAX, component_base);

		retval = mem_ap_read_pid_cid(dap, component_base,
				&(entry.PID), &(entry.CID));
		if (! is_pid_cid_valid(entry.PID, entry.CID))
			continue;

		/* find the 1st 4KB address (true component base address ) */
		component_base -= 0x1000 * (get_pid_4k_count(entry.PID) - 1);
		LOG_DEBUG("base(1st 4KB)=0x%.16" PRIxMAX, component_base);

		switch (get_cid_class(entry.CID))
		{
		case CC_VERIFICATION:	/* Generic verification component */
			/* TBD */
			break;

		case CC_ROM:		/* ROM Table */
			retval = dp_scan_romtable(dap, component_base, tbl_level+1);
			break;

		case CC_DEBUG:		/* Debug (CoreSight) component */
			retval = mem_ap_examine_coresight(dap, component_base);
			/* TBD */
			break;

		case CC_PTB:		/* Peripheral Test Block (PTB) */
			/* TBD */
			break;

		case CC_DESS:		/* OptimoDE Data Engine SubSystem component */
			/* TBD */
			break;

		case CC_IP:		/* Generic IP component */
			/* TBD */
			break;

		case CC_PCELL:		/* PrimeCell peripheral */
			/* TBD */
			break;

		default:		/* Invalid component class type */
			break;

		}	/* End of switch (cid class) */


	} /* End of for(entry_offset) */

	return ERROR_OK;
}

int dp_scan_mem_ap(struct adiv5_dap *dap, uint8_t ap)
{
	int retval;
	struct _mem_ap_regs regs;
//	mem_ap_regs_t mem_ap_regs;


	retval = mem_ap_read_registers(dap, ap, &regs);
	if (retval != ERROR_OK)
		return retval;


	/* ARM Juno r1 has the value of 0xE00FF003 */
	if (regs.base == 0xFFFFFFFF) {
		/* Legacy format when no debug entries are present */
		regs.baseaddr = 0x0;
	} else if ((regs.base & 0x2) == 0) {
		/* bit[1]=0: Legacy format for specifying BASEADDR */

		/* Bits[11:0] should be zero. Not checking, just zero it out */
		regs.baseaddr = regs.base & 0xFFFFF000;
	} else {
		/* bit[1]=1: ARM Debug Interface v5 format */
		regs.baseaddr = regs.base & 0xFFFFF000;

		/* Debug register or ROM table address */
		if (regs.base & 0x1) {	/* bit[0]=1: Debug entry present */
			/* Debug entry present */
		} else {
			/* No debug entry present -> ROM table */
		}
	}

	/* PID/CID depends on valid entry */
	if (! has_mem_ap_entry(&regs))
		return ERROR_OK;

LOG_DEBUG("----- Trace line ----- : baseaddr=0x%"PRIXMAX, regs.baseaddr);
#if 0
	retval = mem_ap_read_pid_cid(dap,
			regs.baseaddr,
			&(regs.PID),
			&(regs.CID));
#else
	retval = mem_ap_read_cid(dap, ap,
			regs.baseaddr,
			&(regs.CID));
#endif
	if (retval != ERROR_OK) return retval;


	retval = mem_ap_read_memtype(dap, ap, regs.baseaddr, &(regs.memtype));
	if (retval != ERROR_OK) return retval;


	switch (get_cid_class(regs.CID))
	{
	case CC_VERIFICATION:	/* Generic verification component */
		/* TBD */
		break;

	case CC_ROM:		/* ROM Table */
		retval = dp_scan_romtable(dap, regs.baseaddr, 0 /*root*/);
		break;

	case CC_DEBUG:		/* Debug (CoreSight) component */
		retval = mem_ap_examine_coresight(dap, regs.baseaddr);
		/* TBD */
		break;

	case CC_PTB:		/* Peripheral Test Block (PTB) */
		/* TBD */
		break;

	case CC_DESS:		/* OptimoDE Data Engine SubSystem component */
		/* TBD */
		break;

	case CC_IP:		/* Generic IP component */
		/* TBD */
		break;

	case CC_PCELL:		/* PrimeCell peripheral */
		/* TBD */
		break;

	default:		/* Invalid component class type */
		break;

	}	/* End of switch (cid class) */


	return ERROR_OK;
}

int dp_scan_jtag_ap(struct adiv5_dap *dap, uint8_t ap)
{
//	int retval;


	return ERROR_OK;
}

/*--------------------------------------------------------------------------*/

#define DAP_POWER_DOMAIN_TIMEOUT (10)

/* FIXME don't import ... just initialize as
 * part of DAP transport setup
*/
extern const struct dap_ops jtag_dp_ops;

/*--------------------------------------------------------------------------*/

/**
 * Create a new DAP
 */
struct adiv5_dap *dap_init(void)
{
	struct adiv5_dap *dap = calloc(1, sizeof(struct adiv5_dap));
	int i;
	/* Set up with safe defaults */
	for (i = 0; i <= 255; i++) {
		dap->ap[i].dap = dap;
		dap->ap[i].ap_num = i;
		/* memaccess_tck max is 255 */
		dap->ap[i].memaccess_tck = 255;
		/* Number of bits for tar autoincrement, impl. dep. at least 10 */
		dap->ap[i].tar_autoincr_block = (1<<10);
	}
	INIT_LIST_HEAD(&dap->cmd_journal);
	return dap;
}

/**
 * Initialize a DAP.  This sets up the power domains, prepares the DP
 * for further use and activates overrun checking.
 *
 * @param dap The DAP being initialized.
 */
int dap_dp_init(struct adiv5_dap *dap)
{
	int retval;

	LOG_DEBUG(" ");
	/* JTAG-DP or SWJ-DP, in JTAG mode
	 * ... for SWD mode this is patched as part
	 * of link switchover
	 * FIXME: This should already be setup by the respective transport specific DAP creation.
	 */
	if (!dap->ops)
		dap->ops = &jtag_dp_ops;

	dap->select = DP_SELECT_INVALID;
	dap->last_read = NULL;

	for (size_t i = 0; i < 10; i++) {
		/* DP initialization */

		retval = dap_queue_dp_read(dap, DP_CTRL_STAT, NULL);
		if (retval != ERROR_OK)
			continue;

		retval = dap_queue_dp_write(dap, DP_CTRL_STAT, SSTICKYERR);
		if (retval != ERROR_OK)
			continue;

		retval = dap_queue_dp_read(dap, DP_CTRL_STAT, NULL);
		if (retval != ERROR_OK)
			continue;

		dap->dp_ctrl_stat = CDBGPWRUPREQ | CSYSPWRUPREQ;
		retval = dap_queue_dp_write(dap, DP_CTRL_STAT, dap->dp_ctrl_stat);
		if (retval != ERROR_OK)
			continue;

		/* Check that we have debug power domains activated */
		LOG_DEBUG("DAP: wait CDBGPWRUPACK");
		retval = dap_dp_poll_register(dap, DP_CTRL_STAT,
					      CDBGPWRUPACK, CDBGPWRUPACK,
					      DAP_POWER_DOMAIN_TIMEOUT);
		if (retval != ERROR_OK)
			continue;

		LOG_DEBUG("DAP: wait CSYSPWRUPACK");
		retval = dap_dp_poll_register(dap, DP_CTRL_STAT,
					      CSYSPWRUPACK, CSYSPWRUPACK,
					      DAP_POWER_DOMAIN_TIMEOUT);
		if (retval != ERROR_OK)
			continue;

		retval = dap_queue_dp_read(dap, DP_CTRL_STAT, NULL);
		if (retval != ERROR_OK)
			continue;

		/* With debug power on we can activate OVERRUN checking */
		dap->dp_ctrl_stat = CDBGPWRUPREQ | CSYSPWRUPREQ | CORUNDETECT;
		retval = dap_queue_dp_write(dap, DP_CTRL_STAT, dap->dp_ctrl_stat);
		if (retval != ERROR_OK)
			continue;
		retval = dap_queue_dp_read(dap, DP_CTRL_STAT, NULL);
		if (retval != ERROR_OK)
			continue;

		retval = dap_run(dap);
		if (retval != ERROR_OK)
			continue;

		break;
	}

	return retval;
}

/**
 * Initialize a DAP.  This sets up the power domains, prepares the DP
 * for further use, and arranges to use AP #0 for all AP operations
 * until dap_ap-select() changes that policy.
 *
 * @param ap The MEM-AP being initialized.
 */
int mem_ap_init(struct adiv5_ap *ap)
{
	/* check that we support packed transfers */
	uint32_t csw, cfg;
	int retval;
	struct adiv5_dap *dap = ap->dap;

	retval = mem_ap_setup_transfer(ap, CSW_SIZE_8BIT | CSW_ADDRINC_PACKED, 0);
	if (retval != ERROR_OK)
		return retval;

	retval = dap_queue_ap_read(ap, MEM_AP_REG_CSW, &csw);
	if (retval != ERROR_OK)
		return retval;

	retval = dap_queue_ap_read(ap, MEM_AP_REG_CFG, &cfg);
	if (retval != ERROR_OK)
		return retval;

	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	if (csw & CSW_ADDRINC_PACKED)
		ap->packed_transfers = true;
	else
		ap->packed_transfers = false;

	/* Packed transfers on TI BE-32 processors do not work correctly in
	 * many cases. */
	if (dap->ti_be_32_quirks)
		ap->packed_transfers = false;

	LOG_DEBUG("MEM_AP Packed Transfers: %s",
			ap->packed_transfers ? "enabled" : "disabled");

	/* The ARM ADI spec leaves implementation-defined whether unaligned
	 * memory accesses work, only work partially, or cause a sticky error.
	 * On TI BE-32 processors, reads seem to return garbage in some bytes
	 * and unaligned writes seem to cause a sticky error.
	 * TODO: it would be nice to have a way to detect whether unaligned
	 * operations are supported on other processors. */
	ap->unaligned_access_bad = dap->ti_be_32_quirks;

	LOG_DEBUG("MEM_AP CFG: large data %d, long address %d, big-endian %d",
			!!(cfg & 0x04), !!(cfg & 0x02), !!(cfg & 0x01));

	return ERROR_OK;
}

/* Three power domains are modeled:
 *   Always-on power domain: Must be on for the debugger to connect to the device.
 *   System power domain   : This includes system components.
 *   Debug power domain    : This includes all of the debug subsystem.
 *
 * DP registers reside in the always-on power domain. Therefore, they can always
 * be driven, enabling powerup requests to be made to a system power controller.
 *
 * When CSYSPWRUPREQ is asserted, CDBGPWRUPREQ must also be asserted.
 *   Request of {CSYSPWRUPREQ,CDBGPWRUPREQ} = {1,0} is UNPREDICTABLE.
 */
static int dap_power_on(struct adiv5_dap *dap, uint32_t pwr_req)
{
	int retval;
	uint32_t pwr_ack = 0;

LOG_DEBUG("pwr_req = 0x%x", pwr_req);

	/* When CSYSPWRUPREQ is asserted, CDBGPWRUPREQ must also be asserted */
	if (pwr_req & CSYSPWRUPREQ)	pwr_req |= CDBGPWRUPREQ;
	if (pwr_req & CORUNDETECT)	pwr_req |= CDBGPWRUPREQ; /* (same) */

	/* Set ACK mask bits for comparison later */
	if (pwr_req & CDBGPWRUPREQ)	pwr_ack |= CDBGPWRUPACK;
	if (pwr_req & CSYSPWRUPREQ)	pwr_ack |= CSYSPWRUPACK;


	/* The sequence is important
	 *   1. Clear Error bits (optional)
	 *   2. Reset Debug domain
	 *   3. Enable Power
	 */

//LOG_DEBUG("----- Trace line -----");
	/* Init:Power Off, No lane masking, Normal AP transfer mode,
		OverRun detection off, Clear any error (W1C) */
	/* WARNING: do not call dap_dp_reg_set_bits() to clear error bits.
	 * See the description in dap_dp_reg_set_bits() */
	retval = dap_queue_dp_write(dap, DP_CTRL_STAT,
			SSTICKYERR | SSTICKYCMP | SSTICKYORUN);
	/* Clear any error detection (W1C) */
//	retval = dap_dp_reg_set_bits(dap, DP_CTRL_STAT,
//			SSTICKYERR | SSTICKYCMP | SSTICKYORUN);
	if (retval != ERROR_OK) return retval;
	/* If RESET is requested, do it first */
	/* IHI0031C: 2.4.5 Debug reset control */
	if (pwr_req & CDBGRSTREQ) {
		/* Pull signal High */
		retval = dap_dp_reg_set_bits(dap, DP_CTRL_STAT, CDBGRSTREQ);
		if (retval != ERROR_OK) return retval;

		retval = dap_dp_poll_register(dap, DP_CTRL_STAT,
				CDBGRSTACK, CDBGRSTACK,
				DAP_POWER_DOMAIN_TIMEOUT);
		if (retval == ERROR_WAIT) {
			/* It might be okay, just pull it low */
			LOG_WARNING("CDBGRSTACK not responding (not implemented?)");
		} else if (retval != ERROR_OK)
			return retval;

		/* Pull signal Low */
		retval = dap_dp_reg_clear_bits(dap, DP_CTRL_STAT, CDBGRSTREQ | (0x3 << 2) );
		if (retval != ERROR_OK) return retval;

		retval = dap_dp_poll_register(dap, DP_CTRL_STAT,
				CDBGRSTACK, 0,
				DAP_POWER_DOMAIN_TIMEOUT);
		if (retval == ERROR_WAIT) {
			/* This is an Error, STOP here! (return) */
			LOG_ERROR("CDBGSTACK staying HIGH");
		}
		if (retval != ERROR_OK) return retval;

		/* Clear RESET bit so it won't be turned on later */
		pwr_req &= ~CDBGRSTREQ;
	}


LOG_DEBUG("----- Trace line ----- pwr_req=0x%x", pwr_req);
	/* Enable the power domain requested */
	retval = dap_dp_reg_set_bits(dap, DP_CTRL_STAT, pwr_req);
	if (retval != ERROR_OK) return retval;
	retval = dap_dp_poll_register(dap, DP_CTRL_STAT,
			pwr_ack, pwr_ack, DAP_POWER_DOMAIN_TIMEOUT);
	if (retval == ERROR_WAIT)
		LOG_ERROR("Fail to enable Debug Domain Power");
	if (retval != ERROR_OK) return retval;
LOG_DEBUG("----- Trace line -----");


	/* dap->dp_ctrl_stat will be used to reset CTRL/STAT register
	   once error is detected in jtagdp_transaction_endcheck()

	   How about the code above which might also hit sticky error ?
	   It's true that the code does not handle this case, but since we are
	   in power-on stage, just assume/hope that everything will be fine.
	 */
	dap->dp_ctrl_stat = pwr_req;

	return ERROR_OK;
}

static int dap_power_off(struct adiv5_dap *dap, uint32_t pwr_req)
{
	int retval;
	uint32_t pwr_ack = 0;

	/* Power domains dependency: see dap_power_on() */
	if (pwr_req & CDBGPWRUPREQ)	pwr_req |= CSYSPWRUPREQ;

	/* Set ACK mask bits for comparison later */
	if (pwr_req & CDBGPWRUPREQ)	pwr_ack |= CDBGPWRUPACK;
	if (pwr_req & CSYSPWRUPREQ)	pwr_ack |= CSYSPWRUPACK;

	/* Clear any error detection (W1C) */
	retval = dap_dp_reg_set_bits(dap, DP_CTRL_STAT,
			SSTICKYERR | SSTICKYCMP | SSTICKYORUN);
	if (retval != ERROR_OK) return retval;

	/* Deassert the power domain requested */
	retval = dap_dp_reg_clear_bits(dap, DP_CTRL_STAT, pwr_req);
	if (retval != ERROR_OK) return retval;
	retval = dap_dp_poll_register(dap, DP_CTRL_STAT,
			pwr_ack, 0, DAP_POWER_DOMAIN_TIMEOUT);
	if (retval != ERROR_OK) return retval;

	return ERROR_OK;
}


/**
 * Initialize a DAP.  This sets up the power domains, prepares the DP
 * for further use, and arranges to use AP #0 for all AP operations
 * until dap_ap-select() changes that policy.
 *
 * @param dap The DAP being initialized.
 *
 * @todo Rename this.  We also need an initialization scheme which account
 * for SWD transports not just JTAG; that will need to address differences
 * in layering.  (JTAG is useful without any debug target; but not SWD.)
 * And this may not even use an AHB-AP ... e.g. DAP-Lite uses an APB-AP.
 */
int debugport_init(struct adiv5_dap *dap)
{
	/* check that we support packed transfers */
//	uint32_t csw, cfg;
	uint32_t dp_ctrl_stat;
	int retval;

	LOG_DEBUG(">>>>> Enter");

	/* JTAG-DP or SWJ-DP, in JTAG mode
	 * ... for SWD mode this is patched as part
	 * of link switchover
	 */
LOG_DEBUG("before set OPS(%p) for dap=%p", dap->ops, dap);
	if (!dap->ops)
		dap->ops = &jtag_dp_ops;
LOG_DEBUG("after set OPS(%p) for dap=%p", dap->ops, dap);

#if 0
	retval = dap_dp_read_atomic(dap, DP_CTRL_STAT, &dp_ctrl_stat);
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("DP_CTRL_STAT=0x%x. DbgPwrAck is %s, SysPwrAck is %s",
		dp_ctrl_stat,
		(dp_ctrl_stat & CDBGPWRUPACK) ? "On" : "Off",
		(dp_ctrl_stat & CSYSPWRUPACK) ? "On" : "Off");
#endif

	/* Turn on all power domains after reset */
	/*   With debug power on we can activate OVERRUN checking */
	/*   CDBGRSTREQ has problem on Juno r1 for 2nd TAP */
//	dap_power_on(dap, CDBGRSTREQ | CSYSPWRUPREQ | CDBGPWRUPREQ);
	dap_power_on(dap, CSYSPWRUPREQ | CDBGPWRUPREQ | CORUNDETECT);

#if 0
	/* Transfer mode: Normal operation (bits[3:2]=0b00) */
	retval = dap_dp_reg_clear_bits(dap, DP_CTRL_STAT, (0x3 << 2));
	if (retval != ERROR_OK) return retval;
#endif

	retval = dap_dp_read_atomic(dap, DP_CTRL_STAT, &dp_ctrl_stat);
	if (retval != ERROR_OK) return retval;
	LOG_DEBUG("DP_CTRL_STAT=0x%08x. DbgPwrAck is %s, SysPwrAck is %s",
		dp_ctrl_stat,
		(dp_ctrl_stat & CDBGPWRUPACK) ? "On" : "Off",
		(dp_ctrl_stat & CSYSPWRUPACK) ? "On" : "Off");

//	retval = dap_queue_dp_read(dap, DP_CTRL_STAT, &dp_ctrl_stat);

//#define SSTICKYORUN     (1UL << 1)
/* 3:2 - transaction mode (e.g. pushed compare) */
//#define SSTICKYCMP      (1UL << 4)
//#define SSTICKYERR      (1UL << 5)

#if 0
	/* Maximum AP number is 255 since the SELECT.APSEL is 8-bit */
	for (int ap = 0; ap <= 255; ap++) {
		/* read the IDR register of the Access Port */
		uint32_t id_val;
		dap_ap_select(dap, ap);
		retval = dap_queue_ap_read(dap, AP_REG_IDR, &id_val);
		if (retval != ERROR_OK)
			continue;
		retval = dap_run(dap);	/* do the business */
		if (retval != ERROR_OK)
			continue;

		/* An IDR value of zero indicates that there is no AP present */
		if (id_val == 0x0)
			continue;

		/* Bit definition of AP_REG_IDR (0xFC)
		 *
		 * Revision                     bits[31:28]
		 * JEP106 continuation code     bits[27:24], 4-bits ( 0x4 for ARM)
		 * JEP106 identity code         bits[23:17], 7-bits (0x3B for ARM)
		 * Class                        bits[16:13] (0:JTAG-AP, 0b1000:MEM-AP)
		 * (reserved, SBZ)              bits[12: 8]
		 * AP identification            bits[ 7: 0]
		 *   Variant bits[7:4]
		 *   Type    bits[3:0] (0x0:JTAG, 0x1:AHB, 0x2:APB, 0x4:AXI)
		 */
		enum ap_class class;
		enum ap_type type;

		class = (id_val & IDR_CLASS_MASK) >> IDR_CLASS_SHIFT;
		type = (id_val & IDR_ID_TYPE_MASK) >> IDR_ID_TYPE_SHIFT;
		LOG_DEBUG("AP found at 0x%02x (IDR=0x%08x): Rev=%x, JEP106(cont,code)=(%x,%x), class=%s, ID(var,type)=(%x,%s)",
			ap, id_val,
			(id_val & IDR_REV_MASK) >> IDR_REV_SHIFT,
			(id_val & IDR_JEP106_CONT_MASK) >> IDR_JEP106_CONT_SHIFT,
			(id_val & IDR_JEP106_ID_MASK) >> IDR_JEP106_ID_SHIFT,
			(class == AP_CLASS_MEM) ? "MEM-AP" :
				(class == AP_CLASS_JTAG) ? "JTAG-AP" : "Unknown",
			(id_val & IDR_ID_VART_MASK) >> IDR_ID_VART_SHIFT,
			(type == AP_TYPE_JTAG_AP) ? "JTAG-AP" :
				(type == AP_TYPE_AHB_AP) ? "AHB-AP" :
				(type == AP_TYPE_APB_AP) ? "APB-AP" :
				(type == AP_TYPE_AXI_AP) ? "AXI-AP" : "Unknown");

		switch (class) {
		case AP_CLASS_MEM:	/* Alamy: disable it for now */
			retval = dp_scan_mem_ap(dap, ap);

			break;

		case AP_CLASS_JTAG:
			/* dp_scan_jtag_ap(dap, ap); */
			break;

		default:
			LOG_ERROR("Unknown AP class type");
			break;
		} /* End of switch(type) */

	} /* End of for(ap) */

	if (retval != ERROR_OK)
		return retval;
#endif	// Scan components

	/* Detect Packet transfer (CSW) */
#if 0
	if (csw & CSW_ADDRINC_PACKED)
		dap->packed_transfers = true;
	else
		dap->packed_transfers = false;
#endif

	/* Packed transfers on TI BE-32 processors do not work correctly in
	 * many cases. */
	if (dap->ti_be_32_quirks)
		dap->packed_transfers = false;

	LOG_DEBUG("MEM_AP Packed Transfers: %s",
			dap->packed_transfers ? "enabled" : "disabled");

	/* The ARM ADI spec leaves implementation-defined whether unaligned
	 * memory accesses work, only work partially, or cause a sticky error.
	 * On TI BE-32 processors, reads seem to return garbage in some bytes
	 * and unaligned writes seem to cause a sticky error.
	 * TODO: it would be nice to have a way to detect whether unaligned
	 * operations are supported on other processors. */
	dap->unaligned_access_bad = dap->ti_be_32_quirks;

#if 0
	/* Display CFG info */
	LOG_DEBUG("MEM_AP CFG: large data %d, long address %d, big-endian %d",
			!!(cfg & 0x04), !!(cfg & 0x02), !!(cfg & 0x01));
#endif

	return ERROR_OK;
}

int debugport_deinit(struct adiv5_dap *dap)
{
	int retval;

	retval = dap_queue_dp_read(dap, DP_CTRL_STAT, &(dap->dp_ctrl_stat));
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("DP_CTRL_STAT=0x%x", dap->dp_ctrl_stat);

	/* Turn off all power domains */
	dap_power_off(dap, CSYSPWRUPREQ | CDBGPWRUPREQ);


	return ERROR_OK;
}

/*--------------------------------------------------------------------------*/

static bool is_dap_cid_ok(uint32_t cid3, uint32_t cid2, uint32_t cid1, uint32_t cid0)
{
	return cid3 == 0xb1 && cid2 == 0x05
			&& ((cid1 & 0x0f) == 0) && cid0 == 0x0d;
}

/*
 * It's hard to predict if ARM is going to use 'bit' or 'value' to define
 * MEM-AP type (AHB,APB,AXI,...) in the future.
 * Unlikely to combine two buses together, and 3 out of 4 bits had been used.
 * The implementation assumes it's 'value'.
 */
static const char *ap_type_description[5] = {
	"JTAG-AP",		/* 0 */
	"AHB-AP",
	"APB-AP",
	"Unknown",
	"AXI-AP"		/* 4 */
};

const char *ap_type_to_string(uint8_t ap_type)
{
	/* Unknown detection */
	if (ap_type > 4)
		ap_type = 3;

	return ap_type_description[ap_type];
}

/*
 * This function checks the ID for each access port to find the requested Access Port type
 */
int dap_find_ap(struct adiv5_dap *dap, enum ap_type type_to_find, struct adiv5_ap **ap_out)
{
	int ap_num;

	/* Maximum AP number is 255 since the SELECT register is 8 bits */
	for (ap_num = 0; ap_num <= 255; ap_num++) {

		/* read the IDR register of the Access Port */
		uint32_t id_val = 0;

		int retval = dap_queue_ap_read(dap_ap(dap, ap_num), AP_REG_IDR, &id_val);
		if (retval != ERROR_OK)
			return retval;

		retval = dap_run(dap);
//		LOG_DEBUG("rc=%d: reg[0x%x] = 0x%x", retval, AP_REG_IDR, id_val);

		if (retval != ERROR_OK)
			return retval;

		/* An IDR value of zero indicates that there is no AP present */
		if (id_val == 0x0)
			continue;
		LOG_DEBUG("AP found at %03d, IDR=0x%08x", ap, id_val);

		/* IDR bits:
		 * 31-28 : Revision
		 * 27-24 : JEP106 continuation code (0x4 for ARM)
		 * 23-17 : JEP106 ID code (0x3B for ARM)
		 * 16-13 : Class (0b0000=No defined, 0b1000=Mem-AP)
		 * 12-8  : Reserved
		 *  7-4  : AP Identification Variant
		 *  3-0  : AP Identification Type
		 *         0x0: JTAG-AP. Variant[7:4] must be non-zero
		 *         0x1: AMBA AHB bus.
		 *         0x2: AMBA APB2/APB3 bus.
		 *         0x4: AMBA AXI3/AXI4 bus with optional ACE-Lite support.
		 *         Other: Reserved
		 */

		if ((idr_get_jep106(id_val) == JEP106_ARM) &&	/* JEP106 codes match ARM */
			(idr_get_id_type(id_val) == type_to_find)) {/* type matches */
			/* type verification for WARNING messages */
			switch (type_to_find) {
			case 0x0: if ((id_val & 0xF0) == 0)
					LOG_WARNING("ID Variant is zero for JTAG-AP");
				break;
			case 0x1: /* fall through for MEM-AP */
			case 0x2: /* fall thgough for MEM-AP */
			case 0x4: if ((id_val & (0xF<<13)) != (0x1<<16)) /* MEM-AP */
					LOG_WARNING("Class is not MEM-AP for %s bus",
						(type_to_find == AP_TYPE_AHB_AP) ? "AHB" :
						(type_to_find == AP_TYPE_APB_AP) ? "APB" :
						(type_to_find == AP_TYPE_AXI_AP) ? "AXI" : "Unknown");
				break;
			default:
				LOG_WARNING("Unknown AP type (0x%x)", (id_val & 0xF));
				break;
			}

			LOG_DEBUG("Found %s at AP index: %d (IDR=0x%08" PRIX32 ")",
				ap_type_to_string(type_to_find),
				ap_num, id_val);

			*ap_out = &dap->ap[ap_num];
			return ERROR_OK;
		}	/* End of if (idr_*) */
	}	/* End of for(ap_num) */

	LOG_DEBUG("No %s found", ap_type_to_string(type_to_find));

	return ERROR_FAIL;
}

int dap_get_debugbase(struct adiv5_ap *ap,
			uint32_t *dbgbase, uint32_t *apid)
{
	struct adiv5_dap *dap = ap->dap;
	int retval;

	retval = dap_queue_ap_read(ap, MEM_AP_REG_BASE, dbgbase);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_queue_ap_read(ap, AP_REG_IDR, apid);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

int dap_lookup_cs_component(struct adiv5_ap *ap,
			uint32_t dbgbase, uint8_t type, uint32_t *addr, int32_t *idx)
{
	uint32_t romentry, entry_offset = 0, component_base, devtype;
	int retval;

	*addr = 0;

	do {
		retval = mem_ap_read_atomic_u32(ap, (dbgbase&0xFFFFF000) |
						entry_offset, &romentry);
		if (retval != ERROR_OK)
			return retval;

		component_base = (dbgbase & 0xFFFFF000)
			+ (romentry & 0xFFFFF000);

		if (romentry & 0x1) {
			uint32_t c_cid1;
			retval = mem_ap_read_atomic_u32(ap, component_base | 0xff4, &c_cid1);
			if (retval != ERROR_OK) {
				LOG_ERROR("Can't read component with base address 0x%" PRIx32
					  ", the corresponding core might be turned off", component_base);
				return retval;
			}
			if (((c_cid1 >> 4) & 0x0f) == 1) {
				retval = dap_lookup_cs_component(ap, component_base,
							type, addr, idx);
				if (retval == ERROR_OK)
					break;
				if (retval != ERROR_TARGET_RESOURCE_NOT_AVAILABLE)
					return retval;
			}

			retval = mem_ap_read_atomic_u32(ap,
					(component_base & 0xfffff000) | 0xfcc,
					&devtype);
			if (retval != ERROR_OK)
				return retval;
			if ((devtype & 0xff) == type) {
				if (!*idx) {
					*addr = component_base;
					break;
				} else
					(*idx)--;
			}
		}
		entry_offset += 4;
	} while (romentry > 0);

	if (!*addr)
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;

	return ERROR_OK;
}

int dap_romtable_lookup_cs_component(
	struct adiv5_dap *dap,
	uintmax_t rombase, uint8_t l_devtype,
	uint16_t l_jep106, uint16_t l_partnum,
	int32_t *l_index,
	uint32_t *found_base)
{
	int retval;
	uint32_t entry_offset;		/* Offset to walk through ROM Table */
	uint32_t romentry;
	uintmax_t component_base;
	struct _rom_entry entry;

	if (dap == NULL)		return ERROR_FAIL;
	if (found_base == NULL)	return ERROR_OK;	/* No need to continue */

	/* Clear to zero so we know the component is found or not later */
	*found_base = 0x0;

	/*
	 * Now we read ROM table entries until we
	 *   reach the end of the ROM Table (0xF00)
	 *   or hit a blank entry (0x00000000)
	 */
	for (entry_offset = 0; entry_offset < 0xF00 ; entry_offset += 4) {
		retval = mem_ap_read_atomic_u32(dap,
				rombase | entry_offset,
				&romentry);
		if (retval != ERROR_OK)
			continue;

/*
		LOG_DEBUG("---- Entry[0x%03X/0x%" PRIXMAX "] = 0x%08x %s",
			entry_offset, rombase, romentry,
			(romentry==0x0 ? "(end of table)" : ""));
*/

		if (romentry == 0x0)	/* Table end marker */
			break;


		/* decode entry fields
		 * IHI0031C: Table 10-2 Format of a ROM Table entry
		 */
		entry.addr_ofst			= romentry & (0xFFFFF << 12);	/* signed */
		entry.power_domain_id		= (romentry & (0x1F << 4)) >> 4;
		entry.power_domain_id_valid	= (romentry & ( 0x1 << 2)) >> 2;
		entry.format			= (romentry & ( 0x1 << 1)) >> 1;
		entry.present			= (romentry & ( 0x1 << 0)) >> 0;

		/* 10.2.2 Empty entries and the end of the ROM Table
		 *   When scanning the ROM Table, an entry marked as
		 * not present must be skipped. However you must not assume
		 * that an entry that is marked as not present represents
		 * the end of the ROM Table
		 */
		if (! entry.present)	/* Empty entry */
			continue;

		if (! entry.format)	/* Not a 32-bit ROM entry format */
			continue;

/*
		LOG_DEBUG("ofst=0x%08x, pwrID=0x%x(valid=%d)",
			entry.addr_ofst,
			entry.power_domain_id, entry.power_domain_id_valid);
*/


		/* 10.2.1 Component descriptions and the component base address */
		/* Component_Base = ROM_Base + Address offset */
		component_base = rombase + entry.addr_ofst;
//		LOG_DEBUG("component_base(last 4KB)=0x%.16" PRIxMAX, component_base);

		retval = mem_ap_read_pid_cid(dap, component_base,
				&(entry.PID), &(entry.CID));
		if (! is_pid_cid_valid(entry.PID, entry.CID))
			continue;

		/* find the 1st 4KB address (true component base address ) */
		component_base -= 0x1000 * (get_pid_4k_count(entry.PID) - 1);
//		LOG_DEBUG("base(1st 4KB)=0x%.16" PRIxMAX, component_base);

		switch (get_cid_class(entry.CID))
		{
		uint32_t devtype;
		case CC_ROM:		/* ROM Table */
			retval = dap_romtable_lookup_cs_component(dap,
				component_base, l_devtype,
				l_jep106, l_partnum,
				l_index,
				found_base);
			if ((retval == ERROR_OK) || (retval == ERROR_FAIL))
				return ERROR_OK;
			/* Let's continue if component not found */
			break;

		case CC_DEBUG:		/* Debug (CoreSight) component */
//			uint32_t devtype;
			retval = mem_ap_read_atomic_u32(dap, component_base | 0xFCC, &devtype);
			if (retval != ERROR_OK) return retval;

			/* Matching devtype & PID */
			if ((devtype & 0xFF) != l_devtype)		continue;
			if (pid_get_jep106(entry.PID) != l_jep106)	continue;
			if (pid_get_partnum(entry.PID) != l_partnum)	continue;
//			if ((*l_index)--)				continue;

			/* Found the component */
			if ( !((*l_index)--) ) {
				*found_base = component_base;
				return ERROR_OK;
			}
			break;

		default:
			/* We are not interested in other classes
				CC_VERIFICATION:	Generic verification component
				CC_PTB:			Peripheral Test Block (PTB)
				CC_DESS:		OptimoDE Data Engine SubSystem component
				CC_IP:			Generic IP component
				CC_PCELL:		PrimeCell peripheral
			 */
			break;
		}	/* End of switch (cid class) */
	} /* End of for(entry_offset) */

	/* Woops, component not found */
	return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
}

/* The designer identity code is encoded as:
 * bits 11:8 : JEP106 Bank (number of continuation codes), only valid when bit 7 is 1.
 * bit 7     : Set when bits 6:0 represent a JEP106 ID and cleared when bits 6:0 represent
 *             a legacy ASCII Identity Code.
 * bits 6:0  : JEP106 Identity Code (without parity) or legacy ASCII code according to bit 7.
 * JEP106 is a standard available from jedec.org
 */

/* Part number interpretations are from Cortex
 * core specs, the CoreSight components TRM
 * (ARM DDI 0314H), CoreSight System Design
 * Guide (ARM DGI 0012D) and ETM specs; also
 * from chip observation (e.g. TI SDTI).
 */

/* The legacy code only used the part number field to identify CoreSight peripherals.
 * This meant that the same part number from two different manufacturers looked the same.
 * It is desirable for all future additions to identify with both part number and JEP106.
 * "ANY_ID" is a wildcard (any JEP106) only to preserve legacy behavior for legacy entries.
 */

#define ANY_ID 0x1000

#define ARM_ID 0x4BB

static const struct {
	uint16_t designer_id;
	uint16_t part_num;
	const char *type;
	const char *full;
} dap_partnums[] = {
	{ ARM_ID, 0x000, "Cortex-M3 SCS",    "(System Control Space)", },
	{ ARM_ID, 0x001, "Cortex-M3 ITM",    "(Instrumentation Trace Module)", },
	{ ARM_ID, 0x002, "Cortex-M3 DWT",    "(Data Watchpoint and Trace)", },
	{ ARM_ID, 0x003, "Cortex-M3 FBP",    "(Flash Patch and Breakpoint)", },
	{ ARM_ID, 0x008, "Cortex-M0 SCS",    "(System Control Space)", },
	{ ARM_ID, 0x00a, "Cortex-M0 DWT",    "(Data Watchpoint and Trace)", },
	{ ARM_ID, 0x00b, "Cortex-M0 BPU",    "(Breakpoint Unit)", },
	{ ARM_ID, 0x00c, "Cortex-M4 SCS",    "(System Control Space)", },
	{ ARM_ID, 0x00d, "CoreSight ETM11",  "(Embedded Trace)", },
	{ ARM_ID, 0x490, "Cortex-A15 GIC",   "(Generic Interrupt Controller)", },
	{ ARM_ID, 0x4c7, "Cortex-M7 PPB",    "(Private Peripheral Bus ROM Table)", },
	{ ARM_ID, 0x906, "CoreSight CTI",    "(Cross Trigger)", },
	{ ARM_ID, 0x907, "CoreSight ETB",    "(Trace Buffer)", },
	{ ARM_ID, 0x908, "CoreSight CSTF",   "(Trace Funnel)", },
	{ ARM_ID, 0x910, "CoreSight ETM9",   "(Embedded Trace)", },
	{ ARM_ID, 0x912, "CoreSight TPIU",   "(Trace Port Interface Unit)", },
	{ ARM_ID, 0x913, "CoreSight ITM",    "(Instrumentation Trace Macrocell)", },
	{ ARM_ID, 0x914, "CoreSight SWO",    "(Single Wire Output)", },
	{ ARM_ID, 0x917, "CoreSight HTM",    "(AHB Trace Macrocell)", },
	{ ARM_ID, 0x920, "CoreSight ETM11",  "(Embedded Trace)", },
	{ ARM_ID, 0x921, "Cortex-A8 ETM",    "(Embedded Trace)", },
	{ ARM_ID, 0x922, "Cortex-A8 CTI",    "(Cross Trigger)", },
	{ ARM_ID, 0x923, "Cortex-M3 TPIU",   "(Trace Port Interface Unit)", },
	{ ARM_ID, 0x924, "Cortex-M3 ETM",    "(Embedded Trace)", },
	{ ARM_ID, 0x925, "Cortex-M4 ETM",    "(Embedded Trace)", },
	{ ARM_ID, 0x930, "Cortex-R4 ETM",    "(Embedded Trace)", },
	{ ARM_ID, 0x931, "Cortex-R5 ETM",    "(Embedded Trace)", },
	{ ARM_ID, 0x941, "CoreSight TPIU-Lite", "(Trace Port Interface Unit)", },
	{ ARM_ID, 0x950, "Cortex-A9 PTM", "(Program Trace Macrocell)", },
	{ ARM_ID, 0x955, "Cortex-A5 ETM", "(Embedded Trace)", },
	{ ARM_ID, 0x95a, "Cortex-A72 ETM",   "(Embedded Trace)", },
	{ ARM_ID, 0x95b, "Cortex-A17 PTM",   "(Program Trace Macrocell)", },
	{ ARM_ID, 0x95d, "Cortex-A53 ETM",   "(Embedded Trace)", },
	{ ARM_ID, 0x95e, "Cortex-A57 ETM",   "(Embedded Trace)", },
	{ ARM_ID, 0x95f, "Cortex-A15 PTM",   "(Program Trace Macrocell)", },
	{ ARM_ID, 0x961, "CoreSight TMC",    "(Trace Memory Controller)", },
	{ ARM_ID, 0x962, "CoreSight STM",    "(System Trace Macrocell)", },
	{ ARM_ID, 0x9a0, "CoreSight PMU",    "(Performance Monitoring Unit)", },
	{ ARM_ID, 0x9a1, "Cortex-M4 TPIU",   "(Trace Port Interface Unit)", },
	{ ARM_ID, 0x9a5, "Cortex-A5 PMU",    "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9a7, "Cortex-A7 PMU",    "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9a8, "Cortex-A53 CTI",    "(Cross Trigger)", },
	{ ARM_ID, 0x9ae, "Cortex-A17 PMU",    "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9af, "Cortex-A15 PMU",   "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9b7, "Cortex-R7 PMU",    "(Performance Monitoring Unit)", },
	{ ARM_ID, 0x9d3, "Cortex-A53 PMU",    "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9d7, "Cortex-A57 PMU",    "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9d8, "Cortex-A72 PMU",    "(Performance Monitor Unit)", },
	{ ARM_ID, 0xc05, "Cortex-A5 Debug",  "(Debug Unit)", },
	{ ARM_ID, 0xc07, "Cortex-A7 Debug",  "(Debug Unit)", },
	{ ARM_ID, 0xc08, "Cortex-A8 Debug",  "(Debug Unit)", },
	{ ARM_ID, 0xc09, "Cortex-A9 Debug",  "(Debug Unit)", },
	{ ARM_ID, 0xc0e, "Cortex-A17 Debug", "(Debug Unit)", },
	{ ARM_ID, 0xc0f, "Cortex-A15 Debug", "(Debug Unit)", },
	{ ARM_ID, 0xc14, "Cortex-R4 Debug",  "(Debug Unit)", },
	{ ARM_ID, 0xc15, "Cortex-R5 Debug",  "(Debug Unit)", },
	{ ARM_ID, 0xc17, "Cortex-R7 Debug",  "(Debug Unit)", },
	{ ARM_ID, 0xd03, "Cortex-A53 Debug",  "(Debug Unit)", },
	{ ARM_ID, 0xd07, "Cortex-A57 Debug",  "(Debug Unit)", },
	{ ARM_ID, 0xd08, "Cortex-A72 Debug",  "(Debug Unit)", },
	{ 0x0E5,  0x000, "SHARC+/Blackfin+", "", },
	/* legacy comment: 0x113: what? */
	{ ANY_ID,  0x120, "TI SDTI",         "(System Debug Trace Interface)", }, /* from OMAP3 memmap */
	{ ANY_ID,  0x343, "TI DAPCTL",       "", }, /* from OMAP3 memmap */
};

static int dap_rom_display(struct command_context *cmd_ctx,
				struct adiv5_ap *ap, uint32_t dbgbase, int depth)
{
	struct adiv5_dap *dap = ap->dap;
	int retval;
	uint32_t cid0, cid1, cid2, cid3, memtype, romentry;
	uint16_t entry_offset;
	char tabs[7] = "";

	if (depth > 16) {
		command_print(cmd_ctx, "\tTables too deep");
		return ERROR_FAIL;
	}

	if (depth)
		snprintf(tabs, sizeof(tabs), "[L%02d] ", depth);

	/* bit 16 of apid indicates a memory access port */
	if (dbgbase & 0x02)
		command_print(cmd_ctx, "\t%sValid ROM table present", tabs);
	else
		command_print(cmd_ctx, "\t%sROM table in legacy format", tabs);

	/* Now we read ROM table ID registers, ref. ARM IHI 0029B sec  */
	retval = mem_ap_read_u32(ap, (dbgbase&0xFFFFF000) | 0xFF0, &cid0);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, (dbgbase&0xFFFFF000) | 0xFF4, &cid1);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, (dbgbase&0xFFFFF000) | 0xFF8, &cid2);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, (dbgbase&0xFFFFF000) | 0xFFC, &cid3);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, (dbgbase&0xFFFFF000) | 0xFCC, &memtype);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	if (!is_dap_cid_ok(cid3, cid2, cid1, cid0))
		command_print(cmd_ctx, "\t%sCID3 0x%02x"
				", CID2 0x%02x"
				", CID1 0x%02x"
				", CID0 0x%02x",
				tabs,
				(unsigned)cid3, (unsigned)cid2,
				(unsigned)cid1, (unsigned)cid0);
	if (memtype & 0x01)
		command_print(cmd_ctx, "\t%sMEMTYPE system memory present on bus", tabs);
	else
		command_print(cmd_ctx, "\t%sMEMTYPE system memory not present: dedicated debug bus", tabs);

	/*
	 * Now we read ROM table entries from dbgbase&0xFFFFF000) | 0x000
	 * Until we reach the end of the ROM Table (0xF00)
	 *       or hit a blank entry (0x00000000)
	 */
	for (entry_offset = 0; entry_offset < 0xF00 ; entry_offset += 4) {
		retval = mem_ap_read_atomic_u32(ap, (dbgbase&0xFFFFF000) | entry_offset, &romentry);
		if (retval != ERROR_OK)
			return retval;
		command_print(cmd_ctx, "\t%sROMTABLE[0x%x] = 0x%" PRIx32 "",
				tabs, entry_offset, romentry);
		if (romentry == 0x0) {	/* Table end marker */
			break;
		}
		if ((romentry & 0x01) == 0x0) {	/* Empty entry */
			command_print(cmd_ctx, "\t\tComponent not present");
			continue;
		}

		/* An entry is present (romentry bit[1] == 0b1) */
		{
		uint32_t c_cid0, c_cid1, c_cid2, c_cid3;
		uint32_t c_pid0, c_pid1, c_pid2, c_pid3, c_pid4;
		uint32_t component_base;
		uint32_t part_num;
		const char *type, *full;

		component_base = (dbgbase & 0xFFFFF000) + (romentry & 0xFFFFF000);

		/* IDs are in last 4K section */
		retval = mem_ap_read_atomic_u32(dap, component_base + 0xFE0, &c_pid0);
		if (retval != ERROR_OK) {
			command_print(cmd_ctx, "\t%s\tCan't read component with base address 0x%" PRIx32
				      ", the corresponding core might be turned off", tabs, component_base);
			continue;
		}
		c_pid0 &= 0xff;
		retval = mem_ap_read_atomic_u32(dap, component_base + 0xFE4, &c_pid1);
		if (retval != ERROR_OK)
			return retval;
		c_pid1 &= 0xff;
		retval = mem_ap_read_atomic_u32(dap, component_base + 0xFE8, &c_pid2);
		if (retval != ERROR_OK)
			return retval;
		c_pid2 &= 0xff;
		retval = mem_ap_read_atomic_u32(dap, component_base + 0xFEC, &c_pid3);
		if (retval != ERROR_OK)
			return retval;
		c_pid3 &= 0xff;
		retval = mem_ap_read_atomic_u32(dap, component_base + 0xFD0, &c_pid4);
		if (retval != ERROR_OK)
			return retval;
		c_pid4 &= 0xff;
		/* Alamy: PIC_5-7, 0xFD4, 0xFD8, 0xFDC */

		retval = mem_ap_read_atomic_u32(dap, component_base + 0xFF0, &c_cid0);
		if (retval != ERROR_OK)
			return retval;
		c_cid0 &= 0xff;
		retval = mem_ap_read_atomic_u32(dap, component_base + 0xFF4, &c_cid1);
		if (retval != ERROR_OK)
			return retval;
		c_cid1 &= 0xff;
		retval = mem_ap_read_atomic_u32(dap, component_base + 0xFF8, &c_cid2);
		if (retval != ERROR_OK)
			return retval;
		c_cid2 &= 0xff;
		retval = mem_ap_read_atomic_u32(dap, component_base + 0xFFC, &c_cid3);
		if (retval != ERROR_OK)
			return retval;
		c_cid3 &= 0xff;

		/* Alamy: ERROR: pid4 >> 4 is a log value */
		command_print(cmd_ctx, "\t\tComponent base address 0x%" PRIx32 ", "
			      "start address 0x%" PRIx32, component_base,
			      /* component may take multiple 4K pages */
			      (uint32_t)(component_base - 0x1000*(c_pid4 >> 4)));
		command_print(cmd_ctx, "\t\tComponent class is 0x%" PRIx8 ", %s",
				(uint8_t)((c_cid1 >> 4) & 0xf),
				/* See ARM IHI 0029B Table 3-3 */
				class_description[(c_cid1 >> 4) & 0xf]);

		/* CoreSight component? */	/* Alamy: other component ? */
		if (((c_cid1 >> 4) & 0x0f) == 9) {
			uint32_t devtype;
			unsigned minor;
			const char *major = "Reserved", *subtype = "Reserved";

			retval = mem_ap_read_atomic_u32(dap,
					(component_base & 0xfffff000) | 0xfcc,
					&devtype);
			if (retval != ERROR_OK)
				return retval;
			minor = (devtype >> 4) & 0x0f;
			switch (devtype & 0x0f) {
			case 0:
				major = "Miscellaneous";
				switch (minor) {
				case 0:
					subtype = "other";
					break;
				case 4:
					subtype = "Validation component";
					break;
				}
				break;
			case 1:
				major = "Trace Sink";
				switch (minor) {
				case 0:
					subtype = "other";
					break;
				case 1:
					subtype = "Port";
					break;
				case 2:
					subtype = "Buffer";
					break;
				case 3:
					subtype = "Router";
					break;
				}
				break;
			case 2:
				major = "Trace Link";
				switch (minor) {
				case 0:
					subtype = "other";
					break;
				case 1:
					subtype = "Funnel, router";
					break;
				case 2:
					subtype = "Filter";
					break;
				case 3:
					subtype = "FIFO, buffer";
					break;
				}
				break;
			case 3:
				major = "Trace Source";
				switch (minor) {
				case 0:
					subtype = "other";
					break;
				case 1:
					subtype = "Processor";
					break;
				case 2:
					subtype = "DSP";
					break;
				case 3:
					subtype = "Engine/Coprocessor";
					break;
				case 4:
					subtype = "Bus";
					break;
				case 6:
					subtype = "Software";
					break;
				}
				break;
			case 4:
				major = "Debug Control";
				switch (minor) {
				case 0:
					subtype = "other";
					break;
				case 1:
					subtype = "Trigger Matrix";
					break;
				case 2:
					subtype = "Debug Auth";
					break;
				case 3:
					subtype = "Power Requestor";
					break;
				}
				break;
			case 5:
				major = "Debug Logic";
				switch (minor) {
				case 0:
					subtype = "other";
					break;
				case 1:
					subtype = "Processor";
					break;
				case 2:
					subtype = "DSP";
					break;
				case 3:
					subtype = "Engine/Coprocessor";
					break;
				case 4:
					subtype = "Bus";
					break;
				case 5:
					subtype = "Memory";
					break;
				}
				break;
			case 6:
				major = "Perfomance Monitor";
				switch (minor) {
				case 0:
					subtype = "other";
					break;
				case 1:
					subtype = "Processor";
					break;
				case 2:
					subtype = "DSP";
					break;
				case 3:
					subtype = "Engine/Coprocessor";
					break;
				case 4:
					subtype = "Bus";
					break;
				case 5:
					subtype = "Memory";
					break;
				}
				break;
			}
			command_print(cmd_ctx, "\t\tType is 0x%02" PRIx8 ", %s, %s",
					(uint8_t)(devtype & 0xff),
					major, subtype);
			/* REVISIT also show 0xfc8 DevId */
		}

		if (!is_dap_cid_ok(cid3, cid2, cid1, cid0))
			command_print(cmd_ctx,
					"\t\tCID3 0%02x"
					", CID2 0%02x"
					", CID1 0%02x"
					", CID0 0%02x",
					(int)c_cid3,
					(int)c_cid2,
					(int)c_cid1,
					(int)c_cid0);
		command_print(cmd_ctx,
			"\t\tPeripheral ID[4..0] = hex "
			"%02x %02x %02x %02x %02x",
			(int)c_pid4, (int)c_pid3, (int)c_pid2,
			(int)c_pid1, (int)c_pid0);

		/* Part number interpretations are from Cortex
		 * core specs, the CoreSight components TRM
		 * (ARM DDI 0314H), CoreSight System Design
		 * Guide (ARM DGI 0012D) and ETM specs; also
		 * from chip observation (e.g. TI SDTI).
		 */

		part_num = (c_pid0 & 0xff);
		part_num |= (c_pid1 & 0x0f) << 8;
		designer_id = (c_pid1 & 0xf0) >> 4;
		designer_id |= (c_pid2 & 0x0f) << 4;
		designer_id |= (c_pid4 & 0x0f) << 8;
		if ((designer_id & 0x80) == 0) {
			/* Legacy ASCII ID, clear invalid bits */
			designer_id &= 0x7f;
		}

		/* default values to be overwritten upon finding a match */
		type = NULL;
		full = "";

		/* search dap_partnums[] array for a match */
		unsigned entry;
		for (entry = 0; entry < ARRAY_SIZE(dap_partnums); entry++) {

			if ((dap_partnums[entry].designer_id != designer_id) && (dap_partnums[entry].designer_id != ANY_ID))
				continue;

			if (dap_partnums[entry].part_num != part_num)
				continue;

			type = dap_partnums[entry].type;
			full = dap_partnums[entry].full;
			break;
		}

		if (type) {
			command_print(cmd_ctx, "\t\tPart is %s %s",
					type, full);
		} else {
			command_print(cmd_ctx, "\t\tUnrecognized (Part 0x%" PRIx16 ", designer 0x%" PRIx16 ")",
					part_num, designer_id);
		}

		/* ROM Table? */
		if (((c_cid1 >> 4) & 0x0f) == 1) {
			/*
			 * Recursive is okay because the following references are prohibited:
			 * A. Entries in ROM Table A and B both point to ROM Table C.
			 * B. ROM Table being referenced from different MEM-AP. (similiar to case A)
			 * C. Circular ROM Table references.
			 */
			retval = dap_rom_display(cmd_ctx, ap, component_base, depth + 1);
			if (retval != ERROR_OK)
				return retval;
		}
	}
	command_print(cmd_ctx, "\t%s\tEnd of ROM table", tabs);
	return ERROR_OK;
}

static int dap_info_command(struct command_context *cmd_ctx,
		struct adiv5_ap *ap)
{
	int retval;
	uint32_t dbgbase, apid;
	int romtable_present = 0;
	uint8_t mem_ap;

	/* Now we read ROM table ID registers, ref. ARM IHI 0029B sec  */
	retval = dap_get_debugbase(ap, &dbgbase, &apid);
	if (retval != ERROR_OK)
		return retval;

	command_print(cmd_ctx, "AP ID register 0x%8.8" PRIx32, apid);
	if (apid == 0) {
		command_print(cmd_ctx, "No AP found at this ap 0x%x", ap->ap_num);
		return ERROR_FAIL;
	}

	switch (apid & (IDR_JEP106 | IDR_TYPE)) {
	case IDR_JEP106_ARM | AP_TYPE_JTAG_AP:
		command_print(cmd_ctx, "\tType is JTAG-AP");
		break;
	case IDR_JEP106_ARM | AP_TYPE_AHB_AP:
		command_print(cmd_ctx, "\tType is MEM-AP AHB");
		break;
	case IDR_JEP106_ARM | AP_TYPE_APB_AP:
		command_print(cmd_ctx, "\tType is MEM-AP APB");
		break;
	case IDR_JEP106_ARM | AP_TYPE_AXI_AP:
		command_print(cmd_ctx, "\tType is MEM-AP AXI");
		break;
	default:
		command_print(cmd_ctx, "\tUnknown AP type");
		break;
	}

	/* NOTE: a MEM-AP may have a single CoreSight component that's
	 * not a ROM table ... or have no such components at all.
	 */
	mem_ap = (apid & IDR_CLASS) == AP_CLASS_MEM_AP;
	if (mem_ap) {
		command_print(cmd_ctx, "MEM-AP BASE 0x%8.8" PRIx32, dbgbase);

		romtable_present = dbgbase != 0xFFFFFFFF;
		if (romtable_present)
			dap_rom_display(cmd_ctx, ap, dbgbase, 0);
		else
			command_print(cmd_ctx, "\tNo ROM table present");
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_dap_info_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;
	uint32_t apsel;

	switch (CMD_ARGC) {
	case 0:
		apsel = dap->apsel;
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], apsel);
		if (apsel >= 256)
			return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	return dap_info_command(CMD_CTX, &dap->ap[apsel]);
}

COMMAND_HANDLER(dap_baseaddr_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t apsel, baseaddr;
	int retval;

	switch (CMD_ARGC) {
	case 0:
		apsel = dap->apsel;
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], apsel);
		/* AP address is in bits 31:24 of DP_SELECT */
		if (apsel >= 256)
			return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	/* NOTE:  assumes we're talking to a MEM-AP, which
	 * has a base address.  There are other kinds of AP,
	 * though they're not common for now.  This should
	 * use the ID register to verify it's a MEM-AP.
	 */
	retval = dap_queue_ap_read(dap_ap(dap, apsel), MEM_AP_REG_BASE, &baseaddr);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	command_print(CMD_CTX, "0x%8.8" PRIx32, baseaddr);

	return retval;
}

COMMAND_HANDLER(dap_memaccess_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t memaccess_tck;

	switch (CMD_ARGC) {
	case 0:
		memaccess_tck = dap->ap[dap->apsel].memaccess_tck;
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], memaccess_tck);
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}
	dap->ap[dap->apsel].memaccess_tck = memaccess_tck;

	command_print(CMD_CTX, "memory bus access delay set to %" PRIi32 " tck",
			dap->ap[dap->apsel].memaccess_tck);

	return ERROR_OK;
}

COMMAND_HANDLER(dap_apsel_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t apsel, apid;
	int retval;

	switch (CMD_ARGC) {
	case 0:
		apsel = dap->apsel;
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], apsel);
		/* AP address is in bits 31:24 of DP_SELECT */
		if (apsel >= 256)
			return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	dap->apsel = apsel;

	retval = dap_queue_ap_read(dap_ap(dap, apsel), AP_REG_IDR, &apid);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	command_print(CMD_CTX, "ap %" PRIi32 " selected, identification register 0x%8.8" PRIx32,
			apsel, apid);

	return retval;
}

COMMAND_HANDLER(dap_apcsw_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t apcsw = dap->ap[dap->apsel].csw_default, sprot = 0;

	switch (CMD_ARGC) {
	case 0:
		command_print(CMD_CTX, "apsel %" PRIi32 " selected, csw 0x%8.8" PRIx32,
			(dap->apsel), apcsw);
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], sprot);
		/* AP address is in bits 31:24 of DP_SELECT */
		if (sprot > 1)
			return ERROR_COMMAND_SYNTAX_ERROR;
		if (sprot)
			apcsw |= CSW_SPROT;
		else
			apcsw &= ~CSW_SPROT;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}
	dap->ap[dap->apsel].csw_default = apcsw;

	return 0;
}



COMMAND_HANDLER(dap_apid_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t apsel, apid;
	int retval;

	switch (CMD_ARGC) {
	case 0:
		apsel = dap->apsel;
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], apsel);
		/* AP address is in bits 31:24 of DP_SELECT */
		if (apsel >= 256)
			return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	retval = dap_queue_ap_read(dap_ap(dap, apsel), AP_REG_IDR, &apid);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	command_print(CMD_CTX, "0x%8.8" PRIx32, apid);

	return retval;
}

COMMAND_HANDLER(dap_ti_be_32_quirks_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t enable = dap->ti_be_32_quirks;

	switch (CMD_ARGC) {
	case 0:
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], enable);
		if (enable > 1)
			return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}
	dap->ti_be_32_quirks = enable;
	command_print(CMD_CTX, "TI BE-32 quirks mode %s",
		enable ? "enabled" : "disabled");

	return 0;
}

static const struct command_registration dap_commands[] = {
	{
		.name = "info",
		.handler = handle_dap_info_command,
		.mode = COMMAND_EXEC,
		.help = "display ROM table for MEM-AP "
			"(default currently selected AP)",
		.usage = "[ap_num]",
	},
	{
		.name = "apsel",
		.handler = dap_apsel_command,
		.mode = COMMAND_EXEC,
		.help = "Set the currently selected AP (default 0) "
			"and display the result",
		.usage = "[ap_num]",
	},
	{
		.name = "apcsw",
		.handler = dap_apcsw_command,
		.mode = COMMAND_EXEC,
		.help = "Set csw access bit ",
		.usage = "[sprot]",
	},

	{
		.name = "apid",
		.handler = dap_apid_command,
		.mode = COMMAND_EXEC,
		.help = "return ID register from AP "
			"(default currently selected AP)",
		.usage = "[ap_num]",
	},
	{
		.name = "baseaddr",
		.handler = dap_baseaddr_command,
		.mode = COMMAND_EXEC,
		.help = "return debug base address from MEM-AP "
			"(default currently selected AP)",
		.usage = "[ap_num]",
	},
	{
		.name = "memaccess",
		.handler = dap_memaccess_command,
		.mode = COMMAND_EXEC,
		.help = "set/get number of extra tck for MEM-AP memory "
			"bus access [0-255]",
		.usage = "[cycles]",
	},
	{
		.name = "ti_be_32_quirks",
		.handler = dap_ti_be_32_quirks_command,
		.mode = COMMAND_CONFIG,
		.help = "set/get quirks mode for TI TMS450/TMS570 processors",
		.usage = "[enable]",
	},
	COMMAND_REGISTRATION_DONE
};

const struct command_registration dap_command_handlers[] = {
	{
		.name = "dap",
		.mode = COMMAND_EXEC,
		.help = "DAP command group",
		.usage = "",
		.chain = dap_commands,
	},
	COMMAND_REGISTRATION_DONE
};
