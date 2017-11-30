#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "imp.h"
#include "spi.h"
#include "nxpqspi.h"

struct nxpqspi_flash_bank {
	int probed;
	uint32_t reg_base;
	uint32_t bank_num;
	const struct flash_device *dev;
};

static inline uint32_t swab32(uint32_t x)
{
	return (uint32_t)(
		(((uint32_t)(x) & (uint32_t)0x000000ffUL) << 24) |
		(((uint32_t)(x) & (uint32_t)0x0000ff00UL) <<  8) |
		(((uint32_t)(x) & (uint32_t)0x00ff0000UL) >>  8) |
		(((uint32_t)(x) & (uint32_t)0xff000000UL) >> 24));
}

static uint32_t qspi_read32(struct flash_bank *bank, uint32_t addr)
{
	struct target *target = bank->target;
	uint32_t value = 0xdeadbeef;

	target_read_u32(target, addr, &value);

	return be_to_h_u32((uint8_t *)&value);
}

static int qspi_write32(struct flash_bank *bank, uint32_t addr, uint32_t val)
{
	struct target *target = bank->target;
	uint32_t value;

	h_u32_to_be((uint8_t *)&value, val);
	return target_write_u32(target, addr, value);
}


static void qspi_set_lut_sequence(struct flash_bank *bank, uint32_t seqid,
	uint32_t lut_cmd0, uint32_t lut_cmd1, uint32_t lut_cmd2, uint32_t lut_cmd3)
{
	qspi_write32(bank, QSPI_LUT_BASE + ((seqid * 4 + 0) * sizeof(uint32_t)), lut_cmd0);
	qspi_write32(bank, QSPI_LUT_BASE + ((seqid * 4 + 1) * sizeof(uint32_t)), lut_cmd1);
	qspi_write32(bank, QSPI_LUT_BASE + ((seqid * 4 + 2) * sizeof(uint32_t)), lut_cmd2);
	qspi_write32(bank, QSPI_LUT_BASE + ((seqid * 4 + 3) * sizeof(uint32_t)), lut_cmd3);
}

static void qspi_set_lut(struct flash_bank *bank)
{
	qspi_write32(bank, QSPI_LUTKEY, LUT_KEY_VALUE);
	qspi_write32(bank, QSPI_LCKCR, QSPI_LCKCR_UNLOCK);

	/* Write Enable */
	qspi_set_lut_sequence(bank, SEQID_WREN,
		OPRND0(QSPI_CMD_WREN) | PAD0(LUT_PAD1) | INSTR0(LUT_CMD),
		0, 0, 0);
	/* Fast Read */
	qspi_set_lut_sequence(bank, SEQID_FAST_READ,
		OPRND0(QSPI_CMD_FAST_READ_4B) | PAD0(LUT_PAD1) | INSTR0(LUT_CMD)
		| OPRND1(ADDR32BIT) | PAD1(LUT_PAD1) | INSTR1(LUT_ADDR),
		OPRND0(8) | PAD0(LUT_PAD1) | INSTR0(LUT_DUMMY) | OPRND1(RX_BUFFER_SIZE)
		| PAD1(LUT_PAD1) | INSTR1(LUT_READ),
		0, 0);
	/* Read Status */
	qspi_set_lut_sequence(bank, SEQID_RDSR,
		OPRND0(QSPI_CMD_RDSR) | PAD0(LUT_PAD1) | INSTR0(LUT_CMD) | OPRND1(1)
		| PAD1(LUT_PAD1) | INSTR1(LUT_READ),
		0, 0, 0);
	/* Erase a sector */
	qspi_set_lut_sequence(bank, SEQID_SE,
		OPRND0(QSPI_CMD_SE_4B) | PAD0(LUT_PAD1) | INSTR0(LUT_CMD) | OPRND1(ADDR32BIT)
		| PAD1(LUT_PAD1) | INSTR1(LUT_ADDR),
		0, 0, 0);
	/* Erase the whole chip */
	qspi_set_lut_sequence(bank, SEQID_CHIP_ERASE,
		OPRND0(QSPI_CMD_CHIP_ERASE) | PAD0(LUT_PAD1) | INSTR0(LUT_CMD), 0, 0, 0);
	/* Page Program */
	qspi_set_lut_sequence(bank, SEQID_PP,
		OPRND0(QSPI_CMD_PP_4B) | PAD0(LUT_PAD1) | INSTR0(LUT_CMD) | OPRND1(ADDR32BIT)
		| PAD1(LUT_PAD1) | INSTR1(LUT_ADDR),
		OPRND0(TX_BUFFER_SIZE) | PAD0(LUT_PAD1) | INSTR0(LUT_WRITE), 0, 0);
	/* Read JEDEC ID */
	qspi_set_lut_sequence(bank, SEQID_RDID,
		OPRND0(QSPI_CMD_RDID) | PAD0(LUT_PAD1) | INSTR0(LUT_CMD) | OPRND1(8)
		| PAD1(LUT_PAD1) | INSTR1(LUT_READ),
		0, 0, 0);
	/* sub sector 4K erase */
	qspi_set_lut_sequence(bank, SEQID_BE_4K,
		OPRND0(QSPI_CMD_BE_4K) | PAD0(LUT_PAD1) | INSTR0(LUT_CMD) | OPRND1(ADDR24BIT)
		| PAD1(LUT_PAD1) | INSTR1(LUT_ADDR),
		0, 0, 0);
	/* read any device register */
	qspi_set_lut_sequence(bank, SEQID_RDAR,
		OPRND0(QSPI_CMD_RDAR) | PAD0(LUT_PAD1) | INSTR0(LUT_CMD) | OPRND1(ADDR24BIT)
		| PAD1(LUT_PAD1) | INSTR1(LUT_ADDR),
		OPRND0(8) | PAD0(LUT_PAD1) | INSTR0(LUT_DUMMY) | OPRND1(1) | PAD1(LUT_PAD1)
		| INSTR1(LUT_READ),
		0, 0);
	/* write any device register */
	qspi_set_lut_sequence(bank, SEQID_WRAR,
		OPRND0(QSPI_CMD_WRAR) | PAD0(LUT_PAD1) | INSTR0(LUT_CMD) | OPRND1(ADDR24BIT)
		| PAD1(LUT_PAD1) | INSTR1(LUT_ADDR),
		OPRND0(1) | PAD0(LUT_PAD1) | INSTR0(LUT_WRITE),
		0, 0);

	/* lock the LUT */
	qspi_write32(bank, QSPI_LUTKEY, LUT_KEY_VALUE);
	qspi_write32(bank, QSPI_LCKCR, QSPI_LCKCR_LOCK);
}

