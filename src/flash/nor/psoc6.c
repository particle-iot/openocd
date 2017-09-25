/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2017 by Bohdan Tymkiv                                   *
 *   bohdan.tymkiv@cypress.com bohdan200@gmail.com                         *
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

#include <time.h>

#include "imp.h"
#include "target/cortex_m.h"
#include "target/breakpoints.h"
#include "target/target_type.h"

/**************************************************************************************************
 * PSoC6 device definitions
 *************************************************************************************************/
#define FLASH_ROW_SIZE                  512u
#define FLASH_SECTOR_SIZE               (256u*1024u)
#define FLASH_NUM_ROWS_IN_SECTOR        (FLASH_SECTOR_SIZE / FLASH_ROW_SIZE)
#define MEM_BASE_MFLASH                 0x10000000u
#define MEM_BASE_WFLASH                 0x14000000u
#define MEM_BASE_SFLASH                 0x16000000u
#define RAM_LOOP_WA_SIZE                2048u

#define SFLASH_USER_INDEX               0
#define SFLASH_NAR_INDEX                1
#define SFLASH_KEY_INDEX                2
#define SFLASH_TOC2_INDEX               3

#define PROTECTION_UNKNOWN              0x00u
#define PROTECTION_VIRGIN               0x01u
#define PROTECTION_NORMAL               0x02u
#define PROTECTION_SECURE               0x03u
#define PROTECTION_DEAD                 0x04u

#define MEM_BASE_IPC                    0x40230000u
#define IPC_STRUCT_SIZE                 0x20u
#define MEM_IPC(n)                      (MEM_BASE_IPC + (n) * IPC_STRUCT_SIZE)
#define MEM_IPC_ACQUIRE(n)              (MEM_IPC(n) + 0x00u)
#define MEM_IPC_NOTIFY(n)               (MEM_IPC(n) + 0x08u)
#define MEM_IPC_DATA(n)                 (MEM_IPC(n) + 0x0Cu)
#define MEM_IPC_LOCK_STATUS(n)          (MEM_IPC(n) + 0x10u)

#define MEM_BASE_IPC_INTR               0x40231000u
#define IPC_INTR_STRUCT_SIZE            0x20u
#define MEM_IPC_INTR(n)                 (MEM_BASE_IPC_INTR + (n) * IPC_INTR_STRUCT_SIZE)
#define MEM_IPC_INTR_MASK(n)            (MEM_IPC_INTR(n) + 0x08u)
#define IPC_ACQUIRE_SUCCESS_MSK         0x80000000u
#define IPC_LOCK_ACQUIRED_MSK           0x80000000u

#define IPC_ID                          2u
#define IPC_INTR_ID                     0u
#define IPC_TIMEOUT_MS                  1000

#define SROMAPI_SIID_REQ                    0x00000001u
#define SROMAPI_SIID_REQ_FAMILY_REVISION    (SROMAPI_SIID_REQ | 0x000u)
#define SROMAPI_SIID_REQ_SIID_PROTECTION    (SROMAPI_SIID_REQ | 0x100u)
#define SROMAPI_WRITEROW_REQ                0x05000100u
#define SROMAPI_PROGRAMROW_REQ              0x06000100u
#define SROMAPI_ERASESECTOR_REQ             0x14000100u
#define SROMAPI_ERASEALL_REQ                0x0A000100u
#define SROMAPI_ERASEROW_REQ                0x1C000100u

#define SROMAPI_STATUS_MSK                  0xF0000000u
#define SROMAPI_STAT_SUCCESS                0xA0000000u
#define SROMAPI_DATA_LOCATION_MSK           0x00000001u

struct mxs40_chip_info_s {
	uint32_t silicon_id;
	const char *mpn_str;
	uint32_t mainfl_kb;
	uint32_t workfl_kb;
};

struct row_region_s {
	uint32_t addr;
	size_t size;
};

struct psoc6_target_info_s {
	struct mxs40_chip_info_s *info;
	uint32_t silicon_id;
	uint32_t protection;
	int is_probed;
};

struct timeout_s {
	struct timespec start_time;
	long timeout_ms;
};

