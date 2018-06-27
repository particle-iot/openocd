/***************************************************************************
 *   Copyright (C) 2009 by Zachary T Welch <zw@superlucidity.net>          *
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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "usb_common.h"

bool jtag_usb_location_equal(const char *location, uint8_t dev_bus, uint8_t *port_path,
			     size_t path_len)
{
	size_t path_step;
	char *ptr, *loc;
	bool equal = false;

	/* strtok need non const char */
	loc = strdup(location);

	ptr = strtok(loc, "-");
	if (ptr == NULL) {
		printf("no '-' in path\n");
		goto done;
	}

	/* check bus mismatch */
	if (atoi(ptr) != dev_bus)
		goto done;

	path_step = 0;
	while (path_step < path_len) {
		ptr = strtok(NULL, ".");

		/* no more tokens in path */
		if (ptr == NULL)
			break;

		/* path mismatch at some step */
		if (path_step < path_len && atoi(ptr) != port_path[path_step])
			break;

		path_step++;
	};

	/* walked the full path, all elements match */
	if (path_step == path_len)
		equal = 1;
	else
		fprintf(stderr, " excluded by device path option\n");

done:
	free(loc);
	return equal;
}

static bool jtag_usb_match(struct usb_device *dev,
		const uint16_t vids[], const uint16_t pids[])
{
	for (unsigned i = 0; vids[i] && pids[i]; i++) {
		if (dev->descriptor.idVendor == vids[i] &&
			dev->descriptor.idProduct == pids[i])
			return true;
	}
	return false;
}

int jtag_usb_open(const uint16_t vids[], const uint16_t pids[],
		struct usb_dev_handle **out)
{
	usb_find_busses();
	usb_find_devices();

	struct usb_bus *busses = usb_get_busses();
	for (struct usb_bus *bus = busses; bus; bus = bus->next) {
		for (struct usb_device *dev = bus->devices; dev; dev = dev->next) {
			if (!jtag_usb_match(dev, vids, pids))
				continue;

			*out = usb_open(dev);
			if (NULL == *out)
				return -errno;
			return 0;
		}
	}
	return -ENODEV;
}
