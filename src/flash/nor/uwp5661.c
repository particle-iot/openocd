/***************************************************************************
 *   Copyright (C) 2019 by UNISOC                                          *
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

#include <helper/types.h>
#include "uwp5661.h"
#include "spi.h"

static void uwp_target_read(struct target *target, target_addr_t address, uint32_t *value)
{
	int retval = ERROR_OK;
	retval = target_read_u32(target, address, value);
	if (retval != ERROR_OK)
		LOG_ERROR("target read fail.");
}

static void uwp_target_write(struct target *target, target_addr_t address, uint32_t value)
{
	int retval = ERROR_OK;
	retval = target_write_u32(target, address, value);
	if (retval != ERROR_OK)
		LOG_ERROR("target read fail.");
}

static void uwp5661_init_sw_csn(struct target *target)
{
	uint32_t rd_dat = 0;
	/* GPIO init for faster programming */
	/* set GPIO_MODE1 register to select GPIO's group. */
	uwp_target_read(target, REG_AON_GLB_RF_GPIO_MODE1, &rd_dat);
	rd_dat &= (~(BIT(28)));
	uwp_target_write(target, REG_AON_GLB_RF_GPIO_MODE1, rd_dat);
	/* GPIO1_EB */
	uwp_target_write(target, REG_AON_GLB_RF_APB_EB_SET, BIT(13));
	/* use CSN as GPIO bit28: GROUP1 BIT12 */
	/* Dir */
	uwp_target_read(target, REG_AON_GPIO1_RF_GPIO_DIR, &rd_dat);
	rd_dat |= BIT(12);
	uwp_target_write(target, REG_AON_GPIO1_RF_GPIO_DIR, rd_dat);
	/* Mask */
	uwp_target_read(target, REG_AON_GPIO1_RF_GPIO_MSK, &rd_dat);
	rd_dat |= BIT(12);
	uwp_target_write(target, REG_AON_GPIO1_RF_GPIO_MSK, rd_dat);
	/* Value */
	uwp_target_read(target, REG_AON_GPIO1_RF_GPIO_VAL, &rd_dat);
	rd_dat &= (~BIT(12));
	uwp_target_write(target, REG_AON_GPIO1_RF_GPIO_VAL, rd_dat);
}

static void uwp5661_force_csn(struct target *target, uint32_t op)
{
	uint32_t rd_dat;
	if (op == TRUE) {  /* use CS as GPIO */
		uwp_target_read(target, REG_AON_PIN_RF_ESMCSN_CFG, &rd_dat);
		rd_dat &= (~FUNC_MSK);
		rd_dat |= FUNC_3;
		uwp_target_write(target, REG_AON_PIN_RF_ESMCSN_CFG, rd_dat);
	} else {  /* not GPIO */
		uwp_target_read(target, REG_AON_PIN_RF_ESMCSN_CFG, &rd_dat);
		rd_dat &= (~FUNC_MSK);
		uwp_target_write(target, REG_AON_PIN_RF_ESMCSN_CFG, rd_dat);
	}
}

static void uwp5661_set_sfc_clk(struct target *target)
{
uwp_target_write(target, SFC_CLK_CFG, SFC_CLK_OUT_DIV_2 | SFC_CLK_OUT_2X_EN |
			SFC_CLK_2X_EN | SFC_CLK_SAMPLE_2X_PHASE_1 | SFC_CLK_SAMPLE_2X_EN);
	/*
	 * cgm_sfc_1x_div: clk_sfc_1x = clk_src/(bit 9:8 + 1)
	 * */
	uwp_target_write(target, REG_AON_CLK_RF_CGM_SFC_1X_CFG, 0x00000100);
	/* 0: xtal MHz 1: 133MHz 2: 139MHz 3: 160MHz 4: 208MHz
	 * cgm_sfc_2x_sel: clk_sfc_1x source (bit 2:1:0)
	 * */
	uwp_target_write(target, REG_AON_CLK_RF_CGM_SFC_2X_CFG, 0x00000000);
}

static void sfcdrv_req(struct target *target)
{
	uint32_t int_status = 0;
	uint32_t int_timeout = 0;
	uwp_target_write(target, SFC_SOFT_REQ, (1 << SHIFT_SOFT_REQ));
	do {
		uwp_target_read(target, SFC_INT_RAW, &int_status);
		if (int_timeout++ > SFC_DRVREQ_TIMEOUT) {
			LOG_ERROR("SFCDRV Req time out!\n");
			break;
		}
	} while (int_status == 0);
	uwp_target_write(target, SFC_INT_CLR , (1 << SHIFT_INT_CLR));
}

static void sfcdrv_int_cfg(struct target *target, uint32_t op)
{
	if (op == TRUE)  /* for CS1 interrupt */
		uwp_target_write(target, SFC_IEN, 0x000000FF);
	else  /* for CS0 interrupt */
		uwp_target_write(target, SFC_IEN, 0x00000000);
}

static uint32_t sfcdrv_get_init_addr(struct uwp5661_flash_bank *uwp5661_info, struct target *target)
{
	uint32_t start_addr = uwp5661_info->sfc_cmd_cfg_cache ;

	if (uwp5661_info->sfc_cmd_cfg_cache == 0xFFFFFFFF)
		uwp_target_read(target, SFC_CMD_CFG, &start_addr);

	start_addr = (start_addr & MSK_STS_INI_ADDR_SEL) >> SHIFT_STS_INI_ADDR_SEL;

	switch (start_addr) {
		case INI_CMD_BUF_6:
			start_addr = CMD_BUF_6;
			break;

		case INI_CMD_BUF_5:
			start_addr = CMD_BUF_5;
			break;

		case INI_CMD_BUF_4:
			start_addr = CMD_BUF_4;
			break;

		default:
			start_addr = CMD_BUF_7;
			break;
	}
	return start_addr;
}

