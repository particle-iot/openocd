/*
 *   Driver for USB-JTAG, Altera USB-Blaster II and compatibles
 *
 *   Copyright (C) 2013 Franck Jullien franck.jullien@gmail.com
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
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <jtag/interface.h>
#include <jtag/commands.h>
#include <libusb_common.h>

#include "ublast_access.h"
#include "firmware.h"

#define USBBLASTER_CTRL_READ_REV	0x94
#define USBBLASTER_CTRL_LOAD_FIRM	0xA0
#define USBBLASTER_EPOUT		4
#define USBBLASTER_EPIN			8

extern struct ctrl_payload init_blaster[];

static int ublast2_libusb_read(struct ublast_lowlevel *low, uint8_t *buf,
			      unsigned size, uint32_t *bytes_read)
{
	*bytes_read = jtag_libusb_bulk_read(low->libusb_dev,
					    USBBLASTER_EPIN | \
					    LIBUSB_ENDPOINT_IN,
					    (char *)buf,
					    size,
					    100);
	return ERROR_OK;
}

static int ublast2_libusb_write(struct ublast_lowlevel *low, uint8_t *buf,
			       int size, uint32_t *bytes_written)
{
	*bytes_written = jtag_libusb_bulk_write(low->libusb_dev,
						USBBLASTER_EPOUT | \
						LIBUSB_ENDPOINT_OUT,
						(char *)buf,
						size,
						100);
	return ERROR_OK;
}

static int load_usb_blaster_firmware(struct jtag_libusb_device_handle *libusb_dev)
{
	int i = 0;

	while (init_blaster[i].address != 0xffff) {
		jtag_libusb_control_transfer(libusb_dev,
					     LIBUSB_REQUEST_TYPE_VENDOR | \
					     LIBUSB_ENDPOINT_OUT,
					     USBBLASTER_CTRL_LOAD_FIRM,
					     init_blaster[i].address,
					     0,
					     init_blaster[i].data,
					     init_blaster[i].len,
					     100);
		i++;
	}

	return ERROR_OK;
}

static int ublast2_libusb_init(struct ublast_lowlevel *low)
{
	const uint16_t vids[] = { low->ublast_vid_uninit, 0 };
	const uint16_t pids[] = { low->ublast_pid_uninit, 0 };
	struct jtag_libusb_device_handle *temp;
	bool renumeration = false;

	if (jtag_libusb_open(vids, pids, &temp) == ERROR_OK) {
		LOG_INFO("Altera USB-Blaster II (uninitialized) found");
		LOG_INFO("Loading firmware...");
		load_usb_blaster_firmware(temp);
		jtag_libusb_close(temp);
		renumeration = true;
	}

	const uint16_t vids_renum[] = { low->ublast_vid, 0 };
	const uint16_t pids_renum[] = { low->ublast_pid, 0 };

	if (renumeration == false) {
		if (jtag_libusb_open(vids_renum, pids_renum, &low->libusb_dev) != ERROR_OK) {
			LOG_ERROR("Altera USB-Blaster II not found");
			return ERROR_FAIL;
		}
	} else {
		int retry = 10;
		while (jtag_libusb_open(vids_renum, pids_renum, &low->libusb_dev) != ERROR_OK && retry--) {
			sleep(1);
			LOG_INFO("Waiting for renumerate...");
		}

		if (!retry) {
			LOG_ERROR("Altera USB-Blaster II not found");
			return ERROR_FAIL;
		}
	}

	char buffer[5];
	jtag_libusb_control_transfer(low->libusb_dev,
				     LIBUSB_REQUEST_TYPE_VENDOR | \
				     LIBUSB_ENDPOINT_IN,
				     USBBLASTER_CTRL_READ_REV,
				     0,
				     0,
				     buffer,
				     5,
				     100);

	LOG_INFO("Altera USB-Blaster II found (Firm. rev. = %s)", buffer);

	return ERROR_OK;
}

static int ublast2_libusb_quit(struct ublast_lowlevel *low)
{
	jtag_libusb_close(low->libusb_dev);
	return ERROR_OK;
};

static struct ublast_lowlevel low = {
	.open = ublast2_libusb_init,
	.close = ublast2_libusb_quit,
	.read = ublast2_libusb_read,
	.write = ublast2_libusb_write,
};

struct ublast_lowlevel *ublast2_register_libusb(void)
{
	return &low;
}
