/***************************************************************************
 *   Copyright (C) 2009 by Zachary T Welch <zw@superlucidity.net>          *
 *                                                                         *
 *   Copyright (C) 2011 Mauro Gamba <maurillo71@gmail.com>                 *
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
#include "libusb_common.h"

static struct libusb_context *jtag_libusb_context; /**< Libusb context **/
static libusb_device **devs; /**< The usb device list **/

static bool jtag_libusb_match(struct libusb_device *dev,
		const uint16_t vids[], const uint16_t pids[])
{
	struct libusb_device_descriptor dev_desc;
	
	for (unsigned i = 0; vids[i] && pids[i]; i++)
	{
		if (libusb_get_device_descriptor(dev, &dev_desc) == 0)
		{
		    if (dev_desc.idVendor == vids[i] &&
			dev_desc.idProduct == pids[i])
		    {
			    return true;
		    }
		}
	}
	return false;
}

int jtag_libusb_open(const uint16_t vids[], const uint16_t pids[],
		libusb_device_handle **out)
{
	int cnt,idx,errCode;
	
	if (libusb_init(&jtag_libusb_context) < 0)
		return -ENODEV;

	libusb_set_debug(jtag_libusb_context,3);

	cnt = libusb_get_device_list(jtag_libusb_context, &devs);
	
	for (idx = 0; idx < cnt; idx++)
	{
		if (!jtag_libusb_match(devs[idx], vids, pids))
			continue;
		
		errCode = libusb_open(devs[idx],out);
		
		/** Free the device list **/
		libusb_free_device_list(devs, 1);

		if (errCode < 0)
			return errCode;
		return 0;
	}
	return -ENODEV;
}

void jtag_libusb_close(libusb_device_handle *dev)
{
	/* Close device */
	libusb_close(dev);
	
	libusb_exit(jtag_libusb_context);
}
