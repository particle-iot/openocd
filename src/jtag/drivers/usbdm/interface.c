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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* project specific includes */
#include <jtag/interface.h>
#include <jtag/tcl.h>
#include <transport/transport.h>
#include <helper/time_support.h>

#include <target/target.h>
#include <target/bdm.h>
#include <libusb_common.h>

#include "protocol.h"
#include "io.h"

/**
 * @file Driver for USBDM BDM adapters
 *
 * This file contains subroutines to work with adapters running either
 * this firmware https://github.com/podonoghue/usbdm-firmware or
 * implementing the same protocol.
 *
 */


/*
   Represents a USBDM device
 */
struct usbdm {
	struct bdm_driver bdm;			/* This is a vtable used by all of
						 * the functions that expect a
						 * generic BDM deivce, so it must be
						 * placed at the beginning of the structure */

	struct jtag_libusb_device_handle *devh; /* USB device handle */

	struct {
		unsigned int in, out;
	} endpoint;				/* Stored info about endpoints */
};

static struct usbdm *dongle;

/**
 * Get the software and hardware version from firmware
 * @param bdev USBDM device
 * @returns ERROR_OK on success and negative error code on failure
 */
static int usbdm_get_version(struct usbdm *bdev)
{
	static const struct {
		uint8_t version;
		const char *name;
	} hardware_table[] = {
		{ USBDM_HW_USBDM,			"Universal TBDML/OSBDM JB16" },
		{ USBDM_HW_TBDMLSwin,			"Minimal JB16 version (JB16DWE,JB16JDWE)" },
		{ USBDM_HW_OSBDM,			"Basic OSBDM hardware" },
		{ USBDM_HW_WTBDM,			"Wiztronics BDMS08/12" },
		{ USBDM_HW_OSBDME,			"OSBDM+Flash supply" },
		{ USBDM_HW_USBDM_JMxxCLD,		"USBDM hardware using 9S08JM16/32/60CLD (44p package)" },
		{ USBDM_HW_USBDM_JMxxCLC,		"USBDM hardware using 9S08JM16CLC (32p package)" },
		{ USBDM_HW_USBSPYDER,			"SofTec USBSPYDER08" },
		{ USBDM_HW_USBDM_UF32PBE,		"USBDM hardware using MC9S12UF32PBE (64p package)" },
		{ USBDM_HW_USBDM_CF_JS16CWJ,		"USBDM hardware CF/DSC only using MC9S08JS16CWJ (20p SOIC package)" },
		{ USBDM_HW_USBDM_CF_JMxxCLD,		"Combined USBDM/TBLCF using 9S08JM16/32/60CLD (44p package)" },
		{ USBDM_HW_USBDM_JS16CWJ,		"USBDM hardware using MC9S08JS16CWJ (20p SOIC package)" },
		{ USBDM_HW_USBDM_MC56F8006DEMO,		"MC56F8006DEMO Board (Axiom)" },
		{ USBDM_HW_CUSTOM,			"Custom hardware" },
		{ USBDM_HW_USBDM_CF_SER_JS16CWJ,	"USBDM hardware CF/DSC only using MC9S08JS16CWJ (20p SOIC package)"
							" with serial interface" },
		{ USBDM_HW_USBDM_CF_SER_JMxxCLD,	"Combined USBDM/TBLCF/Serial using 9S08JM16/32/60CLD (44p package)" },
		{ USBDM_HW_USBDM_TWR_KINETIS,		"TWR Kinetis board" },
		{ USBDM_HW_USBDM_TWR_CFV1,		"TWR Coldfire V1 board" },
		{ USBDM_HW_USBDM_TWR_HCS08,		"TWR HCS08 board" },
		{ USBDM_HW_USBDM_TWR_CFVx,		"TWR Coldfire Vx board" },
		{ USBDM_HW_USBDM_SWD_SER_JS16CWJ,	"USBDM MC9S08JS16CWJ with BDM, SWD & Serial" },
		{ USBDM_HW_USBDM_SWD_JS16CWJ,		"USBDM MC9S08JS16CWJ with BDM & SWD" },
		{ USBDM_HW_USBDM_FREEDOM,		"Freescale FRDM-KL25 board (MK20 chip)" },
		{ USBDM_HW_USBDM_MKL25Z,		"MKL25Z" },
		{ USBDM_HW_USBDM_MK20D5,		"MK20DX5" },
		{ USBDM_HW_USBDM_TWR_HCS12,		"TWR HCS12 board" },
	};


	int ret;
	uint8_t rxb[5];

	/*
	   Command:

	   bRequest  = 12 (0x0C)
	   wValue    = 1
	   wIndex    = 0

	   Result:

	   +--------------------------+
	   |  SW version              |  1
	   +--------------------------+
	   |  HW version              |  1
	   +--------------------------+
	   |  Booloader SW version    |  1
	   +--------------------------+
	   |  Booloader HW version    |  1
	   +--------------------------+
	 */

	ret = usbdm_control_in(bdev->devh,
			       USBDM_CMD_GET_VER, 1, 0,
			       rxb, sizeof(rxb));
	if (ret == (int)sizeof(rxb) &&
	    rxb[0] == USBDM_RC_OK) {

		int major, minor;
#if 0
		major = rxb[1] >> 4;
		minor = rxb[1] & 0x0F;

		LOG_INFO("USBDM software version: %d.%d", major, minor);
#endif
		major = rxb[3] >> 4;
		minor = rxb[3] & 0x0F;

		LOG_INFO("USBDM bootloader software version: %d.%d", major, minor);


		if (rxb[2] != rxb[4]) {
			LOG_WARNING("Hardware version reported by bootloader firmware and application firmware do not match\n");
		} else {

			const char *vi, *hwtype;
			int version;

			version = rxb[2];

			if (version > USBMD_HW_VI_UF) {
				vi       = "UF32";
				version -= USBMD_HW_VI_UF;
			} else if (version > USBMD_HW_VI_JM) {
				vi       = "JMxx/JS16";
				version -= USBMD_HW_VI_JM;
			} else if (version > USBMD_HW_VI_ARM) {
				vi       = "ARM";
				version -= USBMD_HW_VI_ARM;
			} else {
				vi       = "JB";
			}

			hwtype = NULL;

			for (size_t i = 0; i < ARRAY_SIZE(hardware_table); i++) {
				if (version == hardware_table[i].version) {
					hwtype = hardware_table[i].name;
					break;
				}
			}

			if (hwtype)
				LOG_INFO("USBDM hardware: [%s] %s", vi, hwtype);
			else
				LOG_WARNING("Unknown type of USBDM hardware(%d)", version);
		}

		return 0;
	} else {
		ret = -EINVAL;
	}

	return ret;
}