/* Target memory layout */
static struct mxs40_chip_info_s psoc6_devices[] = {
	{ 0xE2102100, "CY8C6036BZI-F04",   .mainfl_kb = 512,   .workfl_kb = 32 },
	{ 0xE2112100, "CY8C6016BZI-F04",   .mainfl_kb = 512,   .workfl_kb = 32 },
	{ 0xE2122100, "CY8C6116BZI-F54",   .mainfl_kb = 512,   .workfl_kb = 32 },
	{ 0xE2132100, "CY8C6136BZI-F14",   .mainfl_kb = 512,   .workfl_kb = 32 },
	{ 0xE2142100, "CY8C6136BZI-F34",   .mainfl_kb = 512,   .workfl_kb = 32 },
	{ 0xE2152100, "CY8C6137BZI-F14",   .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2162100, "CY8C6137BZI-F34",   .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2172100, "CY8C6137BZI-F54",   .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2182100, "CY8C6117BZI-F34",   .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2192100, "CY8C6246BZI-D04",   .mainfl_kb = 512,   .workfl_kb = 32 },
	{ 0xE21A2100, "CY8C6247BZI-D44",   .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE21B2100, "CY8C6247BZI-D34",   .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2062100, "CY8C6247BZI-D54",   .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2001100, "CY8C637BZI-MD76",   .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2202100, "CY8C6336BZI-BLF03", .mainfl_kb = 512,   .workfl_kb = 32 },
	{ 0xE2212100, "CY8C6316BZI-BLF03", .mainfl_kb = 512,   .workfl_kb = 32 },
	{ 0xE2222100, "CY8C6316BZI-BLF53", .mainfl_kb = 512,   .workfl_kb = 32 },
	{ 0xE2232100, "CY8C6336BZI-BLD13", .mainfl_kb = 512,   .workfl_kb = 32 },
	{ 0xE2242100, "CY8C6347BZI-BLD43", .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2252100, "CY8C6347BZI-BLD33", .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2072100, "CY8C6347BZI-BLD53", .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2262100, "CY8C6347FMI-BLD13", .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2272100, "CY8C6347FMI-BLD43", .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2282100, "CY8C6347FMI-BLD33", .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2082100, "CY8C6347FMI-BLD53", .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2011100, "CY8C637BZI-BLD74",  .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2021100, "CY8C637FMI-BLD73",  .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2051100, "CY8C68237BZ-BLE",   .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2041100, "CY8C68237FM-BLE",   .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0xE2000100, "CY8C622PSVP",       .mainfl_kb = 1024,  .workfl_kb = 32 },
	{ 0, NULL, 0, 0 },
};

struct row_region_s safe_sflash_regions[] = {
	{0x16000800, 0x800},	/* SFLASH: User Data */
	{0x16001A00, 0x200},	/* SFLASH: NAR */
	{0x16005A00, 0xC00},	/* SFLASH: Public Key */
	{0x16007C00, 0x400},	/* SFLASH: TOC2 */
};

static struct mxs40_chip_info_s *g_current_device;
static struct working_area *g_stack_area;

/**************************************************************************************************
 * Initializes timeout_s structure with given timeout in milliseconds
 *************************************************************************************************/
static void timeout_init(struct timeout_s *to, long timeout_ms)
{
	clock_gettime(CLOCK_MONOTONIC, &to->start_time);
	to->timeout_ms = timeout_ms;
}

/**************************************************************************************************
 * Returns true if given timeout_s object has expired
 *************************************************************************************************/
static int timeout_expired(struct timeout_s *to)
{
	struct timespec now_time;
	struct timespec result;

	clock_gettime(CLOCK_MONOTONIC, &now_time);

	if (now_time.tv_nsec < to->start_time.tv_nsec) {
		result.tv_sec = now_time.tv_sec - to->start_time.tv_sec - 1;
		result.tv_nsec = now_time.tv_nsec - to->start_time.tv_nsec + 1000000000;
	} else {
		result.tv_sec = now_time.tv_sec - to->start_time.tv_sec;
		result.tv_nsec = now_time.tv_nsec - to->start_time.tv_nsec;
	}

	const long diff_ms = result.tv_sec * 1000 + result.tv_nsec / 1000000;
	return diff_ms >= to->timeout_ms ? 1 : 0;
}

