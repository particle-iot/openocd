/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2017 by Angelo Dureghello                               *
 *   angelo@sysam.it                                                       *                                         *
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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>  *
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/drivers/libusb_common.h>
#include <jtag/interface.h>
#include <jtag/tcl.h>
#include <helper/log.h>
#include <target/bdm_cf26.h>

#include "libusb_common.h"

/* size of usb communication buffers */
#define PEMU_MAX_PKT_SIZE	1280
#define PEMU_MAX_BIG_BLOCK 	0x4a8
#define PEMU_STD_PKT_SIZE	256

#define PEMU_USB_BUFSIZE	(PEMU_MAX_PKT_SIZE + 1)
/* initialize P&E Multilink Universal device */
#define PEMU_CMD_INIT		0x11
/* timeout */
#define PEMU_USB_TIMEOUT	1000
/* len of reply */
#define PEMU_CMD_REPLY_LEN	4

static const uint16_t pemu_vid[] = { 0x1357, 0 };
static const uint16_t pemu_pid[] = { 0x0503, 0 };

static const char REP_VERSION_INFO[] = {0x99, 0x66, 0x00, 0x64};

enum pemu_pkt_types {
	PEMU_PT_GENERIC = 0xaa55,
	PEMU_PT_CMD = 0xab56,
	PEMU_PT_WBLOCK = 0xbc67,
};

enum {
	CMD_TYPE_GENERIC = 0x01,
	CMD_TYPE_IFACE = 0x04,
	CMD_TYPE_DATA = 0x07,
};

enum {
	CMD_PEMU_RESET = 0x01,
	CMD_PEMU_GO = 0x02,
	CMD_PEMU_BDM_MEM_R = 0x11,
	CMD_PEMU_BDM_MEM_W = 0x15,
	CMD_PEMU_BDM_REG_R = 0x13,
	CMD_PEMU_BDM_REG_W = 0x16,
	CMD_PEMU_BDM_SCR_W = 0x14,
	CMD_PEMU_GET_ALL_CPU_REGS = 0x18,
	CMD_PEMU_W_MEM_BLOCK = 0x19,
	CMD_PEMU_GET_VERSION_STR = 0x0b,
};

struct pemu_hdr {
	uint16_t pkt_type;
	uint16_t len;
} __attribute__ ((packed));

struct pemu_pkt {
	struct pemu_hdr hdr;
	uint8_t cmd_type;
	uint8_t cmd_pemu;
	uint8_t payload[PEMU_MAX_PKT_SIZE - (sizeof(struct pemu_hdr) + 1)];
};

struct pemu_pkt_wmem {
	struct pemu_hdr hdr;
	uint8_t cmd_type;
	uint8_t cmd_pemu;
	uint16_t dlen;
	uint32_t address;
};

struct pemu {
	/* this driver struct must be the first struct field */
	struct bdm_cf26_driver bdm_cf26;
	/* usb handle */
	struct jtag_libusb_device_handle *devh;
	/* send and receive buffer */
	uint8_t buffer[PEMU_USB_BUFSIZE];
	/* count data to send and to read */
	unsigned int count;
	/* endpoints */
	unsigned int endpoint_in;
	unsigned int endpoint_out;
};

#define OFFS_BDM_BUFF	sizeof(struct pemu_hdr) + 1

/* pemu instance */
static struct pemu pemu_context;

extern struct jtag_interface pemu_interface;

static int_least32_t pemu_read_pc(const struct bdm_cf26_driver *bdm);
static int pemu_write_pc(const struct bdm_cf26_driver *bdm, uint32_t value);

static int get_version_field(char *buff, int pos, char *res)
{
	int p = 0;
	char *c = buff, *q;

	for (pos--; p < pos; p++) {
		c = strchr(c, ',');
		if (!c) return ERROR_FAIL;
		c++;
	}

	q = strchr(c, ',');
	memcpy(res, c, q - c);
	buff[q - c] = 0;

	return ERROR_OK;
}

