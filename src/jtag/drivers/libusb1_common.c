/***************************************************************************
 *   Copyright (C) 2009 by Zachary T Welch <zw@superlucidity.net>          *
 *                                                                         *
 *   Copyright (C) 2011 by Mauro Gamba <maurillo71@gmail.com>              *
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "log.h"
#include "libusb1_common.h"
#include "types.h"
#include "unicode.h"


static struct libusb_context *jtag_libusb_context; /**< Libusb context **/

/* Maximum number of devices to report which fail to match vid/pid/serial
 * criteria.
 */
#define MAX_CANDIDATE_DEVICES	    10

struct candidate_device {
	struct jtag_libusb_device_handle *device_handle;
	uint16_t vid;
	uint16_t pid;
	uint8_t serial_utf8[256+1];  /** Max USB descriptor size 256 + null */
};


static int jtag_libusb_match(struct libusb_device_descriptor *dev_desc,
		const uint16_t vids[], const uint16_t pids[])
{
	for (size_t i = 0; vids[i]; i++) {
		if (dev_desc->idVendor == vids[i] &&
			dev_desc->idProduct == pids[i]) {
			return i;
		}
	}
	return -1;
}

/* Obtain a USB string descriptor in utf8 format.
 */
static bool get_string_descriptor_utf8(libusb_device_handle *device, uint8_t str_index,
					uint8_t *desc_utf8, size_t desc_utf8_size)
{
	int retval;
	unsigned char tbuf[255];
	uint16_t langid;
	uint8_t desc_utf16le[2+256*2+1];  /* Max size of string in UTF16 */
					/* plus 2 byte length  */

	if (str_index == 0)
		return false;

	/* Asking for the zero'th index is special - it returns a string
	   descriptor that contains all the language IDs supported by the device.
	   Typically there aren't many - often only one. The language IDs are 16
	   bit numbers, and they start at the third byte in the descriptor. See
	   USB 2.0 specification section 9.6.7 for more information.
	   Note from libusb 1.0 sources (descriptor.c) */

	retval = libusb_get_string_descriptor(device, 0, 0, tbuf, sizeof(tbuf));
	if (retval < 4) {
		LOG_ERROR("libusb_get_descriptor() failed to obtain language id with %d",
				retval);
		return false;
	}

	langid = tbuf[2] | (tbuf[3] << 8);

	/* libusb1's libusb_get_string_descriptor_ascii() replaces non ASCII
	 * characters with '?' (0x3f). So use libusb_get_string_descriptor() instead.
	 * Non ASCII characters in USB serials are found in the wild on
	 * ST-Link and STM32 Discovery boards, which have serials like
	 * "Q\377j\006I\207PS(H\t\207".
	 * */

	memset(desc_utf16le, 0, sizeof(desc_utf16le));
	retval = libusb_get_string_descriptor(device, str_index, langid,
			desc_utf16le, sizeof(desc_utf16le) - 1);
	if (retval < 0) {
		LOG_ERROR("libusb_get_string_descriptor() failed with %d", retval);
		return false;
	}

	if (retval < 2
			|| desc_utf16le[1] != LIBUSB_DT_STRING
			|| desc_utf16le[0] > retval) {
		LOG_ERROR("libusb_get_string_descriptor() string descriptor "
				"validation failed");
		return false;
	}

	/* USB string descriptors are stored in UTF-16LE encoding.  Conversion to
	 * UTF-8 allows comparison with user entered string such as serial numbers.
	 */
	if (utf16le_to_utf8(&desc_utf16le[2], retval - 2, desc_utf8, desc_utf8_size) < 0) {
		LOG_DEBUG("Invalid USB descriptor utf-16le encoding");
		return false;
	}

	return true;
}