/**************************************************************************************************
 * Acquires PSoC6 device
 * This function implements so-called 'alternative mode' to acquire the device.
 *
 * ARM Vector Catch is not supported by PSoC devices. This is the main reason why they can not
 * be cleanly hated by the debugger. Also, all Flash-related operations (includeing probing)
 * require that target is in Running state. In Cypress proprietary debug probes this is achieved
 * by setting special TEST_MODE bit is short period of time after deasserting HW Reset. This is
 * done by the probe itself by issuing special USB request.
 * Acquisition procedure is time-critical, all sequence has to be done in less than 1 ms after
 * HW Reset has been deasserted. Third-party debug probes can not enter TEST MODE because
 * of USB latency, etc. Procedure below uses 'alternative' acquisition method as described in
 * "PSoC6 Programming Specification" document.
 *************************************************************************************************/
static int device_acquire(struct target *tgt)
{
	int hr = ERROR_OK;
	const bool is_cm0 = (tgt->coreid == 0);
	const struct armv7m_common *cm = target_to_armv7m(tgt);

	/* Read status of CM4 core */
	uint32_t cm4_ctl;
	target_read_u32(tgt, 0x40210080, &cm4_ctl);

	/* Reset and enable CM4 core if it is not enabled already */
	if ((cm4_ctl & 0x03) != 0x03) {
		hr = target_write_u32(tgt, 0x40210080, 0x05FA0001);
		if (hr != ERROR_OK)
			return hr;

		hr = target_write_u32(tgt, 0x40210080, 0x05FA0003);
		if (hr != ERROR_OK)
			return hr;
	}

	/* Halt target device */
	if (tgt->state != TARGET_HALTED) {
		hr = target_halt(tgt);
		if (hr != ERROR_OK)
			return hr;

		target_wait_state(tgt, TARGET_HALTED, IPC_TIMEOUT_MS);
		if (hr != ERROR_OK)
			return hr;
	}

	do {
		/* Read Vector Offset register */
		uint32_t vt_base;
		const uint32_t vt_offset_reg = is_cm0 ? 0x402102B0 : 0x402102C0;
		hr = target_read_u32(tgt, vt_offset_reg, &vt_base);
		if (hr != ERROR_OK)
			return hr;

		/* Invalid value means  */
		vt_base &= 0xFFFFFF00;
		if ((vt_base == 0) || (vt_base == 0xFFFFFF00))
			break;

		uint32_t reset_addr;
		hr = target_read_u32(tgt, vt_base + 4, &reset_addr);
		if (hr != ERROR_OK)
			return hr;

		if ((reset_addr == 0) || (reset_addr == 0xFFFFFF00))
			break;

		/* Set breakpoint at User Application entry point */
		hr = breakpoint_add(tgt, reset_addr, 2, BKPT_HARD);
		if (hr != ERROR_OK)
			return hr;

		/* Reset the device by asserting SYSRESETREQ */
		hr = mem_ap_write_atomic_u32(cm->debug_ap,
				NVIC_AIRCR,
				AIRCR_VECTKEY | AIRCR_SYSRESETREQ);

		/* Wait for bootcode and initialize DAP */
		usleep(3000);
		dap_dp_init(cm->debug_ap->dap);

		/* Remove the break point */
		breakpoint_remove(tgt, reset_addr);

		if (hr != ERROR_OK)
			return hr;
	} while (0);

	/* Allocate Working Area for RAM Loop and Stack */
	hr = target_alloc_working_area(tgt, RAM_LOOP_WA_SIZE, &g_stack_area);
	if (hr != ERROR_OK)
		return hr;

	const uint32_t wa_addr = g_stack_area->address;
	const uint32_t wa_size = g_stack_area->size;

	/* Write infinite loop to RAM */
	hr = target_write_u32(tgt, g_stack_area->address, 0xE7FEE7FE);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	/* Set stack pointer to the end of RAM buffer */
	hr = cm->store_core_reg_u32(tgt, ARMV7M_R13, wa_addr + wa_size);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	/* Restore THUMB bit in xPSR register */
	hr = cm->store_core_reg_u32(tgt, ARMV7M_xPSR, 0x01000000);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	/* Start execution of infinite loop */
	hr = target_resume(tgt, 0, wa_addr, 1, 1);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	return hr;

exit_free_wa:
	if (g_stack_area) {
		target_free_working_area(tgt, g_stack_area);
		g_stack_area = NULL;
	}

	return hr;
}

/**************************************************************************************************
 * Releases acquired device. With 'alternative' acquisition procedure this just halts the target.
 *************************************************************************************************/
