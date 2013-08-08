/***************************************************************************
 *   Copyright (C) 2013 by Franck Jullien                                  *
 *   elec4fun@gmail.com                                                    *
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

#include "or1k_tap.h"
#include "or1k.h"

#include <jtag/jtag.h>

/* Contains constants relevant to the Altera Virtual JTAG
 * device, which are not included in the BSDL.
 * As of this writing, these are constant across every
 * device which supports virtual JTAG.
 */

/* These are commands for the FPGA's IR. */
#define ALTERA_CYCLONE_CMD_VIR		0x0E
#define ALTERA_CYCLONE_CMD_VDR		0x0C

/* These defines are for the virtual IR (not the FPGA's)
 * The virtual TAP was defined in hardware to match the OpenCores native
 * TAP in both IR size and DEBUG command.
 */
#define ALT_VJTAG_IR_SIZE		4
#define ALT_VJTAG_CMD_DEBUG		0x8

/* SLD node ID. */
#define JTAG_TO_AVALON_NODE_ID		0x84
#define VJTAG_NODE_ID			0x08
#define SIGNAL_TAP_NODE_ID		0x00
#define SERIAL_FLASH_LOADER_NODE_ID	0x04

#define VER(x)				((x >> 27) & 0x1f)
#define NB_NODES(x)			((x >> 19) & 0xff)
#define ID(x)				((x >> 19) & 0xff)
#define MANUF(x)			((x >> 8)  & 0x7ff)
#define M_WIDTH(x)			((x >> 0)  & 0xff)
#define INST_ID(x)			((x >> 0)  & 0xff)

/* tap instructions - Mohor JTAG TAP */
#define OR1K_TAP_INST_IDCODE 0x2
#define OR1K_TAP_INST_DEBUG 0x8

static char *id_to_string(unsigned char id)
{
	switch (id) {
	case VJTAG_NODE_ID:
		return "Virtual JTAG";
	case JTAG_TO_AVALON_NODE_ID:
		return "JTAG to avalon bridge";
	case SIGNAL_TAP_NODE_ID:
		return "Signal TAP";
	case SERIAL_FLASH_LOADER_NODE_ID:
		return "Serial Flash Loader";
	}
	return "unknown";
}

static unsigned char guess_addr_width(unsigned char number_of_nodes)
{
	unsigned char width = 0;

	while (number_of_nodes) {
		number_of_nodes >>= 1;
		width++;
	}

	return width;
}

