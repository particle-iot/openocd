/***************************************************************************
 *									   *
 * Copyright (c) 2014, Andrey Smirnov <andrew.smirnov@gmail.com>	   *
 * All rights reserved.							   *
 *									   *
 * Redistribution and use in source and binary forms, with or without	   *
 * modification, are permitted provided that the following conditions are  *
 * met:									   *
 *									   *
 * 1. Redistributions of source code must retain the above copyright	   *
 *    notice, this list of conditions and the following disclaimer.	   *
 *									   *
 * 2. Redistributions in binary form must reproduce the above copyright	   *
 *    notice, this list of conditions and the following disclaimer in the  *
 *    documentation and/or other materials provided with the		   *
 *    distribution.							   *
 *									   *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS	   *
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT	   *
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR   *
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT	   *
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,   *
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT	   *
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,   *
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY   *
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT	   *
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE   *
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.	   *
 *									   *
 ***************************************************************************/

#include <stdio.h>
#include <stdarg.h>

#include <F021.h>
#include <flash/nor/f0xx.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))


enum {
	F0XX_FSM_TIMEOUT = 2000000,
	F0XX_PING_PONG_TIMEOUT = 500000,
};

#define F0XX_ERROR_OK	 (0)
#define F0XX_ERROR_FAIL (-__LINE__)


#ifdef DEBUG

#define F0XX_OUTBUFFER_SIZE (2048 + 1024)

static void f0xx_log(const char *fmt, ...)
{
	extern char outbuffer[];

	int n;
	va_list args;
	static char *outbufferpos = outbuffer;

	va_start(args, fmt);
	n = vsprintf(outbufferpos, fmt, args);
	va_end(args);

	outbufferpos += n;
}

#else

#define F0XX_OUTBUFFER_SIZE (0)

static void f0xx_log(const char *fmt, ...) {}

#endif

char outbuffer[F0XX_OUTBUFFER_SIZE];

uint32_t f0xx_do_perf_counting(uint32_t counter)
{
	/*
	 * WARNING! WARNING! WARNING!
	 *
	 * Host must know the exact timing of this function, so if for
	 * whatever reason you decide to change the perf loop body,
	 * you have to update the following constant
	 *
	 * WARNING! WARNING! WARNING!
	 */
	enum {
		F0XX_DO_PERF_LOOK_CPU_CYCLES_IN_ONE_ITERATION = 4,
	};
	__asm__ __volatile__("mov	r1, %[initial_value]\n\t"
			     "perf_loop:\n\t"
			     "subs	r1, r1, #1\n\t"
			     "bne	perf_loop\n\t"
			     : : [initial_value]"r"(counter) : "r1");

	return F0XX_DO_PERF_LOOK_CPU_CYCLES_IN_ONE_ITERATION;
}

static Fapi_FlashBankType f0xx_bank_nr_to_bank_type(uint32_t bank_nr)
{
	Fapi_FlashBankType bank[] = {
		[0] = Fapi_FlashBank0,
		[1] = Fapi_FlashBank1,
		[2] = Fapi_FlashBank2,
		[3] = Fapi_FlashBank3,
		[4] = Fapi_FlashBank4,
		[5] = Fapi_FlashBank5,
		[6] = Fapi_FlashBank6,
		[7] = Fapi_FlashBank7,
	};

	if (bank_nr < ARRAY_SIZE(bank))
		return bank[bank_nr];
	else
		return F0XX_ERROR_OK;
}

static int f0xx_wait_for_fsm(int timeout)
{
	uint32_t status;
	do {
		status = FAPI_CHECK_FSM_READY_BUSY;
	} while (status == Fapi_Status_FsmBusy && --timeout > 0);

	return (timeout > 0) ? F0XX_ERROR_OK : F0XX_ERROR_FAIL;
}

