/**************************************************************************
*   Copyright (C) 2014 by Rob Brown <rob@cobbleware.com>                  *
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
 * This file loads the required functions from the ftd2xx DLL file that's
 * installed with FTDI drivers.
 * There's a very basic reference count in case more that one driver is
 * active at once; I don't know if that's possible or whether it will be
 * possible in the future, but it won't hurt in any case.
 * The 64-bit DLL will be loaded if possible. That will require:
 * a) a 64-bit OS
 * b) the 64-bit drivers from FTDI to be installed
 * c) openocd.exe to be built 64-bit.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <log.h>

#include <windows.h>

#include "ftd2xx_api.h"

static bool loadSuccess;
static int refCount;
static HMODULE hDll;

FT_Open_t FTAPI_Open;
FT_OpenEx_t FTAPI_OpenEx;
FT_Close_t FTAPI_Close;
FT_Purge_t FTAPI_Purge;
FT_Write_t FTAPI_Write;
FT_Read_t FTAPI_Read;
FT_ListDevices_t FTAPI_ListDevices;
FT_SetLatencyTimer_t FTAPI_SetLatencyTimer;
FT_GetLatencyTimer_t FTAPI_GetLatencyTimer;
FT_GetDriverVersion_t FTAPI_GetDriverVersion;
FT_SetTimeouts_t FTAPI_SetTimeouts;
FT_SetBitMode_t FTAPI_SetBitMode;
FT_GetDeviceInfo_t FTAPI_GetDeviceInfo;
FT_ReadEE_t FTAPI_ReadEE;
FT_WriteEE_t FTAPI_WriteEE;
FT_SetBaudRate_t FTAPI_SetBaudRate;

bool ftd2xx_dll_api_init(void)
{
  /* Only want to load the DLL once, just in case multiple drivers call this */
  if (loadSuccess) {
    refCount++;
    return true;
  }

  hDll = LoadLibrary("ftd2xx64.dll");

  if (hDll != NULL)
    LOG_DEBUG("Opened the 64-bit DLL.");
  else {
    hDll = LoadLibrary("ftd2xx.dll");
    if (hDll == NULL) {
      LOG_USER("Failed to open the FTDI DLL.");
      return FALSE;
    } else
      LOG_DEBUG("Opened the 32-bit DLL.");
  }

  FTAPI_Open = (FT_Open_t)GetProcAddress(hDll, "FT_Open");
  FTAPI_OpenEx = (FT_OpenEx_t)GetProcAddress(hDll, "FT_OpenEx");
  FTAPI_Close = (FT_Close_t)GetProcAddress(hDll, "FT_Close");
  FTAPI_Purge = (FT_Purge_t)GetProcAddress(hDll, "FT_Purge");
  FTAPI_Write = (FT_Write_t)GetProcAddress(hDll, "FT_Write");
  FTAPI_Read = (FT_Read_t)GetProcAddress(hDll, "FT_Read");
  FTAPI_ListDevices = (FT_ListDevices_t)GetProcAddress(hDll, "FT_ListDevices");
  FTAPI_SetLatencyTimer = (FT_SetLatencyTimer_t)GetProcAddress(hDll, "FT_SetLatencyTimer");
  FTAPI_GetLatencyTimer = (FT_GetLatencyTimer_t)GetProcAddress(hDll, "FT_GetLatencyTimer");
  FTAPI_GetDriverVersion = (FT_GetDriverVersion_t)GetProcAddress(hDll, "FT_GetDriverVersion");
  FTAPI_SetTimeouts = (FT_SetTimeouts_t)GetProcAddress(hDll, "FT_SetTimeouts");
  FTAPI_SetBitMode = (FT_SetBitMode_t)GetProcAddress(hDll, "FT_SetBitMode");
  FTAPI_GetDeviceInfo = (FT_GetDeviceInfo_t)GetProcAddress(hDll, "FT_GetDeviceInfo");
  FTAPI_ReadEE = (FT_ReadEE_t)GetProcAddress(hDll, "FT_ReadEE");
  FTAPI_WriteEE = (FT_WriteEE_t)GetProcAddress(hDll, "FT_WriteEE");
  FTAPI_SetBaudRate = (FT_SetBaudRate_t)GetProcAddress(hDll, "FT_SetBaudRate");

  if ((FTAPI_Open == NULL) ||
      (FTAPI_OpenEx == NULL) ||
      (FTAPI_Close == NULL) ||
      (FTAPI_Purge == NULL) ||
      (FTAPI_Write == NULL) ||
      (FTAPI_Read == NULL) ||
      (FTAPI_ListDevices == NULL) ||
      (FTAPI_SetLatencyTimer == NULL) ||
      (FTAPI_GetLatencyTimer == NULL) ||
      (FTAPI_GetDriverVersion == NULL) ||
      (FTAPI_SetTimeouts == NULL) ||
      (FTAPI_SetBitMode == NULL) ||
      (FTAPI_GetDeviceInfo == NULL) ||
      (FTAPI_ReadEE == NULL) ||
      (FTAPI_WriteEE == NULL) ||
      (FTAPI_SetBaudRate == NULL))
  {
    LOG_USER("Failed to load the DLL functions.");
    FreeLibrary(hDll);
  }
  else {
    LOG_DEBUG("Loaded DLL functions successfully.");
    loadSuccess = true;
  }

  return loadSuccess;
}

void ftd2xx_dll_api_shutdown(void)
{
  if (loadSuccess) {
    if (refCount) {
      refCount--;
      return;
    }
    FreeLibrary(hDll);
    loadSuccess = false;
  }
}