static size_t iterate_devs(libusb_device **devs,
		const uint16_t vids[], const uint16_t pids[],
		const uint8_t *serial_utf8,
		struct candidate_device candidate_devices[],
		size_t n_candidate_devices)
{
	struct jtag_libusb_device_handle *libusb_handle = NULL;
	int errCode;
	size_t num_matched = 0;

	for (size_t idx = 0; devs[idx] != NULL; idx++) {
		if (num_matched >= n_candidate_devices)
			break;

		struct libusb_device_descriptor dev_desc;

		if (libusb_get_device_descriptor(devs[idx], &dev_desc) != 0)
			continue;

		int vid_pid_match_idx = jtag_libusb_match(&dev_desc, vids, pids);
		if (vid_pid_match_idx < 0)
			continue;

		struct candidate_device *candidate_device = &candidate_devices[num_matched++];
		candidate_device->vid = vids[vid_pid_match_idx];
		candidate_device->pid = pids[vid_pid_match_idx];

		errCode = libusb_open(devs[idx], &libusb_handle);

		if (errCode) {
			LOG_ERROR("libusb_open() on VID:PID %u:%u failed with %s",
					candidate_device->vid,
					candidate_device->pid,
					libusb_error_name(errCode));
			continue;
		}

		/* Device must be open to use libusb_get_string_descriptor. */
		get_string_descriptor_utf8(libusb_handle, dev_desc.iSerialNumber,
				candidate_device->serial_utf8,
				sizeof(candidate_device->serial_utf8));

		if (serial_utf8 != NULL) {
			if (candidate_device->serial_utf8[0] == 0)
				continue;

			bool matched = !strncmp((const char *)serial_utf8,
					(const char *)candidate_device->serial_utf8,
					sizeof(candidate_device->serial_utf8));

			if (!matched) {
				libusb_close(libusb_handle);
				continue;
			}
		}

		/* Success. */
		candidate_device->device_handle = libusb_handle;
		break;
	}
	return num_matched;
}

int jtag_libusb_open(const uint16_t vids[], const uint16_t pids[],
		const char *serial,
		struct jtag_libusb_device_handle **out)
{
	const uint8_t *serial_utf8 = (const uint8_t *)serial;

	if (libusb_init(&jtag_libusb_context) < 0)
		return -ENODEV;

	libusb_device **devs;
	ssize_t cnt = libusb_get_device_list(jtag_libusb_context, &devs);
	if (cnt < 0) {
		LOG_ERROR("libusb_get_device_list() failed with %s",
			  libusb_error_name(cnt));
		return -ENODEV;
	}

	struct candidate_device candidate_devices[MAX_CANDIDATE_DEVICES] = { { 0 } };

	/* There will be at most one candidate device returned for a
	 * succesfully matched and opened device, or there will be a
	 * list of devices which failed the vid/pid/serial matching
	 * criteria.
	 */
	size_t n_candidates = iterate_devs(devs, vids, pids, serial_utf8,
			candidate_devices, ARRAY_SIZE(candidate_devices));

	char serial_text[256*4+1]; /* Max 256 byte descriptor formatted */
				   /* as \xHH chars */
	utf8_to_text(serial_utf8, serial_text, sizeof(serial_text));

	*out = NULL;
	for (size_t idx = 0; idx < n_candidates; ++idx) {
		char descriptor_text[256*4+1] = {0};
		struct candidate_device *candidate_device = &candidate_devices[idx];

		utf8_to_text(candidate_device->serial_utf8, descriptor_text,
				sizeof(descriptor_text));

		if (candidate_device->device_handle != NULL) {
			LOG_INFO("Device with vid: 0x%04x pid: 0x%04x serial: %s opened",
					candidate_device->vid,
					candidate_device->pid,
					descriptor_text);
			*out = candidate_device->device_handle;
			break;
		}

		LOG_ERROR("Device with vid: 0x%04x pid: 0x%04x serial: %s doesn't match requested "
				"serial: %s",
				candidate_device->vid,
				candidate_device->pid,
				descriptor_text,
				serial_text);
	}

	libusb_free_device_list(devs, 1);

	return *out != NULL ? 0 : -ENODEV;
}

void jtag_libusb_close(jtag_libusb_device_handle *dev)
{
	/* Close device */
	libusb_close(dev);

	libusb_exit(jtag_libusb_context);
}