static int device_release(struct target *tgt)
{
	int hr = target_halt(tgt);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	hr = target_wait_state(tgt, TARGET_HALTED, IPC_TIMEOUT_MS);

exit_free_wa:
	if (g_stack_area) {
		target_free_working_area(tgt, g_stack_area);
		g_stack_area = NULL;
	}

	return hr;
}

/**************************************************************************************************
 * Waits for expected IPC lock status.
 * PSoC6 uses IPC structures for inter-core communication. Same IPCs are used to invoke SROM API.
 * IPC structure must be locked prior to invoking any SROM API. This ensures nothing else in the
 * system will use same IPC thus corrupting our data. Locking is performed by ipc_acquire(), this
 * function ensures that IPC is actually in expected state
 *************************************************************************************************/
static int ipc_poll_lock_stat(struct target *tgt, uint32_t ipc_id, bool lock_expected)
{
	int hr;
	uint32_t reg_val;

	struct timeout_s to;
	timeout_init(&to, IPC_TIMEOUT_MS);

	while (!timeout_expired(&to)) {
		/* Process any server requests */
		keep_alive();

		/* Read IPC Lock status */
		hr = target_read_u32(tgt, MEM_IPC_LOCK_STATUS(ipc_id), &reg_val);
		if (hr != ERROR_OK) {
			LOG_ERROR("Unable to read IPC Lock Status register");
			return hr;
		}

		bool is_locked = (reg_val & IPC_LOCK_ACQUIRED_MSK) != 0;
		bool is_status_expected = !!lock_expected == !!is_locked;

		if (is_status_expected)
			return ERROR_OK;
	}

	LOG_ERROR("Timeout polling IPC Lock Status");
	return ERROR_TARGET_TIMEOUT;
}

/**************************************************************************************************
 * Acquires IPC structure
 * PSoC6 uses IPC structures for inter-core communication. Same IPCs are used to invoke SROM API.
 * IPC structure must be locked prior to invoking any SROM API. This ensures nothing else in the
 * system will use same IPC thus corrupting our data. This function locks the IPC.
 *************************************************************************************************/
static int ipc_acquire(struct target *tgt, char ipc_id)
{
	int hr = ERROR_OK;
	bool is_acquired;
	uint32_t reg_val;

	struct timeout_s to;
	timeout_init(&to, IPC_TIMEOUT_MS);

	while (!timeout_expired(&to)) {
		keep_alive();

		hr = target_write_u32(tgt, MEM_IPC_ACQUIRE(ipc_id), IPC_ACQUIRE_SUCCESS_MSK);
		if (hr != ERROR_OK) {
			LOG_ERROR("Unable to write to IPC Acquire register");
			return hr;
		}

		/* Check if data is writen on first step */
		hr = target_read_u32(tgt, MEM_IPC_ACQUIRE(ipc_id), &reg_val);
		if (hr != ERROR_OK) {
			LOG_ERROR("Unable to read IPC Acquire register");
			return hr;
		}

		is_acquired = (reg_val & IPC_ACQUIRE_SUCCESS_MSK) != 0;
		if (is_acquired)
			return ERROR_OK;
	}

	if (!is_acquired) {
		LOG_ERROR("Timeout acquiring IPC structure");
		return ERROR_TARGET_TIMEOUT;
	}

	/* If IPC structure is acquired, the lock status should be set */
	hr = ipc_poll_lock_stat(tgt, ipc_id, true);
	return hr;
}

/**************************************************************************************************
 * Invokes SROM API functions which are responsible for Flash operations
 *************************************************************************************************/
