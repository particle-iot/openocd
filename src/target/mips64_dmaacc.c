/***************************************************************************
 *   Copyright (C) 2008 by John McCarthy                                   *
 *   jgmcc@magma.ca                                                        *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by David T.L. Wong                                 *
 *                                                                         *
 *   Copyright (C) 2013 by Dongxue Zhang                                   *
 *   elta.era@gmail.com                                                    *
 *                                                                         *
 *   Copyright (C) 2013 by FengGao                                         *
 *   gf91597@gmail.com                                                     *
 *                                                                         *
 *   Copyright (C) 2013 by Jia Liu                                         *
 *   proljc@gmail.com                                                      *
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
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mips64_dmaacc.h"

static int mips64_dmaacc_read_mem8(struct mips_ejtag *ejtag_info,
		uint64_t addr, int count, uint8_t *buf);
static int mips64_dmaacc_read_mem16(struct mips_ejtag *ejtag_info,
		uint64_t addr, int count, uint16_t *buf);
static int mips64_dmaacc_read_mem32(struct mips_ejtag *ejtag_info,
		uint64_t addr, int count, uint32_t *buf);
static int mips64_dmaacc_read_mem64(struct mips_ejtag *ejtag_info,
		uint64_t addr, int count, uint64_t *buf);

static int mips64_dmaacc_write_mem8(struct mips_ejtag *ejtag_info,
		uint64_t addr, int count, uint8_t *buf);
static int mips64_dmaacc_write_mem16(struct mips_ejtag *ejtag_info,
		uint64_t addr, int count, uint16_t *buf);
static int mips64_dmaacc_write_mem32(struct mips_ejtag *ejtag_info,
		uint64_t addr, int count, uint32_t *buf);
static int mips64_dmaacc_write_mem64(struct mips_ejtag *ejtag_info,
		uint64_t addr, int count, uint64_t *buf);

/*
 * The following logic shamelessly cloned from HairyDairyMaid's wrt54g_debrick
 * to support the Broadcom BCM5352 SoC in the Linksys WRT54GL wireless router
 * (and any others that support EJTAG DMA transfers).
 * Note: This only supports memory read/write. Since the BCM5352 doesn't
 * appear to support PRACC accesses, all debug functions except halt
 * do not work.  Still, this does allow erasing/writing flash as well as
 * displaying/modifying memory and memory mapped registers.
 */

static int ejtag_dma_read(struct mips_ejtag *ejtag_info, uint64_t addr, uint64_t *data)
{
	uint64_t v;
	uint32_t ejtag_ctrl;
	int retries = RETRY_ATTEMPTS;

begin_ejtag_dma_read:

	/* Setup Address */
	v = addr;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ADDRESS);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Initiate DMA Read & set DSTRT */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = EJTAG_CTRL_DMAACC | EJTAG_CTRL_DRWN | EJTAG_CTRL_DMA_WORD | EJTAG_CTRL_DSTRT | ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);

	/* Wait for DSTRT to Clear */
	do {
		ejtag_ctrl = EJTAG_CTRL_DMAACC | ejtag_info->ejtag_ctrl;
		mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	} while (ejtag_ctrl & EJTAG_CTRL_DSTRT);

	/* Read Data */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
	mips_ejtag_drscan_64(ejtag_info, data);

	/* Clear DMA & Check DERR */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	if (ejtag_ctrl  & EJTAG_CTRL_DERR) {
		if (retries--) {
			LOG_ERROR("DMA Read Addr = %16.16" PRIx64 "  Data = ERROR ON READ (retrying)", addr);
			goto begin_ejtag_dma_read;
		} else
			LOG_ERROR("DMA Read Addr = %16.16" PRIx64 "  Data = ERROR ON READ", addr);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

static int ejtag_dma_read_w(struct mips_ejtag *ejtag_info, uint64_t addr, uint32_t *data)
{
	uint64_t v;
	uint32_t ejtag_ctrl;
	int retries = RETRY_ATTEMPTS;

begin_ejtag_dma_read:

	/* Setup Address */
	v = addr;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ADDRESS);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Initiate DMA Read & set DSTRT */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = EJTAG_CTRL_DMAACC | EJTAG_CTRL_DRWN | EJTAG_CTRL_DMA_WORD | EJTAG_CTRL_DSTRT | ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);

	/* Wait for DSTRT to Clear */
	do {
		ejtag_ctrl = EJTAG_CTRL_DMAACC | ejtag_info->ejtag_ctrl;
		mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	} while (ejtag_ctrl & EJTAG_CTRL_DSTRT);

	/* Read Data */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
	mips_ejtag_drscan_32(ejtag_info, data);

	/* Clear DMA & Check DERR */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	if (ejtag_ctrl  & EJTAG_CTRL_DERR) {
		if (retries--) {
			LOG_ERROR("DMA Read Addr = %16.16" PRIx64 "  Data = ERROR ON READ (retrying)", addr);
			goto begin_ejtag_dma_read;
		} else
			LOG_ERROR("DMA Read Addr = %16.16" PRIx64 "  Data = ERROR ON READ", addr);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	/* Handle the bigendian/littleendian */
	if (addr & 0x2)
		*data = (v >> 32) & 0xffffffff;
	else
		*data = (v & 0xffffffff);


	return ERROR_OK;
}

