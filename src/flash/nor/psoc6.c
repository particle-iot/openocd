/*******************************************************************************
 *   Copyright (C) 2017 by Yuriy Vynnychek (PSoC 6 support derived from PSoC 4)*
 *   Yuriy.Vynnychek.cypress.com                                               *
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
 *                                                                             *
 *   You should have received a copy of the GNU General Public License         *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.     *
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <jtag/jtag.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

/*	device documets:
	PSoC(R) 6: PSoC CY8C6XXX Family Datasheet
	Document Number:

	PSoC CY8C6XXX Family PSoC(R) 6 Architecture TRM
	Document No. 002-15785 Rev. ** xx/xx/2016

	CY8C6XXX PSOC(R) 6 BLE 2 REGISTERS TECHNICAL REFERENCE MANUAL (TRM)
	Document No. 002-12544 Rev. ** 05/09/2016

	CY8C6XXX Programming Specifications
	Document No.*/
/*--------------------------------------------------------------------------------------------
 *Base addresses
 *--------------------------------------------------------------------------------------------
 */
/*	256kB System RAM */
#define MEM_BASE_SRAM0							0x08000000u
/*	1024kB FLASH Main Region */
#define MEM_BASE_FLASH							0x10000000u
/*	Peripheral Interconnect */
#define MEM_BASE_MMIO							0x40000000u
/*	0x40200000: Core platform peripherals */
#define MEM_BASE_MMIO2							(MEM_BASE_MMIO + 0x200000u)
/*	0x40230000: Base address for IPC structs */
#define MEM_BASE_IPC							(MEM_BASE_MMIO2 + 0x30000u)
/*	0x40231000: Base address for IPC_INTR struct */
#define MEM_BASE_IPCINTR						(MEM_BASE_MMIO2 + 0x31000u)

#define PSOC6_CHIP_PROT_UNKNOWN					0x0u
#define PSOC6_CHIP_PROT_VIRGIN					0x1u
#define PSOC6_CHIP_PROT_NORMAL					0x2u
#define PSOC6_CHIP_PROT_SECURE					0x3u
#define PSOC6_CHIP_PROT_DEAD					0x4u

/*	Addresses for IPC_STRUCT and IPC_INTR_STRUCT */
#define IPC_INTR_STRUCT_SIZE					0x20u
#define IPC_STRUCT_SIZE							0x20u
/*	0x40230000: CM0+ IPC_STRUCT absolute address */
#define IPC_STRUCT0								MEM_BASE_IPC
/*	0x40230020: CM4 IPC_STRUCT absolute address */
#define IPC_STRUCT1								(IPC_STRUCT0 + IPC_STRUCT_SIZE)
/*	0x40230040: DAP IPC_STRUCT absolute address */
#define IPC_STRUCT2								(IPC_STRUCT1 + IPC_STRUCT_SIZE)
/*	0x40231000: IPC_INTR struct absolute address */
#define IPC_INTR_STRUCT							MEM_BASE_IPCINTR

#define FLASH_SECTOR_LENGTH						256u
#define PSOC6_SPCIF_GEOMETRY					(MEM_BASE_MMIO2+0x5f00cu)

/*	Registers offsets in IPC_STRUCT[x]
 *	This register is used to acquire a lock. This register is NOT SW writable.*/
#define IPC_STRUCT_ACQUIRE_OFFSET				0x00u
/*	This field allows for the generation of notification events to the IPC interrupt structures. */
#define IPC_STRUCT_NOTIFY_OFFSET				0x08u
/*	This field holds a 32-bit data element that is associated with the IPC structure. */
#define IPC_STRUCT_DATA_OFFSET					0x0Cu
/*	IPC lock status */
#define IPC_STRUCT_LOCK_STATUS_OFFSET			0x10u

/*	Registers offsets in IPC_INTR_STRUCT
 *	IPC interrupt mask */
#define IPC_INTR_STRUCT_INTR_IPC_MASK_OFFSET	0x08u
/*	Specifies if the lock is successfully acquired or not: '0': Not successfully acquired, '1': Successfully acquired.*/
#define IPC_STRUCT_ACQUIRE_SUCCESS_MSK			0x80000000u
/*	Specifies if the lock is acquired. */
#define IPC_STRUCT_LOCK_STATUS_ACQUIRED_MSK		0x80000000u

/*	Misc
 *	Timeout attempts of IPC_STRUCT acuire*/
#define IPC_STRUCT_ACQUIRE_TIMEOUT_ATTEMPTS		250u
/*	Timeout attempts of IPC_STRUCT data */
#define IPC_STRUCT_DATA_TIMEOUT_ATTEMPTS		250u
/*	0x08001000: Address of SRAM where the API’s parameters are stored by SW.*/
#define SRAM_SCRATCH_ADDR						(MEM_BASE_SRAM0 + 0x00001000u)
#define ROW_SIZE								512u
/*	Timemout 10 ms */
#define DELAY_10_MS								10000u

/*--------------------------------------------------------------------------------------------
 *SROM APIs
 *--------------------------------------------------------------------------------------------
 *SROM APIs masks
 *	[0]: 1 - arguments are passed in IPC.DATA. 0 - arguments are passed in SRAM*/
#define MXS40_SROMAPI_DATA_LOCATION_MSK			0x00000001u
/*	Status Code: 4 bits [31:28] of the data register */
#define MXS40_SROMAPI_STATUS_MSK				0xF0000000u
/*	Status Code = 0xA*/
#define MXS40_SROMAPI_STAT_SUCCESS				0xA0000000u