static int pemu_send_and_recv(struct pemu *pemu)
{
	unsigned int count;

	/* send request */
	count = jtag_libusb_bulk_write(pemu->devh,
			pemu->endpoint_out | LIBUSB_ENDPOINT_OUT,
			(char *)pemu->buffer, pemu->count, PEMU_USB_TIMEOUT);

	if (count != pemu->count) {
		LOG_ERROR("PEMU communication error: can't write");
		return ERROR_FAIL;
	}

	count = jtag_libusb_bulk_read(pemu->devh,
			pemu->endpoint_in | LIBUSB_ENDPOINT_IN ,
			(char *)pemu->buffer,
			PEMU_STD_PKT_SIZE, PEMU_USB_TIMEOUT * 3);

	if (count != PEMU_STD_PKT_SIZE) {
		LOG_ERROR("PEMU communication error: can't read");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}


static int pemu_open(struct pemu *pemu)
{
	(void)memset(pemu, 0, sizeof(*pemu));

	if (jtag_libusb_open(pemu_vid, pemu_pid, NULL, &pemu->devh)
		!= ERROR_OK) {

		LOG_ERROR("P&E Multilink Universal interface not found");

		return ERROR_FAIL;
	}

	LOG_INFO("P&E Multilink Universal interface detected");

	if (jtag_libusb_claim_interface(pemu->devh, 0) != ERROR_OK)
		return ERROR_FAIL;

	return ERROR_OK;
}

static inline uint8_t * pemu_get_bdm_buff(struct pemu *bdev)
{
	return (bdev->buffer + OFFS_BDM_BUFF);
}

static int pemu_send_generic(struct pemu *bdev, uint8_t cmd_type, uint16_t len)
{
	struct pemu_pkt *pb = (struct pemu_pkt *)bdev->buffer;

	h_u16_to_be((uint8_t *)&pb->hdr.pkt_type, PEMU_PT_CMD);
	h_u16_to_be((uint8_t *)&pb->hdr.len, len + 1);

	pb->cmd_type = cmd_type;

	bdev->count = PEMU_STD_PKT_SIZE;

	if (pemu_send_and_recv(bdev) != ERROR_OK)
		return ERROR_FAIL;

	return ERROR_OK;
}

static int pemu_assert_reset(const struct bdm_cf26_driver *bdm)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_RESET;
	buff[1] = 0xf0;

	return pemu_send_generic(bdev, CMD_TYPE_DATA, 2);
}

static int pemu_deassert_reset(const struct bdm_cf26_driver *bdm)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_RESET;
	buff[1] = 0xf8;

	return pemu_send_generic(bdev, CMD_TYPE_DATA, 2);
}

static int pemu_halt(const struct bdm_cf26_driver *bdm)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_RESET;
	buff[1] = 0xf8;

	return pemu_send_generic(bdev, CMD_TYPE_DATA, 2);
}

static int pemu_go(const struct bdm_cf26_driver *bdm)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_GO;
	buff[1] =  0xfc;
	h_u16_to_be((uint8_t *)&buff[2], CMD_BDM_GO);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 4);

	buff[0] = CMD_PEMU_BDM_REG_R;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_READ_BDM_REG);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 3);

	return ERROR_OK;
}

static int pemu_get_versions(struct pemu *pemu)
{
	char version_fw[16];
	struct pemu_pkt *pb = (struct pemu_pkt *)pemu_context.buffer;

	h_u16_to_be((uint8_t *)&pb->hdr.pkt_type, PEMU_PT_GENERIC);
	h_u16_to_be((uint8_t *)&pb->hdr.len, 2);
	pb->cmd_type = CMD_TYPE_GENERIC;
	pb->cmd_pemu = CMD_PEMU_GET_VERSION_STR;

	pemu_context.count = PEMU_STD_PKT_SIZE;

	if (pemu_send_and_recv(&pemu_context) != ERROR_OK)
		return ERROR_FAIL;

	if (memcmp(pemu_context.buffer,
		REP_VERSION_INFO, sizeof(REP_VERSION_INFO)) != 0) {
		LOG_ERROR("PEMU communication error: can't get version info");
		return ERROR_FAIL;
	}

	if (get_version_field((char *)&pemu_context.buffer[PEMU_CMD_REPLY_LEN],
		7, version_fw) != ERROR_OK) {
		LOG_ERROR("PEMU communication error: can't get version info");
		return ERROR_FAIL;
	}

	LOG_INFO("P&E Multilink Universal fw version %s", version_fw);

	if (strcmp(version_fw, "9.60") != 0) {
		LOG_ERROR("P&E Multilink Universal wrong FW version, "
			  "please flash correct CFv234 firmware."
		);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int_least32_t pemu_read_scr_reg(const struct bdm_cf26_driver *bdm,
				      uint16_t reg)
{
	uint32_t value;
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_MEM_R;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_READ_SCM_REG);
	h_u32_to_be((uint8_t *)&buff[3], reg);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 7);

	value = be_to_h_u32(buff);

	return value;
}

