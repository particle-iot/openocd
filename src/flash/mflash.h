/***************************************************************************
 *   Copyright (C) 2007-2008 by unsik Kim <donari75@gmail.com>             *
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

#ifndef _MFLASH_H
#define _MFLASH_H

struct command_context;

struct mflash_gpio_num {
	char port[2];
	signed short num;
};

struct mflash_gpio_drv {
	const char *name;
	int (*set_gpio_to_output)(struct mflash_gpio_num gpio);
	int (*set_gpio_output_val)(struct mflash_gpio_num gpio, uint8_t val);
};

struct mg_io_type_drv_info {

	uint16_t general_configuration;				/* 00 */
	uint16_t number_of_cylinders;				/* 01 */
	uint16_t reserved1;							/* 02 */
	uint16_t number_of_heads;					/* 03 */
	uint16_t unformatted_bytes_per_track;		/* 04 */
	uint16_t unformatted_bytes_per_sector;		/* 05 */
	uint16_t sectors_per_track;					/* 06 */
	uint16_t vendor_unique1[3];					/* 07/08/09 */

	uint8_t serial_number[20];					/* 10~19 */

	uint16_t buffer_type;						/* 20 */
	uint16_t buffer_sector_size;				/* 21 */
	uint16_t number_of_ecc_bytes;				/* 22 */

	uint8_t firmware_revision[8];				/* 23~26 */
	uint8_t model_number[40];					/* 27 */

	uint8_t maximum_block_transfer;			/* 47 low byte */
	uint8_t vendor_unique2;					/* 47 high byte */
	uint16_t dword_io;							/* 48 */

	uint16_t capabilities;						/* 49 */
	uint16_t reserved2;							/* 50 */

	uint8_t vendor_unique3;					/* 51 low byte */
	uint8_t pio_cycle_timing_mode;				/* 51 high byte */
	uint8_t vendor_unique4;					/* 52 low byte */
	uint8_t dma_cycle_timing_mode;				/* 52 high byte */
	uint16_t translation_fields_valid;			/* 53 (low bit) */
	uint16_t number_of_current_cylinders;		/* 54 */
	uint16_t number_of_current_heads;			/* 55 */
	uint16_t current_sectors_per_track;			/* 56 */
	uint16_t current_sector_capacity_lo;		/* 57 & 58 */
	uint16_t current_sector_capacity_hi;		/* 57 & 58 */
	uint8_t multi_sector_count;					/* 59 low */
	uint8_t multi_sector_setting_valid;			/* 59 high (low bit) */

	uint16_t total_user_addressable_sectors_lo;	/* 60 & 61 */
	uint16_t total_user_addressable_sectors_hi;	/* 60 & 61 */

	uint8_t single_dma_modes_supported;			/* 62 low byte */
	uint8_t single_dma_transfer_active;			/* 62 high byte */
	uint8_t multi_dma_modes_supported;			/* 63 low byte */
	uint8_t multi_dma_transfer_active;			/* 63 high byte */
	uint16_t adv_pio_mode;
	uint16_t min_dma_cyc;
	uint16_t recommend_dma_cyc;
	uint16_t min_pio_cyc_no_iordy;
	uint16_t min_pio_cyc_with_iordy;
	uint8_t reserved3[22];
	uint16_t major_ver_num;
	uint16_t minor_ver_num;
	uint16_t feature_cmd_set_suprt0;
	uint16_t feature_cmd_set_suprt1;
	uint16_t feature_cmd_set_suprt2;
	uint16_t feature_cmd_set_en0;
	uint16_t feature_cmd_set_en1;
	uint16_t feature_cmd_set_en2;
	uint16_t reserved4;
	uint16_t req_time_for_security_er_done;
	uint16_t req_time_for_enhan_security_er_done;
	uint16_t adv_pwr_mgm_lvl_val;
	uint16_t reserved5;
	uint16_t re_of_hw_rst;
	uint8_t reserved6[68];
	uint16_t security_stas;
	uint8_t vendor_uniq_bytes[62];
	uint16_t cfa_pwr_mode;
	uint8_t reserved7[186];

	uint16_t scts_per_secure_data_unit;
	uint16_t integrity_word;

};

