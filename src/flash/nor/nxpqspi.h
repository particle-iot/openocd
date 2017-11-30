#pragma once

#define QSPI_BASE		0x1550000
#define QSPI_MCR		(QSPI_BASE + 0x000)
#define QSPI_MCR_END_CFD_SHIFT	2
#define QSPI_MCR_END_CFD_MASK	(3 << QSPI_MCR_END_CFD_SHIFT)
#define QSPI_MCR_END_CFD_LE	(1 << QSPI_MCR_END_CFD_SHIFT)
#define QSPI_MCR_CLR_RXF_SHIFT	10
#define QSPI_MCR_CLR_RXF_MASK	(1 << QSPI_MCR_CLR_RXF_SHIFT)
#define QSPI_MCR_CLR_TXF_SHIFT	11
#define QSPI_MCR_CLR_TXF_MASK	(1 << QSPI_MCR_CLR_TXF_SHIFT)
#define QSPI_MCR_RESERVED_SHIFT	16
#define QSPI_MCR_RESERVED_MASK	(0xf << QSPI_MCR_RESERVED_SHIFT)
#define QSPI_IPCR		(QSPI_BASE + 0x008)
#define QSPI_FLSHCR		(QSPI_BASE + 0x00c)
#define QSPI_BUF0CR		(QSPI_BASE + 0x010)
#define QSPI_BUF1CR		(QSPI_BASE + 0x014)
#define QSPI_BUF2CR		(QSPI_BASE + 0x018)
#define QSPI_BUF3CR		(QSPI_BASE + 0x01c)
#define QSPI_SFAR		(QSPI_BASE + 0x100)
#define QSPI_RBSR		(QSPI_BASE + 0x10c)
#define QSPI_RBCT		(QSPI_BASE + 0x110)
#define QSPI_TBDR		(QSPI_BASE + 0x154)
#define QSPI_SR			(QSPI_BASE + 0x15c)
#define QSPI_SR_BUSY		(1 << 0)
#define QSPI_RBDR_BASE		(QSPI_BASE + 0x200)
#define QSPI_LUTKEY		(QSPI_BASE + 0x300)
#define LUT_KEY_VALUE		0x5AF05AF0
#define QSPI_LCKCR		(QSPI_BASE + 0x304)
#define QSPI_LCKCR_LOCK		0x1
#define QSPI_LCKCR_UNLOCK	0x2
#define QSPI_LUT_BASE		(QSPI_BASE + 0x310)

#define SEQID_WREN		1
#define SEQID_FAST_READ		2
#define SEQID_RDSR		3
#define SEQID_SE		4
#define SEQID_CHIP_ERASE	5
#define SEQID_PP		6
#define SEQID_RDID		7
#define SEQID_BE_4K		8
#define SEQID_BRRD		9
#define SEQID_BRWR		10
#define SEQID_RDEAR		11
#define SEQID_WREAR		12
#define SEQID_WRAR		13
#define SEQID_RDAR		14

/* QSPI commands */
#define QSPI_CMD_PP		0x02	/* Page Program (up to 256 byte) */
#define QSPI_CMD_PP_4B		0x12
#define QSPI_CMD_RDSR		0x05	/* Read status register */
#define QSPI_CMD_WREN		0x06	/* Write Enable */
#define QSPI_CMD_FAST_READ		0x0b	/* Read data bytes (high frequency) */
#define QSPI_CMD_FAST_READ_4B	0x0c
#define QSPI_CMD_BE_4K		0x20	/* 4K erase */
#define QSPI_CMD_CHIP_ERASE	0xc7	/* Erase whole flash chip */
#define QSPI_CMD_SE		0xd8	/* Sector erase (usually 64 KiB) */
#define QSPI_CMD_SE_4B		0xdc
#define QSPI_CMD_RDID		0x9f	/* Read JEDEC ID */
#define QSPI_CMD_WRAR		0x71
#define QSPI_CMD_RDAR		0x65


#define OPRND0_SHIFT		0
#define OPRND0(x)		((x) << OPRND0_SHIFT)
#define PAD0_SHIFT		8
#define PAD0(x)			((x) << PAD0_SHIFT)
#define INSTR0_SHIFT		10
#define INSTR0(x)		((x) << INSTR0_SHIFT)
#define OPRND1_SHIFT		16
#define OPRND1(x)		((x) << OPRND1_SHIFT)
#define PAD1_SHIFT		24
#define PAD1(x)			((x) << PAD1_SHIFT)
#define INSTR1_SHIFT		26
#define INSTR1(x)		((x) << INSTR1_SHIFT)

#define LUT_CMD			1
#define LUT_ADDR		2
#define LUT_DUMMY		3
#define LUT_READ		7
#define LUT_WRITE		8
#define LUT_PAD1		0
#define LUT_PAD2		1
#define LUT_PAD3		2

#define ADDR24BIT		0x18
#define ADDR32BIT		0x20
#define TX_BUFFER_SIZE		0x40
#define RX_BUFFER_SIZE		0x80