static void sfcdrv_set_cmd_cfg_reg(struct uwp5661_flash_bank *uwp5661_info, struct target *target,
				CMD_MODE_E cmdmode, BIT_MODE_E bitmode, INI_ADD_SEL_E iniAddSel)
{
	uint32_t nxt_sfc_cmd_cfg = ((cmdmode << SHIFT_CMD_SET)|
								(bitmode << SHIFT_RDATA_BIT_MODE)|
								(iniAddSel << SHIFT_STS_INI_ADDR_SEL));

	if (uwp5661_info->sfc_cmd_cfg_cache != nxt_sfc_cmd_cfg) {
		uwp_target_write(target, SFC_CMD_CFG, nxt_sfc_cmd_cfg);
		uwp5661_info->sfc_cmd_cfg_cache = nxt_sfc_cmd_cfg;
	}
}

static void sfcdrv_set_cmd_buf(struct uwp5661_flash_bank *uwp5661_info, struct target *target,
				CMD_BUF_INDEX_E index, uint32_t value)
{
	uwp5661_info->cmd_buf_cache_bitmap |= 1<<index;
	uwp5661_info->cmd_info_buf_cache[index] = value;
}

static void sfcdrv_set_type_inf_buf(struct uwp5661_flash_bank *uwp5661_info, struct target *target,
			CMD_BUF_INDEX_E index, BIT_MODE_E bitmode, BYTE_NUM_E bytenum, CMD_MODE_E cmdmode,
			SEND_MODE_E sendmode)
{
	switch (index) {
		case CMD_BUF_0:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_0] |=   (VALID0|
												(bitmode << SHIFT_BIT_MODE0)|
												(bytenum << SHIFT_BYTE_NUM0)|
												(cmdmode << SHIFT_OPERATION_STATUS0)|
												(sendmode << SHIFT_BYTE_SEND_MODE0));
			break;

		case CMD_BUF_1:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_0] |=   (VALID1|
												(bitmode << SHIFT_BIT_MODE1)|
												(bytenum << SHIFT_BYTE_NUM1)|
												(cmdmode << SHIFT_OPERATION_STATUS1)|
												(sendmode << SHIFT_BYTE_SEND_MODE1));
			break;

		case CMD_BUF_2:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_0] |=   (VALID2|
												(bitmode << SHIFT_BIT_MODE2)|
												(bytenum << SHIFT_BYTE_NUM2)|
												(cmdmode << SHIFT_OPERATION_STATUS2)|
												(sendmode << SHIFT_BYTE_SEND_MODE2));
			break;

		case CMD_BUF_3:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_0] |=   (VALID3|
												(bitmode << SHIFT_BIT_MODE3)|
												(bytenum << SHIFT_BYTE_NUM3)|
												(cmdmode << SHIFT_OPERATION_STATUS3)|
												(sendmode << SHIFT_BYTE_SEND_MODE3));
			break;

		case CMD_BUF_4:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_1] |=   (VALID4|
												(bitmode << SHIFT_BIT_MODE4)|
												(bytenum << SHIFT_BYTE_NUM4)|
												(cmdmode << SHIFT_OPERATION_STATUS4)|
												(sendmode << SHIFT_BYTE_SEND_MODE4));
			break;

		case CMD_BUF_5:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_1] |=   (VALID5|
												(bitmode << SHIFT_BIT_MODE5)|
												(bytenum << SHIFT_BYTE_NUM5)|
												(cmdmode << SHIFT_OPERATION_STATUS5)|
												(sendmode << SHIFT_BYTE_SEND_MODE5));
			break;

		case CMD_BUF_6:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_1] |=   (VALID6|
												(bitmode << SHIFT_BIT_MODE6)|
												(bytenum << SHIFT_BYTE_NUM6)|
												(cmdmode << SHIFT_OPERATION_STATUS6)|
												(sendmode << SHIFT_BYTE_SEND_MODE6));
			break;

		case CMD_BUF_7:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_1] |=   (VALID7|
												(bitmode << SHIFT_BIT_MODE7)|
												(bytenum << SHIFT_BYTE_NUM7)|
												(cmdmode << SHIFT_OPERATION_STATUS7)|
												(sendmode << SHIFT_BYTE_SEND_MODE7));
			break;

		case CMD_BUF_8:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_2] |=   (VALID8|
												(bitmode << SHIFT_BIT_MODE8)|
												(bytenum << SHIFT_BYTE_NUM8)|
												(cmdmode << SHIFT_OPERATION_STATUS8)|
												(sendmode << SHIFT_BYTE_SEND_MODE8));
			break;

		case CMD_BUF_9:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_2] |=   (VALID9|
												(bitmode << SHIFT_BIT_MODE9)|
												(bytenum << SHIFT_BYTE_NUM9)|
												(cmdmode << SHIFT_OPERATION_STATUS9)|
												(sendmode << SHIFT_BYTE_SEND_MODE9));
			break;

		case CMD_BUF_10:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_2] |=   (VALID10|
												(bitmode << SHIFT_BIT_MODE10)|
												(bytenum << SHIFT_BYTE_NUM10)|
												(cmdmode << SHIFT_OPERATION_STATUS10)|
												(sendmode << SHIFT_BYTE_SEND_MODE10));
			break;

		case CMD_BUF_11:
			uwp5661_info->cmd_info_buf_cache[INFO_BUF_2] |=   (VALID11|
												(bitmode << SHIFT_BIT_MODE11)|
												(bytenum << SHIFT_BYTE_NUM11)|
												(cmdmode << SHIFT_OPERATION_STATUS11)|
												(sendmode << SHIFT_BYTE_SEND_MODE11));
			break;

		default:
			break;
	}
}

