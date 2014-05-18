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

#ifndef __F0XX_H__
#define __F0XX_H__

#include <stdint.h>

enum f0xx_helper_execution_status {
	F0XX_HELPER_STATUS_SUCCESS = 0,
	F0XX_HELPER_ERROR_FAIL = -1,
};

enum {
	F0XX_ERROR_OK = 0,
	F0XX_DATA_BUFFER_SIZE = 1024,
	F0XX_DATA_NO_MARKER = 0x00000000,
	F0XX_DATA_MARKER_1  = 0x48455250,
	F0XX_DATA_MARKER_2  = 0x44455250,
};

struct f0xx_data_exchange_buffer {
	struct {
		volatile uint32_t odd;
		volatile uint32_t even;
	} marker;

	struct {
		uint8_t odd[F0XX_DATA_BUFFER_SIZE];
		uint8_t even[F0XX_DATA_BUFFER_SIZE];
	} buffer;
} __attribute__((__packed__));

#endif	/* __F0XX_H__ */
