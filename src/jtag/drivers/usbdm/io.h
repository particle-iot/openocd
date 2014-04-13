/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2014 by Andrey Smirnov                                  *
 *   andrew.smirnov@gmail.com                                              *
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
 *   along with this program; see the file COPYING.  If not see            *
 *   <http://www.gnu.org/licenses/>                                        *
 *                                                                         *
 ***************************************************************************/

#ifndef __IO_H__
#define __IO_H__

/**
 * Send a special command that is accepted via USB control requests
 * @param dev USB device handle
 * @param bRequest bRequest field of the control request
 * @param wValue wValue field of the control request
 * @param wIndex wIndex field of the control request
 * @param buffer buffer containing additional request data
 * @param buffer_size size of the @buffer
 * @returns ERROR_OK on success, negative error code on failure
 */
int usbdm_control_in(struct jtag_libusb_device_handle *dev,
		     uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
		     uint8_t *buffer, size_t buffer_size);

/**
 * Send a command pacet to the adapter and retrieve a response for it
 * @param dev USB device handle
 * @param inep IN endpoint number
 * @param outep OUT enpoint number
 * @param command buffer containing command packet
 * @param command_size size of the command packet @command
 * @param buffer buffer to store the response in (can be NULL)
 * @param buffer_size size of the @buffer
 * @returns ERROR_OK on success, negative error code on failure
 */
int usbdm_command_response(struct jtag_libusb_device_handle *dev,
			   uint8_t inep, uint8_t outep,
			   const uint8_t *command, size_t command_size,
			   uint8_t *buffer, size_t buffer_size);

/**
 * Send a packet for a command that does not send any extra data
 * @param dev USB device handle
 * @param inep IN endpoint number
 * @param outep OUT enpoint number
 * @param command buffer containing command packet
 * @param command_size size of the command packet @command
 */
static inline int usbdm_command_response_no_data(struct jtag_libusb_device_handle *dev,
						 uint8_t inep, uint8_t outep,
						 const uint8_t *command, size_t command_size)
{
	return usbdm_command_response(dev, inep, outep, command, command_size, NULL, 0);
}

#endif
