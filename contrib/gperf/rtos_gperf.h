
#ifndef _CONTRIB_GPERF_RTOS_GPERF
#define _CONTRIB_GPERF_RTOS_GPERF

#include <stdlib.h>
#include <string.h>

/* Generated by gperf_convert.py. Do not edit directly. */


typedef enum {
	rtos_freertos = 0,
	rtos_invalid = -1
} rtos_option_t;



struct rtos_lookup_entry { char * name; rtos_option_t option; };


struct rtos_lookup_entry * rtos_in_word_set (register const char *str, register unsigned int len);

#endif // _CONTRIB_GPERF_RTOS_GPERF