/**
 * Query and dispaly device's capabilities
 * @param bdev USBDM device handle
 * @returns ERROR_OK on success, negative error code on failure
 */
static int usbdm_get_capabilities(struct usbdm *bdev)
{
	/*
	  Command:

	  +--------------------------+
	  |  0x05                    |  1
	  +--------------------------+


	  Result:

	  +---------------------------+
	  |  Capabilities bitmask MSB |  1
	  +---------------------------+
	  |  Capabilities bitmask LSB |  1
	  +---------------------------+
	  |  Command buffer size      |  1
	  +---------------------------+
	  |  SW version major         |  1
	  +---------------------------+
	  |  SW version minor         |  1
	  +---------------------------+
	  |  SW version micro         |  1
	  +---------------------------+
	*/

	uint8_t rxb[7];
	const uint8_t command[] = {
		USBDM_CMD_GET_CAPABILITIES,
	};
	const int ret = usbdm_command_response(bdev->devh,
					       bdev->endpoint.in, bdev->endpoint.out,
					       command, sizeof(command),
					       rxb, sizeof(rxb));
	if (ret < 0)
		return ret;

	const uint8_t major = rxb[4];
	const uint8_t minor = rxb[5];
	const uint8_t micro = rxb[6];

	LOG_INFO("USBDM software version: %"PRIu8".%"PRIu8".%" PRIu8, major, minor, micro);

	LOG_INFO("USBDM command buffer size: %d", rxb[2]);

	const uint16_t caps = (rxb[0] << 8) | rxb[1];

	if (caps & USBDM_CAP_HCS12)
		LOG_INFO("USBDM reported capability: supports HCS12 targets");
	if (caps & USBDM_CAP_RS08)
		LOG_INFO("USBDM reported capability: supports RS08 targets");
	if (caps & USBDM_CAP_VDDCONTROL)
		LOG_INFO("USBDM reported capability: control over target Vdd");
	if (caps & USBDM_CAP_VDDSENSE)
		LOG_INFO("USBDM reported capability: sensing of target Vdd");
	if (caps & USBDM_CAP_HCS08)
		LOG_INFO("USBDM reported capability: supports HCS08 targets");
	if (caps & USBDM_CAP_CFVx)
		LOG_INFO("USBDM reported capability: sensing of target Vdd");
	if (caps & USBDM_CAP_CFV1)
		LOG_INFO("USBDM reported capability: supports CFV1 targets");
	if (caps & USBDM_CAP_DSC)
		LOG_INFO("USBDM reported capability: supports DSC targets");
	if (caps & USBDM_CAP_ARM_JTAG)
		LOG_INFO("USBDM reported capability: supports ARM targets via JTAG");
	if (caps & USBDM_CAP_RST)
		LOG_INFO("USBDM reported capability: control & sensing of RESET");
	if (caps & USBDM_CAP_PST)
		LOG_INFO("USBDM reported capability: supports PST signal sensing");
	if (caps & USBDM_CAP_CDC)
		LOG_INFO("USBDM reported capability: supports CDC Serial over USB interface");
	if (caps & USBDM_CAP_ARM_SWD)
		LOG_INFO("USBDM reported capability: supports ARM targets via SWD");

	return ERROR_OK;
}