static void sfcdrv_get_read_buf(struct uwp5661_flash_bank *uwp5661_info, struct target *target,
					uint32_t *buffer, uint32_t word_cnt)
{
	int ret = ERROR_OK;
	uint32_t i = 0;
	uint32_t read_buf_index = sfcdrv_get_init_addr(uwp5661_info, target);
	uint8_t tmp_buf[INFO_BUF_MAX*4] = {0};

	ret = target_read_memory(target, SFC_CMD_BUF0+read_buf_index*4, 4, word_cnt, tmp_buf);
	if (ret != ERROR_OK)
		LOG_ERROR("Get read buffer fail.");
	for (i = 0; i < word_cnt; i++)
		buffer[i] = target_buffer_get_u32(target, tmp_buf+i*4);
}

static void sfcdrv_set_cmd_data(struct uwp5661_flash_bank *uwp5661_info, struct target *target,
					uint32_t cmd_buf_index, SFC_CMD_DES_T *cmd_des_ptr)
{
	if (cmd_des_ptr != NULL) {
		sfcdrv_set_cmd_buf(uwp5661_info, target, cmd_buf_index, cmd_des_ptr->cmd);
		sfcdrv_set_type_inf_buf(uwp5661_info, target, cmd_buf_index,
							cmd_des_ptr->bit_mode,
							cmd_des_ptr->cmd_byte_len,
							cmd_des_ptr->cmd_mode,
							cmd_des_ptr->send_mode);
	}
}

static void sfcdrv_set_read_buf(struct uwp5661_flash_bank *uwp5661_info, struct target *target,
					uint32_t read_buf_index, SFC_CMD_DES_T *cmd_des_ptr)
{
	if (cmd_des_ptr != NULL) {
		sfcdrv_set_type_inf_buf(uwp5661_info, target, read_buf_index,
							cmd_des_ptr->bit_mode,
							cmd_des_ptr->cmd_byte_len,
							cmd_des_ptr->cmd_mode,
							cmd_des_ptr->send_mode);
	}
}

static void create_cmd(SFC_CMD_DES_T *cmd_desc_ptr, uint32_t cmd, uint32_t byte_len,
			CMD_MODE_E cmd_mode, BIT_MODE_E bit_mode, SEND_MODE_E send_mode)
{
	cmd_desc_ptr->cmd = cmd;
	cmd_desc_ptr->cmd_byte_len = byte_len;
	cmd_desc_ptr->cmd_mode = cmd_mode;
	cmd_desc_ptr->bit_mode = bit_mode;
	cmd_desc_ptr->send_mode = send_mode;
}

static void spiflash_read_write(struct uwp5661_flash_bank *uwp5661_info, struct target *target,
			SFC_CMD_DES_T *cmd_des_ptr, uint32_t cmd_len, uint32_t *din)
{
	uint32_t i = 0;
	uint32_t read_count = 0;
	uint32_t read_buf_index = sfcdrv_get_init_addr(uwp5661_info, target);
	uint8_t tmp_buf[INFO_BUF_MAX*4] = {0};
	uint32_t update_info_buf = FALSE;

	uwp5661_info->cmd_buf_cache_bitmap = 0;
	memset(uwp5661_info->cmd_info_buf_cache, 0 , sizeof(uint32_t)*INFO_BUF_MAX);


	for (i = 0; i < cmd_len; i++) {
		cmd_des_ptr[i].is_valid = TRUE;
		if ((cmd_des_ptr[i].cmd_mode == CMD_MODE_WRITE) ||
			(cmd_des_ptr[i].cmd_mode == CMD_MODE_HIGHZ))
			sfcdrv_set_cmd_data(uwp5661_info, target, i, &(cmd_des_ptr[i]));
		else if (cmd_des_ptr[i].cmd_mode == CMD_MODE_READ) {
			sfcdrv_set_cmd_buf(uwp5661_info, target, read_buf_index, 0);
			sfcdrv_set_read_buf(uwp5661_info, target, read_buf_index, &(cmd_des_ptr[i]));
			read_buf_index++;
			read_count++;
		}
	}

	if ((uwp5661_info->prev_cmd_info_buf_cache[INFO_BUF_0] != uwp5661_info->cmd_info_buf_cache[INFO_BUF_0]) ||
		(uwp5661_info->prev_cmd_info_buf_cache[INFO_BUF_1] != uwp5661_info->cmd_info_buf_cache[INFO_BUF_1]) ||
		(uwp5661_info->prev_cmd_info_buf_cache[INFO_BUF_2] != uwp5661_info->cmd_info_buf_cache[INFO_BUF_2])) {
		for (i = INFO_BUF_0; i < INFO_BUF_MAX; i++) {
			target_buffer_set_u32(target, tmp_buf+i*4, uwp5661_info->cmd_info_buf_cache[i]);
			uwp5661_info->prev_cmd_info_buf_cache[i] = uwp5661_info->cmd_info_buf_cache[i];
		}

		update_info_buf = TRUE;
	}

	if (cmd_len <= 2) {
		for (i = CMD_BUF_0; i < CMD_BUF_MAX; i++) {
			if (uwp5661_info->cmd_buf_cache_bitmap & (1<<i))
				uwp_target_write(target, SFC_CMD_BUF0+i*4, uwp5661_info->cmd_info_buf_cache[i]);
		}

		if (update_info_buf == TRUE)
			target_write_memory(target, SFC_TYPE_BUF0, 4, INFO_BUF_MAX - INFO_BUF_0, tmp_buf+INFO_BUF_0*4);
	} else {
		if (update_info_buf == TRUE) {
			for (i = CMD_BUF_0; i < INFO_BUF_MAX; i++)
				target_buffer_set_u32(target, tmp_buf+i*4, uwp5661_info->cmd_info_buf_cache[i]);
			target_write_memory(target, SFC_CMD_BUF0, 4, INFO_BUF_MAX, tmp_buf);
		} else {
			for (i = CMD_BUF_0; i < CMD_BUF_MAX; i++)
				target_buffer_set_u32(target, tmp_buf+i*4, uwp5661_info->cmd_info_buf_cache[i]);
			target_write_memory(target, SFC_CMD_BUF0, 4, CMD_BUF_MAX, tmp_buf);
		}
	}

	sfcdrv_req(target);

	if (0 != read_count)
		sfcdrv_get_read_buf(uwp5661_info, target, din, read_count);
}