static int call_sromapi(struct target *tgt,
	uint32_t req_and_params,
	uint32_t working_area,
	uint32_t *dataOut)
{
	int hr;

	bool is_data_in_ram = (req_and_params & SROMAPI_DATA_LOCATION_MSK) == 0;

	hr = ipc_acquire(tgt, IPC_ID);
	if (hr != ERROR_OK)
		return hr;

	if (is_data_in_ram)
		target_write_u32(tgt, MEM_IPC_DATA(IPC_ID), working_area);
	else
		target_write_u32(tgt, MEM_IPC_DATA(IPC_ID), req_and_params);

	/* Enable notification interrupt of IPC_INTR_STRUCT0(CM0+) for IPC_STRUCT2 */
	target_write_u32(tgt, MEM_IPC_INTR_MASK(IPC_INTR_ID), 1u << (16 + IPC_ID));

	/* Notify to IPC_INTR_STRUCT0. IPC_STRUCT2.MASK <- Notify */
	target_write_u32(tgt, MEM_IPC_NOTIFY(IPC_ID), 1);

	/* Poll lock status */
	hr = ipc_poll_lock_stat(tgt, IPC_ID, false);
	if (hr != ERROR_OK)
		return hr;

	/* Poll Data byte */
	if (is_data_in_ram)
		hr = target_read_u32(tgt, working_area, dataOut);
	else
		hr = target_read_u32(tgt, MEM_IPC_DATA(IPC_ID), dataOut);

	if (hr != ERROR_OK) {
		LOG_ERROR("Error reading SROM API Status location");
		return hr;
	}

	bool is_success = (*dataOut & SROMAPI_STATUS_MSK) == SROMAPI_STAT_SUCCESS;
	if (!is_success) {
		LOG_ERROR("SROM API execution failed. Status: 0x%08X", (uint32_t)*dataOut);
		return ERROR_TARGET_FAILURE;
	}

	return ERROR_OK;
}

/**************************************************************************************************
 * Retrieves SiliconID and Protection status of the target device
 *************************************************************************************************/
static int get_silicon_id(struct target *tgt, uint32_t *si_id, uint8_t *protection)
{
	int hr;
	uint32_t family_rev, siid_prot;

	/* Read FamilyID and Revision */
	hr = call_sromapi(tgt, SROMAPI_SIID_REQ_FAMILY_REVISION, 0, &family_rev);
	if (hr != ERROR_OK)
		return hr;

	/* Read SiliconID and Protection */
	hr = call_sromapi(tgt, SROMAPI_SIID_REQ_SIID_PROTECTION, 0, &siid_prot);
	if (hr != ERROR_OK)
		return hr;

	*si_id  = (siid_prot & 0x0000FFFF) << 16;
	*si_id |= (family_rev & 0x00FF0000) >> 8;
	*si_id |= (family_rev & 0x000000FF) >> 0;

	*protection = (siid_prot & 0x000F0000) >> 0x10;

	return ERROR_OK;
}

/**************************************************************************************************
 * Translates Protection status to openocd-friendly boolean value
 *************************************************************************************************/
static int psoc6_protect_check(struct flash_bank *bank)
{
	int is_protected;

	struct psoc6_target_info_s *psoc6_info = bank->driver_priv;
	switch (psoc6_info->protection) {
		case PROTECTION_VIRGIN:
		case PROTECTION_NORMAL:
			is_protected = 0;
			break;

		case PROTECTION_UNKNOWN:
		case PROTECTION_SECURE:
		case PROTECTION_DEAD:
		default:
			is_protected = 1;
			break;
	}

	for (int i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_protected = is_protected;

	return ERROR_OK;
}

/**************************************************************************************************
 * Lifecycle transition is not currently supported
 *************************************************************************************************/
static int psoc6_protect(struct flash_bank *bank, int set, int first, int last)
{
	(void)bank;
	(void)set;
	(void)first;
	(void)last;

	LOG_WARNING("Lifecycle transition for PSoC6 is not supported");
	return ERROR_OK;
}

/**************************************************************************************************
 * Translates Protection status to string
 *************************************************************************************************/
static const char *protection_to_str(uint8_t protection)
{
	switch (protection) {
		case PROTECTION_VIRGIN:
			return "VIRGIN";
			break;
		case PROTECTION_NORMAL:
			return "NORMAL";
			break;
		case PROTECTION_SECURE:
			return "SECURE";
			break;
		case PROTECTION_DEAD:
			return "DEAD";
			break;
		case PROTECTION_UNKNOWN:
		default:
			return "UNKNOWN";
			break;
	}
}

/**************************************************************************************************
 * Displays human-readable information about acquired device
 *************************************************************************************************/
static int psoc6_get_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct psoc6_target_info_s *psoc6_info = bank->driver_priv;

	if (psoc6_info->is_probed == 0)
		return ERROR_FAIL;

	snprintf(buf, buf_size,
		"Detected device: %s, Silicon ID: 0x%08X\n" \
		"Protection: %s\n"
		"Main Flash size: %d kB\n" \
		"Work Flash size: %d kB\n",
		g_current_device->mpn_str,
		g_current_device->silicon_id,
		protection_to_str(psoc6_info->protection),
		g_current_device->mainfl_kb,
		g_current_device->workfl_kb);

	return ERROR_OK;
}


