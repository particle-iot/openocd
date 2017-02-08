/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2017 by Angelo Dureghello                               *
 *   angelo@sysam.it                                                       *
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

#ifndef __BDM_CF26_H__
#define __BDM_CF26_H__


enum pemu_commands {
	CMD_BDM_GO = 0x0c00,
	CMD_BDM_READ_CPU_AD_REG = 0x2180,
	CMD_BDM_READ_BDM_REG = 0x2d80,
	CMD_BDM_READ_SCM_REG = 0x2980,
	CMD_BDM_READ_MEM_BYTE = 0x1900,
	CMD_BDM_READ_MEM_WORD = 0x1940,
	CMD_BDM_READ_MEM_LONG = 0x1980,
	CMD_BDM_WRITE_CPU_AD_REG = 0x2080,
	CMD_BDM_WRITE_BDM_REG = 0x2c80,
	CMD_BDM_WRITE_SCM_REG = 0x2880,
	CMD_BDM_WRITE_MEM_BYTE = 0x1800,
	CMD_BDM_WRITE_MEM_WORD = 0x1840,
	CMD_BDM_WRITE_MEM_LONG = 0x1880,
};

/**
 * BDM registers
 */
enum bdm_cf26_registers {
	BDM_REG_CSR = 0x00,
	BDM_REG_XCSR = 0x01,
};

/*
 * system control register, BDM access
 */
enum cf_sysctl_registers {
	SYSC_CACR = 0x02,
	SYSC_PC = 0x80f,
};

/**
 * Issues READ BDM debug module register
 *
 * @param target The target to issue BDM command to
 * @param reg One of the debug module registers
 * @returns Negative error code on failure, retrived value on success
 */
int_least32_t bdm_cf26_read_dm_reg(struct target *target,
					enum bdm_cf26_registers reg);

/**
 * Issues READ cpu AD register
 *
 * @param target The target to issue BDM command to
 * @param reg One of the A/D cpu registers
 * @returns Negative error code on failure, retrived value on success
 */
int_least32_t bdm_cf26_read_ad_reg(struct target *target,
					enum bdm_cf26_registers reg);


/**
 * Issues READ system register
 *
 * @param target The target to issue BDM command to
 * @param reg One of the system registers
 * @returns Negative error code on failure, retrived value on success
 */
int_least32_t bdm_cf26_read_sc_reg(struct target *target,
					enum cf_sysctl_registers reg);

/**
 * Issues bdm READ memory byte
 *
 * @param target The target to issue BDM command to
 * @param address The target memory address
 * @returns Negative error code on failure, retrived value on success
 */
int_least8_t bdm_cf26_read_mem_byte(struct target *target,
					uint32_t address);

/**
 * Issues bdm READ memory word
 *
 * @param target The target to issue BDM command to
 * @param address The target memory address
 * @returns Negative error code on failure, retrived value on success
 */
int_least16_t bdm_cf26_read_mem_word(struct target *target, uint32_t address);

/**
 * Issues bdm READ memory long word
 *
 * @param target The target to issue BDM command to
 * @param address The target memory address
 * @returns Negative error code on failure, retrived value on success
 */
int_least32_t bdm_cf26_read_mem_long(struct target *target, uint32_t address);

/**
 * Issues WRITE BDM debug module register
 *
 * @param target The target to issue BDM command to
 * @param reg One of the debug module registers
 * @returns Negative error code on failure, retrived value on success
 */
int bdm_cf26_write_dm_reg(struct target *, enum bdm_cf26_registers reg,
					uint32_t value);

/**
 * Issues WRITE cpu A/D register
 *
 * @param target The target to issue BDM command to
 * @param reg One of the A/D cpu registers
 * @returns Negative error code on failure, retrived value on success
 */
int bdm_cf26_write_ad_reg(struct target *, enum bdm_cf26_registers reg,
					uint32_t value);

/**
 * Issues WRITE systemctrl reg
 *
 * @param target The target to issue BDM command to
 * @param reg One of the system registers
 * @returns Negative error code on failure, retrived value on success
 */
int bdm_cf26_write_sc_reg(struct target *, enum cf_sysctl_registers reg,
					uint32_t value);

/**
 * Issues bdm WRITE memory byte
 *
 * @param target The target to issue BDM command to
 * @param address The target memory address
 * @param value The value to write
 * @returns Negative error code on failure, retrived value on success
 */
int bdm_cf26_write_mem_byte(struct target *target, uint32_t address,
		      uint8_t value);

/**
 * Issues bdm WRITE memory word
 *
 * @param target The target to issue BDM command to
 * @param address The target memory address
 * @param value The value to write
 * @returns Negative error code on failure, retrived value on success
 */
int bdm_cf26_write_mem_word(struct target *target, uint32_t address,
		      uint16_t value);

/**
 * Issues bdm WRITE memory long word
 *
 * @param target The target to issue BDM command to
 * @param address The target memory address
 * @param value The value to write
 * @returns Negative error code on failure, retrived value on success
 */
int bdm_cf26_write_mem_long(struct target *target, uint32_t address,
		      uint32_t value);

/**
 * Dongle specific commands
 */

/**
 * Asserts(active low) reset pin of the BDM dongle
 *
 * @param target The target
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_cf26_assert_reset(struct target *target);

/**
 * Deasserts reset pin of the BDM dongle
 *
 * @param target The target
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_cf26_deassert_reset(struct target *target);

/**
 * Halt cpu
 *
 * @param target The target
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_cf26_halt(struct target *target);


/******************************************************************************
 *        BDM Firmware commands(accessible only in BDM acive mode)
 ******************************************************************************/