static int pemu_write_scr_reg(const struct bdm_cf26_driver *bdm,
				      uint16_t reg, uint32_t value)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_SCR_W;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_WRITE_SCM_REG);
	h_u32_to_be((uint8_t *)&buff[3], reg);
	h_u32_to_be((uint8_t *)&buff[7], value);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 11);

	return ERROR_OK;
}

static int_least32_t pemu_read_pc(const struct bdm_cf26_driver *bdm)
{
	return pemu_read_scr_reg(bdm, SYSC_PC);
}

static int pemu_write_pc(const struct bdm_cf26_driver *bdm,
				   uint32_t value)
{
	return pemu_write_scr_reg(bdm, SYSC_PC, value);
}

/**
 * Read all regs in one shot
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @returns memory buffer with all regs
 */
static int pemu_read_all_cpu_regs(const struct bdm_cf26_driver *bdm,
				  uint8_t **reg_buf)
{
	struct pemu *bdev = (struct pemu *)bdm;
	struct pemu_pkt *pkt = (struct pemu_pkt *)bdev->buffer;

	pkt->cmd_pemu = CMD_PEMU_GET_ALL_CPU_REGS;

	if (pemu_send_generic(bdev, CMD_TYPE_DATA, 1) == ERROR_OK) {

		*reg_buf = &bdev->buffer[5];

		return ERROR_OK;
	}

	return ERROR_FAIL;
}

/**
 * Read a BDM module register
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param reg 00 to 1f register to read
 * @returns 8-bit memory contents, negative error code failure
 */
static int_least32_t pemu_read_dm_reg(const struct bdm_cf26_driver *bdm,
				      uint8_t reg)
{
	uint32_t value;
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_REG_R;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_READ_BDM_REG | reg);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 3);

	value = be_to_h_u32(buff);

	return value;
}

/**
 * Read a A/D CPU register
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param reg AD register to read
 * @returns 8-bit memory contents, negative error code failure
 */
static int_least32_t pemu_read_ad_reg(const struct bdm_cf26_driver *bdm,
				      uint8_t reg)
{
	uint32_t value;
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_REG_R;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_READ_CPU_AD_REG | reg);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 3);

	value = be_to_h_u32(buff);

	return value;
}

/**
 * Read a byte from CPU memory location
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param address address to read the data form
 * @returns 8-bit memory contents, negative error code failure
 */
static int_least8_t pemu_read_mem_byte(const struct bdm_cf26_driver *bdm,
					uint32_t address)
{
	int value;
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_MEM_R;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_READ_MEM_BYTE);
	h_u32_to_be((uint8_t *)&buff[3], address);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 7);

	/*
	 * heh, this command (read byte) returns anyway a short, not a byte
	 */
	value = be_to_h_u16(buff);

	return (uint8_t)(value);
}

/**
 * Read a word (16 bit) from CPU memory location
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param address address to read the data form
 * @returns 16-bit memory contents, negative error code failure
 */
static int_least16_t pemu_read_mem_word(const struct bdm_cf26_driver *bdm,
					uint32_t address)
{
	int value;
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_MEM_R;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_READ_MEM_WORD);
	h_u32_to_be((uint8_t *)&buff[3], address);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 7);

	/*
	 * heh, this command (read byte) returns anyway a short, not a byte
	 */
	value = be_to_h_u16(buff);

	return (uint16_t)value;
}

/**
 * Read a long word (32-bit) from CPU memory location
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param address address to read the data form
 * @returns 328-bit memory contents, negative error code failure
 */
static int_least32_t pemu_read_mem_long(const struct bdm_cf26_driver *bdm,
					uint32_t address)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_MEM_R;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_READ_MEM_LONG);
	h_u32_to_be((uint8_t *)&buff[3], address);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 7);

	return be_to_h_u32(buff);
}

/**
 * Write a BDM module register
 * @param reg of bdm module to write
 * @param value address to write the data to
 * @returns error code
 */
static int pemu_write_dm_reg(const struct bdm_cf26_driver *bdm,
					uint8_t reg, uint32_t value)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_REG_W;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_WRITE_BDM_REG | reg);
	h_u32_to_be((uint8_t *)&buff[3], value);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 7);

	return ERROR_OK;
}

/**
 * Write a cpu A/D register
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param address address to write the data to
 * @returns error code
 */