/**************************************************************************************************
 * Probes the device and populates related data structures with target flash geometry data
 *************************************************************************************************/
static int psoc6_probe(struct flash_bank *bank)
{
	struct target *tgt = bank->target;
	struct psoc6_target_info_s *psoc6_info = bank->driver_priv;

	int hr = ERROR_OK;

	static uint32_t g_silicon_id;
	static uint8_t g_protection;

	if (g_silicon_id == 0 && bank->target->coreid != 0) {
		LOG_ERROR("Please attach to CM0 core first!");
		return ERROR_FAIL;
	}

	if (bank->target->coreid == 0) {
		hr = device_acquire(tgt);
		if (hr != ERROR_OK)
			return hr;

		hr = get_silicon_id(tgt, &g_silicon_id, &g_protection);
		if (hr != ERROR_OK)
			return hr;
	}

	psoc6_info->protection = g_protection;
	psoc6_info->silicon_id = g_silicon_id;

	g_current_device = &psoc6_devices[0];
	while (g_current_device->silicon_id) {
		if (g_current_device->silicon_id == g_silicon_id)
			break;
		g_current_device++;
	}

	if (g_current_device->silicon_id == 0) {
		LOG_ERROR("Unknown PSoC6 device! SiliconID = 0x%08X", g_silicon_id);
		return ERROR_TARGET_INVALID;
	}

	LOG_DEBUG("Detected device: %s, SiliconID: 0x%08X, Flash Bank: %s",
		g_current_device->mpn_str,
		g_current_device->silicon_id,
		bank->name);

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}

	uint32_t base_addr;
	size_t bank_size;

	if (strstr(bank->name, "main_flash") == bank->name) {
		base_addr = MEM_BASE_MFLASH;
		bank_size = 1024 * g_current_device->mainfl_kb;
	} else if (strstr(bank->name, "work_flash") == bank->name) {
		base_addr = MEM_BASE_WFLASH;
		bank_size = 1024 * g_current_device->workfl_kb;
	} else if (strstr(bank->name, "super_flash_user") == bank->name) {
		base_addr = safe_sflash_regions[SFLASH_USER_INDEX].addr;
		bank_size = safe_sflash_regions[SFLASH_USER_INDEX].size;
	} else if (strstr(bank->name, "super_flash_nar") == bank->name) {
		base_addr = safe_sflash_regions[SFLASH_NAR_INDEX].addr;
		bank_size = safe_sflash_regions[SFLASH_NAR_INDEX].size;
	} else if (strstr(bank->name, "super_flash_key") == bank->name) {
		base_addr = safe_sflash_regions[SFLASH_KEY_INDEX].addr;
		bank_size = safe_sflash_regions[SFLASH_KEY_INDEX].size;
	} else if (strstr(bank->name, "super_flash_toc2") == bank->name) {
		base_addr = safe_sflash_regions[SFLASH_TOC2_INDEX].addr;
		bank_size = safe_sflash_regions[SFLASH_TOC2_INDEX].size;
	} else {
		LOG_ERROR("Unknown flash type given, should be 'main_flash', 'work_flash', 'super_flash_user', "
			"'super_flash_nar', 'super_flash_key' or 'super_flash_toc2' suffixed by '_cm0' or '_cm4'");

		return ERROR_FLASH_BANK_INVALID;
	}

	size_t num_sectors = bank_size / FLASH_ROW_SIZE;
	bank->base = base_addr;
	bank->size = bank_size;
	bank->chip_width = 4;
	bank->bus_width = 4;
	bank->erased_value = 0;
	bank->default_padded_value = 0;

	bank->num_sectors = num_sectors;
	bank->sectors = calloc(num_sectors, sizeof(struct flash_sector));
	for (size_t i = 0; i < num_sectors; i++) {
		bank->sectors[i].size = FLASH_ROW_SIZE;
		bank->sectors[i].offset = i * FLASH_ROW_SIZE;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = -1;
	}

	if (bank->target->coreid == 0)
		hr = device_release(tgt);

	psoc6_info->is_probed = 1;

	return hr;
}