/*	Sys calls IDs (SROM API Op code)
 *	[31:24]: Opcode = 0x00; [0]: 1 - arguments are passed in IPC.DATA*/
#define MXS40_SROMAPI_SILID_CODE				0x00000001u
/*	[15:8]: ID type*/
#define MXS40_SROMAPI_SILID_TYPE_MSK			0x0000FF00u
#define MXS40_SROMAPI_SILID_TYPE_ROL			0x08u
/*	[15:8]: Family Id Hi*/
#define MXS40_SROMAPI_SILID_FAMID_HI_MSK		0x0000FF00u
#define MXS40_SROMAPI_SILID_FAMID_HI_ROR		0x08u
/*	[7:0]: Family Id Lo*/
#define MXS40_SROMAPI_SILID_FAMID_LO_MSK		0x000000FFu
#define MXS40_SROMAPI_SILID_FAMID_LO_ROR		0u
/*	[19:16]: Protection state*/
#define MXS40_SROMAPI_SILID_PROT_MSK			0x000F0000u
#define MXS40_SROMAPI_SILID_PROT_ROR			0x10u
/*	[15:8]: Silicon Id Hi*/
#define MXS40_SROMAPI_SILID_SILID_HI_MSK		0x0000FF00u
#define MXS40_SROMAPI_SILID_SILID_HI_ROR		0x08u
/*	[15:8]: Silicon Id Lo*/
#define MXS40_SROMAPI_SILID_SILID_LO_MSK		0x000000FFu
#define MXS40_SROMAPI_SILID_SILID_LO_ROR		0x00u
/*	[31:24]: Opcode = 0x06; [0]: 0 - arguments are passed in SRAM*/
#define MXS40_SROMAPI_PROGRAMROW_CODE			0x06000100u
/*	[31:24]: Opcode = 0x14; [0]: 0 - arguments are passed in SRAM*/
#define MXS40_SROMAPI_ERASESECTOR_CODE			0x14000100u
/*	[31:24]: Opcode = 0x1C; [0]: 0 - arguments are passed in SRAM*/
#define MXS40_SROMAPI_ERASEROW_CODE				0x1C000100u
#define IPC_ID									2u
#define LENGHT_SILICON_ID						16u
#define SIZE_OF_STRING							32u

/*Offset for data location/size and Integrity check*/
#define DATA_LOCATION_OFFSET					0x04

/*Property for data location/size and Integrity check*/
#define DATA_LOCATION_PROPERTY					0x106

/*Offset for flash address which will be programed*/
#define FLASH_ADDRESS_OFFSET					0x08

/*Offset for first data byte in SRAM*/
#define DATA_OFFSET								0x10

/*Offset for set pointer to the first data byte location*/
#define POINTER_ON_FIRST_BYTE_LOCATION_OFFSET	0x0C

struct Psoc6ChipDetails {
	uint32_t id;
	const char *type;
	uint32_t flashSizeInKb;
};

/* list of PSoC 6 chips
 * flashSizeInKb is not necessary as it can be decoded from SPCIF_GEOMETRY*/
const struct Psoc6ChipDetails psoc6Devices[] = {
	/* PSoC 6 BLE II */
	{ 0xE2071100, "CY8C616FMI-BL603", .flashSizeInKb = 512 },
	{ 0xE2081100, "CY8C616FMI-BL673", .flashSizeInKb = 512 },
	{ 0xE2091100, "CY8C616LQI-BL601", .flashSizeInKb = 512 },
	{ 0xE20A1100, "CY8C616LQI-BL671", .flashSizeInKb = 512 },
	{ 0xE20B1100, "CY8C617FMI-BL603", .flashSizeInKb = 1024 },
	{ 0xE20C1100, "CY8C617FMI-BLD73", .flashSizeInKb = 1024 },
	{ 0xE20D1100, "CY8C626FMI-BL603", .flashSizeInKb = 512 },
	{ 0xE20E1100, "CY8C626BZI-BL604", .flashSizeInKb = 512 },
	{ 0xE20F1100, "CY8C626BZI-BL674", .flashSizeInKb = 512 },
	{ 0xE2111100, "CY8C627BZI-BL604", .flashSizeInKb = 1024 },
	{ 0xE2121100, "CY8C627FMI-BLD73", .flashSizeInKb = 1024 },
	{ 0xE2131100, "CY8C627BZI-BLD74", .flashSizeInKb = 1024 },
	{ 0xE2141100, "CY8C636BZI-BL604", .flashSizeInKb = 512 },
	{ 0xE2151100, "CY8C636BZI-BL674", .flashSizeInKb = 512 },
	{ 0xE2161100, "CY8C636FMI-BL603", .flashSizeInKb = 512 },
	{ 0xE2171100, "CY8C636FMI-BL673", .flashSizeInKb = 512 },
	{ 0xE2181100, "CY8C636LQI-BL601", .flashSizeInKb = 512 },
	{ 0xE2191100, "CY8C636LQI-BL671", .flashSizeInKb = 512 },
	{ 0xE21A1100, "CY8C637BZI-BLD04", .flashSizeInKb = 1024 },
	{ 0xE2011100, "CY8C637BZI-BLD74", .flashSizeInKb = 1024 },
	{ 0xE21C1100, "CY8C637FMI-BLD03", .flashSizeInKb = 1024 },
	{ 0xE2021100, "CY8C637FMI-BLD73", .flashSizeInKb = 1024 },
	{ 0xE21E1100, "CY8C637LQI-BLD01", .flashSizeInKb = 1024 },
	{ 0xE2031100, "CY8C637LQI-BLD71", .flashSizeInKb = 1024 },
	{ 0xE2041100, "CY8C68237FM-BLE", .flashSizeInKb = 1024 },
	{ 0xE2051100, "CY8C68237BZ-BLE", .flashSizeInKb = 1024 },

	/* PSoC 6 M */
	{ 0xE2001100, "CY8C637BZI-MD76", .flashSizeInKb = 1024 },
	{ 0xE2201100, "CY8C616BZI-M606", .flashSizeInKb = 512 },
	{ 0xE2211100, "CY8C616BZI-M676", .flashSizeInKb = 512 },
	{ 0xE2221100, "CY8C617BZI-MD76", .flashSizeInKb = 1024 },
	{ 0xE2231100, "CY8C626BZI-M606", .flashSizeInKb = 512 },
	{ 0xE2241100, "CY8C627BZI-MD76", .flashSizeInKb = 1024 },
	{ 0xE2251100, "CY8C636BZI-MD06", .flashSizeInKb = 512 },
	{ 0xE2261100, "CY8C636BZI-MD76", .flashSizeInKb = 512 },
	{ 0xE2271100, "CY8C637BZI-MD06", .flashSizeInKb = 1024 },
};

