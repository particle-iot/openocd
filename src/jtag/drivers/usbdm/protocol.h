/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2014 by Andrey Smirnov                                  *
 *   andrew.smirnov@gmail.com                                              *
 *                                                                         *
 *   This files is based on the source code of the                         *
 *   'usbdm-eclipse-makefiles-build' projec availible here:                *
 *   https://github.com/podonoghue/usbdm-eclipse-makefiles-build.git       *
 *   Copyright (C) 2008  Peter O'Donoghue                                  *
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
#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

/**
 * @file Various numerical constants used by USBDM firmware,
 * see "usbdm-firmware/USBDM_JB16_V4_10/Sources/Commands.h" at
 * https://github.com/podonoghue/usbdm-firmware.git for more details
 */

enum usbdm_commands {
	/* Status of last/current command */
	USBDM_CMD_GET_COMMAND_RESPONSE  = 0,	/* 0x00 */
	/* Set target */
	USBDM_CMD_SET_TARGET            = 1,	/* 0x01 */
	/* Set target Vdd (immediate effect) */
	USBDM_CMD_SET_VDD               = 2,	/* 0x02 */
	/* Debugging commands (parameter determines actual command) */
	USBDM_CMD_DEBUG                 = 3,	/* 0x03 */
	/* Get BDM status (16-bit status value reflecting BDM status) */
	USBDM_CMD_GET_BDM_STATUS        = 4,	/* 0x04 */
	/* Get capabilities of USBDM */
	USBDM_CMD_GET_CAPABILITIES      = 5,	/* 0x05 */
	/* Set BDM options */
	USBDM_CMD_SET_OPTIONS           = 6,	/* 0x06 */

	/*
	  Commented out in the firmware source code

	  USBDM_CMD_GET_SETTINGS        = 7,   //!< Get BDM setting
	*/

	/* Directly control BDM interface levels */
	USBDM_CMD_CONTROL_PINS          = 8,	/* 0x08 */

	/*
	  Reserved 7 ... 11
	*/

	/* Sent to EP0. Get firmware version in BCD */
	USBDM_CMD_GET_VER               = 12,	/* 0x0C */


	/*
	   Reserved 13
	*/

	/* Sent to EP0. Requests reboot to ICP mode. */
	USBDM_CMD_ICP_BOOT              = 14,	/* 0x0E */
	/* Try to connect to the target */
	USBDM_CMD_CONNECT               = 15,	/* 0x0F */
	/* Sets-up the BDM interface for a new bit rate & tries to enable ackn feature */
	USBDM_CMD_SET_SPEED             = 16,	/* 0x10 */
	/* Read speed of the target */
	USBDM_CMD_GET_SPEED             = 17,	/* 0x11 */
	/* Directly control BDM interface levels */
	USBDM_CMD_CONTROL_INTERFACE     = 18,	/* 0x12 */

	/*
	  Reserved 19
	*/

	/* Get BDM status */
	USBDM_CMD_READ_STATUS_REG       = 20,	/* 0x14 */
	/* Write to target Control register */
	USBDM_CMD_WRITE_CONTROL_REG     = 21,	/* 0x15 */
	/* Reset target */
	USBDM_CMD_TARGET_RESET          = 22,	/* 0x16 */
	/* Perform single step */
	USBDM_CMD_TARGET_STEP           = 23,	/* 0x17 */
	/* Start code execution */
	USBDM_CMD_TARGET_GO             = 24,	/* 0x18 */
	/* Stop the CPU and bring it into background mode */
	USBDM_CMD_TARGET_HALT           = 25,	/* 0x19 */
	/* Write to target register */
	USBDM_CMD_WRITE_REG             = 26,	/* 0x1A */
	/* Read target register */
	USBDM_CMD_READ_REG              = 27,	/* 0x1B */

	/* Write target Core register */
	USBDM_CMD_WRITE_CREG            = 28,	/* 0x1C */
	/* Read from target Core register */
	USBDM_CMD_READ_CREG             = 29,	/* 0x1D */
	/* Write target Debug register */
	USBDM_CMD_WRITE_DREG            = 30,	/* 0x1E */
	/* Read from target Debug register */
	USBDM_CMD_READ_DREG             = 31,	/* 0x1F */
	/* Write to target memory */
	USBDM_CMD_WRITE_MEM             = 32,	/* 0x20 */
	/* Read from target memory */
	USBDM_CMD_READ_MEM              = 33,	/* 0x21 */

