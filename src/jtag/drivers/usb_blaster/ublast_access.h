
struct ublast_lowlevel {
	uint16_t ublast_vid;
	uint16_t ublast_pid;
	char *ublast_device_desc;

	int (*write)(struct ublast_lowlevel *low, uint8_t *buf, int size,
		     uint32_t *bytes_written);
	int (*read)(struct ublast_lowlevel *low, uint8_t *buf, unsigned size,
		    uint32_t *bytes_read);
	int (*open)(struct ublast_lowlevel *low);
	int (*close)(struct ublast_lowlevel *low);
	int (*speed)(struct ublast_lowlevel *low, int speed);

	void *priv;
};

/**
 * ublast_register_ftdi - get a lowlevel USB Blaster driver
 * ublast_register_ftd2xx - get a lowlevel USB Blaster driver
 *
 * Get a lowlevel USB-Blaster driver. In the current implementation, there are 2
 * possible lowlevel drivers :
 *  - one based on libftdi from ftdichip.com
 *  - one based on libftdxx, the free alternative
 *
 * Returns the lowlevel driver structure.
 */
extern struct ublast_lowlevel *ublast_register_ftdi(void);
extern struct ublast_lowlevel *ublast_register_ftd2xx(void);