/**
 * Sets the target type(HCS12, ColdFire, etc.) for BDM adapter.
 * @param bdev BDM adapter
 * @param type Type of the target BDM adapter is connecte to
 * @returns ERROR_OK on success, negative error code on failure
 */
static int usbdm_set_target_type(struct usbdm *bdev, enum usbdm_target_types type)
{
	/*
	   Command:

	   +--------------------------+
	   |  0x01                    |  1
	   +--------------------------+
	   |  Target type             |  1
	   +--------------------------+
	*/

	const uint8_t command[] = {
		USBDM_CMD_SET_TARGET,
		(uint8_t) type
	};

	return usbdm_command_response_no_data(bdev->devh,
					      bdev->endpoint.in, bdev->endpoint.out,
					      command, sizeof(command));
}

/**
 * Manage the state of the controllable pins of BDM adapter.
 * @param bdev BDM adapter
 * @param mask pin mask representing desired pin state(See
 *         @usbdm_pin_control_masks for possible values)
 * @returns ERROR_OK on success, negative error code on failure
 */
static int usbdm_control_pins(struct usbdm *bdev, uint16_t mask)
{
	/*
	   Command:

	   +--------------------------+
	   |  0x08                    |  1
	   +--------------------------+
	   |  16-bit pin mask MSB     |  1
	   +--------------------------+
	   |  16-bit pin mask LSB     |  1
	   +--------------------------+

	   Result:

	   +--------------------------+
	   |  16-bit pin mask MSB     |  1
	   +--------------------------+
	   |  16-bit pin mask LSB     |  1
	   +--------------------------+
	 */

	const uint8_t command[] = {
		USBDM_CMD_CONTROL_PINS,
		(uint8_t) (mask >> 8),
		(uint8_t) (mask >> 0)
	};

	uint8_t res[2];
	return usbdm_command_response(bdev->devh,
				      bdev->endpoint.in,
				      bdev->endpoint.out,
				      command, sizeof(command),
				      res, sizeof(res));
}

/**
 * Generic implementation of the "read register" command used by more
 * specialized register functions.
 * @param bdev BDM adapter
 * @param cmd BDM read register command to use
 * @param address register address
 * @param value variable to store result in
 * @returns ERROR_OK on success negative error code on faliure
 */