int jtag_libusb_control_transfer(jtag_libusb_device_handle *dev, uint8_t requestType,
		uint8_t request, uint16_t wValue, uint16_t wIndex, char *bytes,
		uint16_t size, unsigned int timeout)
{
	int transferred = 0;

	transferred = libusb_control_transfer(dev, requestType, request, wValue, wIndex,
				(unsigned char *)bytes, size, timeout);

	if (transferred < 0)
		transferred = 0;

	return transferred;
}

int jtag_libusb_bulk_write(jtag_libusb_device_handle *dev, int ep, char *bytes,
		int size, int timeout)
{
	int transferred = 0;

	libusb_bulk_transfer(dev, ep, (unsigned char *)bytes, size,
			     &transferred, timeout);
	return transferred;
}

int jtag_libusb_bulk_read(jtag_libusb_device_handle *dev, int ep, char *bytes,
		int size, int timeout)
{
	int transferred = 0;

	libusb_bulk_transfer(dev, ep, (unsigned char *)bytes, size,
			     &transferred, timeout);
	return transferred;
}

int jtag_libusb_set_configuration(jtag_libusb_device_handle *devh,
		int configuration)
{
	struct jtag_libusb_device *udev = jtag_libusb_get_device(devh);
	int retCode = -99;

	struct libusb_config_descriptor *config = NULL;
	int current_config = -1;

	retCode = libusb_get_configuration(devh, &current_config);
	if (retCode != 0)
		return retCode;

	retCode = libusb_get_config_descriptor(udev, configuration, &config);
	if (retCode != 0 || config == NULL)
		return retCode;

	/* Only change the configuration if it is not already set to the
	   same one. Otherwise this issues a lightweight reset and hangs
	   LPC-Link2 with JLink firmware. */
	if (current_config != config->bConfigurationValue)
		retCode = libusb_set_configuration(devh, config->bConfigurationValue);

	libusb_free_config_descriptor(config);

	return retCode;
}

int jtag_libusb_choose_interface(struct jtag_libusb_device_handle *devh,
		unsigned int *usb_read_ep,
		unsigned int *usb_write_ep,
		int bclass, int subclass, int protocol)
{
	struct jtag_libusb_device *udev = jtag_libusb_get_device(devh);
	const struct libusb_interface *inter;
	const struct libusb_interface_descriptor *interdesc;
	const struct libusb_endpoint_descriptor *epdesc;
	struct libusb_config_descriptor *config;

	*usb_read_ep = *usb_write_ep = 0;

	libusb_get_config_descriptor(udev, 0, &config);
	for (int i = 0; i < (int)config->bNumInterfaces; i++) {
		inter = &config->interface[i];

		interdesc = &inter->altsetting[0];
		for (int k = 0;
		     k < (int)interdesc->bNumEndpoints; k++) {
			if ((bclass > 0 && interdesc->bInterfaceClass != bclass) ||
			    (subclass > 0 && interdesc->bInterfaceSubClass != subclass) ||
			    (protocol > 0 && interdesc->bInterfaceProtocol != protocol))
				continue;

			epdesc = &interdesc->endpoint[k];

			uint8_t epnum = epdesc->bEndpointAddress;
			bool is_input = epnum & 0x80;
			LOG_DEBUG("usb ep %s %02x",
				  is_input ? "in" : "out", epnum);

			if (is_input)
				*usb_read_ep = epnum;
			else
				*usb_write_ep = epnum;

			if (*usb_read_ep && *usb_write_ep) {
				LOG_DEBUG("Claiming interface %d", (int)interdesc->bInterfaceNumber);
				libusb_claim_interface(devh, (int)interdesc->bInterfaceNumber);
				libusb_free_config_descriptor(config);
				return ERROR_OK;
			}
		}
	}
	libusb_free_config_descriptor(config);

	return ERROR_FAIL;
}

int jtag_libusb_get_pid(struct jtag_libusb_device *dev, uint16_t *pid)
{
	struct libusb_device_descriptor dev_desc;

	if (libusb_get_device_descriptor(dev, &dev_desc) == 0) {
		*pid = dev_desc.idProduct;

		return ERROR_OK;
	}

	return ERROR_FAIL;
}