#define F0XX_WAIT_FOR_FSM()						\
	do {								\
		uint32_t __status =					\
			f0xx_wait_for_fsm(F0XX_FSM_TIMEOUT);		\
		if (__status != F0XX_ERROR_OK) {			\
			f0xx_log("Timeout while waiting for FSM\n");	\
			return F0XX_ERROR_FAIL;				\
		}							\
		if (FAPI_GET_FSM_STATUS != Fapi_Status_Success) {	\
			f0xx_log("FSMSTAT indicated error 0x%08x\n",	\
				 FAPI_GET_FSM_STATUS);			\
			return F0XX_ERROR_FAIL;				\
		}							\
	} while (0)


#define F0XX_CHECK_STATUS(name, status)					\
	do {								\
		if (status != Fapi_Status_Success) {			\
			f0xx_log(#name " failed with %d\n", status);	\
			return F0XX_ERROR_FAIL;				\
		}							\
	} while (0)


uint32_t f0xx_erase_sectors(uint32_t frequency_mhz,
			    uint32_t bank_nr,
			    int32_t first, int32_t last,
			    uint32_t start_address,
			    uint32_t sector_size)
{
	uint32_t status;
	const Fapi_FlashBankType bank = f0xx_bank_nr_to_bank_type(bank_nr);

	f0xx_log("==== %s ====\n", __func__);
	f0xx_log("frequency_mhz: 0x%08x\n", frequency_mhz);
	f0xx_log("bank_nr:	 0x%08x\n", bank_nr);
	f0xx_log("first:	 0x%08x\n", first);
	f0xx_log("last:		 0x%08x\n", last);
	f0xx_log("start_address: 0x%08x\n", start_address);
	f0xx_log("sector_size:	 0x%08x\n", sector_size);

	if (bank < 0) {
		f0xx_log("Unknown bank number %d\n", bank_nr);
		return F0XX_ERROR_FAIL;
	}

	Fapi_issueAsyncCommand(Fapi_ClearStatus);
	F0XX_WAIT_FOR_FSM();

	status = Fapi_initializeFlashBanks(frequency_mhz);
	F0XX_CHECK_STATUS(Fapi_initializeFlashBanks, status);

	status = Fapi_setActiveFlashBank(bank);
	F0XX_CHECK_STATUS(Fapi_setActiveFlashBank, status);

	if (first == last && first == -1) {
		/* TODO: Erase bank */
	} else {
		uint16_t bit  = 1 << first;
		uint16_t mask = 0;

		for (int s = first; s <= last; s++) {
			mask |= bit;
			bit <<= 1;
		}

		status = Fapi_enableMainBankSectors(mask);
		F0XX_CHECK_STATUS(Fapi_enableMainBankSectors, status);

		F0XX_WAIT_FOR_FSM();

		uint32_t erase_address = start_address;

		for (int s = first; s <= last; s++) {
			status = Fapi_issueAsyncCommandWithAddress(Fapi_EraseSector,
								   (uint32_t *)erase_address);
			F0XX_CHECK_STATUS(Fapi_issueAsyncCommandWithAddress, status);

			F0XX_WAIT_FOR_FSM();

			erase_address += sector_size;
		}
	}

	return F0XX_ERROR_OK;
}

static int f0xx_write_block_to_flash(uint32_t frequency_mhz,
				     uint32_t bank_nr,
				     uint32_t address,
				     uint32_t block_size,
				     const uint8_t *block)
{
	uint32_t status;
	const uint8_t *data_pointer;
	uint8_t	 chunk_size;
	uint32_t remainder;


	if (!block_size) {
		f0xx_log("Zero-length write requested\n");
		return F0XX_ERROR_FAIL;
	}

	data_pointer = block;
	remainder    = block_size;

	const Fapi_FlashBankType bank = f0xx_bank_nr_to_bank_type(bank_nr);

	if (bank < 0) {
		f0xx_log("Unknown bank number %d\n", bank_nr);
		return F0XX_ERROR_FAIL;
	}

	status = Fapi_initializeFlashBanks(frequency_mhz);
	F0XX_CHECK_STATUS(Fapi_initializeFlashBanks, status);

	status = Fapi_setActiveFlashBank(bank);
	F0XX_CHECK_STATUS(Fapi_setActiveFlashBank, status);

	status = Fapi_enableMainBankSectors(0xFFFF);
	F0XX_CHECK_STATUS(Fapi_enableMainBankSectors, status);

	F0XX_WAIT_FOR_FSM();

	do {
		chunk_size = MIN(16, remainder);
		status = Fapi_issueProgrammingCommand((uint32_t *)address,
						      (uint8_t *) data_pointer,
						      chunk_size,
						      0,
						      0,
						      Fapi_AutoEccGeneration);

		F0XX_CHECK_STATUS(Fapi_issueProgrammingCommand, status);

		F0XX_WAIT_FOR_FSM();

		remainder -= chunk_size;
		data_pointer += chunk_size;
		address += chunk_size;
	} while (remainder);

	return F0XX_ERROR_OK;
}

int32_t f0xx_write_block(uint32_t frequency_mhz,
			 uint32_t bank_nr,
			 uint32_t address, uint32_t total_size,
			 struct f0xx_data_exchange_buffer *dbuffer)
{
	int ret, timeout;
	uint32_t size;
	uint32_t remainder;

	f0xx_log("==== %s ====\n\n", __func__);
	f0xx_log("frequency_mhz: 0x%08x\n", frequency_mhz);
	f0xx_log("bank_nr:	 0x%08x\n", bank_nr);
	f0xx_log("address:	 0x%08x\n", address);
	f0xx_log("total_size:	 0x%08x\n", total_size);
	f0xx_log("dbuffer:	 0x%08x\n", dbuffer);

	remainder = total_size;

	if (!remainder) {
		f0xx_log("Zero length write requested\n");
		return F0XX_ERROR_FAIL;
	}

	for (;;) {
		/* First act: At this point we exepect that odd buffer
		   is being written by the host with new data and odd
		   buffer has just been written out to flash */
		size	= MIN(remainder, sizeof(dbuffer->buffer.odd));
		timeout	= F0XX_PING_PONG_TIMEOUT;
		/* Tell the host that it can use even buffer for next
		 * data batch transfer */
		dbuffer->marker.even = F0XX_DATA_NO_MARKER;
		/* Wait for the host to tell us that odd buffer is
		 * ready to be consumed */
		while (dbuffer->marker.odd != F0XX_DATA_MARKER_1 && --timeout)
			;

		if (!timeout) {
			f0xx_log("Timed out waiting for odd chunk\n");
			return F0XX_ERROR_FAIL;
		}

		ret = f0xx_write_block_to_flash(frequency_mhz, bank_nr,
						address, size, dbuffer->buffer.odd);
		if (ret != F0XX_ERROR_OK) {
			f0xx_log("Failed to write data to flash\n");
			return ret;
		}

		remainder -= size;
		address	  += size;

		/* Interlude: Check if our previous write took care of
		   the last chunk of data and if so finish the
		   algorithm early */
		if (!remainder)
			break;

		/* Second act: We just finished writing odd buffer to
		 * flash while in parallel to use host was transfering
		 * another chunk of data to even buffer */
		size	= MIN(remainder, sizeof(dbuffer->buffer.even));
		timeout	= F0XX_PING_PONG_TIMEOUT;
		/* Mark odd buffer as availibe to host */
		dbuffer->marker.odd = F0XX_DATA_NO_MARKER;
		/* Wait for the host to tell us that even buffer is
		 * ready to be consumed */
		while (dbuffer->marker.even != F0XX_DATA_MARKER_2 && --timeout)
			;
		if (!timeout) {
			f0xx_log("Timed out waiting for even chunk\n");
			return F0XX_ERROR_FAIL;
		}

		ret = f0xx_write_block_to_flash(frequency_mhz, bank_nr,
						address, size, dbuffer->buffer.even);
		if (ret != F0XX_ERROR_OK) {
			f0xx_log("Failed to write data to flash\n");
			return ret;
		}

		remainder -= size;
		address	  += size;

		if (!remainder)
			break;
	}

	f0xx_log("Finished");

	return F0XX_ERROR_OK;
}
