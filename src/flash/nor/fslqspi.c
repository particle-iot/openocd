/***************************************************************************
 *   Copyright (C) 2017 DEIF A/S                                           *
 *   by Esben Haabendal <eha@deif.com>                                     *
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
 *   Free Software Foundation, Inc.                                        *
 *                                                                         *
 ***************************************************************************/
/*
 * Freescale Quad Serial Peripheral Interface (QSPI) driver
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include "spi.h"
#include <helper/time_support.h>
#include <helper/types.h>
#include <jtag/jtag.h>

struct fslqspi_target {
	char *name;
	uint32_t tap_idcode;
	uint32_t amba_base;
	uint32_t amba_size;
	uint32_t reg_base;
	unsigned txbuf_len; /* Length of TX buffer (bytes) */
};

static struct fslqspi_target target_devices[] = {
	/* name,         tap_idcode, amba_base,  amba_size,  reg_base, txbuf_len */
	{ "LS1021A/SAP", 0x16b0001d, 0x40000000, 0x20000000, 0x01550000, 64 },
	{ "LS1021A/DAP", 0x5ba00477, 0x40000000, 0x20000000, 0x01550000, 64 },
	{ NULL,          0,          0,          0,          0,          0 }
};

enum fslqspi_chipselect {
	PCSFA1,
	PCSFA2,
	PCSFB1,
	PCSFB2
};

enum fslqspi_endianness {
	BE64 = 0,
	LE32 = 1,
	BE32 = 2,
	LE64 = 3
};

struct fslqspi_flash_bank {
	enum fslqspi_chipselect chipselect;
	int probed;
	struct fslqspi_target *target_device;
	const struct flash_device *dev;
	enum fslqspi_endianness endianness;
};

static inline bool is_little_endian(struct flash_bank *bank)
{
	struct fslqspi_flash_bank *fslqspi_info = bank->driver_priv;
	/* LE32 & LE64 */
	return (fslqspi_info->endianness & 0x1) == 1;
}

static inline bool need_word_swap(struct flash_bank *bank)
{
	struct fslqspi_flash_bank *fslqspi_info = bank->driver_priv;
	/* BE32 & LE64 */
	return (fslqspi_info->endianness & 0x2) == 2;
}

/* Flash command timeouts (in ms) */
#define CMD_TIMEOUT	100
#define ERASE_TIMEOUT	3000
#define PROGRAM_TIMEOUT	100
#define TXB_TIMEOUT	100

/* Register addresses (offset from register base) */
#define FSLQSPI_MCR	0x0000
#define FSLQSPI_IPCR	0x0008
#define FSLQSPI_FLSHCR	0x000c
#define FSLQSPI_BUFnCR	0x0010
#define FSLQSPI_BFGENCR	0x0020
#define FSLQSPI_BUFnIND	0x0030
#define FSLQSPI_SFAR	0x0100
#define FSLQSPI_SMPR	0x0108
#define FSLQSPI_RBSR	0x010c
#define FSLQSPI_RBCT	0x0110
#define FSLQSPI_TBSR	0x0150
#define FSLQSPI_TBDR	0x0154
#define FSLQSPI_SR	0x015c
#define FSLQSPI_FR	0x0160
#define FSLQSPI_RSER	0x0164
#define FSLQSPI_SPNDST	0x0168
#define FSLQSPI_SPTRCLR	0x016c
#define FSLQSPI_SFA1AD	0x0180
#define FSLQSPI_SFA2AD	0x0184
#define FSLQSPI_SFB1AD	0x0188
#define FSLQSPI_SFB2AD	0x018c
#define FSLQSPI_RBDRn	0x0200
#define FSLQSPI_LUTKEY	0x0300
#define FSLQSPI_LCKCR	0x0304
#define FSLQSPI_LUTn	0x0310

/* Module configuration register bits */
#define FSLQSPI_MCR_CFG_MASK	0xffff000c
#define FSLQSPI_MCR_MDIS	0x00004000
#define FSLQSPI_MCR_CLR_TXF	0x00000800
#define FSLQSPI_MCR_CLR_RXF	0x00000400
#define FSLQSPI_MCR_END_CFG(mcr) ((mcr & 0xc) >> 2)
#define FSLQSPI_MCR_END_64BE	0x00000000
#define FSLQSPI_MCR_END_32LE	0x00000004
#define FSLQSPI_MCR_END_32BE	0x00000008
#define FSLQSPI_MCR_END_64LE	0x0000000c

/* IP configuration register */
#define FSLQSPI_IPCR_VALUE(seqid, paren, idatsz) \
	(((seqid & 0xf) << 24) | ((!!paren) << 16) | (idatsz & 0xffff))

/* RX buffer status register */
#define FSLQSPI_RBSR_RDCTR(rbsr) (rbsr >> 16)
#define FSLQSPI_RBSR_RDBFL(rbsr) ((rbsr & 0x00003f00) >> 8)

