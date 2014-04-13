/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2013 by Andrey Yurovsky                                 *
 *   yurovsky@gmail.com                                                    *
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif


#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <libusb_common.h>

#include <log.h>
#include <helper/types.h>

#include "protocol.h"

#define NUM_RETRY		5
#define TIMEOUT_MS		100

/**
 * Send command data out to USBDM device
 * @param dev USB device handle
 * @param outep OUT endpoint number
 * @param data command data
 * @parma data_size length of @data
 */
static int usbdm_data_out(struct jtag_libusb_device_handle *dev,
			  uint8_t outep,
			  const uint8_t *data, size_t data_size)
{
	/* To initiate a command execution host sends a command data
	 * which, if it exeeceds maximum transfer size for OUT
	 * enpoint, can be split into two packets:

	 1st pkt
	 +--------------------------+
	 |  Size of entire command  |  1 - size of the command data*
	 +--------------------------+
	 |  Command byte            |  1
	 +--------------------------+
	 |                          |  0... up to 62
	 | //// DATA ////////////// |
	 |                          |
	 +--------------------------+

	 2nd pkt (optional)
	 +--------------------------+
	 |  0                       |  1 - Ensures pkt can't be mistaken as 1st pkt
	 +--------------------------+
	 |                          |  1 ... up to 63
	 | //// DATA ////////////// |
	 |                          |
	 +--------------------------+

	 __________________
	 * including the size byte but not including the 0 byte in the
	   second packet

	*/
	assert(data_size <= USBDM_PACKET_CMD_TRANSFER_MAX_DATA_SIZE);

	uint8_t packet[USBDM_PACKET_MAX_SIZE];
	int     packet_nr;
	size_t  residue, size;
	int     ret;

	packet_nr = 1;
	residue   = data_size;

	do {
		size = MIN(residue, USBDM_PACKET_MAX_DATA_SIZE);

		switch (packet_nr) {
		case 1:
			packet[0] = data_size + 1;
			break;
		case 2:
			packet[0] = 0;
			break;
		default:
			assert(0);
		}

		memcpy(&packet[1], data, size);

		ret = jtag_libusb_bulk_write(dev, outep,
					     (char *)packet, size + 1,
					     TIMEOUT_MS);
		if (!ret) {
			LOG_ERROR("Failed to write command to device");
			return -EIO;
		}

		data    += size;
		residue -= size;
		packet_nr++;
	} while (residue);

	return ERROR_OK;
}

/**
 * Return the number of bytes that command is expected to return
 * @parma cmd command structure
 * @returns size of the data expected to be returned
 */
static size_t usbdm_get_expected_result_size(const uint8_t *cmd)
{
	const struct {
		uint8_t cmd;
		size_t size;
	} lut[] = {
		{ USBDM_CMD_GET_CAPABILITIES,	7 },
		{ USBDM_CMD_SET_TARGET,		0 },
		{ USBDM_CMD_CONTROL_PINS,	2 },
		{ USBDM_CMD_READ_REG,		4 },
		{ USBDM_CMD_READ_DREG,		4 },
		{ USBDM_CMD_WRITE_REG,		0 },
		{ USBDM_CMD_WRITE_DREG,		0 },
		{ USBDM_CMD_WRITE_MEM,		0 },
		{ USBDM_CMD_CONNECT,		0 },
		{ USBDM_CMD_TARGET_HALT,	0 },
		{ USBDM_CMD_TARGET_GO,		0 },
		{ USBDM_CMD_TARGET_STEP,	0 },
	};

	if (cmd[0] == USBDM_CMD_READ_MEM)
		return cmd[2];

	for (size_t i = 0; i < ARRAY_SIZE(lut); i++) {
		if (cmd[0] == lut[i].cmd)
			return lut[i].size;
	}

	assert(0);
	return 0;
}
/**
 * Check and see if the command was executed successfully and if not
 * decode returned error code and print appropriate message.
 * @param ret return value from the USB transfer funciton
 * @param rxb buffer
 * @returns ERROR_OK on success and ERROR_FAIL on failure
 */