static int pemu_write_ad_reg(const struct bdm_cf26_driver *bdm,
					uint8_t reg, uint32_t value)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_REG_W;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_WRITE_CPU_AD_REG | reg);
	h_u32_to_be((uint8_t *)&buff[3], value);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 7);

	return ERROR_OK;
}

/**
 * Write a byte at CPU memory location
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param address address to write the data to
 * @returns error code
 */
static int pemu_write_mem_byte(const struct bdm_cf26_driver *bdm,
					uint32_t address, uint8_t value)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_MEM_W;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_WRITE_MEM_BYTE);
	h_u32_to_be((uint8_t *)&buff[3], address);
	/*
	 * heh, pemu wants a 16 bit value here
	 */
	h_u16_to_be((uint8_t *)&buff[7], value);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 11);

	return ERROR_OK;
}

/**
 * Write a word at CPU memory location
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param address address to write the data to
 * @param value value to write
 * @returns error code
 */
static int pemu_write_mem_word(const struct bdm_cf26_driver *bdm,
					uint32_t address, uint16_t value)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_MEM_W;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_WRITE_MEM_WORD);
	h_u32_to_be((uint8_t *)&buff[3], address);
	/*
	 * heh, pemu wants a 16 bit value here
	 */
	h_u16_to_be((uint8_t *)&buff[7], value);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 11);

	return ERROR_OK;
}

/**
 * Write a long at CPU memory location
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param address address to write the data to
 * @param value value to write
 * @returns error code
 */
static int pemu_write_mem_long(const struct bdm_cf26_driver *bdm,
					uint32_t address, uint32_t value)
{
	struct pemu *bdev = (struct pemu *)bdm;
	uint8_t *buff = pemu_get_bdm_buff(bdev);

	buff[0] = CMD_PEMU_BDM_MEM_W;
	h_u16_to_be((uint8_t *)&buff[1], CMD_BDM_WRITE_MEM_LONG);
	h_u32_to_be((uint8_t *)&buff[3], address);
	h_u32_to_be((uint8_t *)&buff[7], value);

	pemu_send_generic(bdev, CMD_TYPE_DATA, 11);

	return ERROR_OK;
}

/**
 * Dump memory block, using specific BDM commands
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param address address to read the data form
 * @returns 8-bit memory contents, negative error code failure
 */
int pemu_read_memory (const struct bdm_cf26_driver *bdm, uint32_t address,
		    uint32_t size, uint32_t count, uint8_t *buffer)
{
	int len, longs, bytes;

	len = size * count;
	longs = len / 4;
	bytes = len % 4;

	while(longs--) {
		h_u32_to_be(buffer, pemu_read_mem_long(bdm, address));
		address += sizeof(uint32_t);
		buffer += sizeof(uint32_t);
	}
	while(bytes--)
		*buffer++ = pemu_read_mem_byte(bdm, address++);

	return ERROR_OK;
}

/**
 * Write a memory buffer to a specific location
 * @param bdm_cf26 pointer to a gnenric BDM deivce
 * @param address address where to write the data (target sram)
 * @param dlen length of data to write (in bytes)
 * @param buffer source data buffer
 *
 * P&E m.u. has propertary all-inclusive bdm-fahter commands for memory write,
 * i am assuming they are more performant then their bdm dump/write friends,
 * so i am using them
 */
static void pemu_send_bigbuff(const struct bdm_cf26_driver *bdm, uint32_t address,
		    uint32_t dlen, const uint8_t *buffer)
{
	struct pemu *bdev = (struct pemu *)bdm;
	struct pemu_pkt_wmem *pkt = (struct pemu_pkt_wmem *)bdev->buffer;
	uint16_t to_send, remainder;

	remainder = dlen % 4;

	while (dlen >= 4) {
		if (dlen > PEMU_MAX_BIG_BLOCK)
			to_send = PEMU_MAX_BIG_BLOCK;
		else {
			to_send = dlen - remainder;
		}

		h_u16_to_be((uint8_t *)&pkt->hdr.pkt_type, PEMU_PT_WBLOCK);
		pkt->cmd_type = CMD_TYPE_DATA;
		pkt->cmd_pemu = CMD_PEMU_W_MEM_BLOCK;
		h_u16_to_be((uint8_t *)&pkt->hdr.len, to_send + 8);
		h_u16_to_be((uint8_t *)&pkt->dlen, to_send);
		h_u32_to_be((uint8_t *)&pkt->address, address);

		memcpy(bdev->buffer + sizeof(struct pemu_pkt_wmem),
			       buffer, to_send);

		/* pemu wants a padded packet */
		bdev->count = PEMU_MAX_PKT_SIZE;

		pemu_send_and_recv(bdev);

		dlen -= to_send;
		buffer += to_send;
		address += to_send;
	}

	while (remainder--)
		pemu_write_mem_byte(bdm, address++, *buffer++);
}