static int usbdm_generic_read_register(struct usbdm *bdev,
				       enum usbdm_commands cmd,
				       uint16_t address, uint32_t *value)
{
	/*
	   Both "read CPU register" and "read BDM register"
	   commands use the same command frame:

	   +--------------------------+
	   |  Command byte            |  1
	   +--------------------------+
	   |  16-bit address MSB      |  1
	   +--------------------------+
	   |  16-bit address LSB      |  1
	   +--------------------------+

	   Result:

	   +--------------------------------------+
	   |  32-bit register value byte[0] (MSB) |  1
	   +--------------------------------------+
	   |  32-bit register value byte[1]       |  1
	   +--------------------------------------+
	   |  32-bit register value byte[2]       |  1
	   +--------------------------------------+
	   |  32-bit register value byte[3] (LSB) |  1
	   +--------------------------------------+
	 */

	const uint8_t command[] = {
		cmd,
		(uint8_t) (address >> 8),
		(uint8_t) address
	};
	uint8_t rxb[4];

	const int ret = usbdm_command_response(bdev->devh,
					       bdev->endpoint.in, bdev->endpoint.out,
					       command, sizeof(command),
					       rxb, sizeof(rxb));
	if (ret < 0)
		return ret;

	*value = rxb[0] << 24 |
		 rxb[1] << 16 |
		 rxb[2] <<  8 |
		 rxb[3];

	return ERROR_OK;
}

/**
 * Generic wrapper to implement individual CPU register reader
 * functions.
 * @param bdm pointer to a gneric BDM deivce
 * @param reg address of the register to read
 * @returns register value on success, negative error code on error
 */
static int_least32_t usbdm_read_xx(const struct bdm_driver *bdm, uint16_t reg)
{
	struct usbdm *bdev = (struct usbdm *)bdm;

	int ret;
	uint32_t value;

	ret = usbdm_generic_read_register(bdev, USBDM_CMD_READ_REG, reg, &value);
	if (ret != ERROR_OK)
		return ret;
	else
		return value & 0xFFFF;
}

static int_least32_t usbdm_read_pc(const struct bdm_driver *bdm)
{
	return usbdm_read_xx(bdm, USBDM_HCS12_REG_PC);
}

static int_least32_t usbdm_read_sp(const struct bdm_driver *bdm)
{
	return usbdm_read_xx(bdm, USBDM_HCS12_REG_SP);
}

static int_least32_t usbdm_read_d(const struct bdm_driver *bdm)
{
	return usbdm_read_xx(bdm, USBDM_HCS12_REG_D);
}

static int_least32_t usbdm_read_x(const struct bdm_driver *bdm)
{
	return usbdm_read_xx(bdm, USBDM_HCS12_REG_X);
}

static int_least32_t usbdm_read_y(const struct bdm_driver *bdm)
{
	return usbdm_read_xx(bdm, USBDM_HCS12_REG_Y);
}

/**
 * Read contents of the CPU memory(one byte) with BDM memory mapped
 * in(thus enabling access to BDM registers).
 * @param bdm pointer to a gnenric BDM deivce
 * @param address address to read the data form
 * @returns 8-bit memory contents, negative error code failure
 */
static int_least16_t usbdm_read_bd_byte(const struct bdm_driver *bdm, uint32_t address)
{
	uint32_t value;
	int ret;

	struct usbdm *bdev = (struct usbdm *)bdm;

	ret = usbdm_generic_read_register(bdev, USBDM_CMD_READ_DREG,
					  0xFFFF & address, &value);

	return (ret < 0) ? ret : (uint8_t) value;
}

/**
 * Generic implementation of the "write register" command used by more
 * specialized register functions.
 * @param bdev BDM adapter
 * @param cmd BDM read register command to use
 * @param address register address
 * @param value variable to write
 * @returns ERROR_OK on success negative error code on faliure
 */
static int usbdm_generic_write_register(struct usbdm *bdev,
					enum usbdm_commands cmd,
					uint16_t address, uint32_t value)
{
	/*
	   Both "write CPU register" and "write BDM register"
	   commands use the same command frame:

	   +--------------------------------------+
	   |  Command byte                        |  1
	   +--------------------------------------+
	   |  16-bit address MSB                  |  1
	   +--------------------------------------+
	   |  16-bit address LSB                  |  1
	   +--------------------------------------+
	   |  32-bit register value byte[0] (MSB) |  1
	   +--------------------------------------+
	   |  32-bit register value byte[1]       |  1
	   +--------------------------------------+
	   |  32-bit register value byte[2]       |  1
	   +--------------------------------------+
	   |  32-bit register value byte[3] (LSB) |  1
	   +--------------------------------------+

	*/

	const uint8_t command[] = {
		cmd,

		(uint8_t) (address >> 8),
		(uint8_t) (address >> 0),

		(uint8_t) (value >> 24),
		(uint8_t) (value >> 16),
		(uint8_t) (value >>  8),
		(uint8_t) (value >>  0)
	};

	return usbdm_command_response_no_data(bdev->devh,
					      bdev->endpoint.in, bdev->endpoint.out,
					      command, sizeof(command));
}

