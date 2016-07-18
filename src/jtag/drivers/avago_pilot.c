/*******************************************************************************
 *
 * Copyright (C) 2015-2016 Avago Technologies. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of version 2 of the GNU General Public License as published by the
 * Free Software Foundation.
 * This program is distributed in the hope that it will be useful. ALL EXPRESS
 * OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES, INCLUDING ANY IMPLIED
 * WARRANTY OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, OR
 * NON-INFRINGEMENT, ARE DISCLAIMED, EXCEPT TO THE EXTENT THAT SUCH DISCLAIMERS
 * ARE HELD TO BE LEGALLY INVALID. See the GNU General Public License for more
 * details, a copy of which can be found in the file COPYING included
 * with this package.
 *
 ********************************************************************************/
/*********************************************************************************
 * This code supports the JTAG Master in the Pilot4 ASIC, this code currenlty
 * supports NON-DISCRETE mode, the advantage of this mode is that the state mach-
 * -ne is maintained by the hardware itself, relieving the users of the hardwork
 * Ain't it cool!!
 *********************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/interface.h>
#include <jtag/swd.h>
#include <jtag/commands.h>

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#if 0
#define PRINTF(fmt...) printf(fmt)
#else
#define PRINTF(fmt...) dbg(x)

#define dbg(x)
#endif



#define JTAG_BASE	0x40429100
#define CONF_A		(avago_master + 0x0)
#define CONF_B		(avago_master + 0x4)
#define STS			(avago_master + 0x8)
#define COMMAND		(avago_master + 0xC)
#define TDO			(avago_master + 0x10)
#define TDI			(avago_master + 0x14)
#define COUNTER		(avago_master + 0x18)
#define DIS_CTRL	(avago_master + 0x1C)
#define AVAGO_MAX_SPEED 7800
int fd;
void *avago_master_void;
unsigned char *avago_master;

#define AVAGO_TAP_BUFFER_SIZE 2048
static uint8_t tdi_buffer[AVAGO_TAP_BUFFER_SIZE];
#define IOR(A) 		(*((volatile unsigned char *) A))
#define IOW(A, B)	(*(volatile unsigned char *)A) = B

/* DEBUG */
void dump_scan_command(struct scan_command *scan)
{
	int i;
	PRINTF("Scan command dump\n");
	struct scan_field *fld = scan->fields;
	if (scan->ir_scan) {
		PRINTF(" IR SCAN PRESENT\n");
	} else {
		PRINTF(" IR SCAN NOT PRESENT\n");
	}


	PRINTF(" Number of fields is %d\n", scan->num_fields);
	PRINTF(" End state is %x\n", scan->end_state);
	for (i = 0; i < scan->num_fields; i++) {
		PRINTF(" Num of bits for %d field is %d\n", i, fld->num_bits);
		fld++;
	}
}
/* DEBUG END */
void wait_for_idle(void)
{
	int count = 0;
	/* Make sure we are not busy */
	while (((IOR(STS)) & 0xE7) != 0x0) {
		PRINTF("Busy %x\n", IOR(STS));
		usleep(50);
		count++;
		if (count > 100000) {
			printf("wait_for_idle bailing out\n");
			exit(255);
		}

	}
}
void wait_for_mode(void)
{
	volatile int count = 0;
	unsigned char b;
	b = IOR(CONF_B);
	switch (b) {
		case 0xA0:
			count = 4;
		break;
		case 0xC0:
			count = 8;
		break;
		case 0xE0:
			count = 16;
		break;
		case 0x10:
			count = 32;
		break;
		case 0x30:
			count = 64;
		break;
		case 0x50:
			count = 128;
		break;
		case 0x70:
			count = 256;
		break;
		case 0x90:
			count = 512;
		break;
		case 0xb0:
			count = 1024;
		break;
		case 0xd0:
		case 0x00:
		case 0xF0:
			count = 2048;
		break;
		case 0x80:
			count = 2;
		break;
		case 0x60:
			count = 1;
		break;
		default:
		break;
	}
	while (count) {
		b = IOR(STS);
		count--;
	}
}
void wait_for_tdo(void)
{
	int count = 0;
	while ((IOR(STS) & 0x40) != 0x00) {
		usleep(50);
		count++;
		if (count > 100000) {
			printf("wait_for_tdo bailing out\n");
			exit(255);
		}
	}
}
void wait_for_tdi(void)
{
	int count = 0;
	while ((IOR(STS) & 0x80) == 0x00) {
		usleep(50);
		count++;
		if (count > 100000) {
			printf("wait_for_tdi bailing out\n");
			exit(255);
		}
	}
}