struct psoc6FlashBank {
	uint32_t rowSize;
	uint32_t userBankSize;
	int probed;
	uint32_t siliconId;
	uint8_t chipProtection;
	uint32_t flashSizeInKb;
};

static const struct Psoc6ChipDetails *psoc6_details_by_id(uint32_t siliconId)
{
	const struct Psoc6ChipDetails *p = psoc6Devices;
	const struct Psoc6ChipDetails *chipInfo;
	uint16_t i;
	uint16_t id = siliconId >> LENGHT_SILICON_ID; /* ignore die revision */
	for (i = 0; i < sizeof(psoc6Devices)/sizeof(psoc6Devices[0]); i++) {
		if (p->id == id)
			chipInfo = p;
		p++;
	}
	LOG_INFO("Unknown PSoC 6 device silicon id 0x%08" PRIx32 ".", siliconId);
	return chipInfo;
}

static const char *psoc6_decode_chipProtection(uint8_t protection)
{
	char *protectType = calloc(SIZE_OF_STRING, sizeof(*protectType));
	switch (protection) {
	case PSOC6_CHIP_PROT_UNKNOWN:
		strcpy(protectType, "protection UNKNOWN");
		break;
	case PSOC6_CHIP_PROT_VIRGIN:
		strcpy(protectType, "protection VIRGIN");
		break;
	case PSOC6_CHIP_PROT_NORMAL:
		strcpy(protectType, "protection NORMAL");
		break;
	case PSOC6_CHIP_PROT_SECURE:
		strcpy(protectType, "protection SECURE");
		break;
	case PSOC6_CHIP_PROT_DEAD:
		strcpy(protectType, "protection DEAD");
		break;
	default:
		LOG_WARNING("Unknown protection state 0x%02" PRIx8 "", protection);
		strcpy(protectType, "Not allowed");
		break;
	}

	return protectType;
}

FLASH_BANK_COMMAND_HANDLER(psoc6_flash_bank_command)
{
	struct psoc6FlashBank *psoc6_info;
	int hr = ERROR_OK;

	if (CMD_ARGC < 6) {
		hr = ERROR_COMMAND_SYNTAX_ERROR;
	} else {
		psoc6_info = calloc(1, sizeof(struct psoc6FlashBank));
		bank->driver_priv = psoc6_info;
		psoc6_info->userBankSize = bank->size;
	}
	return hr;
}


/*------------------------------------------------------------------------------
 SROM APIs basics
--------------------------------------------------------------------------------
******************************************************************************
*	Purpose :	Polls lock status of IPC structure
*	Parameter :
*			target - current target device
*			ipcId - Id of IPC structure
*								- 0: IPC_STRUCT0 (CM0+)
*								- 1: IPC_STRUCT1 (CM4)
*								- 2: IPC_STRUCT2 (DAP)
*			lockExpected - true if look state is expected, or false  if look state is not expected
*			timeOutAttempts - timeout
* Return :
*		ERROR_OK : IPC structure locked successfully
*		ERROR_FAIL : Cannot lock IPC structure
*******************************************************************************/
int Ipc_PollLockStatus(struct target *target, uint32_t ipcId, bool lockExpected, int timeOutAttempts)
{
	/* Poll lock status*/
	int hr = ERROR_OK;
	int attemptsElapsed = 0x00;
	bool isExpectedStatus = false;
	uint32_t readData;
	uint32_t ipcAddr = IPC_STRUCT0 + IPC_STRUCT_SIZE * ipcId;
	do {
		/* Check lock status*/
		hr = target_read_u32(target, ipcAddr + IPC_STRUCT_LOCK_STATUS_OFFSET, &readData);
		if (hr == ERROR_OK) {
			bool isLocked = (readData & IPC_STRUCT_LOCK_STATUS_ACQUIRED_MSK) != 0;
			isExpectedStatus = (lockExpected && isLocked) || (!lockExpected && !isLocked);
		}
		/* Check for timeout*/
		if (!isExpectedStatus) {
			if (attemptsElapsed > timeOutAttempts) {
				LOG_ERROR("Timeout polling lock status of IPC_STRUCT");
				hr = ERROR_FAIL;
				break;
			}
			usleep(DELAY_10_MS);
			attemptsElapsed++;
		}
	} while (!isExpectedStatus);
	return hr;
}


