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

#ifndef __BDM_H__
#define __BDM_H__

/*
 * Here's the list of documents describing how BDM works:
 *
 *  - MC9S12XEP100RMV1.pdf, "MC9S12XE-Family Reference Manual",
 *    Chapter 7, "Background Debug Module (S12XBDMV2)" specifically.
 *
 *  - S12BDMV4.pdf, "Background Debug Module (BDM) V4"
 */

/*
   HACK: This is a hack to allow to access CPU memory using both
   globabl and local addresses. Using this local addresses are "mapped"
   to area from 0x80000000 to 0x8000FFFF
 */
#define BDM_LOCAL_ADDR(a)  (0x80000000 | (a))

/**
 * BDM registers. For description see section 7.3.2 of
 * "MC9S12XE-Family Reference Manual"
 */
enum bdm_registers {
	BDM_REG_BDMSTS		= 0x7FFF01,
	BDM_REG_BDMCCRL		= 0x7FFF06,
	BDM_REG_BDMCCRH		= 0x7FFF07,
	BDM_REG_BDMGPR		= 0x7FFF08,
	BDM_REG_FAMILY_ID	= 0x7FFF0F,
};


/******************************************************************************
 *           BDM Hardware commands(accessible with in both
 *                    BDM acive and inactive modes)
 ******************************************************************************/

/**
 * Issues BACKGROUND command to the target.
 *
 * @param target The target to issue BDM command to
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_background(struct target *target);

/**
 * Issues ACK_ENABLE command to the target.
 *
 * @param target The target to issue BDM command to
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_ack_enable(struct target *target);

/**
 * Issues READ_BD_BYTE command to the target, which reads a byte from
 * memory with BDM firmware address space mapped in.
 *
 * This function is inteded to be used to access BDM registers, to
 * access anything else bdm_read_byte should be used. This is BDM
 * hardware command availible both in active and incative BDM modes.
 *
 * @param target The target to issue BDM command to
 * @param reg One of the BDM registers
 * @returns Negative error code on failure, retrived value on success
 */
int_least16_t bdm_read_bd_byte(struct target *target, enum bdm_registers reg);

/**
 * Issues READ_BD_WORD command to the target, which reads a word from
 * memory with BDM firmware address space mapped in. Read address
 * must be aligned on the word boundary.
 *
 * This function is inteded to be used to access BDM registers, to
 * access anything else bdm_read_word should be used. This is BDM
 * hardware command availible both in active and incative BDM modes.
 *
 * @param target The target to issue BDM command to
 * @param reg One of the BDM registers
 * @returns Negative error code on failure, retrived value on success
 */
int_least32_t bdm_read_bd_word(struct target *target, enum bdm_registers reg);

/**
 * Issues WRITE_BD_BYTE command to the target, which writes a byte to
 * memory with BDM firmware address space mapped in.
 *
 * This function is inteded to be used to access BDM registers, to
 * access anything else bdm_write_byte should be used. This is BDM
 * hardware command availible both in active and incative BDM modes.
 *
 * @param target The target to issue BDM command to
 * @param reg One of the BDM registers
 * @param value The value to put into @reg
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_write_bd_byte(struct target *target, enum bdm_registers reg,
		      uint8_t value);

/**
 * Issues WRITE_BD_WORD command to the target, which writes a word to
 * memory with BDM firmware address space mapped in. Write address
 * must be aligned on the word boundary.
 *
 * This function is inteded to be used to access BDM registers, to
 * access anything else bdm_write_word should be used. This is BDM
 * hardware command availible both in active and incative BDM modes.
 *
 * @param target The target to issue BDM command to
 * @param reg One of the BDM registers
 * @param value The value to put into @reg
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_write_bd_word(struct target *target, enum bdm_registers reg,
		      uint16_t value);

/**
 * Issues READ_BYTE command to the target, which reads a byte from
 * memory with BDM firmware address space mapped out.
 *
 * @param target The target to issue BDM command to
 * @param address Address to read data from
 * @returns Negative error code on failure, retrived value on success
 */
int_least16_t bdm_read_byte(struct target *target, uint32_t address);

/**
 * Issues READ_WORD command to the target, which reads a word from
 * memory at @address with BDM firmware address space mapped out. Note
 * that @address must be aligned.
 *
 * @param target The target to issue BDM command to
 * @param address Address to read data from
 * @returns Negative error code on failure, retrived value on success
 */
int_least32_t bdm_read_word(struct target *target, uint32_t address);

/**
 * Issues WRITE_BYTE command to the target, which reads a byte from
 * memory with BDM firmware address space mapped out.
 *
 * @param target The target to issue BDM command to
 * @param address Address to write the data to
 * @parma value Value to write
 * @returns Negative error code on failure, ERROR_OK on success
 */
int bdm_write_byte(struct target *target, uint32_t address, uint8_t value);

/**
 * Issues WRITE_WORD command to the target, which writes @value to
 * memory at @address with BDM firmware address space mapped out.
 * Note that @address must be aligned.
 *
 * @param target The target to issue BDM command to
 * @param address Address to write the data to
 * @parma value Value to write
 * @returns Negative error code on failure, ERROR_OK on success
 */
int bdm_write_word(struct target *target, uint32_t address, uint16_t value);

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
int_least32_t bdm_read_pc(struct target *target);

/**
 * Issues READ_SP command to the target to read the contents of the
 * stack pointer.
 *
 * @param target The target to issue BDM command to
 * @returns The value of SP register on success, negative error code
 * on failure
 */
int_least32_t bdm_read_sp(struct target *target);

/**
 * Issues READ_D command to the target to read the contents of the
 * D register.
 *
 * @param target The target to issue BDM command to
 * @returns The value of D register on success, negative error code on
 * failure
 */