static int usbdm_check_exit_status(int ret, const uint8_t rxb[], int expected_size)
{
	if (ret <= 0) {
		LOG_ERROR("Command failed without any status code");
		return -EIO;
	}

	const enum usbdm_return_codes rc = rxb[0];

	if (rc == USBDM_RC_OK) {
		if (ret - 1 != expected_size) {
			LOG_ERROR("Command reported success, but not all of the data was returned(short read)");
			return ERROR_FAIL;
		} else {
			return ERROR_OK;
		}
	}

	const char *reason;

	switch (rc) {
	case USBDM_RC_ILLEGAL_PARAMS:
		reason = "Illegal parameters to command";
		break;
	case USBDM_RC_FAIL:
		reason = "Genral Fail";
		break;
	case USBDM_RC_ILLEGAL_COMMAND:
		reason = "Illegal (unknown) command (may be in wrong target mode)";
		break;
	case USBDM_RC_NO_CONNECTION:
		reason = "No connection to target";
		break;
	case USBDM_RC_OVERRUN:
		reason = "New command before previous command completed";
		break;
	case USBDM_RC_CF_ILLEGAL_COMMAND:
		reason = "Coldfire BDM interface did not recognize the command";
		break;
	case USBDM_RC_UNKNOWN_TARGET:
		reason = "Target not supported";
		break;
	case USBDM_RC_NO_TX_ROUTINE:
		reason = "No Tx routine available at measured BDM communication speed";
		break;
	case USBDM_RC_NO_RX_ROUTINE:
		reason = "No Rx routine available at measured BDM communication speed";
		break;
	case USBDM_RC_BDM_EN_FAILED:
		reason = "Failed to enable BDM mode in target (warning)";
		break;
	case USBDM_RC_RESET_TIMEOUT_FALL:
		reason = "RESET signal failed to fall";
		break;
	case USBDM_RC_BKGD_TIMEOUT:
		reason = "BKGD signal failed to rise/fall";
		break;
	case USBDM_RC_SYNC_TIMEOUT:
		reason = "No response to SYNC sequence";
		break;
	case USBDM_RC_UNKNOWN_SPEED:
		reason = "Communication speed is not known or cannot be determined";
		break;
	case USBDM_RC_WRONG_PROGRAMMING_MODE:
		reason = "Attempted Flash programming when in wrong mode (e.g. Vpp off)";
		break;
	case USBDM_RC_FLASH_PROGRAMING_BUSY:
		reason = "Busy with last Flash programming command";
		break;
	case USBDM_RC_VDD_NOT_REMOVED:
		reason = "Target Vdd failed to fall";
		break;
	case USBDM_RC_VDD_NOT_PRESENT:
		reason = "Target Vdd not present/failed to rise";
		break;
	case USBDM_RC_VDD_WRONG_MODE:
		reason = "Attempt to cycle target Vdd when not controlled by BDM interface";
		break;
	case USBDM_RC_CF_BUS_ERROR:
		reason = "Illegal bus cycle on target (Coldfire)";
		break;
	case USBDM_RC_USB_ERROR:
		reason = "Indicates USB transfer failed (returned by driver not BDM)";
		break;
	case USBDM_RC_ACK_TIMEOUT:
		reason = "Indicates an expected ACK was missing";
		break;
	case USBDM_RC_FAILED_TRIM:
		reason = "Trimming of target clock failed (out of clock range?).";
		break;
	case USBDM_RC_FEATURE_NOT_SUPPORTED:
		reason = "Feature not supported by this version of hardware/firmware";
		break;
	case USBDM_RC_RESET_TIMEOUT_RISE:
		reason = "RESET signal failed to rise";
		break;

	default:
		LOG_ERROR("Command execution failed, with unknown reason: %d", rc);
		return ERROR_FAIL;
	}

	LOG_ERROR("Command execution failed, reason: %s", reason);

	return ERROR_FAIL;
}


/**
 * Revice and pre-process data from USBDM device's IN enpoint
 * @param dev USB device handle
 * @param inep IN enpoint number
 * @param cmd command for which retrieve a response
 * @param rxb buffer to store received data in
 * @param np length of @rxb
 * @returns ERROR_OK on success, negative error code on failure
 */
static int usbdm_data_in(struct jtag_libusb_device_handle *dev,
			 uint8_t inep, const uint8_t *cmd, uint8_t *rxb, size_t nb)
{
	/*
	  In response to any command USBDM firmware responds with a
	  packet that looks like this:

	  +--------------------------+
	  |  satus byte              |  Status byte containing the resulting error code
	  +--------------------------+
	  |                          |  0 ... up to 63 (Optional data)
	  | //// DATA ////////////// |
	  |                          |
	  +--------------------------+
	*/

	int ret;

	uint8_t packet[USBDM_PACKET_MAX_SIZE];

	const int longest_pause = 5000;
	int pause_duration      = 1;
	bool busy;

	do {
		ret = jtag_libusb_bulk_read(dev, inep,
					    (char *)packet, sizeof(packet),
					    TIMEOUT_MS);
		/*
		  It is possible that FW didn't have enought time to
		  process our request. In that case it would return us
		  a 1 byte packet conatinging USBBDM_RC_BUSY error
		  code.

		  In case this happens we need to back off for a while
		  and retry again later. Re-reading immediately only
		  makes the situation worse, because device does not
		  have enought to place anyting into it's USB
		  peripheral buffer, so immediate second read results
		  in a NAK.

		  This code implements the same algorithm that is
		  implemented in
		  https://github.com/podonoghue/usbdm-eclipse-makefiles-build
		 */

		busy = ret && packet[0] == USBDM_RC_BUSY;

		if (busy) {
			usleep(pause_duration * 1000);
			pause_duration *= 2;
		}
	} while (busy && pause_duration <= longest_pause);

	const size_t extra_data_size = usbdm_get_expected_result_size(cmd);

	ret = usbdm_check_exit_status(ret, packet, extra_data_size);
	if (ret < 0)
		return ret;

	/*
	   If caller provided us with the return buffer(for commands that
	   do not fetch additional data besides the status byte it
	   makes sense to chose not to) copy the result into it.
	 */
	if (rxb && nb) {
		if (nb != extra_data_size) {
			LOG_ERROR("User provided buffer size does not match returned data size(%u bytes given, but %u was returned)",
				  (unsigned)nb, (unsigned)extra_data_size);
			return -EINVAL;
		}

		memcpy(rxb, packet + 1, nb);
	}

	return ERROR_OK;
}

int usbdm_command_response(struct jtag_libusb_device_handle *dev,
			   uint8_t inep, uint8_t outep,
			   const uint8_t *command, size_t command_size,
			   uint8_t *buffer, size_t buffer_size)
{
	int ret;

	ret = usbdm_data_out(dev, outep, command, command_size);
	if (ret < 0)
		return ret;

	return usbdm_data_in(dev, inep, command, buffer, buffer_size);
}

int usbdm_control_in(struct jtag_libusb_device_handle *dev,
		     uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
		     uint8_t *buffer, size_t buffer_size)
{
	return jtag_libusb_control_transfer(dev,
					    LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN,
					    bRequest,
					    wValue,
					    wIndex,
					    (char *)buffer,
					    sizeof(bRequest) +
					    sizeof(wValue) +
					    sizeof(wIndex) +
					    buffer_size,
					    (unsigned int)TIMEOUT_MS);
}
