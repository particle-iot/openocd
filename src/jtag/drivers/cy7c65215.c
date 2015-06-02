/*******************************************************************************
 *   Driver for Cypress CY7C65215 Dual Channel USB-Serial Bridge Controller    *
 *   Protocol is compatible with OpenJTAG                                      *
 *                                                                             *
 *   Copyright (C) 2015 Vianney le Clément de Saint-Marcq, Essensium NV        *
 *                      <vianney.leclement@essensium.com>                      *
 *                                                                             *
 *   Based on openjtag.c                                                       *
 *   Copyright (C) 2010 by Ivan Meleca <mileca@gmail.com>                      *
 *   Copyright (C) 2013 by Ryan Corbin, GlueLogix Inc. <corbin.ryan@gmail.com> *
 *                                                                             *
 *   Based on usb_blaster.c                                                    *
 *   Copyright (C) 2009 Catalin Patulea                                        *
 *   Copyright (C) 2006 Kolja Waschk                                           *
 *                                                                             *
 *   And jlink.c                                                               *
 *   Copyright (C) 2008 by Spencer Oliver                                      *
 *   spen@spen-soft.co.uk                                                      *
 *                                                                             *
 *   This program is free software; you can redistribute it and/or modify      *
 *   it under the terms of the GNU General Public License as published by      *
 *   the Free Software Foundation; either version 2 of the License, or         *
 *   (at your option) any later version.                                       *
 *                                                                             *
 *   This program is distributed in the hope that it will be useful,           *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
 *   GNU General Public License for more details.                              *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/interface.h>
#include <jtag/commands.h>
#include "libusb_common.h"

/*
 * CY7C65215-OpenOCD state conversion
 */
typedef enum cy7c65215_tap_state {
	CY7C65215_TAP_INVALID    = -1,
	CY7C65215_TAP_RESET      = 0,
	CY7C65215_TAP_IDLE       = 1,
	CY7C65215_TAP_SELECT_DR  = 2,
	CY7C65215_TAP_CAPTURE_DR = 3,
	CY7C65215_TAP_SHIFT_DR   = 4,
	CY7C65215_TAP_EXIT1_DR   = 5,
	CY7C65215_TAP_PAUSE_DR   = 6,
	CY7C65215_TAP_EXIT2_DR   = 7,
	CY7C65215_TAP_UPDATE_DR  = 8,
	CY7C65215_TAP_SELECT_IR  = 9,
	CY7C65215_TAP_CAPURE_IR  = 10,
	CY7C65215_TAP_SHIFT_IR   = 11,
	CY7C65215_TAP_EXIT1_IR   = 12,
	CY7C65215_TAP_PAUSE_IR   = 13,
	CY7C65215_TAP_EXIT2_IR   = 14,
	CY7C65215_TAP_UPDATE_IR  = 15,
} cy7c65215_tap_state_t;

/* CY7C65215 control commands */
#define CY7C65215_JTAG_REQUEST  0x40  /* bmRequestType: vendor host-to-device */
#define CY7C65215_JTAG_ENABLE   0xD0  /* bRequest: enable JTAG */
#define CY7C65215_JTAG_DISABLE  0xD1  /* bRequest: disable JTAG */
#define CY7C65215_JTAG_READ     0xD2  /* bRequest: read buffer */
#define CY7C65215_JTAG_WRITE    0xD3  /* bRequest: write buffer */

#define CY7C65215_USB_TIMEOUT   100

/* CY7C65215 vid/pid */
static const uint16_t cy7c65215_vids[] = {0x04b4, 0};
static const uint16_t cy7c65215_pids[] = {0x0007, 0};

#define CY7C65215_JTAG_CLASS     0xff
#define CY7C65215_JTAG_SUBCLASS  0x04

static jtag_libusb_device_handle *usbh;
static unsigned int ep_in, ep_out;

#define CY7C65215_BUFFER_SIZE            504
#define CY7C65215_MAX_PENDING_RESULTS    256

struct cy7c65215_scan_result {
	uint32_t bits;          /* Length in bits*/
	struct scan_command *command;   /* Corresponding scan command */
	uint8_t *buffer;
};

/* USB RX/TX buffers */
static int usb_tx_buf_offs;
static uint8_t usb_tx_buf[CY7C65215_BUFFER_SIZE];
static uint32_t usb_rx_buf_len;
static uint8_t usb_rx_buf[CY7C65215_BUFFER_SIZE];

/* Pending readings */
static struct cy7c65215_scan_result cy7c65215_scan_result_buffer[CY7C65215_MAX_PENDING_RESULTS];
static int cy7c65215_scan_result_count;