static void spiflash_disable_cache(struct target *target)
{
	uint32_t int_status = 0;
	uint32_t int_timeout = 0;

	uwp_target_write(target, REG_ICACHE_INT_EN, 0x00000000);	/* irq not enable */
	uwp_target_write(target, REG_ICACHE_INT_CLR, 0x00000001);	/* cmd_irq_clr */
	uwp_target_write(target, REG_ICACHE_CMD_CFG2, 0x80000004);	/* invalid all */
	do {
		uwp_target_read(target, REG_ICACHE_INT_RAW_STS, &int_status);
		if (int_timeout++ > CACHE_CMD_TIMEOUT) {
			LOG_ERROR("ICache invalid time out!\n");
			break;
		}
	} while ((int_status & BIT(0)) == 0);
	uwp_target_write(target, REG_ICACHE_INT_CLR, 0x00000001);
	uwp_target_write(target, REG_ICACHE_CFG0, 0x00000000);		/* disable all */

	int_timeout = 0;
	uwp_target_write(target, REG_DCACHE_INT_EN, 0x00000000);	/* irq not enable */
	uwp_target_write(target, REG_DCACHE_INT_CLR, 0x00000001);	/* cmd_irq_clr */
	uwp_target_write(target, REG_DCACHE_CMD_CFG2, 0x80000008);	/* clean and invalid all */
	do {
		uwp_target_read(target, REG_DCACHE_INT_RAW_STS, &int_status);
		if (int_timeout++ > CACHE_CMD_TIMEOUT) {
			LOG_ERROR("DCache clean and invalid time out!\n");
			break;
		}
	} while ((int_status & BIT(0)) == 0);
	uwp_target_write(target, REG_DCACHE_INT_CLR, 0x00000001);
	uwp_target_write(target, REG_DCACHE_CFG0, 0x00000000);	/* disable all */
}

static void spiflash_enter_xip(struct uwp5661_flash_bank *uwp5661_info, struct target *target, uint8_t support_4addr)
{
	uint32_t i = 0;
	SFC_CMD_DES_T cmd_desc[3];

	create_cmd(&(cmd_desc[0]), CMD_FAST_READ, BYTE_NUM_1, CMD_MODE_WRITE, BIT_MODE_1, SEND_MODE_0);
	create_cmd(&(cmd_desc[1]), 0x0          , (support_4addr == TRUE) ? BYTE_NUM_4 : BYTE_NUM_3,
															CMD_MODE_WRITE, BIT_MODE_1, SEND_MODE_1);
	create_cmd(&(cmd_desc[2]), 0x0          , BYTE_NUM_1, CMD_MODE_HIGHZ, BIT_MODE_1, SEND_MODE_0);

	for (i = 0; i < 3; i++) {
		cmd_desc[i].is_valid = TRUE;
		sfcdrv_set_cmd_data(uwp5661_info, target, i, &(cmd_desc[i]));
	}

	uwp_target_write(target, SFC_CMD_BUF0 , uwp5661_info->cmd_info_buf_cache[CMD_BUF_0]);
	uwp_target_write(target, SFC_TYPE_BUF0, uwp5661_info->cmd_info_buf_cache[INFO_BUF_0]);
	uwp_target_write(target, SFC_TYPE_BUF1, 0x00000000);	/* set cfg as defaults */
	uwp_target_write(target, SFC_TYPE_BUF2, 0x00000000);	/* set cfg as defaults */
	sfcdrv_set_cmd_cfg_reg(uwp5661_info, target, CMD_MODE_READ , BIT_MODE_1, INI_CMD_BUF_7);
	/* Bus clk register : clk_btwf_mtx = clk_src/(bit 9:8 + 1) */
	uwp_target_write(target, REG_AON_CLK_RF_CGM_MTX_CFG, 0x00000100);
	/* set CPU clk to xtal MHz */
	uwp_target_write(target, REG_AON_CLK_RF_CGM_ARM_CFG, 0x00000000);
	uwp_target_write(target, REG_AON_CLK_RF_CGM_MTX_CFG, 0x00000000);

	spiflash_disable_cache(target);

	LOG_DEBUG("Enter XIP");
}

static void spiflash_exit_xip(struct uwp5661_flash_bank *uwp5661_info, struct target *target)
{
	sfcdrv_set_cmd_cfg_reg(uwp5661_info, target, CMD_MODE_WRITE, BIT_MODE_1, INI_CMD_BUF_7);
	uwp_target_write(target, REG_AON_CLK_RF_CGM_MTX_CFG, 0x00000100);
	uwp_target_write(target, REG_AON_CLK_RF_CGM_ARM_CFG, 0x00000000);
	uwp_target_write(target, REG_AON_CLK_RF_CGM_MTX_CFG, 0x00000000);
	LOG_DEBUG("Exit XIP");
}

static void spiflash_select_xip(struct uwp5661_flash_bank *uwp5661_info, struct target *target,
					uint8_t support_4addr, uint32_t op)
{
	uwp_target_write(target, SFC_INT_CLR , (1 << SHIFT_INT_CLR));
	if (op == TRUE)
		spiflash_enter_xip(uwp5661_info, target, support_4addr);
	else
		spiflash_exit_xip(uwp5661_info, target);
}