static inline void avago_debug_buffer(uint8_t *buffer, int length)
{
	/* TBD: This is a debug function */
}
static int avago_field_execute_in(uint8_t *buffer, unsigned int scan_size, unsigned char cmd, unsigned char ir_scan)
{
	unsigned char *in = buffer;
	unsigned int num_bytes = (scan_size+7)/8;
	unsigned int i = 0;
	PRINTF("Entered %s num_bytes is %d bits is %d\n", __func__, num_bytes, scan_size);

	wait_for_idle();

	/* printf("COMMAND register is ZERO%x\n", IOR(COMMAND)); */
	PRINTF("Writing to counter register\n");
	IOW(COUNTER, (scan_size & 0xFF));
	IOW(COUNTER, (scan_size & 0xFF00) >> 8);
	IOW(COUNTER, (scan_size & 0xFF0000) >> 16);
	IOW(COUNTER, (scan_size & 0xFF000000) >> 24);
	IOW(COMMAND, cmd);

	wait_for_mode();

	while (i < num_bytes) {
		wait_for_tdi();
		tdi_buffer[i] = IOR(TDI);
		i++;
	}
	memcpy(in, tdi_buffer, i);
	return 0;
}
static int avago_field_execute_out(uint8_t *buffer, unsigned int scan_size, unsigned char cmd, unsigned char ir_scan)
{
	unsigned char *out = buffer;
	unsigned int num_bytes = (scan_size+7)/8;
	unsigned int i = 0;
	PRINTF("Entered %s num_bytes is %d bits is %d\n", __func__, num_bytes, scan_size);

	wait_for_idle();

	/* printf("COMMAND register is ZERO%x\n", IOR(COMMAND)); */
	PRINTF("Writing to counter register\n");
	IOW(COUNTER, (scan_size & 0xFF));
	IOW(COUNTER, (scan_size & 0xFF00) >> 8);
	IOW(COUNTER, (scan_size & 0xFF0000) >> 16);
	IOW(COUNTER, (scan_size & 0xFF000000) >> 24);
	IOW(COMMAND, cmd);

	wait_for_mode();

	while (i < num_bytes) {
		wait_for_tdo();
		IOW(TDO, out[i]);
		i++;
	}
	return 0;
}
static int avago_field_execute_in_out(uint8_t *buffer, unsigned int scan_size, unsigned char cmd, unsigned char ir_scan)
{
	unsigned char *out = buffer;
	unsigned char *in = buffer;
	unsigned int num_bytes = (scan_size+7)/8;
	unsigned int i = 0;
	unsigned int j = 0;
	PRINTF("Entered %s num_bytes is %d bits is %u\n", __function__, num_bytes, scan_size);

	wait_for_idle();

	PRINTF("Writing to counter register\n");
	IOW(COUNTER, (scan_size & 0xFF));
	IOW(COUNTER, (scan_size & 0xFF00) >> 8);
	IOW(COUNTER, (scan_size & 0xFF0000) >> 16);
	IOW(COUNTER, (scan_size & 0xFF000000) >> 24);
	IOW(COMMAND, cmd);

	wait_for_mode();

	while (num_bytes) {
		wait_for_tdo();
		IOW(TDO, out[i]);
		i++;
		if ((i >= 1) || (i == num_bytes)) {
			break;
		}
	}
	while (i < num_bytes) {
		wait_for_tdi();
		tdi_buffer[j] = IOR(TDI);
		j++;
		PRINTF(" Status register is %x\n", IOR(STS));
		wait_for_tdo();
		IOW(TDO, out[i]);
		i++;
	}
	while (j < i) {
		wait_for_tdi();
		tdi_buffer[j] = IOR(TDI);
		j++;
	}
	memcpy(in, tdi_buffer, j);
	PRINTF(" Status register is  3 %x\n", IOR(STS));
	return 0;
}
static int avago_tap_execute(void)
{
	PRINTF(" Entered %s\n", __func__);
	return ERROR_OK;
}
#if 1
/* Ideally all the state transitions needed should be taken care by the HW state machine
 * handler in the hardware. However just to cater to any special transtions it will come
 * Handy !!*/