/*******************************************************************************
*	Purpose :	Acquires MXS40 IPC structure
*	Parameter :
*			target - current target device
*			ipcId - Id of IPC structure
*								- 0: IPC_STRUCT0 (CM0+)
*								- 1: IPC_STRUCT1 (CM4)
*								- 2: IPC_STRUCT2 (DAP)
*			timeOutAttempts - timeout
*	Return :
*		ERROR_OK : IPC structure acquired successfully
*		ERROR_FAIL : Cannon acquire IPC structure
*******************************************************************************/
int Ipc_Acquire(struct target *target, char ipcId, int timeOutAttempts)
{
	int hr = ERROR_OK;
	int attemptsElapsed = 0x00;
	bool isAcquired = false;
	uint32_t readData;
	uint32_t ipcAddr = IPC_STRUCT0 + IPC_STRUCT_SIZE * ipcId;

	do {
		/* Acquire the lock in DAP IPC struct (IPC_STRUCT.ACQUIRE).*/
		hr = target_write_u32(target, ipcAddr + IPC_STRUCT_ACQUIRE_OFFSET, IPC_STRUCT_ACQUIRE_SUCCESS_MSK);
		if (hr == ERROR_OK) {
			/* Check if data is writed on first step */
			hr = target_read_u32(target, ipcAddr + IPC_STRUCT_ACQUIRE_OFFSET, &readData);
			if (hr == ERROR_OK)
				isAcquired = (readData & IPC_STRUCT_ACQUIRE_SUCCESS_MSK) != 0;
		}
		/* Check for timeout */
		if (!isAcquired) {
			if (attemptsElapsed > timeOutAttempts) {
				LOG_ERROR("Timeout acquiring IPC_STRUCT");
				hr = ERROR_FAIL;
				break;
			}
			usleep(DELAY_10_MS);
			attemptsElapsed++;
		}
	} while (!isAcquired);

	if (isAcquired) {
		/* If IPC structure is acquired, the lock status should be set */
		hr = Ipc_PollLockStatus(target, ipcId, true, timeOutAttempts);
	}
	return hr;
}


/*******************************************************************************
* Purpose :	Polls execution status of SROM API
* Parameter :
*			target - current target device
*			address - Memory address of SROM API status word
*			timeOutAttempts - timeout
*			dataOut - status word
* Return :
*			ERROR_OK : SROM API returned successful execution status
*			ERROR_FAIL : SROM API execution failed
*******************************************************************************/
int PollSromApiStatus(struct target *target, int address, int timeOutAttempts, uint32_t *dataOut)
{
	/* Poll data from SRAM, returned after system call execution */
	int hr = ERROR_OK;
	int attemptsElapsed = 0x00;
	bool isAcquired = false;

	do {
		/* Poll data */
		hr = target_read_u32(target, address, dataOut);
		if (hr == ERROR_OK)
			isAcquired = (*dataOut & MXS40_SROMAPI_STATUS_MSK) == MXS40_SROMAPI_STAT_SUCCESS;

		/* Check for timeout */
		if (!isAcquired) {
			if (attemptsElapsed > timeOutAttempts) {
				LOG_DEBUG("PollSromApiStatus - FAIL status - 0x%08x", (unsigned int)*dataOut);
				LOG_ERROR("Timeout waiting for SROM API execution complete");
				hr = ERROR_FAIL;
				break;
			}
			usleep(DELAY_10_MS);
			attemptsElapsed++;
		}
	} while (!isAcquired);

	LOG_DEBUG("PollSromApiStatus - OK status - 0x%08x", (unsigned int)*dataOut);
	return hr;
}


/*******************************************************************************
*	Purpose :
*						Calls SROM API
*						SROM APIs are executed by invoking a system call & providing
*						the corresponding arguments.
*						System calls can be performed by CM0+, CM4 or DAP.
*						Each of them have a reserved IPC structure (used as a mailbox) through which
*						they can request CM0+ to perform a system call.
*						Each one acquires the specific mailbox, writes the opcode and
*						argument to the data field of the mailbox and notifies a dedicated
*						IPC interrupt structure. This results in an NMI interrupt in M0+.
*	Parameter :
*			target - current target device
*			callIdAndParams - OpCode of SROM API and params (in case all params are in IPC structure)
*			dataOut - status word
* Return :
*			ERROR_OK : SROM API returned successful execution status
*			ERROR_FAIL : SROM API execution failed
*******************************************************************************/
int CallSromApi(struct target *target, uint32_t callIdAndParams, uint32_t *dataOut)
{
	int hr;
	/* Check where the arguments for this API are located
		[0]: 1 - arguments are passed in IPC.DATA. 0 - arguments are passed in SRAM */
	bool isDataInRam = (callIdAndParams & MXS40_SROMAPI_DATA_LOCATION_MSK) == 0;
	unsigned long IPC_STRUC = IPC_STRUCT2;
	/* Acquire IPC_STRUCT[0] for CM0+ */
	hr = Ipc_Acquire(target, IPC_ID, IPC_STRUCT_ACQUIRE_TIMEOUT_ATTEMPTS);
	if (hr == ERROR_OK) {
		/* Write to IPC_STRUCT0.DATA - Sys call ID and Parameters
			OR address in SRAM, where they are located */
		if (isDataInRam) {
			LOG_DEBUG("CallSromApi: isDataInRam = true: address -> 0x%x, data -> 0x%x", (unsigned int)(IPC_STRUC + IPC_STRUCT_DATA_OFFSET), SRAM_SCRATCH_ADDR);
			hr = target_write_u32(target, (unsigned int)(IPC_STRUC + IPC_STRUCT_DATA_OFFSET), SRAM_SCRATCH_ADDR);
		} else {
			LOG_DEBUG("CallSromApi: isDataInRam = false: address -> 0x%x, data -> 0x%x", (unsigned int)(IPC_STRUC + IPC_STRUCT_DATA_OFFSET), callIdAndParams);
			hr = target_write_u32(target, (unsigned int)(IPC_STRUC + IPC_STRUCT_DATA_OFFSET), callIdAndParams);
		}
		if (hr == ERROR_OK) {
			/* Enable notification interrupt of IPC_INTR_STRUCT0(CM0+) for IPC_STRUCT2 */
			hr = target_write_u32(target, (IPC_INTR_STRUCT + IPC_INTR_STRUCT_INTR_IPC_MASK_OFFSET), 1 << (16 + IPC_ID));
			if (hr == ERROR_OK) {
				/* Notify to IPC_INTR_STRUCT0. IPC_STRUCT2.MASK <- Notify */
				hr = target_write_u32(target, IPC_STRUC + IPC_STRUCT_NOTIFY_OFFSET, 1);
				if (hr == ERROR_OK) {
					/* Poll lock status */
					hr = Ipc_PollLockStatus(target, IPC_ID, false, IPC_STRUCT_ACQUIRE_TIMEOUT_ATTEMPTS);
					if (hr == ERROR_OK) {
						/* Poll Data byte */
						if (isDataInRam)
							hr = PollSromApiStatus(target, SRAM_SCRATCH_ADDR, IPC_STRUCT_DATA_TIMEOUT_ATTEMPTS, dataOut);
						else
							hr = PollSromApiStatus(target, IPC_STRUC + IPC_STRUCT_DATA_OFFSET, IPC_STRUCT_DATA_TIMEOUT_ATTEMPTS, dataOut);
					}
				}
			}
		}

	}
	return hr;
}