#ifdef _DEBUG_USB_COMMS_

#define DEBUG_TYPE_READ     0
#define DEBUG_TYPE_WRITE    1
#define DEBUG_TYPE_OCD_READ 2
#define DEBUG_TYPE_BUFFER   3

#define LINE_LEN  16
static void cy7c65215_debug_buffer(uint8_t *buffer, int length, uint8_t type)
{
	char line[128];
	char s[4];
	int i;
	int j;

	switch (type) {
		case DEBUG_TYPE_READ:
			sprintf(line, "USB READ %d bytes", length);
			break;
		case DEBUG_TYPE_WRITE:
			sprintf(line, "USB WRITE %d bytes", length);
			break;
		case DEBUG_TYPE_OCD_READ:
			sprintf(line, "TO OpenOCD %d bytes", length);
			break;
		case DEBUG_TYPE_BUFFER:
			sprintf(line, "Buffer %d bytes", length);
			break;
	}

	LOG_DEBUG("%s", line);

	for (i = 0; i < length; i += LINE_LEN) {
		switch (type) {
			case DEBUG_TYPE_READ:
				sprintf(line, "USB READ: %04x", i);
				break;
			case DEBUG_TYPE_WRITE:
				sprintf(line, "USB WRITE: %04x", i);
				break;
			case DEBUG_TYPE_OCD_READ:
				sprintf(line, "TO OpenOCD: %04x", i);
				break;
			case DEBUG_TYPE_BUFFER:
				sprintf(line, "BUFFER: %04x", i);
				break;
		}

		for (j = i; j < i + LINE_LEN && j < length; j++) {
			sprintf(s, " %02x", buffer[j]);
			strcat(line, s);
		}
		LOG_DEBUG("%s", line);
	}

}

#endif

static int8_t cy7c65215_get_tap_state(int8_t state)
{

	switch (state) {
		case TAP_DREXIT2:   return CY7C65215_TAP_EXIT2_DR;
		case TAP_DREXIT1:   return CY7C65215_TAP_EXIT1_DR;
		case TAP_DRSHIFT:   return CY7C65215_TAP_SHIFT_DR;
		case TAP_DRPAUSE:   return CY7C65215_TAP_PAUSE_DR;
		case TAP_IRSELECT:  return CY7C65215_TAP_SELECT_IR;
		case TAP_DRUPDATE:  return CY7C65215_TAP_UPDATE_DR;
		case TAP_DRCAPTURE: return CY7C65215_TAP_CAPTURE_DR;
		case TAP_DRSELECT:  return CY7C65215_TAP_SELECT_DR;
		case TAP_IREXIT2:   return CY7C65215_TAP_EXIT2_IR;
		case TAP_IREXIT1:   return CY7C65215_TAP_EXIT1_IR;
		case TAP_IRSHIFT:   return CY7C65215_TAP_SHIFT_IR;
		case TAP_IRPAUSE:   return CY7C65215_TAP_PAUSE_IR;
		case TAP_IDLE:      return CY7C65215_TAP_IDLE;
		case TAP_IRUPDATE:  return CY7C65215_TAP_UPDATE_IR;
		case TAP_IRCAPTURE: return CY7C65215_TAP_CAPURE_IR;
		case TAP_RESET:     return CY7C65215_TAP_RESET;
		case TAP_INVALID:
		default:            return CY7C65215_TAP_INVALID;
	}
}