static BYTE_NUM_E spi_flash_addr(uint32_t *addr, uint8_t support_4addr)
{
	uint8_t cmd[4] = {0};
	uint32_t address = *addr;

	cmd[0] = ((address >> 0) & (0xFF));
	cmd[1] = ((address >> 8) & (0xFF));
	cmd[2] = ((address >> 16) & (0xFF));
	cmd[3] = ((address >> 24) & (0xFF));

	if (support_4addr == TRUE) {
		*addr = (cmd[3] << 0)  | (cmd[2] << 8) |
				(cmd[1] << 16) | (cmd[0] << 24);
		return BYTE_NUM_4;
	} else {
		*addr = (cmd[2] << 0) | (cmd[1] << 8) | (cmd[0] << 16);
		return BYTE_NUM_3;
	}
}

static void spiflash_cmd_write(struct uwp5661_flash_bank *uwp5661_info, struct target *target, uint8_t cmd,
			uint32_t *data_out, uint32_t data_len, BIT_MODE_E bitmode)
{
	SFC_CMD_DES_T cmd_desc[3];
	BYTE_NUM_E byte_num = BYTE_NUM_1;
	uint32_t cmd_idx = 0;

	create_cmd(&(cmd_desc[cmd_idx++]), cmd, BYTE_NUM_1, CMD_MODE_WRITE, bitmode, SEND_MODE_0);

	if (data_len > 8)
		data_len = 8;

	if (data_len > 4) {
		create_cmd(&(cmd_desc[cmd_idx]), data_out[cmd_idx-1], BYTE_NUM_4, CMD_MODE_WRITE, bitmode, SEND_MODE_0);
		cmd_idx++;
		data_len = data_len - 4;
	}

	if (data_len > 0) {
		byte_num = BYTE_NUM_1 + (data_len - 1);
		create_cmd(&(cmd_desc[cmd_idx]), data_out[cmd_idx-1], byte_num  , CMD_MODE_WRITE, bitmode, SEND_MODE_0);
		cmd_idx++;
	}

	spiflash_read_write(uwp5661_info, target, cmd_desc, cmd_idx, NULL);
}

static void spiflash_cmd_read(struct uwp5661_flash_bank *uwp5661_info, struct target *target,
			uint8_t cmd, uint32_t address, uint8_t support_4addr, uint32_t *data_in, uint32_t data_len)
{
	SFC_CMD_DES_T cmd_desc[4];
	BYTE_NUM_E byte_num = BYTE_NUM_1;
	uint32_t tmp_buf[2] = {0};
	uint32_t cmd_idx = 0;

	create_cmd(&(cmd_desc[cmd_idx++]), cmd, BYTE_NUM_1, CMD_MODE_WRITE, BIT_MODE_1, SEND_MODE_0);

	if (address != 0xFFFFFFFF) {
		uint32_t dest_addr = address;
		byte_num = spi_flash_addr(&dest_addr, support_4addr);
		create_cmd(&(cmd_desc[cmd_idx++]), dest_addr, byte_num , CMD_MODE_WRITE, BIT_MODE_1, SEND_MODE_0);
	}

	if (data_len > 8)
		data_len = 8;

	if (data_len > 4) {
		create_cmd(&(cmd_desc[cmd_idx++]), 0, BYTE_NUM_4, CMD_MODE_READ, BIT_MODE_1, SEND_MODE_0);
		data_len = data_len - 4;
	}

	if (data_len > 0) {
		byte_num = BYTE_NUM_1 + (data_len - 1);
		create_cmd(&(cmd_desc[cmd_idx++]), 0, byte_num  , CMD_MODE_READ, BIT_MODE_1, SEND_MODE_0);
	}

	spiflash_read_write(uwp5661_info, target, cmd_desc, cmd_idx, tmp_buf);

	if (cmd_idx > 1)
		data_in[0] = (((tmp_buf[0] >> 24) & 0xFF) << 0) |
						(((tmp_buf[0] >> 16) & 0xFF) << 8) |
						(((tmp_buf[0] >> 8) & 0xFF) << 16) |
						(((tmp_buf[0] >> 0) & 0xFF) << 24) ;

	if (cmd_idx > 2)
		data_in[1] = (((tmp_buf[1] >> 24) & 0xFF) << 0) |
						(((tmp_buf[1] >> 16) & 0xFF) << 8) |
						(((tmp_buf[1] >> 8) & 0xFF) << 16) |
						(((tmp_buf[1] >> 0) & 0xFF) << 24) ;
}

static int spiflash_cmd_poll_bit(struct uwp5661_flash_bank *uwp5661_info, struct target *target,	uint32_t timeout,
					uint8_t cmd, uint32_t poll_bit, uint32_t bit_value)
{
	uint32_t status = 0;

	do {
		spiflash_cmd_read(uwp5661_info, target, cmd, 0xFFFFFFFF, FALSE, &status, 1);
		status &= 0xFF;
		if (bit_value) {
			if ((status & poll_bit))
				return ERROR_OK;
		} else {
			if ((status & poll_bit) == 0)
				return ERROR_OK;
		}
	} while (timeout--);

	LOG_ERROR("Polling flash status time out!\n");

	return ERROR_FAIL;
}

static void spiflash_write_enable(struct uwp5661_flash_bank *uwp5661_info, struct target *target)
{
	spiflash_cmd_write(uwp5661_info, target, CMD_WRITE_ENABLE, NULL, 0, BIT_MODE_1);
}

static void spiflash_reset_anyway(struct uwp5661_flash_bank *uwp5661_info, struct target *target)
{
	uint32_t i = 0;
	uint32_t dummy_dat = 0;

	spiflash_cmd_write(uwp5661_info, target, CMD_RSTEN, NULL, 0, BIT_MODE_4);
	spiflash_cmd_write(uwp5661_info, target, CMD_RST  , NULL, 0, BIT_MODE_4);
	for (i = 0; i < 10; i++)
		uwp_target_read(target, SFC_CMD_CFG, &dummy_dat);

	spiflash_cmd_write(uwp5661_info, target, CMD_RSTEN, NULL, 0, BIT_MODE_1);
	spiflash_cmd_write(uwp5661_info, target, CMD_RST  , NULL, 0, BIT_MODE_1);
	for (i = 0; i < 10; i++)
		uwp_target_read(target, SFC_CMD_CFG, &dummy_dat);
}