/*******************************************************************************
*	Purpose :	Get Silicon ID for connected target
*	Parameter :
*			target - current target device
*			siliconId - value for Silicon ID
*			protection - value for protected state
*	Return :
*		ERROR_OK : Resault of get silicon id operation is OK
*		ERROR_FAIL : Resault of get silicon id operation is FAIL
*******************************************************************************/
static int Psoc6GetSiliconId(struct target *target, uint32_t *siliconId, uint8_t *protection)
{
	int hr = 0;
	uint32_t dataOut0, dataOut1;
	uint32_t params;
	uint32_t familyIdHi = 0x0;
	uint32_t familyIdLo = 0x0;
	uint32_t siliconIdHi = 0x0;
	uint32_t siliconIdLo = 0x0;

	/* Type 0: Get Family ID & Revision ID
		SRAM_SCRATCH: OpCode */
	params = MXS40_SROMAPI_SILID_CODE + (MXS40_SROMAPI_SILID_TYPE_MSK & (0 << MXS40_SROMAPI_SILID_TYPE_ROL));
	hr = CallSromApi(target, params, &dataOut0);
	if (hr == ERROR_OK) {
		/* Type 1: Get Silicon ID and protection state */
		params = (MXS40_SROMAPI_SILID_CODE + (MXS40_SROMAPI_SILID_TYPE_MSK & (1 << MXS40_SROMAPI_SILID_TYPE_ROL)));
		hr = CallSromApi(target, params, &dataOut1);
		if (hr == ERROR_OK) {
			familyIdHi = (dataOut0 &  MXS40_SROMAPI_SILID_FAMID_HI_MSK) >> MXS40_SROMAPI_SILID_FAMID_HI_ROR;	/* Family ID High */
			familyIdLo = (dataOut0 &  MXS40_SROMAPI_SILID_FAMID_LO_MSK) >> MXS40_SROMAPI_SILID_FAMID_LO_ROR;	/* Family ID Low */
			siliconIdHi = (dataOut1 &  MXS40_SROMAPI_SILID_SILID_HI_MSK) >> MXS40_SROMAPI_SILID_SILID_HI_ROR;	/* Silicon ID High */
			siliconIdLo = (dataOut1 &  MXS40_SROMAPI_SILID_SILID_LO_MSK) >> MXS40_SROMAPI_SILID_SILID_LO_ROR;	/* Silicon ID Low */
			*protection = (dataOut1 &  MXS40_SROMAPI_SILID_PROT_MSK) >> MXS40_SROMAPI_SILID_PROT_ROR;			/* Protection state */
		} else {
			LOG_ERROR("Get Silicon ID and protection state has failed results");
		}
	}
	*siliconId = ((siliconIdHi & 0xFF) << 24) | ((siliconIdLo & 0xFF) << 16) | ((familyIdHi & 0xFF) << 8) | (familyIdLo & 0xFF);
	return hr;
}


/*******************************************************************************
*	Purpose :	Check if bank of flash in protected state
*	Parameter :
*			bank - flash bank
*	Return :
*		ERROR_OK : Resault of check operation is OK
*		ERROR_FAIL : Resault of check operation is FAIL
*******************************************************************************/
static int Psoc6ProtectCheck(struct flash_bank *bank)
{
	LOG_INFO("Get protection state not work in OpenOCD");
	return ERROR_OK;
}


/*******************************************************************************
*	Purpose :	Set protected state in bank of flash
*	Parameter :
*			bank - flash bank
*			set - protected value
*			first - first address with protected data
*			last - last address with protected data
*	Return :
*		ERROR_OK : Resault of protect operation is OK
*		ERROR_FAIL : Resault of protect operation is FAIL
*******************************************************************************/
static int Psoc6Protect(struct flash_bank *bank, int set, int first, int last)
{
	LOG_INFO("Protect for PSoC6 is not support in OpenOCD");
	return ERROR_OK;
}