/**
 * Issues READ_PC command to the target to read the contents of the
 * program counter.
 *
 * @param target The target to issue BDM command to
 * @returns The value of PC register on success, negative error code
 * on failure
 */
int_least32_t bdm_cf26_read_pc(struct target *target);

/**
 * Issues READ_SP command to the target to read the contents of the
 * stack pointer.
 *
 * @param target The target to issue BDM command to
 * @returns The value of SP register on success, negative error code
 * on failure
 */
int_least32_t bdm_cf26_read_sp(struct target *target);

/**
 * Issues WRITE_PC command to the target to set the contents of the
 * program counter.
 *
 * @param target The target to issue BDM command to
 * @param value Value to write to PC register
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_cf26_write_pc(struct target *target, uint32_t value);

/**
 * Issues WRITE_SP command to the target to set the contents of the
 * stack pointer.
 *
 * @param target The target to issue BDM command to
 * @param value Value to write to SP register
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_cf26_write_sp(struct target *target, uint16_t value);

/**
 * Issues GO command to the target which causes it to get out of
 * active BDM mode and continue normal program exceution.
 *
 * @param target The target to issue BDM command to
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_cf26_go(struct target *target);

/******************************************************************************
 *         Dongle specific commands non-mandatory, convinience commands
 ******************************************************************************/

/**
 * Reads all cpu regs
 *
 * @param target The target to issue BDM command to
 * @param reg_buff buffer with all registers
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_cf26_get_all_cpu_regs(struct target *target, uint8_t **reg_buff);

/**
 * Reads a block of target's memory(BDM not mapped)
 *
 * @param target The target to issue BDM command to
 * @param address Start of the read block address
 * @param size Size of the individual element to read
 * @param count Number of individual elemnets to read
 * @param buffer Buffer to read the data to
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_cf26_read_memory(struct target *target, uint32_t address,
		    uint32_t size, uint32_t count, uint8_t *buffer);

/**
 * Write a block to target's memory(BDM not mapped)
 *
 * @param target The target to issue BDM command to
 * @param address Start of the read block address
 * @param size Size of the individual element to read
 * @param count Number of individual elemnets to read
 * @param buffer Buffer to read the data to
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_cf26_write_memory(struct target *target, uint32_t address,
		     uint32_t size, uint32_t count, const uint8_t *buffer);

enum bdm_cf26_reg_bdmsts_bits {
	BDM_REG_BDMSTS_ENBDM	= (1 << 7),
	BDM_REG_BDMSTS_BDMACT	= (1 << 6),
	BDM_REG_BDMSTS_SDV	= (1 << 4),
	BDM_REG_BDMSTS_TRACE	= (1 << 3),
	BDM_REG_BDMSTS_CLKSW	= (1 << 2),
	BDM_REG_BDMSTS_UNSEC	= (1 << 1),
};

enum bdm_cf26_reg_bdmgpr_bits {
	BDM_REG_BDMGPR_BGAE = (1 << 7),
};

/**
 * Represents a generic BDM adapter
 *
 * This a vtable of sorts and is not expected to be used on its own.
 * Specific adapter drivers should incorporate this structure as a
 * part of the driver-specific strucure(should be placed at the very
 * beginning)
 */
struct bdm_cf26_driver {
	/**
	 * Hardware commands
	 */
	int_least32_t (*read_dm_reg)(const struct bdm_cf26_driver *, uint8_t);
	int_least32_t (*read_ad_reg)(const struct bdm_cf26_driver *, uint8_t);
	int_least32_t (*read_sc_reg)(const struct bdm_cf26_driver *, uint16_t);
	int_least8_t (*read_mem_byte)(const struct bdm_cf26_driver *, uint32_t);
	int_least16_t (*read_mem_word)(const struct bdm_cf26_driver *, uint32_t);
	int_least32_t (*read_mem_long)(const struct bdm_cf26_driver *, uint32_t);

	int (*write_dm_reg)(const struct bdm_cf26_driver *, uint8_t, uint32_t);
	int (*write_ad_reg)(const struct bdm_cf26_driver *, uint8_t, uint32_t);
	int (*write_sc_reg)(const struct bdm_cf26_driver *, uint16_t, uint32_t);
	int (*write_mem_byte)(const struct bdm_cf26_driver *, uint32_t, uint8_t);
	int (*write_mem_word)(const struct bdm_cf26_driver *, uint32_t, uint16_t);
	int (*write_mem_long)(const struct bdm_cf26_driver *, uint32_t, uint32_t);

	/*
	  Dongle firmware commands(not a part of BDM specification)
	 */
	int (*assert_reset)(const struct bdm_cf26_driver *);
	int (*deassert_reset)(const struct bdm_cf26_driver *);
	int (*halt)(const struct bdm_cf26_driver *);

	int (*read_memory)(const struct bdm_cf26_driver *, uint32_t, uint32_t, uint32_t, uint8_t *);
	int (*write_memory)(const struct bdm_cf26_driver *, uint32_t, uint32_t, uint32_t, const uint8_t *);

	/*
	   Firmware commands
	 */
	int_least32_t (*read_pc)(const struct bdm_cf26_driver *);
	int (*write_pc)(const struct bdm_cf26_driver *, uint32_t);
	int (*go)(const struct bdm_cf26_driver *);
	int (*get_all_cpu_regs)(const struct bdm_cf26_driver *, uint8_t **);
};

#endif