/* RX buffer control register */
#define FSLQSPI_RBCT_RXBRD	0x00000100
#define FSLQSPI_RBCT_WMRK(wmrk)	((wmrk) & 0x1f)

/* TX buffer status register */
#define FSLQSPI_TBSR_TRCTR(tbsr) (tbsr >> 16)
#define FSLQSPI_TBSR_TRBFL(tbsr) ((tbsr & 0x00001f00) >> 8)

/* Status register bits */
#define FSLQSPI_SR_TXFULL	0x08000000
#define FSLQSPI_SR_TXNE		0x01000000
#define FSLQSPI_SR_RXDMA	0x00800000
#define FSLQSPI_SR_RXFULL	0x00080000
#define FSLQSPI_SR_RXWE		0x00010000
#define FSLQSPI_SR_AHBGNT	0x00000020
#define FSLQSPI_SR_AHB_ACC	0x00000004
#define FSLQSPI_SR_IP_ACC	0x00000002
#define FSLQSPI_SR_BUSY		0x00000001

/* Flag register bits */
#define FSLQSPI_FR_TBFF		0x08000000
#define FSLQSPI_FR_TBUF		0x04000000
#define FSLQSPI_FR_ILLINE	0x00800000
#define FSLQSPI_FR_RBOF		0x00020000
#define FSLQSPI_FR_RBDF		0x00010000
#define FSLQSPI_FR_IUEF		0x00001000
#define FSLQSPI_FR_IPAEF	0x00000080
#define FSLQSPI_FR_IPIEF	0x00000040
#define FSLQSPI_FR_IPGEF	0x00000010
#define FSLQSPI_FR_TFF		0x00000001

/* Sequence pointer clear bits */
#define FSLQSPI_SPTRCLR_IPPTRC	0x00000100
#define FSLQSPI_SPTRCLR_BFPTRC	0x00000001

#define FSLQSPI_INSTR(instr, pads, operand) \
	(((instr & 0x3f) << 10) | ((pads & 0x3) << 8) | (operand & 0xff))

#define STOP_INSTR	0x00
#define CMD_INSTR	0x01
#define ADDR_INSTR	0x02
#define DUMMY_INSTR	0x03
#define MODE_INSTR	0x04
#define MODE2_INSTR	0x05
#define MODE4_INSTR	0x06
#define READ_INSTR	0x07
#define WRITE_INSTR	0x08
#define JMP_ON_CS_INSTR	0x09

struct fslqspi_sequence {
	char *name;
	bool prepared;
	uint16_t instr[8];
};

enum fslqspi_seqid {
	AHB_READ_SEQ = 0,
	READ_ID_SEQ,
	READ_STATUS_SEQ,
	READ_CONFIG_SEQ,
	WREN_SEQ,
	SECTOR_ERASE_SEQ,
	PAGE_PROGRAM_SEQ,
	READ_SEQ,
};

struct fslqspi_sequence fslqspi_sequences[] = {

	{ .name = "AHB read",
	  .instr = {
			/* Read Data bytes command on one pad */
			FSLQSPI_INSTR(CMD_INSTR, 0, 0x03),
			/* Send 24 address bits on one pad */
			FSLQSPI_INSTR(ADDR_INSTR, 0, 0x18),
			/* Read 64 bits (8 bytes) on one pad */
			FSLQSPI_INSTR(READ_INSTR, 0, 0x08),
			/* Jump to instruction 0 (CMD) */
			FSLQSPI_INSTR(JMP_ON_CS_INSTR, 0, 0x00) },
	},

	{ .name = "RDID (Read Identification)",
	  .instr = {
			/* Read Identification command on one pad */
			FSLQSPI_INSTR(CMD_INSTR, 0, 0x9f),
			/* Read 32 bits (4 bytes) on one pad */
			FSLQSPI_INSTR(READ_INSTR, 0, 0x04),
			/* Stop execution (deassert CS) */
			FSLQSPI_INSTR(STOP_INSTR, 0, 0x00) },
	},

	{ .name = "RDSR (Read Status Register)",
	  .instr = {
			/* Read Status Register command on one pad */
			FSLQSPI_INSTR(CMD_INSTR, 0, 0x05),
			/* Read 8 bits (1 byte) on one pad */
			FSLQSPI_INSTR(READ_INSTR, 0, 0x01),
			/* Stop execution (deassert CS) */
			FSLQSPI_INSTR(STOP_INSTR, 0, 0x00) },
	},

	{ .name = "RCR (Read Configuration Register)",
	  .instr = {
			/* Read Configuration Register command on one pad */
			FSLQSPI_INSTR(CMD_INSTR, 0, 0x35),
			/* Read 8 bits (1 byte) on one pad */
			FSLQSPI_INSTR(READ_INSTR, 0, 0x01),
			/* Stop execution (deassert CS) */
			FSLQSPI_INSTR(STOP_INSTR, 0, 0x00) },
	},