	/*
	  Commented out in the firmware source code

	  USBDM_CMD_TRIM_CLOCK            = 34,  //!< Trim target clock - deleted in V3.2
	  USBDM_CMD_RS08_FLASH_ENABLE     = 35,  //!< Enable target flash programming (Vpp on)
	  USBDM_CMD_RS08_FLASH_STATUS     = 36,  //!< Status of target flash programming
	  USBDM_CMD_RS08_FLASH_DISABLE    = 37,  //!< Stop target flash programming (Vpp off)
	*/

	/* Reset JTAG Tap controller */
	USBDM_CMD_JTAG_GOTORESET        = 38,	/* 0x26 */
	/* Move JTAG TAP controller to SHIFT-IR/DR */
	USBDM_CMD_JTAG_GOTOSHIFT        = 39,	/* 0x27 */
	/* Write to JTAG chain */
	USBDM_CMD_JTAG_WRITE            = 40,	/* 0x28 */
	/* Read from JTAG chain */
	USBDM_CMD_JTAG_READ             = 41,	/* 0x29 */
	/* Set VPP level */
	USBDM_CMD_SET_VPP               = 42,	/* 0x2A */
	/* Read & Write to JTAG chain (in-out buffer) */
	USBDM_CMD_JTAG_READ_WRITE       = 43,	/* 0x2B */
	/* Execute sequence of JTAG commands */
	USBDM_CMD_JTAG_EXECUTE_SEQUENCE = 44,	/* 0x2C */
};


enum usbdm_command_sizes {
	USBDM_CMD_WRITE_MEM_SIZE = 7,
	USBDM_CMD_READ_MEM_SIZE = 7,
};

/* For more description of the following code see source code of
 * usbdm_check_exit_status() subroutine in io.c */
enum usbdm_return_codes {
	USBDM_RC_OK			= 0,
	USBDM_RC_ILLEGAL_PARAMS		= 1,
	USBDM_RC_FAIL			= 2,
	USBDM_RC_BUSY			= 3,
	USBDM_RC_ILLEGAL_COMMAND	= 4,
	USBDM_RC_NO_CONNECTION		= 5,
	USBDM_RC_OVERRUN		= 6,
	USBDM_RC_CF_ILLEGAL_COMMAND	= 7,
	USBDM_RC_DEVICE_OPEN_FAILED	= 8,
	USBDM_RC_UNKNOWN_TARGET		= 15,
	USBDM_RC_NO_TX_ROUTINE		= 16,
	USBDM_RC_NO_RX_ROUTINE		= 17,
	USBDM_RC_BDM_EN_FAILED		= 18,
	USBDM_RC_RESET_TIMEOUT_FALL	= 19,
	USBDM_RC_BKGD_TIMEOUT		= 20,
	USBDM_RC_SYNC_TIMEOUT		= 21,
	USBDM_RC_UNKNOWN_SPEED		= 22,
	USBDM_RC_WRONG_PROGRAMMING_MODE	= 23,
	USBDM_RC_FLASH_PROGRAMING_BUSY	= 24,
	USBDM_RC_VDD_NOT_REMOVED	= 25,
	USBDM_RC_VDD_NOT_PRESENT	= 26,
	USBDM_RC_VDD_WRONG_MODE		= 27,
	USBDM_RC_CF_BUS_ERROR		= 28,
	USBDM_RC_USB_ERROR		= 29,
	USBDM_RC_ACK_TIMEOUT		= 30,
	USBDM_RC_FAILED_TRIM		= 31,
	USBDM_RC_FEATURE_NOT_SUPPORTED	= 32,
	USBDM_RC_RESET_TIMEOUT_RISE	= 33,
	USBDM_RC_JTAG_UNMATCHED_REPEAT	= 37,
	USBDM_RC_JTAG_UNMATCHED_RETURN	= 38,
	USBDM_RC_JTAG_UNMATCHED_IF	= 39,
	USBDM_RC_JTAG_STACK_ERROR	= 40,
};

