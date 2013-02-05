/***************************************************************************
 *   Copyright (C) 2012 by Hsiangkai Wang                                  *
 *   Hsiangkai Wang <hkwang@andestech.com>                                 *
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

#include "aice_usb.h"
#include "aice_pipe.h"
#include "aice_port.h"

static const struct aice_port aice_ports[] = {
	{
		.name = "aice_usb",
		.type = AICE_PORT_AICE_USB,
		.api = &aice_usb_api,
	},
	{
		.name = "aice_pipe",
		.type = AICE_PORT_AICE_PIPE,
		.api = &aice_pipe,
	},
	{.name = NULL, /* END OF TABLE */ },
};

/** */
const struct aice_port *aice_port_get_list(void)
{
	return aice_ports;
}