	{ .name = "WREN (Write Enable)",
	  .instr = {
			/* Write Enable command on one pad */
			FSLQSPI_INSTR(CMD_INSTR, 0, 0x06),
			/* Stop execution (deassert CS) */
			FSLQSPI_INSTR(STOP_INSTR, 0, 0x00) },
	},

	{ .name = "SE (64 kByte Sector Erase)",
	  .instr = {
			/* Sector Erase (64 kB) command on one pad */
			FSLQSPI_INSTR(CMD_INSTR, 0, 0xd8),
			/* Send 24 address bits on one pad */
			FSLQSPI_INSTR(ADDR_INSTR, 0, 0x18),
			/* Stop execution (deassert CS) */
			FSLQSPI_INSTR(STOP_INSTR, 0, 0x00) },
	},

	{ .name = "PP (Page Programming)",
	  .instr = {
			/* Page Programming command on one pad */
			FSLQSPI_INSTR(CMD_INSTR, 0, 0x02),
			/* Send 24 address bits on one pad */
			FSLQSPI_INSTR(ADDR_INSTR, 0, 0x18),
			/* Write data bytes on one pad */
			FSLQSPI_INSTR(WRITE_INSTR, 0, 0x00),
			/* Stop execution (deassert CS) */
			FSLQSPI_INSTR(STOP_INSTR, 0, 0x00) },
	},

	{ .name = "READ (Read Data Bytes)",
	  .instr = {
			/* Read Data bytes command on one pad */
			FSLQSPI_INSTR(CMD_INSTR, 0, 0x03),
			/* Send 24 address bits on one pad */
			FSLQSPI_INSTR(ADDR_INSTR, 0, 0x18),
			/* Read data on one pad */
			FSLQSPI_INSTR(READ_INSTR, 0, 0x00),
			/* Stop execution (deassert CS) */
			FSLQSPI_INSTR(STOP_INSTR, 0, 0x00) },
	},
};

FLASH_BANK_COMMAND_HANDLER(fslqspi_flash_bank_command)
{
	struct fslqspi_flash_bank *fslqspi_info;
	enum fslqspi_chipselect chipselect = PCSFA1;

	if (CMD_ARGC < 6 || CMD_ARGC > 7)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (CMD_ARGC == 7) {
		if (strcmp(CMD_ARGV[6], "a1") == 0)
			chipselect = PCSFA1;  /* default */
		else if (strcmp(CMD_ARGV[6], "a2") == 0)
			chipselect = PCSFA2;
		else if (strcmp(CMD_ARGV[6], "b1") == 0)
			chipselect = PCSFA2;
		else if (strcmp(CMD_ARGV[6], "b2") == 0)
			chipselect = PCSFA2;
		else {
			LOG_ERROR("Unknown arg: %s", CMD_ARGV[6]);
			return ERROR_COMMAND_SYNTAX_ERROR;
		}
	}

	fslqspi_info = calloc(1, sizeof(struct fslqspi_flash_bank));
	if (!fslqspi_info) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	fslqspi_info->chipselect = chipselect;
	bank->driver_priv = fslqspi_info;

	return ERROR_OK;
}

/* Read QSPI controller register */
static inline int fslqspi_reg_read(struct flash_bank *bank,
				   uint32_t offset, uint32_t *value)
{
	struct target *target = bank->target;
	struct fslqspi_flash_bank *fslqspi_info = bank->driver_priv;
	struct fslqspi_target *target_device = fslqspi_info->target_device;

	return target_read_u32(target, target_device->reg_base + offset,
			       value);
}

/* Write QSPI controller register */
static inline int fslqspi_reg_write(struct flash_bank *bank,
				    uint32_t offset, uint32_t value)
{
	struct target *target = bank->target;
	struct fslqspi_flash_bank *fslqspi_info = bank->driver_priv;
	struct fslqspi_target *target_device = fslqspi_info->target_device;

	return target_write_u32(target, target_device->reg_base + offset,
				value);
}

/* Poll QSPI controller register until mask is matched, or timeout happens */
static int fslqspi_reg_poll(struct flash_bank *bank,
			    uint32_t reg, uint32_t *reg_value,
			    uint32_t mask, uint32_t value,
			    int timeout,
			    int timeout_retval, const char *timeout_errmsg)
{
	int64_t endtime;
	uint32_t _reg_value;
	int retval;

	if (reg_value == NULL)
		reg_value = &_reg_value;

	endtime = timeval_ms() + timeout;
	do {
		retval = fslqspi_reg_read(bank, reg, reg_value);
		if (retval != ERROR_OK)
			return retval;
		if ((*reg_value & mask) == value)
			return ERROR_OK;
		alive_sleep(1);
	} while (timeval_ms() < endtime);

	if (timeout_errmsg != NULL)
		LOG_ERROR("%s", timeout_errmsg);
	return timeout_retval;
}