/* Capabilities that the USBDM may report in response to the
 * USBDM_CMD_GET_CAPABILITIES command */
enum usbdm_hardware_capabilities {
	USBDM_CAP_NONE		= 0x0000,
	USBDM_CAP_HCS12		= 1 <<  0, /* HCS12 */
	USBDM_CAP_RS08		= 1 <<  1, /* 12V support */
	USBDM_CAP_VDDCONTROL	= 1 <<  2, /* control target VDD */
	USBDM_CAP_VDDSENSE	= 1 <<  3, /* sense target VDD */
	USBDM_CAP_CFVx		= 1 <<  4, /* ColdFire v1, 2, 3 */
	USBDM_CAP_HCS08		= 1 <<  5, /* inverted when querried */
	USBDM_CAP_CFV1		= 1 <<  6, /* inverted when querried */
	USBDM_CAP_JTAG		= 1 <<  7, /* JTAG support */
	USBDM_CAP_DSC		= 1 <<  8, /* DSC support */
	USBDM_CAP_ARM_JTAG	= 1 <<  9, /* ARM JTAG support */
	USBDM_CAP_RST		= 1 << 10, /* sense and control RST# */
	USBDM_CAP_PST		= 1 << 11, /* sense PST signal */
	USBDM_CAP_CDC		= 1 << 12, /* CDC serial port */
	USBDM_CAP_ARM_SWD	= 1 << 13, /* ARM SWD */
	USBDM_CAP_ALL		= 0xFFFF
};


enum usbdm_target_types {
	USBDM_TARGET_HC12      = 0,                  /* HC12 or HCS12 target */
	USBDM_TARGET_HCS12     = USBDM_TARGET_HC12,  /* HC12 or HCS12 target */

	/* Only two targets above are tested and supported */

	USBDM_TARGET_HCS08     = 1,       /* HCS08 target */
	USBDM_TARGET_RS08      = 2,       /* RS08 target */
	USBDM_TARGET_CFV1      = 3,       /* Coldfire Version 1 target */
	USBDM_TARGET_CFVx      = 4,       /* Coldfire Version 2,3,4 target */
	USBDM_TARGET_JTAG      = 5,       /* JTAG target - TAP is set to RUN-TEST/IDLE */
	USBDM_TARGET_EZFLASH   = 6,       /* EzPort Flash interface (SPI?) */
	USBDM_TARGET_MC56F80xx = 7,       /* JTAG with MC56F80xx optimised subroutines */
	USBDM_TARGET_ARM_JTAG  = 8,       /* ARM target using JTAG */
	USBDM_TARGET_ARM_SWD   = 9,       /* ARM target using SWD */
	USBDM_TARGET_ARM       = 10,      /* ARM using SWD (preferred) or JTAG as supported */
	USBDM_TARGET_OFF       = 0xFF,    /* Turn off interface (no target) */
};

enum usbdm_pin_control_masks {
	USBDM_PIN_BKGD_OFFS      = 0,
	/* Mask for BKGD values USBDM_PIN_BKGD_LOW,
	 * USBDM_PIN_BKGD_HIGH, USBDM_PIN_BKGD_3STATE */
	USBDM_PIN_BKGD           = 0b111 << USBDM_PIN_BKGD_OFFS,
	USBDM_PIN_BKGD_NC        = 0 << USBDM_PIN_BKGD_OFFS,  /* No change */
	USBDM_PIN_BKGD_3STATE    = 1 << USBDM_PIN_BKGD_OFFS,
	USBDM_PIN_BKGD_LOW       = 2 << USBDM_PIN_BKGD_OFFS,
	USBDM_PIN_BKGD_HIGH      = 3 << USBDM_PIN_BKGD_OFFS,

	USBDM_PIN_RESET_OFFS     = 2,
	USBDM_PIN_RESET          = 0b111 << USBDM_PIN_RESET_OFFS,
	USBDM_PIN_RESET_NC       = 0 << USBDM_PIN_RESET_OFFS,
	USBDM_PIN_RESET_3STATE   = 1 << USBDM_PIN_RESET_OFFS,
	USBDM_PIN_RESET_LOW      = 2 << USBDM_PIN_RESET_OFFS,