static int ejtag_dma_read_h(struct mips_ejtag *ejtag_info, uint64_t addr, uint16_t *data)
{
	uint64_t v;
	uint32_t ejtag_ctrl;
	int retries = RETRY_ATTEMPTS;

begin_ejtag_dma_read_h:

	/* Setup Address */
	v = addr;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ADDRESS);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Initiate DMA Read & set DSTRT */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = EJTAG_CTRL_DMAACC | EJTAG_CTRL_DRWN | EJTAG_CTRL_DMA_HALFWORD |
			EJTAG_CTRL_DSTRT | ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);

	/* Wait for DSTRT to Clear */
	do {
		ejtag_ctrl = EJTAG_CTRL_DMAACC | ejtag_info->ejtag_ctrl;
		mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	} while (ejtag_ctrl & EJTAG_CTRL_DSTRT);

	/* Read Data */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Clear DMA & Check DERR */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	if (ejtag_ctrl  & EJTAG_CTRL_DERR) {
		if (retries--) {
			LOG_ERROR("DMA Read Addr = %16.16" PRIx64 "  Data = ERROR ON READ (retrying)", addr);
			goto begin_ejtag_dma_read_h;
		} else
			LOG_ERROR("DMA Read Addr = %16.16" PRIx64 "  Data = ERROR ON READ", addr);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	/* Handle the bigendian/littleendian */
	switch (addr & 0x3) {
		case 0:
			*data = v & 0xffff;
			break;
		case 1:
			*data = (v >> 16) & 0xffff;
			break;
		case 2:
			*data = (v >> 32) & 0xffff;
			break;
		case 3:
			*data = (v >> 48) & 0xffff;
			break;
	}

	return ERROR_OK;
}

static int ejtag_dma_read_b(struct mips_ejtag *ejtag_info, uint64_t addr, uint8_t *data)
{
	uint64_t v;
	uint32_t ejtag_ctrl;
	int retries = RETRY_ATTEMPTS;

begin_ejtag_dma_read_b:

	/* Setup Address */
	v = addr;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ADDRESS);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Initiate DMA Read & set DSTRT */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = EJTAG_CTRL_DMAACC | EJTAG_CTRL_DRWN | EJTAG_CTRL_DMA_BYTE | EJTAG_CTRL_DSTRT | ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);

	/* Wait for DSTRT to Clear */
	do {
		ejtag_ctrl = EJTAG_CTRL_DMAACC | ejtag_info->ejtag_ctrl;
		mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	} while (ejtag_ctrl & EJTAG_CTRL_DSTRT);

	/* Read Data */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Clear DMA & Check DERR */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	if (ejtag_ctrl  & EJTAG_CTRL_DERR) {
		if (retries--) {
			LOG_ERROR("DMA Read Addr = %16.16" PRIx64 "  Data = ERROR ON READ (retrying)", addr);
			goto begin_ejtag_dma_read_b;
		} else
			LOG_ERROR("DMA Read Addr = %16.16" PRIx64 "  Data = ERROR ON READ", addr);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	/* Handle the bigendian/littleendian */
	switch (addr & 0x7) {
		case 0:
			*data = v & 0xff;
			break;
		case 1:
			*data = (v >> 8) & 0xff;
			break;
		case 2:
			*data = (v >> 16) & 0xff;
			break;
		case 3:
			*data = (v >> 24) & 0xff;
			break;
		case 4:
			*data = (v >> 32) & 0xff;
			break;
		case 5:
			*data = (v >> 40) & 0xff;
			break;
		case 6:
			*data = (v >> 48) & 0xff;
			break;
		case 7:
			*data = (v >> 56) & 0xff;
			break;
	}

	return ERROR_OK;
}