static int spiflash_4addr_enable(struct uwp5661_flash_bank *uwp5661_info, struct target *target)
{
	spiflash_cmd_write(uwp5661_info, target, CMD_ENTER_4ADDR, NULL, 0, BIT_MODE_1);

	return spiflash_cmd_poll_bit(uwp5661_info, target, SPI_FLASH_ADS_TIMEOUT, CMD_READ_STATUS3, STATUS_ADS, 1);
}

static int spiflash_4addr_disable(struct uwp5661_flash_bank *uwp5661_info, struct target *target)
{
	spiflash_cmd_write(uwp5661_info, target, CMD_EXIT_4ADDR, NULL, 0, BIT_MODE_1);

	return spiflash_cmd_poll_bit(uwp5661_info, target, SPI_FLASH_ADS_TIMEOUT, CMD_READ_STATUS3, STATUS_ADS, 0);
}

static int spiflash_cmd_sector_erase(struct uwp5661_flash_bank *uwp5661_info, struct target *target, uint32_t offset)
{
	uint32_t addr = offset * uwp5661_info->flash.sector_size;
	BYTE_NUM_E addr_byte_num = spi_flash_addr(&addr, uwp5661_info->flash.support_4addr);
	int ret = ERROR_OK;

	spiflash_write_enable(uwp5661_info, target);

	spiflash_cmd_write(uwp5661_info, target, CMD_SECTOR_ERASE, &addr, addr_byte_num + 1, BIT_MODE_1);

	ret = spiflash_cmd_poll_bit(uwp5661_info, target, SPI_FLASH_SECTOR_ERASE_TIMEOUT, CMD_READ_STATUS1, STATUS_WIP, 0);

	return ret;
}

static int uwp5661_erase(struct flash_bank *bank, int first, int last)
{
	struct uwp5661_flash_bank *uwp5661_info = bank->driver_priv;
	struct target *target = bank->target;
	struct uwp_flash *flash = &(uwp5661_info->flash);
	int i = 0;
	int ret = ERROR_OK;

	memset(uwp5661_info->prev_cmd_info_buf_cache, 0 , sizeof(uint32_t)*INFO_BUF_MAX);

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted before erasing flash!\n");
		return ERROR_TARGET_NOT_HALTED;
	}

	uwp5661_init_sw_csn(target);

	uwp5661_set_sfc_clk(target);

	spiflash_select_xip(uwp5661_info, target, FALSE, FALSE);

	if (flash->support_4addr == TRUE) {
		ret = spiflash_4addr_enable(uwp5661_info, target);
		if (ret != ERROR_OK) {
			LOG_ERROR("uwp5661 SPI 4Byte Address mode switching ON failed!\n");
			spiflash_reset_anyway(uwp5661_info, target);
			spiflash_select_xip(uwp5661_info, target, FALSE, TRUE);
			return ret;
		}
	}

	LOG_INFO("Flash Erase");
	uint32_t cur_ratio = 0;
	uint32_t prev_ratio = 0xFF;
	for (i = first; i <= last; i++) {
		ret = spiflash_cmd_sector_erase(uwp5661_info, target, i);
		cur_ratio = (i-first+1)*100/(last-first+1);
		if (((cur_ratio/10) != (prev_ratio/10)) && (cur_ratio != 0)) {
			LOG_INFO("\rFlash Erase %3d%%", cur_ratio);
			prev_ratio = cur_ratio;
		}
		if (ret != ERROR_OK)
			return ret;

		bank->sectors[i].is_erased = 1;
	}

	if (flash->support_4addr == TRUE) {
		ret = spiflash_4addr_disable(uwp5661_info, target);
		if (ret != ERROR_OK) {
			LOG_ERROR("uwp5661 SPI 4Byte Address mode switching OFF failed!\n");
			spiflash_reset_anyway(uwp5661_info, target);
		}
	}

	spiflash_select_xip(uwp5661_info, target, FALSE, TRUE);

	return ret;
}