/*******************************************************************************
*	Purpose :	Detect device and get all main parameters
*	Parameter :
*			bank - flash bank
*	Return :
*		ERROR_OK : Resault of probe operation is OK
*		ERROR_FAIL : Resault of probe operation is FAIL
*******************************************************************************/
static int Psoc6Probe(struct flash_bank *bank)
{
	struct psoc6FlashBank *psoc6Info = bank->driver_priv;
	struct target *target = bank->target;
	uint32_t hr;
	uint32_t flashSizeInKb = 0;
	uint32_t maxFlashSizeInKb = 0;
	uint32_t siliconId = 0;
	uint32_t rowSize = 0;
	uint8_t protection = 0;
	uint32_t spcifGeometry = 0;

	uint32_t haltStatus;

	target_read_u32(target, 0xE000EDF0, &haltStatus);
	LOG_DEBUG("Status HALT - 0x%x", haltStatus);
	psoc6Info->probed = 0;
	hr = target_read_u32(target, PSOC6_SPCIF_GEOMETRY, &spcifGeometry);
	if (hr == ERROR_OK) {
		rowSize = ROW_SIZE;
		flashSizeInKb = (FLASH_SECTOR_LENGTH * (((spcifGeometry >> 24) & 0xFF) + 1));
		LOG_INFO("SPCIF geometry: %" PRIu32 " kb flash, row %" PRIu32 " bytes.", flashSizeInKb, rowSize);

		/* Get silicon ID from target. */
		hr = Psoc6GetSiliconId(target, &siliconId, &protection);
		if (hr == ERROR_OK) {

			const struct Psoc6ChipDetails *details = psoc6_details_by_id(siliconId);
			if (details) {
				LOG_INFO("%s device detected.", details->type);
				if (flashSizeInKb == 0) {
					flashSizeInKb = details->flashSizeInKb;
				} else {
					if (flashSizeInKb != details->flashSizeInKb)
						LOG_ERROR("Flash size mismatch");
				}
			}

			psoc6Info->flashSizeInKb = flashSizeInKb;
			psoc6Info->rowSize = rowSize;
			psoc6Info->siliconId = siliconId;
			psoc6Info->chipProtection = protection;

			/* failed reading flash size or flash size invalid (early silicon),
			default to max target family */
			if (hr != ERROR_OK || flashSizeInKb == 0xffff || flashSizeInKb == 0) {
				LOG_WARNING("PSoC 6 flash size failed, probe inaccurate - assuming %" PRIu32 " k flash",
					maxFlashSizeInKb);
				flashSizeInKb = maxFlashSizeInKb;
			}

			/* if the user sets the size manually then ignore the probed value
			this allows us to work around devices that have a invalid flash size register value */
			if (psoc6Info->userBankSize) {
				LOG_INFO("ignoring flash probed value, using configured bank size");
				flashSizeInKb = psoc6Info->userBankSize / 1024;
			}

			/* did we assign flash size? */
			assert(flashSizeInKb != 0xffff);

			/* calculate numbers of pages */
			uint32_t num_rows = flashSizeInKb * 1024 / rowSize;

			/* check that calculation result makes sense */
			assert(num_rows > 0);

			if (bank->sectors) {
				free(bank->sectors);
				bank->sectors = NULL;
			}

			bank->base = MEM_BASE_FLASH;
			bank->size = num_rows * rowSize;
			bank->num_sectors = num_rows;
			bank->sectors = malloc(sizeof(struct flash_sector) * num_rows);
			/* This part doesn't follow the typical standard of 0xff
			being the erased value.*/
			bank->default_padded_value = bank->erased_value = 0x00;

			uint32_t i;
			for (i = 0; i < num_rows; i++) {
				bank->sectors[i].offset = i * rowSize;
				bank->sectors[i].size = rowSize;
				bank->sectors[i].is_erased = -1;
				bank->sectors[i].is_protected = 1;
			}

			LOG_DEBUG("psoc6Info->flashSizeInKb-> 0x%x", psoc6Info->flashSizeInKb);
			LOG_DEBUG("psoc6Info->rowSize-> 0x%x", psoc6Info->rowSize);
			LOG_DEBUG("psoc6Info->siliconId	-> 0x%x", psoc6Info->siliconId);
			LOG_DEBUG("psoc6Info->chipProtection-> 0x%x", psoc6Info->chipProtection);

			LOG_DEBUG("bank->base -> 0x%x", bank->base);
			LOG_DEBUG("bank->size -> 0x%x", bank->size);
			LOG_DEBUG("bank->num_sectors-> 0x%x", bank->num_sectors);

			psoc6Info->probed = 1;
		}
	}

	return hr;
}


/*******************************************************************************
*	Purpose :	Auto detect device and get all main parameters
*	Parameter :
*			bank - flash bank
*	Return :
*		ERROR_OK : Resault of probe operation is OK
*		ERROR_FAIL : Resault of probe operation is FAIL
*******************************************************************************/
static int Psoc6AutoProbe(struct flash_bank *bank)
{
	struct psoc6FlashBank *psoc6Info = bank->driver_priv;
	int hr;

	if (psoc6Info->probed)
		hr = ERROR_OK;
	else
		hr = Psoc6Probe(bank);

	return hr;
}