/* Helper macros to avoid numerous `if (retval != ERROR_OK) return retval`
 * statements */

/* Note: This macro uses the GCC extension "braced-group within expression",
 * which makes it usable as an r-value */
#define FSLQSPI_REG_READ(offset) ({					\
	uint32_t __value;						\
	int __retval = fslqspi_reg_read(bank, offset, &__value);	\
	if (__retval != ERROR_OK)					\
		return __retval;					\
	__value; })

#define FSLQSPI_REG_WRITE(offset, value) {				\
	int __retval = fslqspi_reg_write(bank, offset, value);		\
	if (__retval != ERROR_OK)					\
		return __retval; }

#define FSLQSPI_REG_POLL(r, rv, m, v, to, to_rv, to_em) {		\
	int __retval = fslqspi_reg_poll(bank, r, rv, m, v, to, to_rv, to_em); \
	if (__retval != ERROR_OK)					\
		return __retval; }

static int clear_rxbuf(struct flash_bank *bank)
{
	uint32_t mcr;

	/* Set MCR[CLR_RXF] */
	mcr = FSLQSPI_REG_READ(FSLQSPI_MCR);
	mcr &= FSLQSPI_MCR_CFG_MASK;
	mcr |= FSLQSPI_MCR_CLR_RXF;
	FSLQSPI_REG_WRITE(FSLQSPI_MCR, mcr);

	/* Wait for RBSR[RDBFL] and RBSR[RDCTR] to reset to 0 */
	FSLQSPI_REG_POLL(FSLQSPI_RBSR, NULL,
			 FSLQSPI_RBSR_RDBFL(0x3f) | FSLQSPI_RBSR_RDCTR(0xffff),
			 0, CMD_TIMEOUT, ERROR_FAIL,
			 "Timeout while waiting for RBSR reset");

	return ERROR_OK;
}

static int clear_txbuf(struct flash_bank *bank)
{
	uint32_t mcr;

	if (!(FSLQSPI_REG_READ(FSLQSPI_SR) & FSLQSPI_SR_TXNE))
		/* TX buffer is already empty */
		return ERROR_OK;

	/* Set MCR[CLR_TXF] */
	mcr = FSLQSPI_REG_READ(FSLQSPI_MCR);
	mcr &= FSLQSPI_MCR_CFG_MASK;
	mcr |= FSLQSPI_MCR_CLR_TXF;
	FSLQSPI_REG_WRITE(FSLQSPI_MCR, mcr);

	/* Wait for buffer to become empty */
	FSLQSPI_REG_POLL(FSLQSPI_SR, NULL, FSLQSPI_SR_TXNE, 0,
			 CMD_TIMEOUT, ERROR_FAIL, "Timeout polling SR[TXNE]");

	return ERROR_OK;
}

static int write_txbuf(struct flash_bank *bank,
		       const uint8_t *buf, size_t count)
{
	assert(count <= 64);

	while (count) {
		uint32_t word = 0;
		unsigned byte, bytes = (count < 4 ? count : 4);
		if (is_little_endian(bank)) {
			for (byte = 0; byte < bytes; byte++)
				word |= *(buf++) << (byte * 8);
		} else {
			for (byte = 0; byte < bytes; byte++)
				word |= *(buf++) << (24 - (byte * 8));
		}
		FSLQSPI_REG_WRITE(FSLQSPI_TBDR, word);
		count -= bytes;
	}

	return ERROR_OK;
}

static int prepare_sequence(struct flash_bank *bank, enum fslqspi_seqid seqid)
{
	struct fslqspi_sequence *seq = &fslqspi_sequences[seqid];
	uint32_t reg_offset, reg_num, reg_val;

	if (seq->prepared)
		return ERROR_OK;

	/* Unlock LUT (Look-up-table) */
	FSLQSPI_REG_WRITE(FSLQSPI_LUTKEY, 0x5af05af0);
	FSLQSPI_REG_WRITE(FSLQSPI_LCKCR, 2);

	/* Write instruction-operand pairs to LUT */
	reg_offset = FSLQSPI_LUTn + (seqid * 0x10);
	for (reg_num = 0; reg_num < 4; reg_num++) {
		/* Pack two instruction-operand pairs for one LUT register,
		 * with the first pair in LSB and second pair in MSB */
		reg_val = (seq->instr[reg_num * 2]) |
			((seq->instr[(reg_num * 2) + 1]) << 16);
		FSLQSPI_REG_WRITE(reg_offset + (reg_num * 4), reg_val);
	}

	return ERROR_OK;
}