static void qspi_op_rdid(struct flash_bank *bank, uint32_t *rxbuf, uint32_t len)
{
	uint32_t mcr_reg, rbsr_reg, data, size;
	int i;

	/* Save MCR */
	mcr_reg = qspi_read32(bank, QSPI_MCR);
	/* Reconfigure MCR */
	qspi_write32(bank, QSPI_MCR, QSPI_MCR_CLR_RXF_MASK | QSPI_MCR_CLR_TXF_MASK
					| QSPI_MCR_RESERVED_MASK | QSPI_MCR_END_CFD_LE);
	/* USE IPS */
	qspi_write32(bank, QSPI_RBCT, 1 << 8);

	/* Select the right chip */
	qspi_write32(bank, QSPI_SFAR, bank->base);
	/* select the LUT command target */
	qspi_write32(bank, QSPI_IPCR, (SEQID_RDID << 24));
	/* wait for the command to finish */
	while (qspi_read32(bank, QSPI_SR) & QSPI_SR_BUSY)
		;

	/* read the data */
	i = 0;
	while ((RX_BUFFER_SIZE >= len) && (len > 0)) {
		rbsr_reg = qspi_read32(bank, QSPI_RBSR);
		if (rbsr_reg & (0x3f << 8)) {
			data = qspi_read32(bank, QSPI_RBDR_BASE + i * sizeof(uint32_t));
			data = swab32(data);
			size = (len < 4) ? len : 4;
			memcpy(rxbuf, &data, size);
			len -= size;
			rxbuf++;
			i++;
		}
	}

	/* restore MCR */
	qspi_write32(bank, QSPI_MCR, mcr_reg);
}


static int nxpqspi_read_id(struct flash_bank *bank, uint32_t *id)
{
	qspi_op_rdid(bank, id, sizeof(uint32_t));

	return ERROR_OK;
}