static int cy7c65215_buf_write(
	uint8_t *buf, int size, uint32_t *bytes_written)
{
	int ret;

#ifdef _DEBUG_USB_COMMS_
	cy7c65215_debug_buffer(buf, size, DEBUG_TYPE_WRITE);
#endif

	if (size == 0) {
		*bytes_written = 0;
		return ERROR_OK;
	}

	ret = jtag_libusb_control_transfer(usbh, CY7C65215_JTAG_REQUEST,
									   CY7C65215_JTAG_WRITE, size, 0,
									   NULL, 0, CY7C65215_USB_TIMEOUT);
	if (ret < 0) {
		LOG_ERROR("vendor command failed, error %d", ret);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	ret = jtag_libusb_bulk_write(usbh, ep_out, (char *)buf, size,
								 CY7C65215_USB_TIMEOUT);
	if (ret < 0) {
		LOG_ERROR("bulk write failed, error %d", ret);
		return ERROR_JTAG_DEVICE_ERROR;
	}
	*bytes_written = ret;

	return ERROR_OK;
}

static int cy7c65215_buf_read(uint8_t *buf, uint32_t qty, uint32_t *bytes_read)
{
	int ret;

	if (qty == 0) {
		*bytes_read = 0;
		goto out;
	}

	ret = jtag_libusb_control_transfer(usbh, CY7C65215_JTAG_REQUEST,
									   CY7C65215_JTAG_READ, qty, 0,
									   NULL, 0, CY7C65215_USB_TIMEOUT);
	if (ret < 0) {
		LOG_ERROR("vendor command failed, error %d", ret);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	ret = jtag_libusb_bulk_read(usbh, ep_in, (char *)buf, qty,
								CY7C65215_USB_TIMEOUT);
	if (ret < 0) {
		LOG_ERROR("bulk read failed, error %d", ret);
		return ERROR_JTAG_DEVICE_ERROR;
	}
	*bytes_read = ret;

out:
#ifdef _DEBUG_USB_COMMS_
	cy7c65215_debug_buffer(buf, *bytes_read, DEBUG_TYPE_READ);
#endif

	return ERROR_OK;
}

static int cy7c65215_sendcommand(uint8_t cmd)
{
	uint32_t written;
	return cy7c65215_buf_write(&cmd, 1, &written);
}

static int cy7c65215_speed(int speed)
{
	int clockcmd;
	switch (speed) {
		case 48000:
			clockcmd = 0x00;
			break;
		case 24000:
			clockcmd = 0x20;
			break;
		case 12000:
			clockcmd = 0x40;
			break;
		case 6000:
			clockcmd = 0x60;
			break;
		case 3000:
			clockcmd = 0x80;
			break;
		case 1500:
			clockcmd = 0xA0;
			break;
		case 750:
			clockcmd = 0xC0;
			break;
		case 375:
			clockcmd = 0xE0;
			break;
		default:
			clockcmd = 0xE0;
			LOG_WARNING("adapter speed not recognized, reverting to 375 kHz");
			break;
	}
	cy7c65215_sendcommand(clockcmd);

	return ERROR_OK;
}

static int cy7c65215_init(void)
{
	int ret;

	usb_tx_buf_offs = 0;
	usb_rx_buf_len = 0;
	cy7c65215_scan_result_count = 0;

	usbh = NULL;
	ret = jtag_libusb_open(cy7c65215_vids, cy7c65215_pids, NULL, &usbh);
	if (ret != ERROR_OK) {
		LOG_ERROR("unable to open cy7c65215 device");
		goto err;
	}

	ret = jtag_libusb_choose_interface(usbh, &ep_in, &ep_out,
									   CY7C65215_JTAG_CLASS,
									   CY7C65215_JTAG_SUBCLASS, -1);
	if (ret != ERROR_OK) {
		LOG_ERROR("unable to claim JTAG interface");
		goto err;
	}

	ret = jtag_libusb_control_transfer(usbh,
									   CY7C65215_JTAG_REQUEST,
									   CY7C65215_JTAG_ENABLE,
									   0, 0, NULL, 0, CY7C65215_USB_TIMEOUT);
	if (ret < 0) {
		LOG_ERROR("could not enable JTAG module");
		goto err;
	}

	cy7c65215_speed(375); /* Start at slowest adapter speed */
	cy7c65215_sendcommand(0x75); /* MSB */

	return ERROR_OK;

err:
	if (usbh != NULL)
		jtag_libusb_close(usbh);
	return ERROR_JTAG_INIT_FAILED;
}

static int cy7c65215_quit(void)
{
	int ret;

	ret = jtag_libusb_control_transfer(usbh,
									   CY7C65215_JTAG_REQUEST,
									   CY7C65215_JTAG_DISABLE,
									   0, 0, NULL, 0, CY7C65215_USB_TIMEOUT);
	if (ret < 0)
		LOG_WARNING("could not disable JTAG module");

	jtag_libusb_close(usbh);

	return ERROR_OK;
}

static void cy7c65215_write_tap_buffer(void)
{
	uint32_t written;

	cy7c65215_buf_write(usb_tx_buf, usb_tx_buf_offs, &written);
	cy7c65215_buf_read(usb_rx_buf, usb_tx_buf_offs, &usb_rx_buf_len);

	usb_tx_buf_offs = 0;
}

static int cy7c65215_execute_tap_queue(void)
{
	cy7c65215_write_tap_buffer();

	int res_count = 0;

	if (cy7c65215_scan_result_count && usb_rx_buf_len) {

		int count;
		int rx_offs = 0;
		int len;

		/* for every pending result */
		while (res_count < cy7c65215_scan_result_count) {

			/* get sent bits */
			len = cy7c65215_scan_result_buffer[res_count].bits;

			count = 0;

			uint8_t *buffer = cy7c65215_scan_result_buffer[res_count].buffer;

			while (len > 0) {
				buffer[count] = usb_rx_buf[rx_offs];
				len -= 8;

				rx_offs++;
				count++;
			}

#ifdef _DEBUG_USB_COMMS_
			cy7c65215_debug_buffer(buffer,
				DIV_ROUND_UP(cy7c65215_scan_result_buffer[res_count].bits, 8),
				DEBUG_TYPE_OCD_READ);
#endif
			jtag_read_buffer(buffer, cy7c65215_scan_result_buffer[res_count].command);

			if (cy7c65215_scan_result_buffer[res_count].buffer)
				free(cy7c65215_scan_result_buffer[res_count].buffer);

			res_count++;
		}
	}

	cy7c65215_scan_result_count = 0;

	return ERROR_OK;
}

static void cy7c65215_add_byte(char buf)
{

	if (usb_tx_buf_offs == CY7C65215_BUFFER_SIZE) {
		DEBUG_JTAG_IO("Forcing execute_tap_queue");
		DEBUG_JTAG_IO("TX Buff offs=%d", usb_tx_buf_offs);
		cy7c65215_execute_tap_queue();
	}

	usb_tx_buf[usb_tx_buf_offs] = buf;
	usb_tx_buf_offs++;
}

static void cy7c65215_add_scan(uint8_t *buffer, int length, struct scan_command *scan_cmd)
{

	/* Ensure space to send long chains */
	/* We add two byte for each eight (or less) bits, one for command, one for data */
	if (usb_tx_buf_offs + (DIV_ROUND_UP(length, 8) * 2) >= CY7C65215_BUFFER_SIZE) {
		DEBUG_JTAG_IO("Forcing execute_tap_queue from scan");
		DEBUG_JTAG_IO("TX Buff offs=%d len=%d", usb_tx_buf_offs, DIV_ROUND_UP(length, 8) * 2);
		cy7c65215_execute_tap_queue();
	}

	cy7c65215_scan_result_buffer[cy7c65215_scan_result_count].bits = length;
	cy7c65215_scan_result_buffer[cy7c65215_scan_result_count].command = scan_cmd;
	cy7c65215_scan_result_buffer[cy7c65215_scan_result_count].buffer = buffer;

	uint8_t command;
	uint8_t bits;
	int count = 0;
	while (length) {

		/* write command */
		command = 6;

		/* last bits? */
		if (length <= 8) {
			/* tms high */
			command |= (1 << 4);

			/* bits to transfer */
			bits = (length - 1);
			command |= bits << 5;
			length = 0;
		} else {
			/* whole byte */

			/* bits to transfer */
			bits = 7;
			command |= (7 << 5);
			length -= 8;
		}

		cy7c65215_add_byte(command);
		cy7c65215_add_byte(buffer[count]);
		count++;
	}

	cy7c65215_scan_result_count++;
}

static void cy7c65215_execute_reset(struct jtag_command *cmd)
{

	DEBUG_JTAG_IO("reset trst: %i srst %i",
			cmd->cmd.reset->trst, cmd->cmd.reset->srst);

	uint8_t buf;

	if (cmd->cmd.reset->trst) {
		buf = 0x03;
	} else {
		buf = 0x04;
		buf |= 0x05 << 4;
	}

	cy7c65215_add_byte(buf);
}

static void cy7c65215_execute_sleep(struct jtag_command *cmd)
{
	jtag_sleep(cmd->cmd.sleep->us);
}

static void cy7c65215_set_state(uint8_t openocd_state)
{
	int8_t state = cy7c65215_get_tap_state(openocd_state);

	uint8_t buf = 0;
	buf = 0x01;
	buf |= state << 4;

	cy7c65215_add_byte(buf);
}

static void cy7c65215_execute_statemove(struct jtag_command *cmd)
{
	DEBUG_JTAG_IO("state move to %i", cmd->cmd.statemove->end_state);

	tap_set_end_state(cmd->cmd.statemove->end_state);

	cy7c65215_set_state(cmd->cmd.statemove->end_state);

	tap_set_state(tap_get_end_state());
}


static void cy7c65215_execute_scan(struct jtag_command *cmd)
{

	int scan_size, old_state;
	uint8_t *buffer;

	DEBUG_JTAG_IO("scan ends in %s", tap_state_name(cmd->cmd.scan->end_state));

	/* get scan info */
	tap_set_end_state(cmd->cmd.scan->end_state);
	scan_size = jtag_build_buffer(cmd->cmd.scan, &buffer);

#ifdef _DEBUG_USB_COMMS_
	cy7c65215_debug_buffer(buffer, (scan_size + 7) / 8, DEBUG_TYPE_BUFFER);
#endif
	/* set state */
	old_state = tap_get_end_state();
	cy7c65215_set_state(cmd->cmd.scan->ir_scan ? TAP_IRSHIFT : TAP_DRSHIFT);
	tap_set_state(cmd->cmd.scan->ir_scan ? TAP_IRSHIFT : TAP_DRSHIFT);
	tap_set_end_state(old_state);

	cy7c65215_add_scan(buffer, scan_size, cmd->cmd.scan);

	cy7c65215_set_state(cmd->cmd.scan->ir_scan ? TAP_IRPAUSE : TAP_DRPAUSE);
	tap_set_state(cmd->cmd.scan->ir_scan ? TAP_IRPAUSE : TAP_DRPAUSE);

	if (tap_get_state() != tap_get_end_state()) {
		cy7c65215_set_state(tap_get_end_state());
		tap_set_state(tap_get_end_state());
	}
}

static void cy7c65215_execute_runtest(struct jtag_command *cmd)
{

	tap_state_t end_state = cmd->cmd.runtest->end_state;
	tap_set_end_state(end_state);

	/* only do a state_move when we're not already in IDLE */
	if (tap_get_state() != TAP_IDLE) {
		cy7c65215_set_state(TAP_IDLE);
		tap_set_state(TAP_IDLE);
	}

	if (cmd->cmd.runtest->num_cycles > 16)
		LOG_WARNING("num_cycles > 16 on run test");

	if (cmd->cmd.runtest->num_cycles) {
		uint8_t command;
		command = 7;
		command |= ((cmd->cmd.runtest->num_cycles - 1) & 0x0F) << 4;

		cy7c65215_add_byte(command);
	}
	tap_set_end_state(end_state);
	if (tap_get_end_state() != tap_get_state()) {
		cy7c65215_set_state(end_state);
		tap_set_state(end_state);
	}
}

static void cy7c65215_execute_command(struct jtag_command *cmd)
{
	DEBUG_JTAG_IO("cy7c65215_execute_command %i", cmd->type);
	switch (cmd->type) {
	case JTAG_RESET:
			cy7c65215_execute_reset(cmd);
			break;
	case JTAG_SLEEP:
			cy7c65215_execute_sleep(cmd);
			break;
	case JTAG_TLR_RESET:
			cy7c65215_execute_statemove(cmd);
			break;
	case JTAG_SCAN:
			cy7c65215_execute_scan(cmd);
			break;
	case JTAG_RUNTEST:
			cy7c65215_execute_runtest(cmd);
			break;
	default:
		LOG_ERROR("BUG: unknown CY7C65215 command type encountered");
		exit(-1);
	}
}

static int cy7c65215_execute_queue(void)
{
	struct jtag_command *cmd = jtag_command_queue;

	while (cmd != NULL) {
		cy7c65215_execute_command(cmd);
		cmd = cmd->next;
	}

	return cy7c65215_execute_tap_queue();
}

static int cy7c65215_speed_div(int speed, int *khz)
{
	*khz = speed;

	return ERROR_OK;
}

static int cy7c65215_khz(int khz, int *jtag_speed)
{
	if (khz >= 48000)
		*jtag_speed = 48000;
	else if (khz >= 24000)
		*jtag_speed = 24000;
	else if (khz >= 12000)
		*jtag_speed = 12000;
	else if (khz >= 6000)
		*jtag_speed = 6000;
	else if (khz >= 3000)
		*jtag_speed = 3000;
	else if (khz >= 1500)
		*jtag_speed = 1500;
	else if (khz >= 750)
		*jtag_speed = 750;
	else
		*jtag_speed = 375;

	return ERROR_OK;
}

static int cy7c65215_power_dropout(int *dropout)
{
	*dropout = 0; /* we cannot detect power dropout */
	return ERROR_OK;
}

static int cy7c65215_srst_asserted(int *srst_asserted)
{
	*srst_asserted = 0; /* we cannot detect srst asserted */
	return ERROR_OK;
}

struct jtag_interface cy7c65215_interface = {
	.name = "cy7c65215",
	.transports = jtag_only,
	.execute_queue = cy7c65215_execute_queue,
	.speed = cy7c65215_speed,
	.init = cy7c65215_init,
	.quit = cy7c65215_quit,
	.speed_div = cy7c65215_speed_div,
	.khz = cy7c65215_khz,
	.power_dropout = cy7c65215_power_dropout,
	.srst_asserted = cy7c65215_srst_asserted,
};
