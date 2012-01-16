#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <jtag/interface.h>
#include <jtag/commands.h>

#include "ublast_access.h"

#if BUILD_USB_BLASTER_LIBFTDI
#include <ftdi.h>

static struct ftdi_context *ublast_getftdic(struct ublast_lowlevel *low)
{
	return low->priv;
}

static int ublast_ftdi_read(struct ublast_lowlevel *low, uint8_t *buf,
			    unsigned size, uint32_t *bytes_read)
{
	int retval;
	int timeout = 100;
	struct ftdi_context *ftdic = ublast_getftdic(low);

	*bytes_read = 0;
	while ((*bytes_read < size) && timeout--) {
		retval = ftdi_read_data(ftdic, buf + *bytes_read,
				size - *bytes_read);
		if (retval < 0)	{
			*bytes_read = 0;
			LOG_ERROR("ftdi_read_data: %s",
					ftdi_get_error_string(ftdic));
			return ERROR_JTAG_DEVICE_ERROR;
		}
		*bytes_read += retval;
	}
	return ERROR_OK;
}

static int ublast_ftdi_write(struct ublast_lowlevel *low, uint8_t *buf, int size,
			     uint32_t *bytes_written)
{
	int retval;
	struct ftdi_context *ftdic = ublast_getftdic(low);

	retval = ftdi_write_data(ftdic, buf, size);
	if (retval < 0)	{
		*bytes_written = 0;
		LOG_ERROR("ftdi_write_data: %s",
			  ftdi_get_error_string(ftdic));
		return ERROR_JTAG_DEVICE_ERROR;
	}
	*bytes_written = retval;
	return ERROR_OK;
}

static int ublast_ftdi_speed(struct ublast_lowlevel *low, int speed)
{
	struct ftdi_context *ftdic = ublast_getftdic(low);

	LOG_DEBUG("TODO: ublast_speed() isn't optimally implemented!");
	/* TODO: libftdi's ftdi_set_baudrate chokes on high rates, use lowlevel
	 * usb function instead! And additionally allow user to throttle.
	 */
	if (ftdi_set_baudrate(ftdic, 3000000 / 4) < 0) {
		LOG_ERROR("Can't set baud rate to max: %s",
			  ftdi_get_error_string(ftdic));
		return ERROR_JTAG_DEVICE_ERROR;
	}
	return ERROR_OK;
}

static int ublast_ftdi_init(struct ublast_lowlevel *low)
{
	uint8_t latency_timer;
	struct ftdi_context *ftdic = ublast_getftdic(low);

	LOG_INFO("usb blaster interface using libftdi");
	if (ftdi_init(ftdic) < 0)
		return ERROR_JTAG_INIT_FAILED;

	/* context, vendor id, product id */
	if (ftdi_usb_open(ftdic, low->ublast_vid, low->ublast_pid) < 0)	{
		LOG_ERROR("unable to open ftdi device: %s", ftdic->error_str);
		return ERROR_JTAG_INIT_FAILED;
	}

	if (ftdi_usb_reset(ftdic) < 0) {
		LOG_ERROR("unable to reset ftdi device");
		return ERROR_JTAG_INIT_FAILED;
	}

	if (ftdi_set_latency_timer(ftdic, 2) < 0) {
		LOG_ERROR("unable to set latency timer");
		return ERROR_JTAG_INIT_FAILED;
	}

	if (ftdi_get_latency_timer(ftdic, &latency_timer) < 0) {
		LOG_ERROR("unable to get latency timer");
		return ERROR_JTAG_INIT_FAILED;
	}
	LOG_DEBUG("current latency timer: %u", latency_timer);

	ftdi_disable_bitbang(ftdic);
	return ERROR_OK;
}

static int ublast_ftdi_quit(struct ublast_lowlevel *low)
{
	struct ftdi_context *ftdic = ublast_getftdic(low);

	ftdi_usb_close(ftdic);
	ftdi_deinit(ftdic);
	return ERROR_OK;
};

static struct ublast_lowlevel_priv {
	struct ftdi_context ftdic;
} info;

static struct ublast_lowlevel low = {
	.open = ublast_ftdi_init,
	.close = ublast_ftdi_quit,
	.read = ublast_ftdi_read,
	.write = ublast_ftdi_write,
	.speed = ublast_ftdi_speed,
	.priv = &info,
};

struct ublast_lowlevel *ublast_register_ftdi(void)
{
	return &low;
}

#else
struct ublast_lowlevel *ublast_register_ftdi(void)
{
	return NULL;
}
#endif
