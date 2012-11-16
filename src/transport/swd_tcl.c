/*
 * TCL Interface for SWD Transport.
 *
 * Copyright (C) 2011-2012 Tomasz Boleslaw CEDRO
 * cederom@tlen.pl, http://www.tomek.cedro.info
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the Tomasz Boleslaw CEDRO nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.*
 *
 * Written by Tomasz Boleslaw CEDRO <cederom@tlen.pl>, 2011-2012;
 *
 */

/** \file swd_tcl.c TCL Interface for SWD Transport.
 * This file contains TCL interface and functions to work with SWD transport.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <interface/interface.h>
#include <transport/transport.h>
#include <transport/swd.h>
#include <transport/swd_libswd.h>
#include <target/arm.h>
#include <target/arm_adi_v5.h>
#include <helper/log.h>

static const struct command_registration oocd_transport_swd_subcommand_handlers[] = {
	{
		/*
		 * Set up SWD and JTAG targets identically, unless/until
		 * infrastructure improves ...  meanwhile, ignore all
		 * JTAG-specific stuff like IR length for SWD.
		 *
		 * REVISIT can we verify "just one SWD DAP" here/early?
		 */
		.name = "newtap",
		.jim_handler = jim_jtag_newtap,
		.mode = COMMAND_CONFIG,
		.help = "declare a new SWD DAP"
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration oocd_transport_swd_command_handlers[] = {
	{
		.name = "swd",
		.mode = COMMAND_ANY,
		.help = "SWD command group",
		.chain = oocd_transport_swd_subcommand_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

int oocd_transport_swd_register_commands(struct command_context *cmd_ctx)
{
	return register_commands(cmd_ctx, NULL, oocd_transport_swd_command_handlers);
}

/** @} */