static void avago_state_move(void)
{
	int i;
	uint8_t tms = 0;
	uint8_t tms_scan = tap_get_tms_path(tap_get_state(), tap_get_end_state());
	uint8_t tms_scan_bits = tap_get_tms_path_len(tap_get_state(), tap_get_end_state());
	PRINTF(" Entered %s\n", __func__);
	PRINTF(" state move scan bits is %d data is %x\n", tms_scan_bits, tms_scan);
	IOW(CONF_A, 2);
#if 1
	for (i = 0; i < tms_scan_bits; i++) {
		tms = ((tms_scan >> i) & 1) << 2;
		IOW(DIS_CTRL, tms);
		IOR(DIS_CTRL);
		/* avago_tap_append_step(tms, 0); */
	}
	IOW(CONF_A, 0);

	tap_set_state(tap_get_end_state());
#endif
}
#endif
static void avago_end_state(tap_state_t state)
{
	PRINTF(" Entered %s\n", __func__);
	if (tap_is_state_stable(state))
		tap_set_end_state(state);
	else {
		LOG_ERROR("BUG: %i is not a valid end state", state);
		exit(-1);
	}
}
static int avago_quit(void)
{
	PRINTF(" Entered %s\n", __func__);
	munmap(avago_master_void, 4096);
	return ERROR_OK;
}
static int avago_init(void)
{
	PRINTF(" Entered %s\n", __func__);
	/* jtag_config_khz(AVAGO_MAX_SPEED); */
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		PRINTF("Mapping failed\n");
	}
	avago_master_void = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED, fd, (JTAG_BASE&~0xfff));
	if (avago_master_void == MAP_FAILED) {
			PRINTF("Mapping failed Exitting\n");
			exit(255);
	}

	avago_master = (unsigned char *) avago_master_void;
	avago_master += 0x100; /*Adjusting the pointer to point to the start of the JTAG master base.*/

	close(fd);
	return ERROR_OK;
}
static int avago_speed_div(int speed, int *khz)
{
	PRINTF(" Entered %s\n", __func__);
	*khz = speed;
	return ERROR_OK;
}
static int avago_khz(int khz, int *jtag_speed)
{
	PRINTF(" Entered %s\n", __func__);
	unsigned char iob_val;
	switch (khz) {
		case 3900:
			iob_val = 0xA0;
		break;
		case 1950:
			iob_val = 0xC0;
		break;
		case 976:
			iob_val = 0xE0;
		break;
		case 488:
			iob_val = 0x10;
		break;
		case 244:
			iob_val = 0x30;
		break;
		case 122:
			iob_val = 0x50;
		break;
		case 61:
			iob_val = 0x70;
		break;
		case 30:
			iob_val = 0x90;
		break;
		case 15:
			iob_val = 0xB0;
		break;
		case 7:
			iob_val = 0xD0;
		break;
		case 7800:
			iob_val = 0x80;
		break;
		case 15620:
			iob_val = 0x60;
		break;
		case 31200:
			iob_val = 0x40;
		break;
		default:
			/* Unsupported frequency */
			printf("Trying to set unsupported frequency\n");
			return ERROR_FAIL;
		break;
	}
	*jtag_speed = khz;
	IOW(CONF_B, iob_val);
	return ERROR_OK;
}