static int usbdm_write_xx(const struct bdm_driver *bdm, uint16_t reg, uint16_t value)
{
	struct usbdm *bdev = (struct usbdm *)bdm;
	return usbdm_generic_write_register(bdev, USBDM_CMD_WRITE_REG, reg, value);
}

static int usbdm_write_pc(const struct bdm_driver *bdm, uint16_t value)
{
	return usbdm_write_xx(bdm, USBDM_HCS12_REG_PC, value);
}

static int usbdm_write_sp(const struct bdm_driver *bdm, uint16_t value)
{
	return usbdm_write_xx(bdm, USBDM_HCS12_REG_SP, value);
}

static int usbdm_write_d(const struct bdm_driver *bdm, uint16_t value)
{
	return usbdm_write_xx(bdm, USBDM_HCS12_REG_D, value);
}

static int usbdm_write_x(const struct bdm_driver *bdm, uint16_t value)
{
	return usbdm_write_xx(bdm, USBDM_HCS12_REG_X, value);
}

static int usbdm_write_y(const struct bdm_driver *bdm, uint16_t value)
{
	return usbdm_write_xx(bdm, USBDM_HCS12_REG_Y, value);
}

static int usbdm_write_bd_byte(const struct bdm_driver *bdm, uint32_t address, uint8_t value)
{
	struct usbdm *bdev = (struct usbdm *)bdm;

	return usbdm_generic_write_register(bdev, USBDM_CMD_WRITE_DREG,
					    0xFFFF & address, value);
}

/**
 * Read a chunk of data from CPU's memory(BDM is not mapped)
 * @param bdm generic BDM dongle
 * @param address address to read the data from
 * @param size read granularity size
 * @param count number of elements to read
 * @param buffer buffer to store data into
 * @returns ERROR_OK on success, negative error code on failure
 */
static int usbdm_read_memory(const struct bdm_driver *bdm, uint32_t address,
				 uint32_t size, uint32_t count, uint8_t *buffer)
{
	/*
	   Command:

	   +-----------------------------+
	   |  0x21                       |  1
	   +-----------------------------+
	   |  Access type(globabl/local) |  1
	   +-----------------------------+
	   |  Read size                  |  1
	   +-----------------------------+
	   |  Address byte[0] (MSB)      |  1
	   +-----------------------------+
	   |  Address byte[1]            |  1
	   +-----------------------------+
	   |  Address byte[2]            |  1
	   +-----------------------------+
	   |  Address byte[3] (LSB)      |  1
	   +-----------------------------+

	   Response:

	   +-----------------------------+
	   |                             |  1... up to 63
	   | //// DATA //////////////    |
	   |                             |
	   +-----------------------------+
	*/
	struct usbdm *bdev = (struct usbdm *)bdm;
	uint8_t access_type;

	/*
	   HACK: This is a hack to allow to access CPU memory using both
	   globabl and local addresses. Using this local addresses are "mapped"
	   to area from 0x80000000 to 0x8000FFFF
	 */
	if (address >= 0x80000000) {
		access_type = USBDM_MS_TYPE_NONE;
		address    &= 0xFFFF;
	} else {
		access_type = USBDM_MS_TYPE_GLOBAL;
	}

	size_t residue = count * size;

	do {
		const size_t data_transfer_size = MIN(residue,
						      USBDM_PACKET_MAX_DATA_SIZE);

		const uint8_t command[USBDM_CMD_READ_MEM_SIZE] = {
			USBDM_CMD_READ_MEM,
			access_type,
			data_transfer_size,
			(uint8_t) (address >> 24),
			(uint8_t) (address >> 16),
			(uint8_t) (address >>  8),
			(uint8_t) (address >>  0)
		};

		const int ret = usbdm_command_response(bdev->devh,
						       bdev->endpoint.in, bdev->endpoint.out,
						       command, sizeof(command),
						       buffer, data_transfer_size);
		if (ret < 0) {
			LOG_ERROR("Failed to read memory @ 0x%02x", address);
			return ERROR_FAIL;
		}

		residue -= data_transfer_size;
		buffer  += data_transfer_size;
		address += data_transfer_size;

	} while (residue);

	return ERROR_OK;
}