/**************************************************************************************************
 * Probes target device only if it hasnt been probed yet
 *************************************************************************************************/
static int psoc6_auto_probe(struct flash_bank *bank)
{
	struct psoc6_target_info_s *psoc6_info = bank->driver_priv;
	int hr;

	if (psoc6_info->is_probed)
		hr = ERROR_OK;
	else
		hr = psoc6_probe(bank);

	return hr;
}

/**************************************************************************************************
 * Returns true if flash bank name represents Supervisory Flash
 *************************************************************************************************/
static bool is_sflash_bank(struct flash_bank *bank)
{
	const bool is_sflash = (strstr(bank->name, "super_flash_user") == bank->name) || \
		(strstr(bank->name, "super_flash_nar") == bank->name) || \
		(strstr(bank->name, "super_flash_key") == bank->name) || \
		(strstr(bank->name, "super_flash_toc2") == bank->name);

	return is_sflash;
}

/**************************************************************************************************
 * Erases set of sectors on target device
 *************************************************************************************************/
static int psoc6_erase_sectors(struct flash_bank *bank, int first, int last)
{
	struct target *tgt = bank->target;
	int hr;
	struct working_area *wa;

	if (is_sflash_bank(bank)) {
		LOG_INFO("Erase operation on Supervisory Flash is not required, skipping");
		return ERROR_OK;
	}

	hr = device_acquire(tgt);
	if (hr != ERROR_OK)
		return hr;

	hr = target_alloc_working_area(tgt, FLASH_ROW_SIZE + 32, &wa);
	if (hr != ERROR_OK)
		goto exit;

	while (((first % FLASH_ROW_SIZE) == 0) &&
		((last - first + 1) >= (int)FLASH_NUM_ROWS_IN_SECTOR)) {
		uint32_t addr = bank->base + first * bank->sectors[0].size;
		LOG_DEBUG("Erasing flash SECTOR '%s' @0x%08X", bank->name, addr);

		hr = target_write_u32(tgt, wa->address, SROMAPI_ERASESECTOR_REQ);
		if (hr != ERROR_OK)
			goto exit_free_wa;

		hr = target_write_u32(tgt, wa->address + 0x04, addr);
		if (hr != ERROR_OK)
			goto exit_free_wa;

		uint32_t dataOut;
		hr = call_sromapi(tgt, SROMAPI_ERASESECTOR_REQ, wa->address, &dataOut);
		if (hr != ERROR_OK) {
			LOG_ERROR("Sector \"%d\" not erased!", first / FLASH_NUM_ROWS_IN_SECTOR);
			goto exit_free_wa;
		}

		first += FLASH_NUM_ROWS_IN_SECTOR;
	}

	for (int i = first; i <= last; i++) {
		uint32_t addr = bank->base + i * bank->sectors[0].size;
		LOG_DEBUG("Erasing flash ROW '%s' @0x%08X", bank->name, addr);

		hr = target_write_u32(tgt, wa->address, SROMAPI_ERASEROW_REQ);
		if (hr != ERROR_OK)
			goto exit_free_wa;

		hr = target_write_u32(tgt, wa->address + 0x04, addr);
		if (hr != ERROR_OK)
			goto exit_free_wa;

		uint32_t dataOut;
		hr = call_sromapi(tgt, SROMAPI_ERASEROW_REQ, wa->address, &dataOut);
		if (hr != ERROR_OK) {
			LOG_ERROR("Row \"%d\" not erased!", i);
			goto exit_free_wa;
		}
	}

exit_free_wa:
	target_free_working_area(tgt, wa);
exit:
	device_release(tgt);

	return hr;
}


/**************************************************************************************************
 * Programs single Flash Row
 *************************************************************************************************/
