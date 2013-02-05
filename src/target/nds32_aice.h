/***************************************************************************
 *   Copyright (C) 2013 Andes technology.                                  *
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
#ifndef __NDS32_AICE_H__
#define __NDS32_AICE_H__

#include <jtag/aice/aice_port.h>

int aice_read_reg_64(struct aice_port_s *aice, uint32_t num, uint64_t *val);
int aice_write_reg_64(struct aice_port_s *aice, uint32_t num, uint64_t val);
int aice_select_target(struct aice_port_s *aice, uint32_t target_id);
int aice_read_tlb(struct aice_port_s *aice, uint32_t virtual_address,
		uint32_t *physical_address);
int aice_cache_ctl(struct aice_port_s *aice, uint32_t subtype, uint32_t address);
int aice_set_retry_times(struct aice_port_s *aice, uint32_t a_retry_times);
int aice_program_edm(struct aice_port_s *aice, char *command_sequence);
int aice_pack_command(struct aice_port_s *aice, bool enable_pack_command);
int aice_execute(struct aice_port_s *aice, uint32_t *instructions,
		uint32_t instruction_num);
int aice_set_custom_srst_script(struct aice_port_s *aice, const char *script);
int aice_set_custom_trst_script(struct aice_port_s *aice, const char *script);
int aice_set_custom_restart_script(struct aice_port_s *aice, const char *script);
int aice_set_count_to_check_dbger(struct aice_port_s *aice, uint32_t count_to_check);

static inline int aice_open(struct aice_port_s *aice, struct aice_port_param_s *param)
{
	return aice->port->api->open(param);
}

static inline int aice_close(struct aice_port_s *aice)
{
	return aice->port->api->close();
}

static inline int aice_reset(struct aice_port_s *aice)
{
	return aice->port->api->reset();
}

static inline int aice_assert_srst(struct aice_port_s *aice,
		enum aice_srst_type_s srst)
{
	return aice->port->api->assert_srst(srst);
}

static inline int aice_run(struct aice_port_s *aice)
{
	return aice->port->api->run();
}

static inline int aice_halt(struct aice_port_s *aice)
{
	return aice->port->api->halt();
}

static inline int aice_step(struct aice_port_s *aice)
{
	return aice->port->api->step();
}

static inline int aice_read_register(struct aice_port_s *aice, uint32_t num,
		uint32_t *val)
{
	return aice->port->api->read_reg(num, val);
}

static inline int aice_write_register(struct aice_port_s *aice, uint32_t num,
		uint32_t val)
{
	return aice->port->api->write_reg(num, val);
}

static inline int aice_read_debug_reg(struct aice_port_s *aice, uint32_t addr,
		uint32_t *val)
{
	return aice->port->api->read_debug_reg(addr, val);
}

static inline int aice_write_debug_reg(struct aice_port_s *aice, uint32_t addr,
		const uint32_t val)
{
	return aice->port->api->write_debug_reg(addr, val);
}

static inline int aice_read_mem_unit(struct aice_port_s *aice, uint32_t addr,
		uint32_t size, uint32_t count, uint8_t *buffer)
{
	return aice->port->api->read_mem_unit(addr, size, count, buffer);
}

static inline int aice_write_mem_unit(struct aice_port_s *aice, uint32_t addr,
		uint32_t size, uint32_t count, const uint8_t *buffer)
{
	return aice->port->api->write_mem_unit(addr, size, count, buffer);
}

static inline int aice_read_mem_bulk(struct aice_port_s *aice, uint32_t addr,
		uint32_t length, uint8_t *buffer)
{
	return aice->port->api->read_mem_bulk(addr, length, buffer);
}

static inline int aice_write_mem_bulk(struct aice_port_s *aice, uint32_t addr,
		uint32_t length, const uint8_t *buffer)
{
	return aice->port->api->write_mem_bulk(addr, length, buffer);
}

static inline int aice_idcode(struct aice_port_s *aice, uint32_t *idcode,
		uint8_t *num_of_idcode)
{
	return aice->port->api->idcode(idcode, num_of_idcode);
}

static inline int aice_state(struct aice_port_s *aice,
		enum aice_target_state_s *state)
{
	return aice->port->api->state(state);
}

static inline int aice_set_jtag_clock(struct aice_port_s *aice, uint32_t a_clock)
{
	return aice->port->api->set_jtag_clock(a_clock);
}

static inline int aice_memory_access(struct aice_port_s *aice,
		enum nds_memory_access a_access)
{
	return aice->port->api->memory_access(a_access);
}

static inline int aice_memory_mode(struct aice_port_s *aice,
		enum nds_memory_select mem_select)
{
	return aice->port->api->memory_mode(mem_select);
}

static inline int aice_set_data_endian(struct aice_port_s *aice,
		enum aice_target_endian target_data_endian)
{
	return aice->port->api->set_data_endian(target_data_endian);
}

#endif
