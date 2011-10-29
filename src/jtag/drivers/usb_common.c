/***************************************************************************
 *   Copyright (C) 2009 by Zachary T Welch <zw@superlucidity.net>          *
 *                                                                         *
 *   Copyright (C) 2011 by Mauro Gamba <maurillo71@gmail.com>              *
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
#include "log.h"
#include "usb_common.h"

#ifdef HAVE_LIBUSB1
static struct libusb_context *jtag_libusb_context; /**< Libusb context **/
static libusb_device **devs; /**< The usb device list **/
#endif

static bool jtag_usb_match(struct jtag_usb_device *dev,
		const uint16_t vids[], const uint16_t pids[])
{
#ifdef HAVE_LIBUSB1
	struct libusb_device_descriptor dev_desc;
#endif

	for (unsigned i = 0; vids[i] && pids[i]; i++)
	{
#ifdef HAVE_LIBUSB1
		if (libusb_get_device_descriptor(dev, &dev_desc) == 0)
		{
		    if (dev_desc.idVendor == vids[i] &&
				dev_desc.idProduct == pids[i])
		    {
			    return true;
		    }
		}
#else
		if (dev->descriptor.idVendor == vids[i] &&
			dev->descriptor.idProduct == pids[i])
		{
			return true;
		}
#endif
	}
	return false;

	for (unsigned i = 0; vids[i] && pids[i]; i++)
	{
	}
	return false;
}

int jtag_usb_open(const uint16_t vids[], const uint16_t pids[],
		struct jtag_usb_device_handle **out)
{
#ifdef HAVE_LIBUSB1
	int cnt,idx,errCode;

	if (libusb_init(&jtag_libusb_context) < 0)
		return -ENODEV;

	libusb_set_debug(jtag_libusb_context,3);

	cnt = libusb_get_device_list(jtag_libusb_context, &devs);

	for (idx = 0; idx < cnt; idx++)
	{
		if (!jtag_usb_match(devs[idx], vids, pids))
			continue;

		errCode = libusb_open(devs[idx],out);

		/** Free the device list **/
		libusb_free_device_list(devs, 1);

		if (errCode < 0)
			return errCode;
		return 0;
	}
	return -ENODEV;
#else
	printf("jtag_usb_open\n");
	usb_init();

	usb_find_busses();
	usb_find_devices();

	struct usb_bus *busses = usb_get_busses();
	for (struct usb_bus *bus = busses; bus; bus = bus->next)
	{
		printf("Examine bus\n");
		for (struct usb_device *dev = bus->devices; dev; dev = dev->next)
		{
		    printf("Examine dev %d %d\n",dev->descriptor.idVendor,dev->descriptor.idProduct);
			if (!jtag_usb_match(dev, vids, pids))
				continue;

			*out = usb_open(dev);
			if (NULL == *out)
				return -errno;
			return 0;
		}
	}
	return -ENODEV;
#endif
}

void jtag_usb_close(jtag_usb_device_handle *dev)
{
	/* Close device */
	jtag_usb_close(dev);

#ifdef HAVE_LIBUSB1
	libusb_exit(jtag_libusb_context);
#endif
}

int jtag_usb_bulk_write(jtag_usb_device_handle *dev, int ep, char *bytes, int size,
						int timeout)
{
#ifdef HAVE_LIBUSB1
	int transferred=0;

	libusb_bulk_transfer(dev, ep, (unsigned char *)bytes, size, &transferred, timeout);
	return transferred;
#else
  return usb_bulk_write(dev,ep,bytes,size,timeout);
#endif
}

int jtag_usb_bulk_read(jtag_usb_device_handle *dev, int ep, char *bytes, int size,
				  int timeout)
{
#ifdef HAVE_LIBUSB1
	int transferred=0;

	libusb_bulk_transfer(dev, ep, (unsigned char *)bytes, size, &transferred, timeout);
	return transferred;

#else
  return usb_bulk_read(dev,ep,bytes,size,timeout);
#endif
}

int jtag_usb_set_configuration(jtag_usb_device_handle *devh, int configuration)
{
	struct jtag_usb_device *udev = jtag_usb_get_device(devh);
	int retCode = -99;

#ifdef HAVE_LIBUSB1
	struct libusb_config_descriptor *config;

	libusb_get_config_descriptor(udev, configuration, &config);
	retCode = libusb_set_configuration(devh, config->bConfigurationValue);

	libusb_free_config_descriptor(config);
#else
	retCode = usb_set_configuration(devh, udev->config[configuration].bConfigurationValue);
#endif

	return retCode;
}

int jtag_usb_get_endpoints(struct jtag_usb_device *udev,
						   unsigned int *usb_read_ep,
						   unsigned int *usb_write_ep)
{
#ifdef HAVE_LIBUSB1
	const struct libusb_interface *inter;
	const struct libusb_interface_descriptor *interdesc;
	const struct libusb_endpoint_descriptor *epdesc;
	struct libusb_config_descriptor *config;

	libusb_get_config_descriptor(udev, 0, &config);
	for(int i = 0; i < (int)config->bNumInterfaces; i++)
	{
		inter = &config->interface[i];

		for(int j = 0; j < inter->num_altsetting; j++)
		{
			interdesc = &inter->altsetting[j];
			for(int k = 0; k < (int)interdesc->bNumEndpoints; k++)
			{
			    epdesc = &interdesc->endpoint[k];

			    uint8_t epnum = epdesc->bEndpointAddress;
			    bool is_input = epnum & 0x80;
			    LOG_DEBUG("usb ep %s %02x", is_input ? "in" : "out", epnum);

			    if (is_input)
				  *usb_read_ep = epnum;
			    else
				  *usb_write_ep = epnum;
			}
		}
	}
	libusb_free_config_descriptor(config);
#else
	struct usb_interface *iface = udev->config->interface;
	struct usb_interface_descriptor *desc = iface->altsetting;
	for (int i = 0; i < desc->bNumEndpoints; i++)
	{
		uint8_t epnum = desc->endpoint[i].bEndpointAddress;
		bool is_input = epnum & 0x80;
		LOG_DEBUG("usb ep %s %02x", is_input ? "in" : "out", epnum);
		if (is_input)
			*usb_read_ep = epnum;
		else
			*usb_write_ep = epnum;
	}
#endif

	return 0;
}