static int check_fr_error_flags(uint32_t fr)
{
	if (fr & FSLQSPI_FR_TBUF) {
		LOG_ERROR("TX buffer underflow (0x%08" PRIx32 ")", fr);
		return ERROR_FLASH_OPERATION_FAILED;
	} else if (fr & FSLQSPI_FR_ILLINE) {
		LOG_ERROR("Illegal instruction (0x%08" PRIx32 ")", fr);
		return ERROR_FLASH_OPERATION_FAILED;
	} else if (fr & FSLQSPI_FR_RBOF) {
		LOG_ERROR("RX buffer overflow (0x%08" PRIx32 ")", fr);
		return ERROR_FLASH_OPERATION_FAILED;
	} else if (fr & FSLQSPI_FR_IUEF) {
		LOG_ERROR("IP command usage error (0x%08" PRIx32 ")", fr);
		return ERROR_FLASH_OPERATION_FAILED;
	} else if (fr & FSLQSPI_FR_IPAEF) {
		LOG_ERROR("IP command trigger during AHB access (0x%08" PRIx32 ")", fr);
		return ERROR_FLASH_OPERATION_FAILED;
	} else if (fr & FSLQSPI_FR_IPIEF) {
		LOG_ERROR("IP command trigger could not be executed (0x%08" PRIx32 ")", fr);
		return ERROR_FLASH_OPERATION_FAILED;
	} else if (fr & FSLQSPI_FR_IPGEF) {
		LOG_ERROR("IP command trigger during AHB grant (0x%08" PRIx32 ")", fr);
		return ERROR_FLASH_OPERATION_FAILED;
	}
	return ERROR_OK;
}

static int start_ip_command(struct flash_bank *bank, enum fslqspi_seqid seqid,
			    bool par_en, uint16_t idatsz)
{
	LOG_DEBUG("IP command: %s", fslqspi_sequences[seqid].name);

	/* Configure IP command and trigger transaction */
	FSLQSPI_REG_WRITE(FSLQSPI_IPCR,
			  FSLQSPI_IPCR_VALUE(seqid, par_en, idatsz));

	/* Check error flags indicating trigger failure */
	return check_fr_error_flags(FSLQSPI_REG_READ(FSLQSPI_FR));
}

/* Execute IP command sequence that does no data read or write */
static int spi_command_nodata(struct flash_bank *bank, enum fslqspi_seqid seqid,
			      uint32_t addr, unsigned timeout)
{
	int retval;
	uint32_t fr;

	retval = prepare_sequence(bank, seqid);
	if (retval != ERROR_OK)
		return retval;

	/* Clear various flags */
	FSLQSPI_REG_WRITE(FSLQSPI_FR, FSLQSPI_FR_TFF |
			  FSLQSPI_FR_TBUF | FSLQSPI_FR_ILLINE |
			  FSLQSPI_FR_RBOF | FSLQSPI_FR_IUEF | FSLQSPI_FR_IPAEF |
			  FSLQSPI_FR_IPIEF | FSLQSPI_FR_IPGEF);

	/* Write address to SFAR register */
	FSLQSPI_REG_WRITE(FSLQSPI_SFAR, bank->base + addr);

	/* Configure and trigger IP command */
	retval = start_ip_command(bank, seqid, 0, 0);
	if (retval != ERROR_OK)
		return retval;

	/* Wait for IP command to finish */
	FSLQSPI_REG_POLL(FSLQSPI_FR, &fr, FSLQSPI_FR_TFF, FSLQSPI_FR_TFF,
			 timeout, ERROR_FLASH_OPERATION_FAILED,
			 "Timeout while polling SR[TFF]");

	return check_fr_error_flags(fr);
}

static int spi_command_read(struct flash_bank *bank,
			    enum fslqspi_seqid seqid, size_t bytes)
{
	int retval;
	uint32_t fr;

	retval = prepare_sequence(bank, seqid);
	if (retval != ERROR_OK)
		return retval;

	/* Clear RX buffer */
	retval = clear_rxbuf(bank);
	if (retval != ERROR_OK)
		return retval;

	/* Clear various flags */
	FSLQSPI_REG_WRITE(FSLQSPI_FR, FSLQSPI_FR_TFF | FSLQSPI_FR_RBDF |
			  FSLQSPI_FR_TBUF | FSLQSPI_FR_ILLINE |
			  FSLQSPI_FR_RBOF | FSLQSPI_FR_IUEF | FSLQSPI_FR_IPAEF |
			  FSLQSPI_FR_IPIEF | FSLQSPI_FR_IPGEF);

	/* Write flash base address to SFAR register */
	FSLQSPI_REG_WRITE(FSLQSPI_SFAR, bank->base);

	/* Configure and trigger IP command */
	retval = start_ip_command(bank, seqid, 0, bytes);
	if (retval != ERROR_OK)
		return retval;

	/* Wait for IP command to finish */
	FSLQSPI_REG_POLL(FSLQSPI_FR, &fr, FSLQSPI_FR_TFF, FSLQSPI_FR_TFF,
			 CMD_TIMEOUT, ERROR_FLASH_OPERATION_FAILED,
			 "Timeout while polling SR[TFF]");