/**
 * Write a chunk of memory to CPU's memory
 * @param bdm abstract BDM adapter
 * @param address address to write data to
 * @param size write granularity
 * @param count number of elements to write
 * @param data buffer contining the data to write
 * @returns ERROR_OK on success, negative error code on failure
 */
static int usbdm_write_memory(const struct bdm_driver *bdm, uint32_t address,
				  uint32_t size, uint32_t count, const uint8_t *data)
{
	/*
	   Command:

	   +-----------------------------+
	   |  0x20                       |  1
	   +-----------------------------+
	   |  Access type(globabl/local) |  1
	   +-----------------------------+
	   |  Write size                 |  1
	   +-----------------------------+
	   |  Address byte[0] (MSB)      |  1
	   +-----------------------------+
	   |  Address byte[1]            |  1
	   +-----------------------------+
	   |  Address byte[2]            |  1
	   +-----------------------------+
	   |  Address byte[3] (LSB)      |  1
	   +-----------------------------+
	   |                             |  1... up to 119
	   | //// DATA //////////////    |
	   |                             |
	   +-----------------------------+
	*/

	struct usbdm *bdev = (struct usbdm *)bdm;

	uint8_t access_type;

	/*
	   HACK: This is a hack to allow to access CPU memory using both
	   globabl and local addresses. Using this local addresses are "mapped"
	   to area from 0x80000000 to 0x8000FFFF
	*/
	if (address >= 0x80000000) {
		access_type = USBDM_MS_TYPE_NONE;
		address    &= 0xFFFF;
	} else {
		access_type = USBDM_MS_TYPE_GLOBAL;
	}

	size_t residue = count * size;

	do {
		const uint8_t data_transfer_size = MIN(residue,
						       USBDM_PACKET_CMD_TRANSFER_MAX_DATA_SIZE - USBDM_CMD_WRITE_MEM_SIZE);

		uint8_t command[data_transfer_size +
				USBDM_CMD_WRITE_MEM_SIZE];

		command[0] = USBDM_CMD_WRITE_MEM;
		command[1] = access_type;
		command[2] = data_transfer_size;
		command[3] = (uint8_t) (address >> 24);
		command[4] = (uint8_t) (address >> 16);
		command[5] = (uint8_t) (address >>  8);
		command[6] = (uint8_t) (address >>  0);

		memcpy(command + USBDM_CMD_WRITE_MEM_SIZE,
		       data, data_transfer_size);

		const int ret = usbdm_command_response_no_data(bdev->devh,
							       bdev->endpoint.in, bdev->endpoint.out,
							       command, sizeof(command));
		if (ret < 0) {
			LOG_ERROR("Failed to write memory @ 0x%02x", address);
			return ERROR_FAIL;
		}

		residue -= data_transfer_size;
		data    += data_transfer_size;
		address += data_transfer_size;
	} while (residue);

	return ERROR_OK;
}

/**
 * Generic wrapper function for all commands that send and recieve
 * only one byte.
 * @param bdev USBDM device
 * @param cmd command code to send
 * @returns ERROR_OK on success, negative error code on failure
 */
static int usbdm_target_command(struct usbdm *bdev, enum usbdm_commands cmd)
{
	/*
	   Command:

	   +--------------------------+
	   |  Command                 |  1
	   +--------------------------+
	*/

	const uint8_t command[] = {
		cmd
	};

	return usbdm_command_response_no_data(bdev->devh,
					      bdev->endpoint.in, bdev->endpoint.out,
					      command, sizeof(command));
}

static int usbdm_connect(struct usbdm *bdev)
{
	return usbdm_target_command(bdev, USBDM_CMD_CONNECT);
}

static int usbdm_target_halt(const struct bdm_driver *bdm)
{
	struct usbdm *bdev = (struct usbdm *)bdm;
	return usbdm_target_command(bdev, USBDM_CMD_TARGET_HALT);
}

static int usbdm_target_go(const struct bdm_driver *bdm)
{
	struct usbdm *bdev = (struct usbdm *)bdm;
	return usbdm_target_command(bdev, USBDM_CMD_TARGET_GO);
}