int pemu_write_memory (const struct bdm_cf26_driver *bdm, uint32_t address,
			uint32_t size, uint32_t count, const uint8_t *buffer)
{
	pemu_send_bigbuff(bdm, address, count * size, buffer);

	return ERROR_OK;
}

static int pemu_interface_init(void)
{
	int err;

	if (pemu_open(&pemu_context) != ERROR_OK) {
		LOG_ERROR("Can't open PEMU device");
		return ERROR_FAIL;
	} else {
		LOG_DEBUG("PEMU init success");
	}

	err = jtag_libusb_choose_interface(pemu_context.devh,
			&pemu_context.endpoint_in,
			&pemu_context.endpoint_out, -1, -1, -1);

	if (!err) {
		LOG_INFO("P&E Multilink Universal endpoints, in %u, out %u",
				pemu_context.endpoint_in,
				pemu_context.endpoint_out
			);
	} else {
		LOG_ERROR("P&E Multilink Universal, can't get endpoints");
		return ERROR_FAIL;
	}

	pemu_assert_reset((const struct bdm_cf26_driver *)&pemu_context);
	usleep(100000);
	pemu_deassert_reset((const struct bdm_cf26_driver *)&pemu_context);
	usleep(100000);

	if (pemu_get_versions(&pemu_context) != ERROR_OK) {
		return ERROR_FAIL;
	}

	pemu_context.bdm_cf26.read_ad_reg = pemu_read_ad_reg;
	pemu_context.bdm_cf26.read_dm_reg = pemu_read_dm_reg;
	pemu_context.bdm_cf26.read_sc_reg = pemu_read_scr_reg;
	pemu_context.bdm_cf26.read_mem_byte = pemu_read_mem_byte;
	pemu_context.bdm_cf26.read_mem_word = pemu_read_mem_word;
	pemu_context.bdm_cf26.read_mem_long = pemu_read_mem_long;
	pemu_context.bdm_cf26.write_ad_reg = pemu_write_ad_reg;
	pemu_context.bdm_cf26.write_dm_reg = pemu_write_dm_reg;
	pemu_context.bdm_cf26.write_sc_reg = pemu_write_scr_reg;
	pemu_context.bdm_cf26.write_mem_byte = pemu_write_mem_byte;
	pemu_context.bdm_cf26.write_mem_word = pemu_write_mem_word;
	pemu_context.bdm_cf26.write_mem_long = pemu_write_mem_long;
	pemu_context.bdm_cf26.read_pc = pemu_read_pc;
	pemu_context.bdm_cf26.write_pc = pemu_write_pc;
	pemu_context.bdm_cf26.assert_reset = pemu_assert_reset;
	pemu_context.bdm_cf26.deassert_reset = pemu_deassert_reset;
	pemu_context.bdm_cf26.halt = pemu_halt;
	pemu_context.bdm_cf26.go = pemu_go;
	pemu_context.bdm_cf26.read_memory = pemu_read_memory;
	pemu_context.bdm_cf26.write_memory = pemu_write_memory;
	pemu_context.bdm_cf26.get_all_cpu_regs = pemu_read_all_cpu_regs;

	/* link interface <-> transport */
	pemu_interface.bdm_cf26 = (const struct bdm_cf26_driver *)&pemu_context;

	return ERROR_OK;
}

static int pemu_interface_quit(void)
{
	jtag_libusb_close(pemu_context.devh);

	return ERROR_OK;
}

static int pemu_interface_execute_queue(void)
{
	return ERROR_OK;
}

static const struct command_registration pemu_interface_command_handlers[] = {
	{
		.name = "jtag",
		.mode = COMMAND_ANY,
		.usage = "",
	},

	COMMAND_REGISTRATION_DONE
};

const char *pemu_transports[] = { "bdm_cf26", NULL };

struct jtag_interface pemu_interface = {
	.name		= "pemu",
	.supported	= 0,
	.init		= pemu_interface_init,
	.quit		= pemu_interface_quit,
	.transports     = pemu_transports,
	.commands	= pemu_interface_command_handlers,
	.execute_queue	= pemu_interface_execute_queue,
};