static void qspi_op_erase(struct flash_bank *bank, uint32_t offset)
{
	uint32_t mcr_reg;

	/* Save MCR */
	mcr_reg = qspi_read32(bank, QSPI_MCR);
	/* Reconfigure MCR */
	qspi_write32(bank, QSPI_MCR, QSPI_MCR_CLR_RXF_MASK | QSPI_MCR_CLR_TXF_MASK
					| QSPI_MCR_RESERVED_MASK | QSPI_MCR_END_CFD_LE);
	/* USE IPS */
	qspi_write32(bank, QSPI_RBCT, 1 << 8);

	/* Select the right chip */
	qspi_write32(bank, QSPI_SFAR, bank->base + offset);

	/* Write enable */
	LOG_INFO("Write Enable");
	qspi_write32(bank, QSPI_IPCR, (SEQID_WREN << 24));
	/* wait for the command to finish */
	while (qspi_read32(bank, QSPI_SR) & QSPI_SR_BUSY)
		;
	LOG_INFO("Write Enable done");

	/* select the LUT command target */
	LOG_INFO("Sector Erase");
	qspi_write32(bank, QSPI_IPCR, (SEQID_SE << 24));
	/* wait for the command to finish */
	while (qspi_read32(bank, QSPI_SR) & QSPI_SR_BUSY)
		;
	LOG_INFO("Sector Erase done");

	/* restore MCR */
	qspi_write32(bank, QSPI_MCR, mcr_reg);
}

static int nxpqspi_block_erase(struct flash_bank *bank, uint32_t offset)
{
	qspi_op_erase(bank, offset);
	return ERROR_OK;
}

static void qspi_op_chip_erase(struct flash_bank *bank)
{
	uint32_t mcr_reg;

	/* Save MCR */
	mcr_reg = qspi_read32(bank, QSPI_MCR);
	/* Reconfigure MCR */
	qspi_write32(bank, QSPI_MCR, QSPI_MCR_CLR_RXF_MASK | QSPI_MCR_CLR_TXF_MASK
					| QSPI_MCR_RESERVED_MASK | QSPI_MCR_END_CFD_LE);
	/* USE IPS */
	qspi_write32(bank, QSPI_RBCT, 1 << 8);

	/* Select the right chip */
	qspi_write32(bank, QSPI_SFAR, bank->base);

	/* Write enable */
	qspi_write32(bank, QSPI_IPCR, (SEQID_WREN << 24));
	/* wait for the command to finish */
	while (qspi_read32(bank, QSPI_SR) & QSPI_SR_BUSY)
		;

	/* select the LUT command target */
	qspi_write32(bank, QSPI_IPCR, (SEQID_CHIP_ERASE << 24));
	/* wait for the command to finish */
	while (qspi_read32(bank, QSPI_SR) & QSPI_SR_BUSY)
		;

	/* restore MCR */
	qspi_write32(bank, QSPI_MCR, mcr_reg);
}

static int nxpqspi_bulk_erase(struct flash_bank *bank)
{
	qspi_op_chip_erase(bank);

	return ERROR_OK;
}

static int nxpqspi_flash_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	struct nxpqspi_flash_bank *info = bank->driver_priv;
	int retval = ERROR_OK;
	int sector;

	LOG_INFO("Erase from sector %d to sector %d", first, last);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((first < 0) || (last < first) || (last >= bank->num_sectors)) {
		LOG_ERROR("Flash sector invalid");
		return ERROR_FLASH_SECTOR_INVALID;
	}

	if (!info->probed) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	for (sector = first; sector <= last; sector++) {
		if (bank->sectors[sector].is_protected) {
			LOG_ERROR("FLash sector %d protected", sector);
			return ERROR_FAIL;
		}
	}

	if (first == 0 && last == (bank->num_sectors - 1) &&
		info->dev->chip_erase_cmd != info->dev->erase_cmd) {
		LOG_DEBUG("Chip supports the bulk erase command."\
			" Will use bulk erase instead of sector by sector erase.");
		retval = nxpqspi_bulk_erase(bank);
		if (retval == ERROR_OK)
			return retval;
		else
			LOG_WARNING("Bulk flash erase failed."
				" Falling back to sector by sector erase.");
	}

	for (sector = first; sector <= last; sector++) {
		retval = nxpqspi_block_erase(bank, sector * info->dev->sectorsize);
		if (retval != ERROR_OK)
			return retval;
	}

	return retval;
}