struct mg_pll {
	unsigned int lock_cyc;
	unsigned short feedback_div;	/* 9bit divider */
	unsigned char input_div;	/* 5bit divider */
	unsigned char output_div;	/* 2bit divider */
};

struct mg_drv_info {
	struct mg_io_type_drv_info drv_id;
	uint32_t tot_sects;
};

struct mflash_bank {
	uint32_t base;

	struct mflash_gpio_num rst_pin;

	struct mflash_gpio_drv *gpio_drv;
	struct target *target;
	struct mg_drv_info *drv_info;
};

int mflash_register_commands(struct command_context *cmd_ctx);

#define MG_MFLASH_SECTOR_SIZE           (0x200)		/* 512Bytes = 2^9 */
#define MG_MFLASH_SECTOR_SIZE_MASK      (0x200-1)
#define MG_MFLASH_SECTOR_SIZE_SHIFT     (9)

#define MG_BUFFER_OFFSET        0x8000
#define MG_REG_OFFSET           0xC000
#define MG_REG_FEATURE          0x2		/* write case */
#define MG_REG_ERROR            0x2		/* read case */
#define MG_REG_SECT_CNT         0x4
#define MG_REG_SECT_NUM         0x6
#define MG_REG_CYL_LOW          0x8
#define MG_REG_CYL_HIGH         0xA
#define MG_REG_DRV_HEAD         0xC
#define MG_REG_COMMAND          0xE		/* write case */
#define MG_REG_STATUS           0xE		/* read  case */
#define MG_REG_DRV_CTRL         0x10
#define MG_REG_BURST_CTRL       0x12

#define MG_OEM_DISK_WAIT_TIME_LONG              15000	/* msec */
#define MG_OEM_DISK_WAIT_TIME_NORMAL    3000	/* msec */
#define MG_OEM_DISK_WAIT_TIME_SHORT             1000	/* msec */

#define MG_PLL_CLK_OUT 66000000.0	/* 66Mhz */
#define MG_PLL_MAX_FEEDBACKDIV_VAL 512
#define MG_PLL_MAX_INPUTDIV_VAL 32
#define MG_PLL_MAX_OUTPUTDIV_VAL 4

#define MG_PLL_STD_INPUTCLK 12000000.0	/* 12Mhz */
#define MG_PLL_STD_LOCKCYCLE 10000

#define MG_UNLOCK_OTP_AREA 0xFF

#define MG_FILEIO_CHUNK 1048576

#define ERROR_MG_IO (-1600)
#define ERROR_MG_TIMEOUT (-1601)
#define ERROR_MG_INVALID_PLL (-1603)
#define ERROR_MG_INTERFACE (-1604)
#define ERROR_MG_INVALID_OSC (-1605)
#define ERROR_MG_UNSUPPORTED_SOC (-1606)

enum mg_io_type_wait {

	mg_io_wait_bsy       = 1,
	mg_io_wait_not_bsy   = 2,
	mg_io_wait_rdy       = 3,
	mg_io_wait_drq       = 4,	/* wait for data request */
	mg_io_wait_drq_noerr = 5,	/* wait for DRQ but ignore the error status bit */
	mg_io_wait_rdy_noerr = 6	/* wait for ready, but ignore error status bit */

};

/*= "Status Register" bit masks. */
enum mg_io_type_rbit_status {

	mg_io_rbit_status_error            = 0x01,	/* error bit in status register */
	mg_io_rbit_status_corrected_error  = 0x04,	/* corrected error in status register */
	mg_io_rbit_status_data_req         = 0x08,	/* data request bit in status register */
	mg_io_rbit_status_seek_done        = 0x10,	/* DSC - Drive Seek Complete */
	mg_io_rbit_status_write_fault      = 0x20,	/* DWF - Drive Write Fault */
	mg_io_rbit_status_ready            = 0x40,
	mg_io_rbit_status_busy             = 0x80

};