static int spiflash_write_page(struct uwp5661_flash_bank *uwp5661_info, struct target *target,
			uint32_t data_addr, uint8_t *data_out, uint32_t data_len)
{
	uint32_t i = 0;
	uint32_t j = 0;
	uint32_t dest_addr = data_addr;
	uint8_t *data_ptr = data_out;
	uint32_t data_tmp = 0;
	uint32_t cmd_idx = 0;
	uint32_t piece_cnt = 0;
	BYTE_NUM_E byte_num = BYTE_NUM_3;
	SFC_CMD_DES_T cmd_desc[CMD_BUF_MAX];
	int ret = ERROR_OK;

	/* using cs as GPIO bit28 and pull it up, then write cmd and all data in */
	for (i = 0; i < data_len;) {
		cmd_idx = 0;
		piece_cnt = 0;

		if (i == 0) {
			spiflash_write_enable(uwp5661_info, target);
			uwp5661_force_csn(target, TRUE);

			byte_num = spi_flash_addr(&dest_addr, uwp5661_info->flash.support_4addr);
			/* write cmd and address in */
			create_cmd(&(cmd_desc[cmd_idx++]), CMD_PAGE_PROGRAM, BYTE_NUM_1,
						CMD_MODE_WRITE, BIT_MODE_1, SEND_MODE_0);
			create_cmd(&(cmd_desc[cmd_idx++]), dest_addr       , byte_num  ,
						CMD_MODE_WRITE, BIT_MODE_1, SEND_MODE_0);
		}

		piece_cnt = min((CMD_BUF_MAX - cmd_idx)*4, data_len - i);
		/* write all data in */
		for (j = 0; j < piece_cnt;) {
			if ((piece_cnt - j) >= 4) {
				byte_num = BYTE_NUM_4;
				data_tmp = (data_ptr[0] << 0)   | (data_ptr[1] << 8) |
							(data_ptr[2] << 16) | (data_ptr[3] << 24);
				data_ptr = data_ptr + 4;
				j = j + 4;
			} else {
				uint32_t tail_bytes = piece_cnt - j;
				byte_num = BYTE_NUM_1 + (tail_bytes - 1);
				switch (tail_bytes) {
					case 1: {
						data_tmp = data_ptr[0];
						break;
					}
					case 2: {
						data_tmp = (data_ptr[0] << 0) | (data_ptr[1] << 8);
						break;
					}
					case 3: {
						data_tmp = (data_ptr[0] << 0) | (data_ptr[1] << 8) | (data_ptr[2] << 16);
						break;
					}
					default:
						break;
				}
				j = piece_cnt;
			}
			create_cmd(&(cmd_desc[cmd_idx++]), data_tmp, byte_num, CMD_MODE_WRITE, BIT_MODE_1, SEND_MODE_0);
		}

		spiflash_read_write(uwp5661_info, target, cmd_desc, cmd_idx, NULL);

		i = i + piece_cnt;
	}
	uwp5661_force_csn(target, FALSE);
	ret = spiflash_cmd_poll_bit(uwp5661_info, target, SPI_FLASH_PAGE_PROG_TIMEOUT, CMD_READ_STATUS1, STATUS_WIP, 0);

	return ret;
}

static int uwp5661_write(struct flash_bank *bank, const uint8_t *buffer,
			uint32_t offset, uint32_t count)
{
	struct uwp5661_flash_bank *uwp5661_info = bank->driver_priv;
	struct uwp_flash *flash = &(uwp5661_info->flash);
	struct target *target = bank->target;
	uint32_t page_size = flash->page_size;
	uint32_t page_addr = 0;
	uint32_t byte_addr = 0;
	uint32_t chunk_len = 0;
	uint32_t actual    = 0;
	uint32_t data_len  = 0;
	uint32_t space_len = 0;
	int ret = ERROR_OK;

	memset(uwp5661_info->prev_cmd_info_buf_cache, 0 , sizeof(uint32_t)*INFO_BUF_MAX);

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted before writing flash!\n");
		return ERROR_TARGET_NOT_HALTED;
	}

	uwp5661_init_sw_csn(target);
	uwp5661_set_sfc_clk(target);

	spiflash_select_xip(uwp5661_info, target, FALSE, FALSE);

	if (flash->support_4addr == TRUE) {
		ret = spiflash_4addr_enable(uwp5661_info, target);
		if (ret != ERROR_OK) {
			LOG_ERROR("uwp5661 SPI 4Byte Address mode switching ON failed!\n");
			spiflash_reset_anyway(uwp5661_info, target);
			spiflash_select_xip(uwp5661_info, target, FALSE, TRUE);
			return ret;
		}
	}

	if (offset != 0) {
		page_addr = offset / page_size;
		byte_addr = offset % page_size;
	}

	LOG_INFO("Flash Write");
	uint32_t cur_ratio = 0;
	uint32_t prev_ratio = 0xFF;
	for (actual = 0; actual < count;) {
		data_len = count - actual;
		space_len = page_size - byte_addr;
		chunk_len = min(data_len, space_len);

		ret = spiflash_write_page(uwp5661_info, target, (page_addr * page_size + byte_addr),
					(uint8_t *)(buffer + actual), chunk_len);

		if (ret != ERROR_OK) {
			LOG_ERROR("Flash write failed\n");
			break;
		}

		page_addr++;
		byte_addr = 0;
		actual += chunk_len;
		cur_ratio = actual*100/count;
		if (((cur_ratio/10) != (prev_ratio/10)) && (cur_ratio != 0)) {
			LOG_INFO("\rFlash Write %3d%%", cur_ratio);
			prev_ratio = cur_ratio;
		}
	}

	if (flash->support_4addr == TRUE) {
		ret = spiflash_4addr_disable(uwp5661_info, target);
		if (ret != ERROR_OK) {
			LOG_ERROR("uwp5661 SPI 4Byte Address mode switching OFF failed!\n");
			spiflash_reset_anyway(uwp5661_info, target);
		}
	}

	spiflash_select_xip(uwp5661_info, target, FALSE, TRUE);

	return ret;
}

static void spiflash_data_read(struct target *target, uint32_t offset,
					uint32_t count, uint8_t *buf)
{
	int ret = ERROR_OK;
	uint32_t i = 0;
	uint32_t addr = offset;
	uint32_t piece_cnt = 0;
	uint8_t tmp_buf[256] = {0};
	uint8_t *data_ptr = buf;
	uint32_t cur_ratio = 0;
	uint32_t prev_ratio = 0xFF;

	for (i = 0; i < count;) {
		piece_cnt = min(count - i, 256-(addr%256));

		ret = target_read_memory(target, UWP5661_FLASH_BASE_ADDRESS+(addr&0xFFFFFF00), 4, 64, tmp_buf);
		if (ret != ERROR_OK)
			LOG_ERROR("Flash read fail");

		memcpy(data_ptr, tmp_buf+(addr%256), piece_cnt);

		i = i + piece_cnt;
		addr = addr + piece_cnt;
		data_ptr = data_ptr + piece_cnt;

		cur_ratio = i*100/count;
		if (((cur_ratio/10) != (prev_ratio/10)) && (cur_ratio != 0)) {
			LOG_INFO("\rFlash Read %3d%%", cur_ratio);
			prev_ratio = cur_ratio;
		}
	}
}