static int usbdm_target_trace1(const struct bdm_driver *bdm)
{
	struct usbdm *bdev = (struct usbdm *)bdm;
	return usbdm_target_command(bdev, USBDM_CMD_TARGET_STEP);
}

static int usbdm_target_assert_reset(const struct bdm_driver *bdm)
{
	struct usbdm *bdev = (struct usbdm *)bdm;
	return usbdm_control_pins(bdev, USBDM_PIN_RESET_LOW);
}

static int usbdm_target_deassert_reset(const struct bdm_driver *bdm)
{
	struct usbdm *bdev = (struct usbdm *)bdm;
	return usbdm_control_pins(bdev, USBDM_PIN_RESET_3STATE);
}

static int usbdm_target_assert_bknd(const struct bdm_driver *bdm)
{
	struct usbdm *bdev = (struct usbdm *)bdm;
	return usbdm_control_pins(bdev, USBDM_PIN_BKGD_LOW);
}

static int usbdm_target_deassert_bknd(const struct bdm_driver *bdm)
{
	struct usbdm *bdev = (struct usbdm *)bdm;
	return usbdm_control_pins(bdev, USBDM_PIN_BKGD_3STATE);
}

extern struct jtag_interface usbdm_interface;

static int usbdm_interface_init(void)
{
	int err;

	const uint16_t bdm_vids[] = { 0x16D0, 0 };
	const uint16_t bdm_pids[] = { 0x0567, 0 };


	dongle = calloc(1, sizeof(*dongle));

	err = jtag_libusb_open(bdm_vids, bdm_pids, &dongle->devh);
	if (err < 0) {
		LOG_ERROR("Failed to open USBDM device");
		goto free_dongle;
	}

	err = jtag_libusb_claim_interface(dongle->devh, 0);
	if (err < 0) {
		LOG_ERROR("Failed to claim interface 0 of USBDM device");
		goto free_dongle;
	}

	err = jtag_libusb_get_endpoints(jtag_libusb_get_device(dongle->devh),
					&dongle->endpoint.in,
					&dongle->endpoint.out);
	if (err < 0) {
		LOG_ERROR("Couldn't find IN and OUT enpoints of USBDM device");
		goto free_dongle;
	}

	err = usbdm_get_version(dongle);
	if (err < 0) {
		LOG_ERROR("Failed to retreive version information from USBDM device");
		goto free_dongle;
	}

	/*
	   On rare occasions device can get stuck in a weired mode in
	   which it would enumerate and return version but fail to
	   return response to capabilities query. Issuing USB reset or
	   completely powerding down the device seem to be the only
	   cures for that.
	 */
	err = jtag_libusb_reset_device(dongle->devh);
	if (err < 0) {
		LOG_ERROR("Failed to reset USBDM device");
		goto free_dongle;
	}

	err = usbdm_get_capabilities(dongle);
	if (err < 0) {
		LOG_ERROR("Failed to retreive capabilities information from USBDM device");
		goto free_dongle;
	}

	err = usbdm_set_target_type(dongle, USBDM_TARGET_HC12);
	if (err < 0) {
		LOG_ERROR("Failed to set target CPU type for USBDM device");
		goto free_dongle;
	}

	err = usbdm_connect(dongle);
	if (err < 0) {
		LOG_ERROR("Failed to connect to USBDM device");
		goto free_dongle;
	}

	dongle->bdm.read_bd_byte	= usbdm_read_bd_byte;
	dongle->bdm.write_bd_byte	= usbdm_write_bd_byte;
	dongle->bdm.read_memory		= usbdm_read_memory;
	dongle->bdm.write_memory	= usbdm_write_memory;
	dongle->bdm.go			= usbdm_target_go;
	dongle->bdm.background		= usbdm_target_halt;
	dongle->bdm.trace1		= usbdm_target_trace1;
	dongle->bdm.assert_reset	= usbdm_target_assert_reset;
	dongle->bdm.deassert_reset	= usbdm_target_deassert_reset;
	dongle->bdm.assert_bknd		= usbdm_target_assert_bknd;
	dongle->bdm.deassert_bknd	= usbdm_target_deassert_bknd;
	dongle->bdm.read_pc		= usbdm_read_pc;
	dongle->bdm.write_pc		= usbdm_write_pc;
	dongle->bdm.read_sp		= usbdm_read_sp;
	dongle->bdm.write_sp		= usbdm_write_sp;
	dongle->bdm.read_d		= usbdm_read_d;
	dongle->bdm.write_d		= usbdm_write_d;
	dongle->bdm.read_x		= usbdm_read_x;
	dongle->bdm.write_x		= usbdm_write_x;
	dongle->bdm.read_y		= usbdm_read_y;
	dongle->bdm.write_y		= usbdm_write_y;

	usbdm_interface.driver.bdm = (const struct bdm_driver *)dongle;

	return ERROR_OK;

free_dongle:
	free(dongle);
	return err;
}