static int avago_speed(int speed)
{
	PRINTF(" Entered %s\n", __func__);
	return ERROR_OK;
}
static void avago_execute_scan(struct jtag_command *cmd)
{
	int scan_size;
	enum scan_type type;
	/* int i,j; */
	uint8_t *buffer;
	PRINTF(" Entered %s\n", __func__);

	dump_scan_command(cmd->cmd.scan);

	PRINTF("scan end in %s", tap_state_name(cmd->cmd.scan->end_state));

	avago_end_state(cmd->cmd.scan->end_state);
	type = jtag_scan_type(cmd->cmd.scan);
	scan_size = jtag_scan_size(cmd->cmd.scan);
	jtag_build_buffer(cmd->cmd.scan, &buffer);
	if (cmd->cmd.scan->ir_scan) {
		if (type == SCAN_IN) {
			PRINTF("SCAN TYPE %x\n", IOR(COMMAND));
			printf("In only !!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
			avago_field_execute_in(buffer, scan_size, 0x1A, 1);
		} else if (type == SCAN_OUT) {
			PRINTF("SCAN TYPE %x\n", IOR(COMMAND));
			avago_field_execute_out(buffer, scan_size, 0x1C, 1);
		} else if (type == (SCAN_IN | SCAN_OUT)) {
			PRINTF("SCAN TYPE %x\n", IOR(COMMAND));
			avago_field_execute_in_out(buffer, scan_size, 0x18, 1);
		} else {
			PRINTF("Error Scan type\n");
			exit(255);
		}
	} else {
		if (type == SCAN_IN) {
			PRINTF("SCAN TYPE %x\n", IOR(COMMAND));
			printf("In only !!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
			avago_field_execute_in(buffer, scan_size, 0x1B, 0);
		} else if (type == SCAN_OUT) {
			PRINTF("SCAN TYPE %x\n", IOR(COMMAND));
			avago_field_execute_out(buffer, scan_size, 0x1D, 0);
		} else if (type == (SCAN_IN | SCAN_OUT)) {
			PRINTF("SCAN TYPE %x\n", IOR(COMMAND));
			avago_field_execute_in_out(buffer, scan_size, 0x19, 0);
		} else {
			PRINTF("Error Scan type\n");
			exit(255);
		}
	}
	jtag_read_buffer(buffer, cmd->cmd.scan);
#if 0
		for (i = 0; i < cmd->cmd.scan->num_fields; i++) {
			if (cmd->cmd.scan->fields[i].in_value != NULL) {
				for (j = 0; j < ((cmd->cmd.scan->fields[i].num_bits+7)/8); j++) {
					PRINTF("Received %x\n", cmd->cmd.scan->fields[i].in_value[j]);
				}
			} else {
				PRINTF("NO, %d\n", i);
			}
		}
			for (i = 0; i < ((scan_size+7)/8); i++) {
				PRINTF("Received %x\n", buffer[i]);
			}
#endif
}

static void avago_execute_runtest(struct jtag_command *cmd)
{
	PRINTF(" Entered %s\n", __func__);
	PRINTF("runtest %i cycles, end in %i",
			cmd->cmd.runtest->num_cycles,
			cmd->cmd.runtest->end_state);
}
static void avago_execute_statemove(struct jtag_command *cmd)
{
	PRINTF(" Entered %s\n", __func__);
	PRINTF("statemove end in %i", cmd->cmd.statemove->end_state);
	DEBUG_JTAG_IO("statemove end in %i", cmd->cmd.statemove->end_state);
#if 1
	avago_end_state(cmd->cmd.statemove->end_state);
	avago_state_move();
#endif
}
static void avago_execute_pathmove(struct jtag_command *cmd)
{
	PRINTF(" Entered %s\n", __func__);
	PRINTF("pathmove: %i states, end in %i",
		cmd->cmd.pathmove->num_states,
		cmd->cmd.pathmove->path[cmd->cmd.pathmove->num_states - 1]);
	DEBUG_JTAG_IO("pathmove: %i states, end in %i",
		cmd->cmd.pathmove->num_states,
		cmd->cmd.pathmove->path[cmd->cmd.pathmove->num_states - 1]);
}
static void avago_execute_reset(struct jtag_command *cmd)
{
	PRINTF("reset trst: %i srst %i",
			cmd->cmd.reset->trst, cmd->cmd.reset->srst);
	DEBUG_JTAG_IO("reset trst: %i srst %i",
			cmd->cmd.reset->trst, cmd->cmd.reset->srst);
#if 0
	avago_tap_execute();
	avago_reset(cmd->cmd.reset->trst, cmd->cmd.reset->srst);
	avago_tap_execute();
#endif
}
static void avago_execute_sleep(struct jtag_command *cmd)
{
	PRINTF(" Entered %s\n", __func__);
	PRINTF("sleep %" PRIi32 "", cmd->cmd.sleep->us);
	DEBUG_JTAG_IO("sleep %" PRIi32 "", cmd->cmd.sleep->us);
	avago_tap_execute();
	jtag_sleep(cmd->cmd.sleep->us);
}
static void avago_execute_command(struct jtag_command *cmd)
{
	PRINTF(" Entered %s\n", __func__);
	switch (cmd->type) {
		case JTAG_RUNTEST:
			PRINTF(" JTAG_RUNTEST\n");
			avago_execute_runtest(cmd);
			break;
		case JTAG_TLR_RESET:
			PRINTF(" JTAG_TLR_RESET\n");
			avago_execute_statemove(cmd);
			/* May have to issue reset - TBD */
			break;
		case JTAG_PATHMOVE:
			PRINTF(" JTAG_PATHMOVE\n");
			avago_execute_pathmove(cmd);
			break;
		case JTAG_SCAN:
			PRINTF(" JTAG_SCAN\n");
			avago_execute_scan(cmd);
			break;
		case JTAG_RESET:
			PRINTF(" JTAG_RESET\n");
			avago_execute_reset(cmd);
			break;
		case JTAG_SLEEP:
			PRINTF(" JTAG_SLEEP\n");
			avago_execute_sleep(cmd);
			break;
		default:
			LOG_ERROR("BUG: unknown JTAG command type encountered");
			exit(-1);
	}
}

static int avago_execute_queue(void)
{
	struct jtag_command *cmd = jtag_command_queue;
	PRINTF(" Entered %s\n", __func__);

	while (cmd != NULL) {
		avago_execute_command(cmd);
		cmd = cmd->next;
	}

	return avago_tap_execute();
}



static const char * const avago_transports[] = { "jtag", NULL };

struct jtag_interface avago_interface = {
	.name = "avago",
	.transports = avago_transports,

	.execute_queue = avago_execute_queue,
	.speed = avago_speed,
	.speed_div = avago_speed_div,
	.khz = avago_khz,
	.init = avago_init,
	.quit = avago_quit,
};