/*******************************************************************************
*	Purpose :	Erase sector operation for connected target
*	Parameter :
*			target - current target device
*			first - first address which will be erased
*			last - last sector which will be erased
*	Return :
*		ERROR_OK : Resault of Erase sector operation is OK
*		ERROR_FAIL : Resault of Erase sector operation is FAIL
*******************************************************************************/
static int EraseSector(struct target *target, int first, int last)
{
	int hr;
	LOG_DEBUG("first-> 0x%x, last-> 0x%x", first, last);

		for (int i = first; i < last; i++) {
			int addr = MEM_BASE_FLASH + (i * FLASH_SECTOR_LENGTH * 1024);

			/* Prepare batch request. Skip immediate responses in batch mode.
				SRAM_SCRATCH: OpCode */
			hr = target_write_u32(target, SRAM_SCRATCH_ADDR, MXS40_SROMAPI_ERASESECTOR_CODE);
			if (hr != ERROR_OK)
				break;

			/* SRAM_SCRATCH + 0x04: Flash address to be erased (in 32-bit system address format) */
			hr = target_write_u32(target, SRAM_SCRATCH_ADDR + DATA_LOCATION_OFFSET, addr);
			if (hr != ERROR_OK)
				break;

			/* Send batch request */
			uint32_t dataOut;
			hr = CallSromApi(target, MXS40_SROMAPI_ERASESECTOR_CODE, &dataOut);
			if (hr != ERROR_OK) {
				LOG_ERROR("Sector \"%d\" from \"%d\" sectors are not erased.  Failed result for Erase operation.", i, last);
				break;
			}

			LOG_DEBUG("Sector -> 0x%x is Erased", addr);
		}
		return hr;
}


/*******************************************************************************
*	Purpose :	Erase  all sectors operation for connected target
*	Parameter :
*			bank - flash bank
*	Return :
*		ERROR_OK : Resault of Erase sector operation is OK
*		ERROR_FAIL : Resault of Erase sector operation is FAIL
*******************************************************************************/
static int psoc6_mass_erase(struct flash_bank *bank)
{
	int hr;
	struct target *target = bank->target;
	struct psoc6FlashBank *psoc6Info = bank->driver_priv;
	int sectors = psoc6Info->flashSizeInKb / FLASH_SECTOR_LENGTH;

	LOG_INFO("sectors-> 0x%x", sectors);

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		hr = ERROR_TARGET_NOT_HALTED;
	}

	if (hr == ERROR_OK)
		hr = EraseSector(target, 0, sectors);

	return hr;
}


/*******************************************************************************
*	Purpose :	Erase sector operation for connected target
*	Parameter :
*			bank - flash bank
*			first - first address which will be erased
*			last - last sector which will be erased
*	Return :
*		ERROR_OK : Resault of Erase sector operation is OK
*		ERROR_FAIL : Resault of Erase sector operation is FAIL
*******************************************************************************/
static int Psoc6Erase(struct flash_bank *bank, int first, int last)
{
	int hr = ERROR_OK;
	struct target *target = bank->target;
	struct psoc6FlashBank *psoc6Info = bank->driver_priv;

	LOG_DEBUG("bank->num_sectors-> 0x%x", bank->num_sectors);
	LOG_DEBUG("Calc-> 0x%x", (psoc6Info->flashSizeInKb / FLASH_SECTOR_LENGTH));
	LOG_DEBUG("first-> 0x%x, last-> 0x%x", first, last - 1);

	if ((unsigned)(last - 1) != (psoc6Info->flashSizeInKb/FLASH_SECTOR_LENGTH))
		LOG_INFO("Count of sector is more then real present");
	else
		hr = EraseSector(target, first, last - 1);

	return hr;
}


/*******************************************************************************
*	Purpose :	Write row operation for connected target
*	Parameter :
*			target - current target device
*			address - start address for write data
*			buffer - buffer with all data which need write
*			count - lenght data
*	Return :
*		ERROR_OK : Resault of write row operation is OK
*		ERROR_FAIL : Resault of write row operation is FAIL
*******************************************************************************/
static int WriteRow(struct target *target, int address, const uint8_t * buffer, int count)
{
	int hr = ERROR_OK;
	uint32_t dataOut;
	/* 1. Prepare data for SROM API - write it to SRAM ---
	   SRAM_SCRATCH: OpCode*/
	hr = target_write_u32(target, SRAM_SCRATCH_ADDR, MXS40_SROMAPI_PROGRAMROW_CODE);
	if (hr == ERROR_OK) {

		/* SRAM_SCRATCH + 0x04: Data location/size and Integrity check
			---
			Bits[31:24]	Bits[23:16]		Bits[15:8]		Bits[7:0]
			xxxxxxxx		Verify row		Data location	Data size
			---
			Verify row: 0-Data integrity check is not performed 1-Data integrity check is performed
			Data location : 0 – page latch , 1- SRAM
			Data size* – 0 – 8b ,1-16b , 2 -32b ,3 – 64b , 4 – 128b , 5 – 256 b , 6 – 512b , 7)
			Data size is ignored for S40 SONOS FLASH as the lowest granularity for program operation equals page size. */
		hr = target_write_u32(target, SRAM_SCRATCH_ADDR + DATA_LOCATION_OFFSET, DATA_LOCATION_OFFSET);
		if (hr == ERROR_OK) {
			/* SRAM_SCRATCH + 0x08:
			Flash address to be programmed (in 32-bit system address format) */
			hr = target_write_u32(target, SRAM_SCRATCH_ADDR + FLASH_ADDRESS_OFFSET, address);
			if (hr == ERROR_OK) {
				/* SRAM_SCRATCH + 0x10...n: Data word 0..n (Data provided should be proportional to data size provided, data to be programmed into LSB’s ) */
				uint32_t dataRamAddr = SRAM_SCRATCH_ADDR + DATA_OFFSET;

				/* SRAM_SCRATCH + 0x0C: Pointer to the first data byte location */
				hr = target_write_u32(target, SRAM_SCRATCH_ADDR + POINTER_ON_FIRST_BYTE_LOCATION_OFFSET, dataRamAddr);
				if (hr == ERROR_OK) {
					if (target_write_buffer(target, dataRamAddr, count, buffer) != ERROR_OK) {
						LOG_ERROR("Write to flash buffer failed");
						hr = ERROR_FAIL;
					} else {
						LOG_DEBUG("PSOC6: WRITE_ROW: ADDRESS->0x%x, PARAMS->0x%x, DATARAMADDR->0x%x, COUNT->0x%x", address, DATA_LOCATION_OFFSET, dataRamAddr, count);
						/* 2. Call SROM API --- */
						hr = CallSromApi(target, MXS40_SROMAPI_PROGRAMROW_CODE, &dataOut);
					}
				}
			}
		}
	}
	return hr;
}