static int uwp5661_read(struct flash_bank *bank, uint8_t *buffer,
			uint32_t offset, uint32_t count)
{
	struct uwp5661_flash_bank *uwp5661_info = bank->driver_priv;
	struct uwp_flash *flash = &(uwp5661_info->flash);
	struct target *target = bank->target;
	int ret = ERROR_OK;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted before reading flash!\n");
		return ERROR_TARGET_NOT_HALTED;
	}

	uwp5661_init_sw_csn(target);

	uwp5661_set_sfc_clk(target);

	spiflash_select_xip(uwp5661_info, target, flash->support_4addr, TRUE);

	if (flash->support_4addr == TRUE) {
		ret = spiflash_4addr_enable(uwp5661_info, target);
		if (ret != ERROR_OK) {
			LOG_ERROR("uwp5661 SPI 4Byte Address mode switching ON failed!\n");
			spiflash_reset_anyway(uwp5661_info, target);
			spiflash_select_xip(uwp5661_info, target, FALSE, TRUE);
			return ret;
		}
	}

	LOG_INFO("Flash Read");
	spiflash_data_read(target, offset, count, buffer);

	if (flash->support_4addr == TRUE) {
		ret = spiflash_4addr_disable(uwp5661_info, target);
		if (ret != ERROR_OK) {
			LOG_ERROR("uwp5661 SPI 4Byte Address mode switching OFF failed!\n");
			spiflash_reset_anyway(uwp5661_info, target);
			spiflash_select_xip(uwp5661_info, target, FALSE, TRUE);
			return ret;
		}
	}

	spiflash_select_xip(uwp5661_info, target, FALSE, TRUE);

	return ret;
}

static int uwp5661_probe(struct flash_bank *bank)
{
	struct uwp5661_flash_bank *uwp5661_info = bank->driver_priv;
	struct target *target = bank->target;
	struct uwp_flash *flash = &(uwp5661_info->flash);
	SFC_CMD_DES_T cmd_desc[2];
	uint32_t read_data = 0;
	const struct flash_device *p = flash_devices;

	bank->base = UWP5661_FLASH_BASE_ADDRESS;
	uwp5661_info->probed = 0;

	memset(uwp5661_info->prev_cmd_info_buf_cache, 0 , sizeof(uint32_t)*INFO_BUF_MAX);
	uwp5661_info->cmd_buf_cache_bitmap = 0xFFF;
	uwp5661_info->sfc_cmd_cfg_cache = 0xFFFFFFFF;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted before probing flash!\n");
		return ERROR_TARGET_NOT_HALTED;
	}

	uwp5661_init_sw_csn(target);
	uwp5661_force_csn(target, FALSE);

	uwp5661_set_sfc_clk(target);

	spiflash_select_xip(uwp5661_info, target, FALSE, FALSE);

	sfcdrv_int_cfg(target, FALSE);

	spiflash_reset_anyway(uwp5661_info, target);

	/* scan device */
	create_cmd(&(cmd_desc[0]), CMD_READ_ID, BYTE_NUM_1, CMD_MODE_WRITE, BIT_MODE_1, SEND_MODE_0);
	create_cmd(&(cmd_desc[1]),           0, BYTE_NUM_3, CMD_MODE_READ , BIT_MODE_1, SEND_MODE_0);

	spiflash_read_write(uwp5661_info, target, cmd_desc, 2, &read_data);
	for (; p->name ; p++) {
		if (p->device_id == read_data) {
			uwp5661_info->param = p;
			break;
		}
	}
	if (p->name == NULL) {
		LOG_ERROR("Unsupported ID : 0x%x", read_data);
		return ERROR_FAIL;
	}

	/* config flash and bank*/
	flash->cs = 1;
	flash->name = p->name;
	flash->size = p->size_in_bytes;
	flash->page_size = p->pagesize;
	flash->sector_size = p->sectorsize;
	flash->work_mode = SPI_MODE;    /* Force using SPI 1bit mode */
	flash->support_4addr = (flash->size > (1<<24)) ? TRUE : FALSE;
	flash->dummy_clocks = DUMMY_4CLOCKS;
	bank->num_sectors = p->size_in_bytes / p->sectorsize;
	bank->sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	for (int i = 0; i < (bank->num_sectors); i++) {
		bank->sectors[i].size = flash->sector_size;
		bank->sectors[i].offset = i * flash->sector_size;
		bank->sectors[i].is_erased = -1;
	}

	uwp5661_info->probed = 1;

	spiflash_select_xip(uwp5661_info, target, FALSE, TRUE);

	return ERROR_OK;
}

static int uwp5661_auto_probe(struct flash_bank *bank)
{
	return uwp5661_probe(bank);
}

static const struct command_registration uwp5661_exec_command_handlers[] = {
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration uwp5661_command_handlers[] = {
	{
		.name = "uwp5661",
		.mode = COMMAND_ANY,
		.help = "uwp5661 flash command group",
		.usage = "",
		.chain = uwp5661_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

FLASH_BANK_COMMAND_HANDLER(uwp5661_flash_bank_command)
{
	struct uwp5661_flash_bank *uwp5661_info = NULL;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	uwp5661_info = malloc(sizeof(struct uwp5661_flash_bank));

	if (uwp5661_info == NULL) {
		LOG_ERROR("No uwp5661_info");
		return ERROR_FAIL;
	}

	bank->driver_priv = uwp5661_info;
	uwp5661_info->probed = 0;

	return ERROR_OK;
}

struct flash_driver uwp5661_flash = {
	.name = "uwp5661",
	.commands = uwp5661_command_handlers,
	.flash_bank_command = uwp5661_flash_bank_command,
	.erase = uwp5661_erase,
	.write = uwp5661_write,
	.read = uwp5661_read,
	.probe = uwp5661_probe,
	.auto_probe = uwp5661_auto_probe,
	.free_driver_priv = default_flash_free_driver_priv,
};