int_least32_t bdm_read_d(struct target *target);

/**
 * Issues READ_X command to the target to read the contents of the
 * X register.
 *
 * @param target The target to issue BDM command to
 * @returns The value of X register on success, negative error code on
 * failure
 */
int_least32_t bdm_read_x(struct target *target);

/**
 * Issues READ_Y command to the target to read the contents of the
 * Y register.
 *
 * @param target The target to issue BDM command to
 * @returns The value of Y register on success, negative error code on
 * failure
 */
int_least32_t bdm_read_y(struct target *target);

/**
 * Issues WRITE_PC command to the target to set the contents of the
 * program counter.
 *
 * @param target The target to issue BDM command to
 * @param value Value to write to PC register
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_write_pc(struct target *target, uint16_t value);

/**
 * Issues WRITE_SP command to the target to set the contents of the
 * stack pointer.
 *
 * @param target The target to issue BDM command to
 * @param value Value to write to SP register
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_write_sp(struct target *target, uint16_t value);

/**
 * Issues WRITE_D command to the target to set the contents of D
 * register.
 *
 * @param target The target to issue BDM command to
 * @param value Value to write to D register
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_write_d(struct target *target, uint16_t value);

/**
 * Issues WRITE_X command to the target to set the contents of X
 * register.
 *
 * @param target The target to issue BDM command to
 * @param value Value to write to X register
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_write_x(struct target *target, uint16_t value);

/**
 * Issues WRITE_Y command to the target to set the contents of Y
 * register.
 *
 * @param target The target to issue BDM command to
 * @param value Value to write to Y register
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_write_y(struct target *target, uint16_t value);

/**
 * Issues GO command to the target which causes it to get out of
 * active BDM mode and continue normal program exceution.
 *
 * @param target The target to issue BDM command to
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_go(struct target *target);

/**
 * Issues GO command to the target which causes it to get out of
 * active BDM mode, execute one instruction of the program code and
 * return back to active BDM mode.
 *
 * @param target The target to issue BDM command to
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_trace1(struct target *target);

/******************************************************************************
 *              Dongle specific commands mandatory commands
 ******************************************************************************/

/**
 * Asserts(active low) reset pin of the BDM dongle
 *
 * @param target The target
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_assert_reset(struct target *target);

/**
 * Deasserts reset pin of the BDM dongle
 *
 * @param target The target
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_deassert_reset(struct target *target);

/**
 * Asserts(active low) BKGD/MODC pin of the BDM dongle
 *
 * @param target The target
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_assert_bknd(struct target *target);

/**
 * Deasserts BKGD/MODC pin of the BDM dongle.
 *
 * @param target The target
 * @returns ERROR_OK on success, negative error code on failure
 */
int bdm_deassert_bknd(struct target *target);

/******************************************************************************
 *         Dongle specific commands non-mandatory, convinience commands
 ******************************************************************************/

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
int bdm_read_memory(struct target *target, uint32_t address,
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
int bdm_write_memory(struct target *target, uint32_t address,
		     uint32_t size, uint32_t count, const uint8_t *buffer);

enum bdm_reg_bdmsts_bits {
	BDM_REG_BDMSTS_ENBDM	= (1 << 7),
	BDM_REG_BDMSTS_BDMACT	= (1 << 6),
	BDM_REG_BDMSTS_SDV	= (1 << 4),
	BDM_REG_BDMSTS_TRACE	= (1 << 3),
	BDM_REG_BDMSTS_CLKSW	= (1 << 2),
	BDM_REG_BDMSTS_UNSEC	= (1 << 1),
};

enum bdm_reg_bdmgpr_bits {
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
struct bdm {
	/*
	   Hardware commands
	 */
	int (*background)(struct bdm *);
	int (*ack_enable)(struct bdm *);
	int_least16_t (*read_bd_byte)(struct bdm *, uint32_t);
	int_least32_t (*read_bd_word)(struct bdm *, uint32_t);
	int_least16_t (*read_byte)(struct bdm *, uint32_t);
	int_least32_t (*read_word)(struct bdm *, uint32_t);
	int (*write_bd_byte)(struct bdm *, uint32_t, uint8_t);
	int (*write_bd_word)(struct bdm *, uint32_t, uint16_t);
	int (*write_byte)(struct bdm *, uint32_t, uint8_t);
	int (*write_word)(struct bdm *, uint32_t, uint16_t);

	/*
	   Firmware commands
	 */
	int_least32_t (*read_next)(struct bdm *);
	int_least32_t (*read_pc)(struct bdm *);
	int_least32_t (*read_d)(struct bdm *);
	int_least32_t (*read_x)(struct bdm *);
	int_least32_t (*read_y)(struct bdm *);
	int_least32_t (*read_sp)(struct bdm *);

	int (*write_next)(struct bdm *, uint16_t);
	int (*write_pc)(struct bdm *, uint16_t);
	int (*write_d)(struct bdm *, uint16_t);
	int (*write_x)(struct bdm *, uint16_t);
	int (*write_y)(struct bdm *, uint16_t);
	int (*write_sp)(struct bdm *, uint16_t);

	int (*go)(struct bdm *);
	int (*go_until)(struct bdm *);
	int (*trace1)(struct bdm *);

	/*
	  Dongle firmware commands(not a part of BDM specification)
	 */
	int (*assert_reset)(struct bdm *);
	int (*deassert_reset)(struct bdm *);
	int (*assert_bknd)(struct bdm *);
	int (*deassert_bknd)(struct bdm *);


	int (*read_memory)(struct bdm *, uint32_t, uint32_t, uint32_t, uint8_t *);
	int (*write_memory)(struct bdm *, uint32_t, uint32_t, uint32_t, const uint8_t *);
};

#endif
