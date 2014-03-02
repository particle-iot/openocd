/***************************************************************************
 *   Written by Rob Brown (rob@cobbleware.com)                             *
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
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/
/**
 * @file
 * This file provides definitions of the FTAPI_xxx functions that are used
 * by code that accesses FTDI chips through the FTD2XX library.
 * If OpenOCD was built to use the FTD2XX DLL (by supplying 
 * "--with-ftd2xx-lib=shared" to configure), then the FTAPI_xxx symbols are
 * function pointers initialised with GetProcAddress() from the DLL.
 * If OpenOCD was built to use the FTD2XX static library, then the FTAPI_xxx
 * symbols are #defined to their FT_xxx equivalents.
 */

#ifndef FTD2XX_API_H
#define FTD2XX_API_H

#include <ftd2xx.h>

#ifdef BUILD_FT2232_WINDOWS_FTD2XX_DLL
#ifdef __cplusplus
extern "C" {
#endif

typedef FT_STATUS __stdcall (*FT_Open_t)(int deviceNumber, FT_HANDLE *pHandle);
typedef FT_STATUS __stdcall (*FT_OpenEx_t)(PVOID pArg1, DWORD Flags, FT_HANDLE *pHandle);
typedef FT_STATUS __stdcall (*FT_Close_t)(FT_HANDLE ftHandle);
typedef FT_STATUS __stdcall (*FT_Purge_t)(FT_HANDLE ftHandle, ULONG Mask);
typedef FT_STATUS __stdcall (*FT_Write_t)(FT_HANDLE ftHandle, LPVOID lpBuffer, DWORD dwBytesToWrite, LPDWORD lpBytesWritten);
typedef FT_STATUS __stdcall (*FT_Read_t)(FT_HANDLE ftHandle, LPVOID lpBuffer, DWORD dwBytesToRead, LPDWORD lpBytesReturned);
typedef FT_STATUS __stdcall (*FT_ListDevices_t)(PVOID pArg1, PVOID pArg2, DWORD Flags);
typedef FT_STATUS __stdcall (*FT_SetLatencyTimer_t)(FT_HANDLE ftHandle, UCHAR ucLatency);
typedef FT_STATUS __stdcall (*FT_GetLatencyTimer_t)(FT_HANDLE ftHandle, PUCHAR pucLatency);
typedef FT_STATUS __stdcall (*FT_GetDriverVersion_t)(FT_HANDLE ftHandle, LPDWORD lpdwVersion);
typedef FT_STATUS __stdcall (*FT_SetTimeouts_t)(FT_HANDLE ftHandle, ULONG ReadTimeout, ULONG WriteTimeout);
typedef FT_STATUS __stdcall (*FT_SetBitMode_t)(FT_HANDLE ftHandle, UCHAR ucMask, UCHAR ucEnable);
typedef FT_STATUS __stdcall (*FT_GetDeviceInfo_t)(FT_HANDLE ftHandle, FT_DEVICE *lpftDevice, LPDWORD lpdwID, PCHAR SerialNumber, PCHAR Description, LPVOID Dummy);
typedef FT_STATUS __stdcall (*FT_ReadEE_t)(FT_HANDLE ftHandle, DWORD dwWordOffset, LPWORD lpwValue);
typedef FT_STATUS __stdcall (*FT_WriteEE_t)(FT_HANDLE ftHandle, DWORD dwWordOffset, WORD wValue);
typedef FT_STATUS __stdcall (*FT_SetBaudRate_t)(FT_HANDLE ftHandle, ULONG BaudRate);

extern FT_Open_t FTAPI_Open;
extern FT_OpenEx_t FTAPI_OpenEx;
extern FT_Close_t FTAPI_Close;
extern FT_Purge_t FTAPI_Purge;
extern FT_Write_t FTAPI_Write;
extern FT_Read_t FTAPI_Read;
extern FT_ListDevices_t FTAPI_ListDevices;
extern FT_SetLatencyTimer_t FTAPI_SetLatencyTimer;
extern FT_GetLatencyTimer_t FTAPI_GetLatencyTimer;
extern FT_GetDriverVersion_t FTAPI_GetDriverVersion;
extern FT_SetTimeouts_t FTAPI_SetTimeouts;
extern FT_SetBitMode_t FTAPI_SetBitMode;
extern FT_GetDeviceInfo_t FTAPI_GetDeviceInfo;
extern FT_ReadEE_t FTAPI_ReadEE;
extern FT_WriteEE_t FTAPI_WriteEE;
extern FT_SetBaudRate_t FTAPI_SetBaudRate;

bool ftd2xx_dll_api_init(void);
void ftd2xx_dll_api_shutdown(void);

#ifdef __cplusplus
}
#endif

#else

#define FTAPI_Open FT_Open
#define FTAPI_OpenEx FT_OpenEx;
#define FTAPI_Close FT_Close;
#define FTAPI_Purge FT_Purge;
#define FTAPI_Write FT_Write;
#define FTAPI_Read FT_Read;
#define FTAPI_ListDevices FT_ListDevices;
#define FTAPI_SetLatencyTimer FT_SetLatencyTimer;
#define FTAPI_GetLatencyTimer FT_GetLatencyTimer;
#define FTAPI_GetDriverVersion FT_GetDriverVersion;
#define FTAPI_SetTimeouts FT_SetTimeouts;
#define FTAPI_SetBitMode FT_SetBitMode;
#define FTAPI_GetDeviceInfo FT_GetDeviceInfo;
#define FTAPI_ReadEE FT_ReadEE;
#define FTAPI_WriteEE FT_WriteEE;
#define FTAPI_SetBaudRate FT_SetBaudRate;

#endif

#endif /* FTD2XX_API_H */