	USBDM_PIN_TA_OFFS        = 4,
	USBDM_PIN_TA             = 0b111 << USBDM_PIN_TA_OFFS,
	USBDM_PIN_TA_NC          = 0 << USBDM_PIN_TA_OFFS,
	USBDM_PIN_TA_3STATE      = 1 << USBDM_PIN_TA_OFFS,
	USBDM_PIN_TA_LOW         = 2 << USBDM_PIN_TA_OFFS,

	USBDM_PIN_DE_OFFS        = 4,
	USBDM_PIN_DE             = 0b111 << USBDM_PIN_DE_OFFS,
	USBDM_PIN_DE_NC          = 0 << USBDM_PIN_DE_OFFS,
	USBDM_PIN_DE_3STATE      = 1 << USBDM_PIN_DE_OFFS,
	USBDM_PIN_DE_LOW         = 2 << USBDM_PIN_DE_OFFS,

	USBDM_PIN_TRST_OFFS      = 6,
	USBDM_PIN_TRST           = 0b111 << USBDM_PIN_TRST_OFFS,
	USBDM_PIN_TRST_NC        = 0 << USBDM_PIN_TRST_OFFS,
	USBDM_PIN_TRST_3STATE    = 1 << USBDM_PIN_TRST_OFFS,
	USBDM_PIN_TRST_LOW       = 2 << USBDM_PIN_TRST_OFFS,

	USBDM_PIN_BKPT_OFFS      = 8,
	USBDM_PIN_BKPT           = 0b111 << USBDM_PIN_BKPT_OFFS,
	USBDM_PIN_BKPT_NC        = 0 << USBDM_PIN_BKPT_OFFS,
	USBDM_PIN_BKPT_3STATE    = 1 << USBDM_PIN_BKPT_OFFS,
	USBDM_PIN_BKPT_LOW       = 2 << USBDM_PIN_BKPT_OFFS,

	USBDM_PIN_SWD_OFFS       = 10,
	USBDM_PIN_SWD            = 0b111 << USBDM_PIN_SWD_OFFS,
	USBDM_PIN_SWD_NC         = 0 << USBDM_PIN_SWD_OFFS,
	USBDM_PIN_SWD_3STATE     = 1 << USBDM_PIN_SWD_OFFS,
	USBDM_PIN_SWD_LOW        = 2 << USBDM_PIN_SWD_OFFS,
	USBDM_PIN_SWD_HIGH       = 3 << USBDM_PIN_SWD_OFFS,

	USBDM_PIN_NOCHANGE       = 0,
	USBDM_PIN_RELEASE        = -1,
};

enum usbdm_memory_spaces {
	USBDM_MS_SPACE_TYPE_MASK	= 0b111 << 4,
	USBDM_MS_ACCESS_SIZE_MASK	= 0b111,
	USBDM_MS_TYPE_NONE		= 0 << 4,
	USBDM_MS_TYPE_PROGRAM		= 1 << 4,
	USBDM_MS_TYPE_DATA		= 2 << 4,
	USBDM_MS_TYPE_GLOBAL		= 3 << 4,
};

enum usbdm_register_numbers {
	USBDM_HCS12_REG_PC = 3,
	USBDM_HCS12_REG_D  = 4,
	USBDM_HCS12_REG_X  = 5,
	USBDM_HCS12_REG_Y  = 6,
	USBDM_HCS12_REG_SP = 7,
};

enum usbdm_transfer_sizes {
	/* Maximium endpoint size */
	USBDM_PACKET_MAX_SIZE = 64,
	/* One byte is always used for size */
	USBDM_PACKET_MAX_DATA_SIZE = USBDM_PACKET_MAX_SIZE - 1,
	/* We can send up to two packets constituting one command */
	USBDM_PACKET_CMD_TRANSFER_MAX_SIZE = 2 * USBDM_PACKET_MAX_SIZE,
	/* Both packets would have the first byte used for command
	 * protocol data */
	USBDM_PACKET_CMD_TRANSFER_MAX_DATA_SIZE = 2 * USBDM_PACKET_MAX_DATA_SIZE,
};