	/* Bail out on error */
	retval = check_fr_error_flags(fr);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

/* Execute IP command sequence that read a single byte from external flash
 * chip */
static int spi_command_read_byte(struct flash_bank *bank,
				 enum fslqspi_seqid seqid, uint32_t *byte)
{
	int retval;
	uint32_t buf;

	retval = spi_command_read(bank, seqid, 1);
	if (retval != ERROR_OK)
		return retval;

	/* Read byte from RX buffer */
	buf = FSLQSPI_REG_READ(FSLQSPI_RBDRn);
	if (is_little_endian(bank))
		/* in bits 0:7 for little endian byte order*/
		*byte = buf & 0xFF;
	else
		/* in bits 31:24 for big endian byte order */
		*byte = buf >> 24;

	return ERROR_OK;
}

static int spi_command_read_word(struct flash_bank *bank,
				 enum fslqspi_seqid seqid, uint32_t *word)
{
	int retval;
	uint32_t buf;

	retval = spi_command_read(bank, seqid, 4);
	if (retval != ERROR_OK)
		return retval;

	/* Read word from RX buffer */
	buf = FSLQSPI_REG_READ(FSLQSPI_RBDRn);
	if (is_little_endian(bank))
		*word = le_to_h_u32((const uint8_t *)&buf);
	else
		*word = be_to_h_u32((const uint8_t *)&buf);