static int ejtag_dma_write(struct mips_ejtag *ejtag_info, uint64_t addr, uint64_t data)
{
	uint64_t v;
	uint32_t ejtag_ctrl;
	int retries = RETRY_ATTEMPTS;

begin_ejtag_dma_write:

	/* Setup Address */
	v = addr;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ADDRESS);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Setup Data */
	v = data;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Initiate DMA Write & set DSTRT */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = EJTAG_CTRL_DMAACC | EJTAG_CTRL_DMA_WORD | EJTAG_CTRL_DSTRT | ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);

	/* Wait for DSTRT to Clear */
	do {
		ejtag_ctrl = EJTAG_CTRL_DMAACC | ejtag_info->ejtag_ctrl;
		mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	} while (ejtag_ctrl & EJTAG_CTRL_DSTRT);

	/* Clear DMA & Check DERR */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	if (ejtag_ctrl  & EJTAG_CTRL_DERR) {
		if (retries--) {
			LOG_ERROR("DMA Write Addr = %16.16" PRIx64 "  Data = ERROR ON WRITE (retrying)", addr);
			goto begin_ejtag_dma_write;
		} else
			LOG_ERROR("DMA Write Addr = %16.16" PRIx64 "  Data = ERROR ON WRITE", addr);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

static int ejtag_dma_write_w(struct mips_ejtag *ejtag_info, uint64_t addr, uint64_t data)
{
	uint64_t v;
	uint32_t ejtag_ctrl;
	int retries = RETRY_ATTEMPTS;

	/* Handle the bigendian/littleendian */
	data &= 0xffffffff;
	data |= data << 32;

begin_ejtag_dma_write:

	/* Setup Address */
	v = addr;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ADDRESS);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Setup Data */
	v = data;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Initiate DMA Write & set DSTRT */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = EJTAG_CTRL_DMAACC | EJTAG_CTRL_DMA_WORD | EJTAG_CTRL_DSTRT | ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);

	/* Wait for DSTRT to Clear */
	do {
		ejtag_ctrl = EJTAG_CTRL_DMAACC | ejtag_info->ejtag_ctrl;
		mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	} while (ejtag_ctrl & EJTAG_CTRL_DSTRT);

	/* Clear DMA & Check DERR */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	if (ejtag_ctrl  & EJTAG_CTRL_DERR) {
		if (retries--) {
			LOG_ERROR("DMA Write Addr = %16.16" PRIx64 "  Data = ERROR ON WRITE (retrying)", addr);
			goto begin_ejtag_dma_write;
		} else
			LOG_ERROR("DMA Write Addr = %16.16" PRIx64 "  Data = ERROR ON WRITE", addr);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

static int ejtag_dma_write_h(struct mips_ejtag *ejtag_info, uint64_t addr, uint64_t data)
{
	uint64_t v;
	uint32_t ejtag_ctrl;
	int retries = RETRY_ATTEMPTS;

	/* Handle the bigendian/littleendian */
	data &= 0xffff;
	data |= data << 16;
	data |= data << 32;

begin_ejtag_dma_write_h:

	/* Setup Address */
	v = addr;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ADDRESS);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Setup Data */
	v = data;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Initiate DMA Write & set DSTRT */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = EJTAG_CTRL_DMAACC | EJTAG_CTRL_DMA_HALFWORD | EJTAG_CTRL_DSTRT | ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);

	/* Wait for DSTRT to Clear */
	do {
		ejtag_ctrl = EJTAG_CTRL_DMAACC | ejtag_info->ejtag_ctrl;
		mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	} while (ejtag_ctrl & EJTAG_CTRL_DSTRT);

	/* Clear DMA & Check DERR */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	if (ejtag_ctrl & EJTAG_CTRL_DERR) {
		if (retries--) {
			LOG_ERROR("DMA Write Addr = %64" PRIx64 "  Data = ERROR ON WRITE (retrying)", addr);
			goto begin_ejtag_dma_write_h;
		} else
			LOG_ERROR("DMA Write Addr = %64" PRIx64 "  Data = ERROR ON WRITE", addr);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

static int ejtag_dma_write_b(struct mips_ejtag *ejtag_info, uint64_t addr, uint64_t data)
{
	uint64_t v;
	uint32_t ejtag_ctrl;
	int retries = RETRY_ATTEMPTS;

	/* Handle the bigendian/littleendian */
	data &= 0xff;
	data |= data << 8;
	data |= data << 16;
	data |= data << 32;

begin_ejtag_dma_write_b:

	/*  Setup Address*/
	v = addr;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_ADDRESS);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Setup Data */
	v = data;
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_DATA);
	mips_ejtag_drscan_64(ejtag_info, &v);

	/* Initiate DMA Write & set DSTRT */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = EJTAG_CTRL_DMAACC | EJTAG_CTRL_DMA_BYTE | EJTAG_CTRL_DSTRT | ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);

	/* Wait for DSTRT to Clear */
	do {
		ejtag_ctrl = EJTAG_CTRL_DMAACC | ejtag_info->ejtag_ctrl;
		mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	} while (ejtag_ctrl & EJTAG_CTRL_DSTRT);

	/* Clear DMA & Check DERR */
	mips_ejtag_set_instr(ejtag_info, EJTAG_INST_CONTROL);
	ejtag_ctrl = ejtag_info->ejtag_ctrl;
	mips_ejtag_drscan_32(ejtag_info, &ejtag_ctrl);
	if (ejtag_ctrl & EJTAG_CTRL_DERR) {
		if (retries--) {
			LOG_ERROR("DMA Write Addr = %16.16" PRIx64 "  Data = ERROR ON WRITE (retrying)", addr);
			goto begin_ejtag_dma_write_b;
		} else
			LOG_ERROR("DMA Write Addr = %16.16" PRIx64 "  Data = ERROR ON WRITE", addr);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

int mips64_dmaacc_read_mem(struct mips_ejtag *ejtag_info, uint64_t addr, int size, int count, void *buf)
{
	switch (size) {
		case 1:
			return mips64_dmaacc_read_mem8(ejtag_info, addr, count, (uint8_t *)buf);
		case 2:
			return mips64_dmaacc_read_mem16(ejtag_info, addr, count, (uint16_t *)buf);
		case 4:
			return mips64_dmaacc_read_mem32(ejtag_info, addr, count, (uint32_t *)buf);
		case 8:
			return mips64_dmaacc_read_mem64(ejtag_info, addr, count, (uint64_t *)buf);
	}

	return ERROR_OK;
}

static int mips64_dmaacc_read_mem64(struct mips_ejtag *ejtag_info, uint64_t addr, int count, uint64_t *buf)
{
	int i;
	int	retval;

	for (i = 0; i < count; i++) {
		retval = ejtag_dma_read(ejtag_info, addr + i * sizeof(*buf), &buf[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int mips64_dmaacc_read_mem32(struct mips_ejtag *ejtag_info, uint64_t addr, int count, uint32_t *buf)
{
	int i;
	int	retval;

	for (i = 0; i < count; i++) {
		retval = ejtag_dma_read_w(ejtag_info, addr + i * sizeof(*buf), &buf[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int mips64_dmaacc_read_mem16(struct mips_ejtag *ejtag_info, uint64_t addr, int count, uint16_t *buf)
{
	int i;
	int retval;

	for (i = 0; i < count; i++) {
		retval = ejtag_dma_read_h(ejtag_info, addr + i * sizeof(*buf), &buf[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int mips64_dmaacc_read_mem8(struct mips_ejtag *ejtag_info, uint64_t addr, int count, uint8_t *buf)
{
	int i;
	int retval;

	for (i = 0; i < count; i++) {
		retval = ejtag_dma_read_b(ejtag_info, addr + i * sizeof(*buf), &buf[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

int mips64_dmaacc_write_mem(struct mips_ejtag *ejtag_info, uint64_t addr, int size, int count, void *buf)
{
	switch (size) {
		case 1:
			return mips64_dmaacc_write_mem8(ejtag_info, addr, count, (uint8_t *)buf);
		case 2:
			return mips64_dmaacc_write_mem16(ejtag_info, addr, count, (uint16_t *)buf);
		case 4:
			return mips64_dmaacc_write_mem32(ejtag_info, addr, count, (uint32_t *)buf);
		case 8:
			return mips64_dmaacc_write_mem64(ejtag_info, addr, count, (uint64_t *)buf);
	}

	return ERROR_OK;
}

static int mips64_dmaacc_write_mem64(struct mips_ejtag *ejtag_info, uint64_t addr, int count, uint64_t *buf)
{
	int i;
	int retval;

	for (i = 0; i < count; i++) {
		retval = ejtag_dma_write(ejtag_info, addr + i * sizeof(*buf), buf[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int mips64_dmaacc_write_mem32(struct mips_ejtag *ejtag_info, uint64_t addr, int count, uint32_t *buf)
{
	int i;
	int retval;

	for (i = 0; i < count; i++) {
		retval = ejtag_dma_write_w(ejtag_info, addr + i * sizeof(*buf), buf[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int mips64_dmaacc_write_mem16(struct mips_ejtag *ejtag_info, uint64_t addr, int count, uint16_t *buf)
{
	int i;
	int retval;

	for (i = 0; i < count; i++) {
		retval = ejtag_dma_write_h(ejtag_info, addr + i * sizeof(*buf), buf[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int mips64_dmaacc_write_mem8(struct mips_ejtag *ejtag_info, uint64_t addr, int count, uint8_t *buf)
{
	int i;
	int retval;

	for (i = 0; i < count; i++) {
		retval = ejtag_dma_write_b(ejtag_info, addr + i * sizeof(*buf), buf[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}