static void qspi_op_write(struct flash_bank *bank, const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	uint32_t mcr_reg, data, status_reg, rbsr_reg;
	int i, size, tx_size;

	LOG_INFO("Writing 0x%08" PRIx32 " bytes at offset 0x%08" PRIx32, count, offset);

	/* Save MCR */
	mcr_reg = qspi_read32(bank, QSPI_MCR);
	/* Reconfigure MCR */
	qspi_write32(bank, QSPI_MCR, QSPI_MCR_CLR_RXF_MASK | QSPI_MCR_CLR_TXF_MASK
					| QSPI_MCR_RESERVED_MASK | QSPI_MCR_END_CFD_LE);
	/* USE IPS */
	qspi_write32(bank, QSPI_RBCT, 1 << 8);

	status_reg = 0;
	while ((status_reg & 0x02) != 0x02) {
		qspi_write32(bank, QSPI_IPCR, SEQID_WREN << 24);
		while (qspi_read32(bank, QSPI_SR) & QSPI_SR_BUSY)
			;

		qspi_write32(bank, QSPI_IPCR, (SEQID_RDSR << 24) | 1 /* PAR_EN */);
		while (qspi_read32(bank, QSPI_SR) & QSPI_SR_BUSY)
			;

		rbsr_reg = qspi_read32(bank, QSPI_RBSR);
		if (rbsr_reg & (0x3f << 8))
			status_reg = qspi_read32(bank, QSPI_RBDR_BASE + 0 * sizeof(uint32_t));

		qspi_write32(bank, QSPI_MCR,
				qspi_read32(bank, QSPI_MCR) | QSPI_MCR_CLR_RXF_MASK);
	}

	qspi_write32(bank, QSPI_SFAR, bank->base + offset);

	tx_size = count > TX_BUFFER_SIZE ? TX_BUFFER_SIZE : count;
	size = tx_size / 16;
	if (tx_size % 16)
		size++;
	for (i = 0; i < (size * 4); i++) {
		memcpy(&data, buffer, sizeof(uint32_t));
		qspi_write32(bank, QSPI_TBDR, data);
		buffer += 4;
	}

	qspi_write32(bank, QSPI_IPCR, (SEQID_PP << 24) | tx_size);
	while (qspi_read32(bank, QSPI_SR) & QSPI_SR_BUSY)
		;

	qspi_write32(bank, QSPI_MCR, mcr_reg);
}

static int nxpqspi_flash_write(struct flash_bank *bank, const uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct nxpqspi_flash_bank *info = bank->driver_priv;
	int retval = ERROR_OK;
	int sector;

	LOG_INFO("Writing 0x%08" PRIx32 " bytes to offset 0x%08" PRIx32, count, offset);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset + count > info->dev->size_in_bytes) {
		LOG_WARNING("Writes past the end of flash. Extra data discarded.");
		count = info->dev->size_in_bytes - offset;
	}

	for (sector = 0; sector < bank->num_sectors; sector++) {
		/* Start offset in or before this sector? */
		/* End offset in or before this sector? */
		if ((offset < (bank->sectors[sector].offset + bank->sectors[sector].size))
			&& ((offset + count - 1) >= bank->sectors[sector].offset)
			&& bank->sectors[sector].is_protected) {
			LOG_ERROR("Flash sector %d is protected", sector);
			return ERROR_FAIL;
		}
	}

	do {
		int len = count > TX_BUFFER_SIZE ? TX_BUFFER_SIZE : count;
		qspi_op_write(bank, buffer, offset, len);
		/* advance */
		count -= len;
		offset += len;
		buffer += len;
	} while (count);

	return retval;
}

static void qspi_op_read(struct flash_bank *bank, uint8_t *buffer, uint32_t offset, uint32_t count)
{
	uint32_t mcr_reg, data;
	int i, size;

	/* Save MCR */
	mcr_reg = qspi_read32(bank, QSPI_MCR);
	/* Reconfigure MCR */
	qspi_write32(bank, QSPI_MCR, QSPI_MCR_CLR_RXF_MASK | QSPI_MCR_CLR_TXF_MASK
					| QSPI_MCR_RESERVED_MASK | QSPI_MCR_END_CFD_LE);
	/* USE IPS */
	qspi_write32(bank, QSPI_RBCT, 1 << 8);

	while (count > 0) {
		/* Select the right block */
		qspi_write32(bank, QSPI_SFAR, bank->base + offset);

		size = (count > RX_BUFFER_SIZE) ? RX_BUFFER_SIZE : count;

		/* select the LUT command target */
		qspi_write32(bank, QSPI_IPCR, (SEQID_FAST_READ << 24));
		/* wait for the command to finish */
		while (qspi_read32(bank, QSPI_SR) & QSPI_SR_BUSY)
			;

		offset += size;
		count -= size;

		i = 0;
		while ((RX_BUFFER_SIZE >= size) && (size > 0)) {
			data = qspi_read32(bank, QSPI_RBDR_BASE + i * sizeof(uint32_t));
			if (size < (int)sizeof(data))
				memcpy(buffer, &data, size);
			else
				memcpy(buffer, &data, sizeof(data));
			buffer += sizeof(data);
			size -= sizeof(data);
			i++;
		}
		qspi_write32(bank, QSPI_MCR, qspi_read32(bank, QSPI_MCR) | QSPI_MCR_CLR_RXF_MASK);
	}

	/* restore MCR */
	qspi_write32(bank, QSPI_MCR, mcr_reg);
}

