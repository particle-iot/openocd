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

#ifndef JTAG_USB_COMMON_H
#define JTAG_USB_COMMON_H

#include <helper/types.h>

#ifdef HAVE_LIBUSB1
#include <libusb.h>

#define jtag_usb_device 				libusb_device
#define jtag_usb_device_handle 			libusb_device_handle
#define jtag_usb_device_descriptor		libusb_device_descriptor
#define jtag_usb_interface 				libusb_interface
#define jtag_usb_interface_descriptor 	libusb_interface_descriptor
#define jtag_usb_endpoint_descriptor 	libusb_endpoint_descriptor
#define jtag_usb_config_descriptor 		libusb_config_descriptor

#define jtag_usb_reset_device(dev) 		libusb_reset_device(dev)
#define jtag_usb_get_device(devh)	 	libusb_get_device(devh)
#define jtag_usb_claim_interface(dev, iface) libusb_claim_interface (dev, iface)
#define jtag_usb_set_interface_alt_setting(dev, prev, alt) libusb_set_altinterface (dev, alt)
#define jtag_usb_release_interface(dev, iface) libusb_release_interface (dev, iface)
#else
#include <usb.h>

#define jtag_usb_device 				usb_device
#define jtag_usb_device_handle			usb_dev_handle
#define jtag_usb_device_descriptor		usb_device_descriptor
#define jtag_usb_interface 				usb_interface
#define jtag_usb_interface_descriptor 	usb_interface_descriptor
#define jtag_usb_endpoint_descriptor 	usb_endpoint_descriptor
#define jtag_usb_config_descriptor 		usb_config_descriptor

#define jtag_usb_reset_device(dev)				usb_reset (dev)
#define jtag_usb_get_device(devh)	 			usb_device(devh)
#define jtag_usb_claim_interface(dev, iface) 	usb_claim_interface (dev, iface)
#define jtag_usb_set_interface_alt_setting(dev, prev, alt) usb_set_altinterface (dev, alt)
#define jtag_usb_release_interface(dev, iface) 	usb_release_interface (dev, iface)

#endif

int jtag_usb_open(const uint16_t vids[], const uint16_t pids[],
		struct jtag_usb_device_handle **out);
void jtag_usb_close(jtag_usb_device_handle *dev);
int jtag_usb_bulk_write(struct jtag_usb_device_handle *dev, int ep, char *bytes,
						int size, int timeout);
int jtag_usb_bulk_read(struct jtag_usb_device_handle *dev, int ep, char *bytes,
					   int size, int timeout);
int jtag_usb_set_configuration(jtag_usb_device_handle *devh, int configuration);
int jtag_usb_get_endpoints(struct jtag_usb_device *udev,
						   unsigned int *usb_read_ep,
						   unsigned int *usb_write_ep);

#endif // JTAG_USB_COMMON_H