enum usbdm_hardware_configurations {
	/* USBDM   - Universal TBDML/OSBDM JB16 */
	USBDM_HW_USBDM                  = 1,
	/* TBDML   - Minimal JB16 version (JB16DWE,JB16JDWE) */
	USBDM_HW_TBDML                  = 2,
	/* No longer used */
	USBDM_HW_TBDMLSwin              = 3,
	/* OSBDM   - Basic OSBDM hardware */
	USBDM_HW_OSBDM                  = 4,
	/* WTBDM08 - Wiztronics BDMS08/12 */
	USBDM_HW_WTBDM                  = 5,
	/* OSBDM+E - OSBDM+Flash supply */
	USBDM_HW_OSBDME                 = 6,
	/* USBDM hardware using 9S08JM16/32/60CLD (44p package) */
	USBDM_HW_USBDM_JMxxCLD          = 7,
	/* USBDM hardware using 9S08JM16CLC (32p package) */
	USBDM_HW_USBDM_JMxxCLC          = 8,
	/* USBSPYDER - SofTec USBSPYDER08 - not functional */
	USBDM_HW_USBSPYDER              = 9,
	/* USBDM hardware using MC9S12UF32PBE (64p package) */
	USBDM_HW_USBDM_UF32PBE         = 10,
	/* USBDM hardware CF/DSC only using MC9S08JS16CWJ (20p SOIC package) */
	USBDM_HW_USBDM_CF_JS16CWJ      = 11,
	/* Combined USBDM/TBLCF using 9S08JM16/32/60CLD (44p package) */
	USBDM_HW_USBDM_CF_JMxxCLD      = 12,
	/* USBDM hardware using MC9S08JS16CWJ (20p SOIC package) */
	USBDM_HW_USBDM_JS16CWJ         = 13,
	/* MC56F8006DEMO Board (Axiom) */
	USBDM_HW_USBDM_MC56F8006DEMO   = 14,
	/* Reserved for USER created custom hardware */
	USBDM_HW_CUSTOM                = 15,
	/* USBDM hardware CF/DSC only using MC9S08JS16CWJ (20p SOIC package)
	   with serial interface */
	USBDM_HW_USBDM_CF_SER_JS16CWJ  = 16,
	/* USBDM hardware using MC9S08JS16CWJ (20p SOIC package) with Serial
	   interface */
	USBDM_HW_USBDM_SER_JS16CWJ     = 17,
	/* Combined USBDM/TBLCF/Serial using 9S08JM16/32/60CLD (44p package) */
	USBDM_HW_USBDM_CF_SER_JMxxCLD  = 18,
	/* TWR Kinetis boards */
	USBDM_HW_USBDM_TWR_KINETIS     = 19,
	/* TWR Coldfire V1 boards */
	USBDM_HW_USBDM_TWR_CFV1        = 20,
	/* TWR HCS08 boards */
	USBDM_HW_USBDM_TWR_HCS08       = 21,
	/* TWR Coldfire Vx boards */
	USBDM_HW_USBDM_TWR_CFVx        = 22,
	/* USBDM MC9S08JS16CWJ with BDM, SWD & Serial interfaces */
	USBDM_HW_USBDM_SWD_SER_JS16CWJ = 23,
	/* USBDM MC9S08JS16CWJ with BDM & SWD interfaces */
	USBDM_HW_USBDM_SWD_JS16CWJ     = 24,
	/* Freescale FRDM-KL25 board (MK20 chip) */
	USBDM_HW_USBDM_FREEDOM         = 25,
	/* Experimental MKL25Z */
	USBDM_HW_USBDM_MKL25Z          = 26,
	/* Experimental MK20DX5 */
	USBDM_HW_USBDM_MK20D5          = 27,
	/* TWR HCS12 boards */
	USBDM_HW_USBDM_TWR_HCS12       = 28,
};

enum usbdm_hardware_version_informations {
	USBMD_HW_VI_JB  = 0x00,
	USBMD_HW_VI_JM  = 0x80,
	USBMD_HW_VI_UF  = 0xC0,
	USBMD_HW_VI_ARM = 0x40,
};


#endif	/* __PROTOCOL_H__ */