	return ERROR_OK;
}

/* Execute IP command sequence that write data to external flash chip */
static int spi_command_write_data(struct flash_bank *bank,
				  enum fslqspi_seqid seqid,
				  const uint8_t *buf, uint32_t addr,
				  size_t count, unsigned timeout)
{
	int retval;
	uint32_t fr;

	retval = prepare_sequence(bank, seqid);
	if (retval != ERROR_OK)
		return retval;

	/* Clear TX buffer */
	retval = clear_txbuf(bank);

	/* Clear various flags */
	FSLQSPI_REG_WRITE(FSLQSPI_FR, FSLQSPI_FR_TFF | FSLQSPI_FR_TBFF |
			  FSLQSPI_FR_TBUF | FSLQSPI_FR_ILLINE |
			  FSLQSPI_FR_RBOF | FSLQSPI_FR_IUEF | FSLQSPI_FR_IPAEF |
			  FSLQSPI_FR_IPIEF | FSLQSPI_FR_IPGEF);

	/* Write flash base address to SFAR register */
	FSLQSPI_REG_WRITE(FSLQSPI_SFAR, bank->base + addr);

	/* Fill TX buffer */
	retval = write_txbuf(bank, buf, count);
	if (retval != ERROR_OK)
		return retval;

	/* Configure and trigger IP command */
	retval = start_ip_command(bank, seqid, 0, count);
	if (retval != ERROR_OK)
		return retval;

	/* Wait for IP command to finish */
	FSLQSPI_REG_POLL(FSLQSPI_FR, &fr, FSLQSPI_FR_TFF, FSLQSPI_FR_TFF,
			 timeout, ERROR_FLASH_OPERATION_FAILED,
			 "Timeout while polling SR[TFF]");

	return check_fr_error_flags(fr);
}

/* Read the status register of the external SPI flash chip */
static inline int read_status_command(struct flash_bank *bank, uint32_t *status)
{
	return spi_command_read_byte(bank, READ_STATUS_SEQ, status);
}

/* Read manufacturer and device ID and length of CFI tables */
static inline int read_id_command(struct flash_bank *bank, uint32_t *id)
{
	return spi_command_read_word(bank, READ_ID_SEQ, id);
}

/* Execute a page program command */
static inline int program_command(struct flash_bank *bank, uint32_t offset,
				  const uint8_t *buffer, uint32_t count)
{
	int retval;
	int64_t endtime;
	uint32_t sr;

	LOG_DEBUG("offset=0x%08" PRIx32 " count=0x%08" PRIx32, offset, count);

	retval = spi_command_nodata(bank, WREN_SEQ, 0, CMD_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	retval = spi_command_write_data(bank, PAGE_PROGRAM_SEQ,
					buffer, offset, count,
					CMD_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	/* Poll external flash for completion of programming operation
	 * (SR[WIP]) */
	endtime = timeval_ms() + PROGRAM_TIMEOUT;
	do {
		retval = read_status_command(bank, &sr);
		if (retval != ERROR_OK)
			return retval;
		if ((sr & SPIFLASH_BSY_BIT) == 0)
			break;
		alive_sleep(1);
	} while (timeval_ms() < endtime);
	if (sr & SPIFLASH_BSY_BIT) {
		LOG_ERROR("Timeout waiting for program operation");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	return ERROR_OK;
}

/* Execute a sector erase command */
static inline int erase_command(struct flash_bank *bank, int sector)
{
	uint32_t sector_offset = bank->sectors[sector].offset;
	int retval;
	int64_t endtime;
	uint32_t sr;

	retval = spi_command_nodata(bank, WREN_SEQ, 0, CMD_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	retval = spi_command_nodata(bank, SECTOR_ERASE_SEQ,
				    sector_offset, CMD_TIMEOUT);
	if (retval != ERROR_OK)
		return retval;

	/* Poll external flash for completion of programming operation
	 * (SR[WIP]) */
	endtime = timeval_ms() + ERASE_TIMEOUT;
	do {
		retval = read_status_command(bank, &sr);
		if (retval != ERROR_OK)
			return retval;
		if ((sr & SPIFLASH_BSY_BIT) == 0)
			break;
		alive_sleep(1);
	} while (timeval_ms() < endtime);
	if (sr & SPIFLASH_BSY_BIT) {
		LOG_ERROR("Timeout waiting for program operation");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	return ERROR_OK;
}

static int fslqspi_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	int retval = ERROR_OK;
	int sector;

	LOG_DEBUG("from sector %d to sector %d", first, last);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((first < 0) || (last < first) || (last >= bank->num_sectors)) {
		LOG_ERROR("Flash sector invalid");
		return ERROR_FLASH_SECTOR_INVALID;
	}

	for (sector = first; sector <= last; sector++) {
		if (bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	for (sector = first; sector <= last; sector++) {
		retval = erase_command(bank, sector);
		if (retval != ERROR_OK)
			break;
		keep_alive();
	}

	return retval;
}

static int fslqspi_protect(struct flash_bank *bank, int set,
			 int first, int last)
{
	int sector;
	for (sector = first; sector <= last; sector++)
		bank->sectors[sector].is_protected = set;
	return ERROR_OK;
}

static int fslqspi_write(struct flash_bank *bank, const uint8_t *buffer,
			 uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct fslqspi_flash_bank *fslqspi_info = bank->driver_priv;
	int sector;
	uint32_t page_size, page_offset, cmd_count;
	int retval;

	LOG_DEBUG("offset=0x%08" PRIx32 " count=0x%08" PRIx32, offset, count);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset + count > fslqspi_info->dev->size_in_bytes) {
		LOG_WARNING("Write pasts end of flash. Extra data discarded.");
		count = fslqspi_info->dev->size_in_bytes - offset;
	}

	if (offset % 4 || count % 4)
		LOG_WARNING("Unaligned (not 32-bit aligned) write: YMMV");

	/* Check sector protection */
	for (sector = 0; sector < bank->num_sectors; sector++) {
		/* Start offset in or before this sector? */
		/* End offset in or behind this sector? */
		struct flash_sector *bs = &bank->sectors[sector];
		if ((offset < (bs->offset + bs->size)) &&
		    ((offset + count - 1) >= bs->offset) &&
		    bs->is_protected) {
			LOG_ERROR("Flash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	page_size = fslqspi_info->dev->pagesize;

	/* Program one page and not more than the size of the TX buffer at
	 * a time */
	while (count) {
		page_offset = offset % page_size;
		/* Clip at page boundary */
		if (page_offset + count > page_size)
			cmd_count = page_size - page_offset;
		else
			cmd_count = count;
		if (cmd_count > fslqspi_info->target_device->txbuf_len)
			cmd_count = fslqspi_info->target_device->txbuf_len;
		retval = program_command(bank, offset, buffer, cmd_count);
		if (retval != ERROR_OK)
			return retval;
		buffer += cmd_count;
		offset += cmd_count;
		count -= cmd_count;
	}

	return ERROR_OK;
}

/* Return ID of flash device */
static int read_flash_id(struct flash_bank *bank, uint32_t *id)
{
	struct target *target = bank->target;
	int retval;
	uint32_t buf;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Flash command: RDID (Read Identification) */
	retval = read_id_command(bank, &buf);
	if (retval != ERROR_OK)
		return retval;

	if (buf == 0xffffffff) {
		LOG_ERROR("No SPI flash found");
		return ERROR_FAIL;
	}

	/* Mask according to what spi.c code expects */
	*id = buf & 0x00ffffff;

	return ERROR_OK;
}

static int fslqspi_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct fslqspi_flash_bank *fslqspi_info = bank->driver_priv;
	struct flash_sector *sectors;
	struct fslqspi_target *target_device;
	int retval;
	uint32_t mcr, id, top_addr_reg, bottom_addr;

	if (fslqspi_info->probed) {
		free(bank->sectors);
		fslqspi_info->probed = 0;
	}

	for (target_device = target_devices; target_device->name;
		++target_device)
		if (target_device->tap_idcode == target->tap->idcode)
			break;

	if (!target_device->name) {
		LOG_ERROR("Device ID 0x%" PRIx32
			  " is not known as FSL QSPI capable",
			  target->tap->idcode);
		return ERROR_FAIL;
	}

	fslqspi_info->target_device = target_device;

	/* Enable QSPI module */
	mcr = FSLQSPI_REG_READ(FSLQSPI_MCR);
	/* Only keep CFG bits, thus clearing MDIS bit */
	mcr &= FSLQSPI_MCR_CFG_MASK;
	FSLQSPI_REG_WRITE(FSLQSPI_MCR, mcr);

	/* Remember endianness configuration */
	fslqspi_info->endianness = FSLQSPI_MCR_END_CFG(mcr);
	if (need_word_swap(bank)) {
		LOG_INFO("MCR[END_CFG]=0x%x requires word swapping for flash programming", fslqspi_info->endianness);
		LOG_WARNING("word swapping not implemented: programming will not do the right thing");
	}

	retval = read_flash_id(bank, &id);
	if (retval != ERROR_OK)
		return retval;

	fslqspi_info->dev = NULL;
	for (const struct flash_device *p = flash_devices; p->name; p++)
		if (p->device_id == id) {
			fslqspi_info->dev = p;
			break;
		}

	if (!fslqspi_info->dev) {
		LOG_ERROR("Unknown flash device (ID 0x%08" PRIx32 ")", id);
		return ERROR_FAIL;
	}

	LOG_INFO("Found flash device \'%s\' (ID 0x%08" PRIx32 ")",
		 fslqspi_info->dev->name, fslqspi_info->dev->device_id);

	/* Set correct size value */
	if (bank->size != fslqspi_info->dev->size_in_bytes) {
		LOG_WARNING("Bad flash bank size: 0x%" PRIx32 " != 0x%lx",
			    bank->size, fslqspi_info->dev->size_in_bytes);
		bank->size = fslqspi_info->dev->size_in_bytes;
	}

	/* Set top address for flash device */
	switch (fslqspi_info->chipselect) {
	case PCSFA1:
		bottom_addr = target_device->amba_base;
		top_addr_reg = FSLQSPI_SFA1AD;
		retval = ERROR_OK;
		break;
	case PCSFA2:
		bottom_addr = FSLQSPI_REG_READ(FSLQSPI_SFA1AD);
		top_addr_reg = FSLQSPI_SFA2AD;
		break;
	case PCSFB1:
		bottom_addr = FSLQSPI_REG_READ(FSLQSPI_SFA2AD);
		top_addr_reg = FSLQSPI_SFB1AD;
		break;
	case PCSFB2:
		bottom_addr = FSLQSPI_REG_READ(FSLQSPI_SFB1AD);
		top_addr_reg = FSLQSPI_SFB2AD;
		break;
	}
	if (retval != ERROR_OK)
		return retval;
	if (bank->base != bottom_addr) {
		LOG_WARNING("Bad flash bank base address: 0x%" PRIx32
			    " != 0x%" PRIx32, bank->base, bottom_addr);
		return ERROR_FLASH_BANK_INVALID;
	}
	FSLQSPI_REG_WRITE(top_addr_reg, bottom_addr + bank->size);

	/* create and fill sectors array */
	bank->num_sectors =
		fslqspi_info->dev->size_in_bytes
		/ fslqspi_info->dev->sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (!sectors) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}
	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset = sector * fslqspi_info->dev->sectorsize;
		sectors[sector].size = fslqspi_info->dev->sectorsize;
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 1;
	}
	bank->sectors = sectors;

	fslqspi_info->probed = 1;
	return ERROR_OK;
}

static int fslqspi_auto_probe(struct flash_bank *bank)
{
	struct fslqspi_flash_bank *fslqspi_info = bank->driver_priv;

	if (fslqspi_info->probed)
		return ERROR_OK;

	return fslqspi_probe(bank);
}

static int fslqspi_protect_check(struct flash_bank *bank)
{
	/* Not implemented */
	return ERROR_OK;
}

static int fslqspi_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct fslqspi_flash_bank *fslqspi_info = bank->driver_priv;

	if (!fslqspi_info->probed) {
		snprintf(buf, buf_size,
			 "\nFSL QSPI flash bank not probed yet\n");
		return ERROR_OK;
	}

	snprintf(buf, buf_size, "\nFSL QSPI flash information:\n"
		"  Device \'%s\' (ID 0x%08" PRIx32 ")\n",
		fslqspi_info->dev->name, fslqspi_info->dev->device_id);

	return ERROR_OK;
}

struct flash_driver fslqspi_flash = {
	.name = "fslqspi",
	.flash_bank_command = fslqspi_flash_bank_command,
	.erase = fslqspi_erase,
	.protect = fslqspi_protect,
	.write = fslqspi_write,
	.read = default_flash_read,
	.probe = fslqspi_probe,
	.auto_probe = fslqspi_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = fslqspi_protect_check,
	.info = fslqspi_info,
};