/*= "Error Register" bit masks. */
enum mg_io_type_rbit_error {

	mg_io_rbit_err_general          = 0x01,
	mg_io_rbit_err_aborted          = 0x04,
	mg_io_rbit_err_bad_sect_num     = 0x10,
	mg_io_rbit_err_uncorrectable    = 0x40,
	mg_io_rbit_err_bad_block        = 0x80

};

/* = "Device Control Register" bit. */
enum mg_io_type_rbit_devc {

	mg_io_rbit_devc_intr      = 0x02,	/* interrupt enable bit (1:disable, 0:enable) */
	mg_io_rbit_devc_srst      = 0x04	/* softwrae reset bit (1:assert, 0:de-assert) */

};

/* "Drive Select/Head Register" values. */
enum mg_io_type_rval_dev {

	mg_io_rval_dev_must_be_on      = 0x80,	/* These 1 bits are always on */
	mg_io_rval_dev_drv_master      = (0x00 | mg_io_rval_dev_must_be_on),	/* Master */
	mg_io_rval_dev_drv_slave0      = (0x10 | mg_io_rval_dev_must_be_on),	/* Slave0 */
	mg_io_rval_dev_drv_slave1      = (0x20 | mg_io_rval_dev_must_be_on),	/* Slave1 */
	mg_io_rval_dev_drv_slave2      = (0x30 | mg_io_rval_dev_must_be_on),	/* Slave2 */
	mg_io_rval_dev_lba_mode        = (0x40 | mg_io_rval_dev_must_be_on)

};

enum mg_io_type_cmd {
	mg_io_cmd_read             = 0x20,
	mg_io_cmd_write            = 0x30,

	mg_io_cmd_setmul           = 0xC6,
	mg_io_cmd_readmul          = 0xC4,
	mg_io_cmd_writemul         = 0xC5,

	mg_io_cmd_idle             = 0x97,	/* 0xE3 */
	mg_io_cmd_idle_immediate   = 0x95,	/* 0xE1 */

	mg_io_cmd_setsleep         = 0x99,	/* 0xE6 */
	mg_io_cmd_stdby            = 0x96,	/* 0xE2 */
	mg_io_cmd_stdby_immediate  = 0x94,	/* 0xE0 */

	mg_io_cmd_identify         = 0xEC,
	mg_io_cmd_set_feature      = 0xEF,

	mg_io_cmd_confirm_write    = 0x3C,
	mg_io_cmd_confirm_read     = 0x40,
	mg_io_cmd_wakeup           = 0xC3

};

enum mg_feature_id {
	mg_feature_id_transmode = 0x3
};

enum mg_feature_val {
	mg_feature_val_trans_default = 0x0,
	mg_feature_val_trans_vcmd = 0x3,
	mg_feature_val_trand_vcmds = 0x2
};

enum mg_vcmd {
	mg_vcmd_update_xipinfo = 0xFA,	/* FWPATCH commmand through IOM I/O */
	mg_vcmd_verify_fwpatch = 0xFB,	/* FWPATCH commmand through IOM I/O */
	mg_vcmd_update_stgdrvinfo = 0xFC,	/* IOM identificatin info program command */
	mg_vcmd_prep_fwpatch = 0xFD,	/* FWPATCH commmand through IOM I/O */
	mg_vcmd_exe_fwpatch = 0xFE,	/* FWPATCH commmand through IOM I/O */
	mg_vcmd_wr_pll = 0x8B,
	mg_vcmd_purge_nand = 0x8C,	/* Only for  Seagle */
	mg_vcmd_lock_otp = 0x8D,
	mg_vcmd_rd_otp = 0x8E,
	mg_vcmd_wr_otp = 0x8F
};

enum mg_opmode {
	mg_op_mode_xip = 1,	/* TRUE XIP */
	mg_op_mode_snd = 2,	/* BOOT + Storage */
	mg_op_mode_stg = 0	/* Only Storage */
};

#endif
