/***************************************************************************
 *   Copyright (C) 2019 by Tomas Vanek                                     *
 *   vanekt@fbl.cz                                                         *
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

#include "imp.h"

FLASH_BANK_COMMAND_HANDLER(rom_bank_command)
{
	bank->read_only = true;
	return ERROR_OK;
}

static int rom_erase(struct flash_bank *bank, int first, int last)
{
	LOG_ERROR("Erase of read-only memory refused");
	return ERROR_FAIL;
}

static int rom_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	LOG_ERROR("Write to read-only memory refused");
	return ERROR_FAIL;
}

static int rom_probe(struct flash_bank *bank)
{
	return ERROR_OK;
}

const struct flash_driver read_only_flash = {
	.name = "read_only",
	.flash_bank_command = rom_bank_command,
	.erase = rom_erase,
	.write = rom_write,
	.read = default_flash_read,
	.probe = rom_probe,
	.auto_probe = rom_probe,
	.erase_check = default_flash_blank_check,

	/* ROM driver doesn't set driver_priv, free(NULL) makes no harm */
	.free_driver_priv = default_flash_free_driver_priv,
};