static int usbdm_interface_quit(void)
{
	return ERROR_OK;
}

static void usbdm_execute_reset(struct jtag_command *cmd)
{
	int ret;

	if (cmd->cmd.reset->srst)
		ret = usbdm_target_assert_reset((const struct bdm_driver *)dongle);
	else
		ret = usbdm_target_deassert_reset((const struct bdm_driver *)dongle);


	if (ret != ERROR_OK)
		LOG_ERROR("usbdm: Interface reset failed");
}

static void usbdm_execute_sleep(struct jtag_command *cmd)
{
	jtag_sleep(cmd->cmd.sleep->us);
}

static void usbdm_execute_command(struct jtag_command *cmd)
{
	switch (cmd->type) {
		case JTAG_RESET:
			usbdm_execute_reset(cmd);
			break;
		case JTAG_SLEEP:
			usbdm_execute_sleep(cmd);
			break;
		default:
			LOG_ERROR("BUG: unknown JTAG command type encountered");
			exit(-1);
	}
}

static int usbdm_interface_execute_queue(void)
{
	struct jtag_command *cmd = jtag_command_queue;

	while (cmd != NULL) {
		usbdm_execute_command(cmd);
		cmd = cmd->next;
	}

	return ERROR_OK;
}

COMMAND_HANDLER(usbdm_transport_jtag_command)
{
	return ERROR_OK;
}

static const struct command_registration usbdm_transport_jtag_subcommand_handlers[] = {
	{
		.name = "init",
		.mode = COMMAND_ANY,
		.handler = usbdm_transport_jtag_command,
		.usage = ""
	},
	{
		.name = "arp_init",
		.mode = COMMAND_ANY,
		.handler = usbdm_transport_jtag_command,
		.usage = ""
	},
	{
		.name = "arp_init-reset",
		.mode = COMMAND_ANY,
		.handler = usbdm_transport_jtag_command,
		.usage = ""
	},
	{
		.name = "tapisenabled",
		.mode = COMMAND_EXEC,
		.jim_handler = jim_jtag_tap_enabler,
	},
	{
		.name = "tapenable",
		.mode = COMMAND_EXEC,
		.jim_handler = jim_jtag_tap_enabler,
	},
	{
		.name = "tapdisable",
		.mode = COMMAND_EXEC,
		.handler = usbdm_transport_jtag_command,
		.usage = "",
	},
	{
		.name = "configure",
		.mode = COMMAND_EXEC,
		.handler = usbdm_transport_jtag_command,
		.usage = "",
	},
	{
		.name = "cget",
		.mode = COMMAND_EXEC,
		.jim_handler = jim_jtag_configure,
	},
	{
		.name = "names",
		.mode = COMMAND_ANY,
		.handler = usbdm_transport_jtag_command,
		.usage = "",
	},

	COMMAND_REGISTRATION_DONE
};

static const struct command_registration usbdm_interface_command_handlers[] = {
	{
		.name = "jtag",
		.mode = COMMAND_ANY,
		.usage = "",
		.chain = usbdm_transport_jtag_subcommand_handlers,
	 },

	COMMAND_REGISTRATION_DONE
};

const char *usbdm_transports[] = { "bdm", NULL };

struct jtag_interface usbdm_interface = {
	.name		= "usbdm",
	.supported	= 0,
	.transports     = usbdm_transports,
	.commands	= usbdm_interface_command_handlers,
	.init		= usbdm_interface_init,
	.quit		= usbdm_interface_quit,

	.execute_queue	= usbdm_interface_execute_queue,
};