static int psoc6_program_row(struct target *tgt,
	uint32_t addr,
	const uint8_t *buffer,
	uint32_t count,
	bool is_sflash)
{
	struct working_area *wa;
	const uint32_t sromapi_req = is_sflash ? SROMAPI_WRITEROW_REQ : SROMAPI_PROGRAMROW_REQ;
	uint32_t dataOut;
	int hr = ERROR_OK;

	hr = target_alloc_working_area(tgt, FLASH_ROW_SIZE + 32, &wa);
	if (hr != ERROR_OK)
		goto exit;

	hr = target_write_u32(tgt, wa->address, sromapi_req);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	hr = target_write_u32(tgt,
			wa->address + 0x04,
			0x106);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	hr = target_write_u32(tgt, wa->address + 0x08, addr);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	hr = target_write_u32(tgt, wa->address + 0x0C, wa->address + 0x10);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	hr = target_write_buffer(tgt, wa->address + 0x10, count, buffer);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	hr = call_sromapi(tgt, sromapi_req, wa->address, &dataOut);

exit_free_wa:
	target_free_working_area(tgt, wa);

exit:
	return hr;
}


/**************************************************************************************************
 * Programs set of Rows
 *************************************************************************************************/
static int psoc6_program(struct flash_bank *bank,
	const uint8_t *buffer,
	uint32_t offset,
	uint32_t count)
{
	struct target *target = bank->target;
	const bool is_sflash = is_sflash_bank(bank);
	int hr;

	hr = device_acquire(target);
	if (hr != ERROR_OK)
		return hr;

	uint32_t remaining = count;
	uint8_t page_buf[FLASH_ROW_SIZE];
	uint32_t addr, size, sourceOffset, maxAdressSize;

	sourceOffset = 0;
	addr = bank->base + offset;
	maxAdressSize = (addr + count);
	while (addr < maxAdressSize) {
		LOG_DEBUG("Writing data to 0x%08X ...", addr);

		size = FLASH_ROW_SIZE;
		if (remaining < FLASH_ROW_SIZE) {
			memset(page_buf, 0x00, size);
			memcpy(page_buf, &buffer[sourceOffset], remaining);
			size = remaining;
		} else
			memcpy(page_buf, &buffer[sourceOffset], size);

		hr = psoc6_program_row(target, addr, page_buf, size, is_sflash);
		if (hr != ERROR_OK) {
			LOG_ERROR("Failed to program Flash at address 0x%08X", addr);
			break;
		}

		sourceOffset += size;
		addr = addr + size;
		remaining -= size;
	}

	hr = device_release(target);
	return hr;
}

/**************************************************************************************************
 * Performs Mass Erase of Main Flash region
 *************************************************************************************************/
COMMAND_HANDLER(psoc6_handle_mass_erase_command)
{
	if (CMD_ARGC)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *tgt = get_current_target(CMD_CTX);
	struct working_area *wa;
	uint32_t dataOut;
	int hr;

	command_print(CMD_CTX, "Performing Mass Erase...");
	hr = device_acquire(tgt);
	if (hr != ERROR_OK)
		goto exit;

	hr = target_alloc_working_area(tgt, FLASH_ROW_SIZE + 32, &wa);
	if (hr != ERROR_OK)
		goto exit;

	hr = target_write_u32(tgt, wa->address, SROMAPI_ERASEALL_REQ);
	if (hr != ERROR_OK)
		goto exit_free_wa;

	hr = call_sromapi(tgt, SROMAPI_ERASEALL_REQ, wa->address, &dataOut);
	if (hr != ERROR_OK)
		goto exit_free_wa;

exit_free_wa:
	if (hr == ERROR_OK)
		command_print(CMD_CTX, "Success!");
	else
		command_print(CMD_CTX, "Fail!");

	device_release(tgt);
	target_free_working_area(tgt, wa);

exit:
	return hr;
}

FLASH_BANK_COMMAND_HANDLER(psoc6_flash_bank_command)
{
	struct psoc6_target_info_s *psoc6_info;
	int hr = ERROR_OK;

	if (CMD_ARGC < 6)
		hr = ERROR_COMMAND_SYNTAX_ERROR;
	else {
		psoc6_info = calloc(1, sizeof(struct psoc6_target_info_s));

		psoc6_info->info = g_current_device;
		psoc6_info->is_probed = 0;
		bank->driver_priv = psoc6_info;
	}
	return hr;
}

static const struct command_registration psoc6_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.handler = psoc6_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.usage = NULL,
		.help = "Erase entire Main Flash",
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
	.erase = psoc6_erase_sectors,
	.protect = psoc6_protect,
	.write = psoc6_program,
	.read = default_flash_read,
	.probe = psoc6_probe,
	.auto_probe = psoc6_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = psoc6_protect_check,
	.info = psoc6_get_info,
};