/*******************************************************************************
*	Purpose :	Write operation for connected target
*	Parameter :
*			bank - flash bank
*			buffer - buffer with all data which need write
*			offset - offset of address where need to write data
*			count - lenght data
*	Return :
*		ERROR_OK : Resault of write operation is OK
*		ERROR_FAIL : Resault of write operation is FAIL
*******************************************************************************/
static int Psoc6Write(struct flash_bank *bank, const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	int hr = ERROR_OK;
	struct psoc6FlashBank *psoc6Info = bank->driver_priv;
	struct target *target = bank->target;
	uint32_t bytesRemaining = count;
	uint8_t pageBuffer[psoc6Info->rowSize];
	uint32_t address, size, sourceOffset, maxAdressSize;

	LOG_DEBUG("PSOC6_WRITE: OFFSET -> 0x%x, COUNT -> 0x%x", offset, count);

	sourceOffset = 0;
	address = bank->base + offset;
	maxAdressSize = (address + count);
	while (address < maxAdressSize) {
		LOG_DEBUG("PSOC6_WRITE: SOURCE_OFFSET->0x%x, ADDRESS->0x%x, ADDRESS+COUNT->0x%x, BYTES_REMAINING->0x%x", sourceOffset, address, maxAdressSize, bytesRemaining);
		LOG_INFO("Write data in 0x%x address", address);
		size = psoc6Info->rowSize;
		if (bytesRemaining < psoc6Info->rowSize) {
			memset(pageBuffer, 0x00, size);
			memcpy(pageBuffer, &buffer[sourceOffset], bytesRemaining);
			size = bytesRemaining;
		} else {
			memcpy(pageBuffer, &buffer[sourceOffset], size);
		}

		hr = WriteRow(target, address, pageBuffer, size);
		if (hr != ERROR_OK)
			break;

		sourceOffset += size;
		address = address + size;
		bytesRemaining -= size;
	}

	return hr;
}

/*******************************************************************************
*	Purpose :	Get information about connected target
*	Parameter :
*			bank - flash bank
*			buf - buffer all information
*			buf_size - size for buffer
*	Return :
*		ERROR_OK : Resault of get info operation operation is OK
*		ERROR_FAIL : Resault of get info operation operation is FAIL
*******************************************************************************/
static int GetPsoc6Info(struct flash_bank *bank, char *buf, int buf_size)
{
	int hr;
	struct psoc6FlashBank *psoc6Info = bank->driver_priv;
	int printed = 0;
	if (psoc6Info->probed == 0) {
		hr = ERROR_FAIL;
	} else {
		const struct Psoc6ChipDetails *details = psoc6_details_by_id(psoc6Info->siliconId);
		if (details) {
			uint32_t chip_revision = psoc6Info->siliconId & 0xffffffff;
			printed = snprintf(buf, buf_size, "PSoC 6 %s rev 0x%04" PRIx32 " ", details->type, chip_revision);
		} else {
			printed = snprintf(buf, buf_size, "PSoC 6 silicon id 0x%x", psoc6Info->siliconId);
		}

		buf += printed;
		buf_size -= printed;

		const char *prot_txt = psoc6_decode_chipProtection(psoc6Info->chipProtection);
		uint32_t size_in_kb = bank->size / 1024;
		snprintf(buf, buf_size, " flash %" PRIu32 " kb %s", size_in_kb, prot_txt);

		hr = ERROR_OK;
	}
	return hr;
}

COMMAND_HANDLER(psoc6_handle_mass_erase_command)
{
	LOG_INFO("psoc6_handle_mass_erase_command function");
	int hr;
	if (CMD_ARGC < 1)
		hr = ERROR_COMMAND_SYNTAX_ERROR;

	if (hr == ERROR_OK) {
		struct flash_bank *bank;
		hr = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
		if (hr == ERROR_OK) {
			hr = psoc6_mass_erase(bank);
			if (hr == ERROR_OK)
				command_print(CMD_CTX, "psoc mass erase complete");
			else
				command_print(CMD_CTX, "psoc mass erase failed");
		}
	}
	return hr;
}

static const struct command_registration psoc6_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.handler = psoc6_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Erase entire flash device.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration psoc6_command_handlers[] = {
	{
		.name = "psoc6",
		.mode = COMMAND_ANY,
		.help = "PSoC 6 flash command group",
		.usage = "",
		.chain = psoc6_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver psoc6_flash = {
	.name = "psoc6",
	.commands = psoc6_command_handlers,
	.flash_bank_command = psoc6_flash_bank_command,
	.erase = Psoc6Erase,
	.protect = Psoc6Protect,
	.write = Psoc6Write,
	.read = default_flash_read,
	.probe = Psoc6Probe,
	.auto_probe = Psoc6AutoProbe,
	.erase_check = default_flash_blank_check,
	.protect_check = Psoc6ProtectCheck,
	.info = GetPsoc6Info,
};