static int nxpqspi_flash_read(struct flash_bank *bank, uint8_t *buffer, uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct nxpqspi_flash_bank *info = bank->driver_priv;
	int retval = ERROR_OK;

	LOG_INFO("Reading 0x%08" PRIx32 " bytes from offset 0x%08" PRIx32, count, offset);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!info->probed) {
		LOG_ERROR("Flash bank not probed");
		return ERROR_FLASH_BANK_NOT_PROBED;
	}

	qspi_op_read(bank, buffer, offset, count);

	return retval;
}

static int nxpqspi_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct nxpqspi_flash_bank *info = bank->driver_priv;
	uint32_t id = 0;
	int retval;
	struct flash_sector *sectors;

	/* If we've already probed, we should be fine to skip this time. */
	if (info->probed)
		return ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	info->probed = 0;
	info->bank_num = bank->bank_number;

	/* Set up the LUT table */
	qspi_set_lut(bank);

	/* Read flash JEDEC ID */
	retval = nxpqspi_read_id(bank, &id);
	if (retval != ERROR_OK)
		return retval;

	info->dev = NULL;
	for (const struct flash_device *p = flash_devices; p->name; p++)
		if (p->device_id == id) {
			info->dev = p;
			break;
		}

	if (!info->dev) {
		LOG_ERROR("Unknown flash device ID 0x%08" PRIx32, id);
		return ERROR_FAIL;
	}

	LOG_INFO("Found flash device \'%s\' ID 0x%08" PRIx32,
		info->dev->name, info->dev->device_id);

	/* Set correct size value */
	bank->size = info->dev->size_in_bytes;

	/* create and fill sectors array */
	bank->num_sectors = info->dev->size_in_bytes / info->dev->sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (!sectors) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	for (int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset = sector * info->dev->sectorsize;
		sectors[sector].size = info->dev->sectorsize;
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 0;
	}

	bank->sectors = sectors;
	info->probed = 1;

	return ERROR_OK;
}

static int nxpqspi_auto_probe(struct flash_bank *bank)
{
	struct nxpqspi_flash_bank *info = bank->driver_priv;
	if (info->probed)
		return ERROR_OK;
	return nxpqspi_probe(bank);
}

static int nxpqspi_flash_erase_check(struct flash_bank *bank)
{
	/* Not implemented yet */
	return ERROR_OK;
}

static int nxpqspi_protect_check(struct flash_bank *bank)
{
	/* Not implemented yet */
	return ERROR_OK;
}

int nxpqspi_get_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct nxpqspi_flash_bank *info = bank->driver_priv;

	if (!info->probed) {
		snprintf(buf, buf_size,
			"\nQSPI flash bank not probed yet\n");
		return ERROR_OK;
	}

	snprintf(buf, buf_size, "\nQSPI flash information:\n"
		"  Device \'%s\' ID 0x%08" PRIx32 "\n",
		info->dev->name, info->dev->device_id);

	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(nxpqspi_flash_bank_command)
{
	struct nxpqspi_flash_bank *info;

	if (CMD_ARGC < 7)
		return ERROR_COMMAND_SYNTAX_ERROR;

	info = malloc(sizeof(struct nxpqspi_flash_bank));
	if (!info) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	/* Get QSPI controller register map base address */
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[6], info->reg_base);
	LOG_INFO("reg_base = 0x%08" PRIx32, info->reg_base);
	bank->driver_priv = info;
	info->probed = 0;

	return ERROR_OK;
}

struct flash_driver nxpqspi_flash = {
	.name			= "nxpqspi",
	.flash_bank_command	= nxpqspi_flash_bank_command,
	.erase			= nxpqspi_flash_erase,
	.protect		= NULL,
	.write			= nxpqspi_flash_write,
	.read			= nxpqspi_flash_read,
	.probe			= nxpqspi_probe,
	.auto_probe		= nxpqspi_auto_probe,
	.erase_check		= nxpqspi_flash_erase_check,
	.protect_check		= nxpqspi_protect_check,
	.info			= nxpqspi_get_info,
};
