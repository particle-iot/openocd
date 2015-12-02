/* bits in SPI flash status register */
#define SPIFLASH_WIP		0	/* Write In Progress bit in status register */
#define SPIFLASH_WEL		1	/* Write Enable Latch bit in status register */

/* register offsets */
#define QSPI_CR			(0x00)	/* Control register */
#define QSPI_SR			(0x08)	/* Status register */
#define QSPI_FCR		(0x0C)	/* Flag clear register */
#define QSPI_DLR		(0x10)	/* Data length register */
#define QSPI_CCR		(0x14)	/* Communication configuration register */
#define QSPI_AR			(0x18)	/* Address register */
#define QSPI_DR			(0x20)	/* Data register */
#define QSPI_PSMKR		(0x24)	/* Polling status mask register */

/* bits in QSPI_CR */
#define QSPI_FSEL_FLASH		7	/* Select flash 2 */
#define QSPI_DUAL_FLASH		6	/* Dual flash mode */
#define QSPI_ABORT			1	/* Abort bit */

/* bits in QSPI_SR */
#define QSPI_BUSY			5	/* Busy flag */
#define QSPI_FTF			2	/* FIFO threshold flag */
#define QSPI_TCF			1	/* Transfer complete flag */

/* fields in QSPI_CCR */
#define QSPI_WRITE_MODE		0x00000000		/* indirect write mode */
#define QSPI_READ_MODE		0x04000000		/* indirect read mode */
#define QSPI_MM_MODE		0x0C000000		/* memory mapped mode */
#define QSPI_1LINE_MODE		0x01000500		/* 1 line for address, data */
#define QSPI_2LINE_MODE		0x02000900		/* 2 lines for address, data */
#define QSPI_4LINE_MODE		0x03000D00		/* 4 lines for address, data */
#define QSPI_NO_DATA		(~0x03000000)	/* no data */
#define QSPI_NO_ADDR		(~0x00000C00)	/* no address */
#define QSPI_ADDR3			0x00002000		/* 3 byte adress */

#define QSPI_DCYC_POS		18				/* bit position of DCYC */
#define QSPI_DCYC_LEN		5				/* length of DCYC field */
#define QSPI_DCYC_MASK		(((1<<QSPI_DCYC_LEN) - 1)<<QSPI_DCYC_POS)
#define QSPI_DCYCLES		0				/* for single data line mode */