static int or1k_tap_vjtag_init(struct or1k_jtag *jtag_info)
{
	struct scan_field field;
	struct jtag_tap *tap;
	int i;
	int node_index;
	int vjtag_node_address = 0;
	uint32_t hub_info = 0;
	uint32_t node_info = 0;
	uint8_t t[8];
	uint8_t ret;
	int m_width;
	int nb_nodes;

	tap = jtag_info->tap;
	if (tap == NULL)
		return ERROR_FAIL;

	LOG_DEBUG("Initialising Altera Virtual JTAG TAP");

	/* Ensure TAP is reset - maybe not necessary*/
	jtag_add_tlr();

	/* Select VIR */
	field.num_bits = tap->ir_length;
	field.out_value = t;
	buf_set_u32(t, 0, field.num_bits, ALTERA_CYCLONE_CMD_VIR);
	field.in_value = NULL;
	jtag_add_ir_scan(tap, &field, TAP_IDLE);

	/* The SLD hub contains the HUB IP Configuration Register and SLD_NODE_INFO
	 * register for each SLD node in the design. The HUB IP configuration register provides
	 * information needed to determine the dimensions of the USER1 DR chain. The
	 * SLD_NODE_INFO register is used to determine the address mapping for Virtual
	 * JTAG instance in your design. This register set is shifted out by issuing the
	 * HUB_INFO instruction. Both the ADDR bits for the SLD hub and the HUB_INFO
	 * instruction is 0 × 0.
	 * Because m and n are unknown at this point, the DR register
	 * (ADDR bits + VIR_VALUE) must be filled with zeros. Shifting a sequence of 64 zeroes
	 * into the USER1 DR is sufficient to cover the most conservative case for m and n.
	 */

	field.num_bits = 64;
	field.out_value = t;
	field.in_value = NULL;
	memset(t, 0, 8);
	jtag_add_dr_scan(tap, 1, &field, TAP_IDLE);

	/* Select VDR */
	field.num_bits = tap->ir_length;
	field.out_value = t;
	buf_set_u32(t, 0, field.num_bits, ALTERA_CYCLONE_CMD_VDR);
	field.in_value = NULL;
	jtag_add_ir_scan(tap, &field, TAP_IDLE);

	jtag_execute_queue();

	for (i = 0; i < 8; i++) {
		field.num_bits = 4;
		field.out_value = NULL;
		field.in_value = &ret;
		jtag_add_dr_scan(tap, 1, &field, TAP_IDLE);
		jtag_execute_queue();
		hub_info = ((hub_info >> 4) | ((ret & 0xf) << 28));
	}

	nb_nodes = NB_NODES(hub_info);
	m_width = M_WIDTH(hub_info);

	LOG_DEBUG("SLD HUB Configuration register");
	LOG_DEBUG("------------------------------");
	LOG_DEBUG("m_width         = %d", m_width);
	LOG_DEBUG("manufacturer_id = 0x%02x", MANUF(hub_info));
	LOG_DEBUG("nb_of_node      = %d", nb_nodes);
	LOG_DEBUG("version         = %d", VER(hub_info));
	LOG_DEBUG("VIR length      = %d", guess_addr_width(nb_nodes) + m_width);

	/* Because the number of SLD nodes is now known, the Nodes on the hub can be
	 * enumerated by repeating the 8 four-bit nibble scans, once for each Node,
	 * to yield the SLD_NODE_INFO register of each Node. The DR nibble shifts
	 * are a continuation of the HUB_INFO DR shift used to shift out the Hub IP
	 * Configuration register.
	 */

	for (node_index = 0; node_index < nb_nodes; node_index++) {

		for (i = 0; i < 8; i++) {
			field.num_bits = 4;
			field.out_value = NULL;
			field.in_value = &ret;
			jtag_add_dr_scan(tap, 1, &field, TAP_IDLE);
			jtag_execute_queue();
			node_info = ((node_info >> 4) | ((ret & 0xf) << 28));
		}

		LOG_DEBUG("Node info register");
		LOG_DEBUG("--------------------");
		LOG_DEBUG("instance_id     = %d", ID(node_info));
		LOG_DEBUG("manufacturer_id = 0x%02x", MANUF(node_info));
		LOG_DEBUG("node_id         = %d (%s)", ID(node_info),
						       id_to_string(ID(node_info)));
		LOG_DEBUG("version         = %d", VER(node_info));

		if (ID(node_info) == VJTAG_NODE_ID)
			vjtag_node_address = node_index + 1;
	}

	/* Select VIR */
	field.num_bits = tap->ir_length;
	field.out_value = t;
	buf_set_u32(t, 0, field.num_bits, ALTERA_CYCLONE_CMD_VIR);
	field.in_value = NULL;
	jtag_add_ir_scan(tap, &field, TAP_IDLE);

	/* Send the DEBUG command */
	field.num_bits = guess_addr_width(nb_nodes) + m_width;
	field.out_value = t;
	buf_set_u32(t, 0, field.num_bits, (vjtag_node_address << m_width) | ALT_VJTAG_CMD_DEBUG);
	field.in_value = NULL;
	jtag_add_dr_scan(tap, 1, &field, TAP_IDLE);

	/* Select VDR */
	field.num_bits = tap->ir_length;
	field.out_value = t;
	buf_set_u32(t, 0, field.num_bits, ALTERA_CYCLONE_CMD_VDR);
	field.in_value = NULL;
	jtag_add_ir_scan(tap, &field, TAP_IDLE);

	jtag_execute_queue();

	return ERROR_OK;
}

static struct or1k_tap_ip vjtag_tap = {
	.name = "vjtag",
	.init = or1k_tap_vjtag_init,
};

int or1k_tap_vjtag_register(void)
{
	list_add_tail(&vjtag_tap.list, &tap_list);
	return 0;
}
