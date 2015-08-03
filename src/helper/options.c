/***************************************************************************
 *   Copyright (C) 2004, 2005 by Dominic Rath                              *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007-2010 Øyvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
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
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "configuration.h"
/* @todo the inclusion of server.h here is a layering violation */
#include <server/server.h>

#include <getopt.h>
#include <unistd.h>

#if defined(__linux__) || defined(IS_MSYS2_64_BIT) || defined(IS_MSYS2_32_BIT)
#include <libgen.h>
#endif

static int help_flag, version_flag;

static const struct option long_options[] = {
	{"help",		no_argument,			&help_flag,		1},
	{"version",		no_argument,			&version_flag,	1},
	{"debug",		optional_argument,		0,				'd'},
	{"file",		required_argument,		0,				'f'},
	{"search",		required_argument,		0,				's'},
	{"log_output",	required_argument,		0,				'l'},
	{"command",		required_argument,		0,				'c'},
	{"pipe",		no_argument,			0,				'p'},
	{0, 0, 0, 0}
};

int configuration_output_handler(struct command_context *context, const char *line)
{
	LOG_USER_N("%s", line);

	return ERROR_OK;
}

#ifdef _WIN32
static char *find_suffix(const char *text, const char *suffix)
{
	size_t text_len = strlen(text);
	size_t suffix_len = strlen(suffix);

	if (suffix_len == 0)
		return (char *)text + text_len;

	if (suffix_len > text_len || strncmp(text + text_len - suffix_len, suffix, suffix_len) != 0)
		return NULL; /* Not a suffix of text */

	return (char *)text + text_len - suffix_len;
}
#endif

static void add_default_dirs(void)
{
	const char *run_prefix = NULL;
	char *path;

	/* Dynamically get the full pathname of the OpenOCD executable
	 * In case it has been unzipped manually in a non-standard location
	 *
	 * This is platform dependent since C does not
	 * have a method for doing this.
	 * See http://stackoverflow.com/a/1024937
	 */
#if defined(_WIN32)
	/* Get the Windows current executable name via GetModuleFileName */
	char strExePath[MAX_PATH];
	GetModuleFileName(NULL, strExePath, MAX_PATH);

	/* Strip executable file name, leaving path */
	*strrchr(strExePath, '\\') = '\0';

	/* Convert path separators to UNIX style, should work on Windows also. */
	for (char *p = strExePath; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}

	char *end_of_prefix = find_suffix(strExePath, BINDIR);
	if (end_of_prefix != NULL)
		*end_of_prefix = '\0';

	run_prefix = strExePath;
#elif defined(__linux__) || defined(IS_MSYS2_64_BIT) || defined(IS_MSYS2_32_BIT)
	run_prefix = "";
	int path_size = 1024;
	char *strExePath;
	ssize_t retval;
	while (1) {
		strExePath = (char *) malloc(path_size);
		if (strExePath == NULL)
			break;
		retval = readlink("/proc/self/exe", strExePath, path_size);
		LOG_DEBUG("retval=%d path_size=%d", (int)retval, (int)path_size);
		if (retval < path_size) {
			strExePath = dirname(strExePath);
			run_prefix = strExePath;
			break;
		} else if (retval == path_size) {
			free(strExePath);
			path_size *= 2;
		} else {
			break;
		}
	}
#else
	LOG_WARNING("Platform has no implementation for getting executable path");
#endif

	/* Installation directory for binaries    - defaults to /usr/bin/ */
	LOG_DEBUG("bindir=%s",     BINDIR);

	/* Installation directory for shared data - defaults to /usr/share/openocd/ */
	LOG_DEBUG("pkgdatadir=%s", PKGDATADIR);

	/*
	 * The directory containing OpenOCD-supplied scripts should be
	 * listed last in the built-in search order, so the site administrator can
	 * override these scripts with site-specific customizations.
	 * User-specific search directories should go first so they can override all others
	 */

	/* User-specific scripts stored in <Home>/.openocd */
	const char *home = getenv("HOME");

	if (home) {
		path = alloc_printf("%s/.openocd", home);
		if (path) {
			add_script_search_dir(path);
			free(path);
		}
	}
#ifdef _WIN32
	/* Windows user-specific scripts stored in <AppData>/OpenOCD */
	const char *appdata = getenv("APPDATA");

	if (appdata) {
		path = alloc_printf("%s/OpenOCD", appdata);
		if (path) {
			add_script_search_dir(path);
			free(path);
		}
	}
#endif

	/* Site specific scripts - offset from installation package data directory */
	path = alloc_printf("%s%s", PKGDATADIR, "/site");
	if (path) {
		add_script_search_dir(path);
		free(path);
	}

	/* Standard scripts location */
	path = alloc_printf("%s%s", PKGDATADIR, "/scripts");
	if (path) {
		add_script_search_dir(path);
		free(path);
	}

	if (run_prefix) {
		LOG_DEBUG("run_prefix=%s", run_prefix); /* Location of current executable */

		/* Running from source tree - TCL directory relative to executable */
		path = alloc_printf("%s%s", run_prefix, "/../tcl");
		if (path) {
			add_script_search_dir(path);
			free(path);
		}

		/* Running from unzipped location - scripts directory relative to executable */
		path = alloc_printf("%s%s", run_prefix, "/../share/openocd/scripts");
		if (path) {
			add_script_search_dir(path);
			free(path);
		}
	} else {
		LOG_WARNING("Executable path could not be determined. "
			"If executable is in non-standard location, then default "
			"script search path will not locate scripts - use '-s' option if needed.");
	}
}

int parse_cmdline_args(struct command_context *cmd_ctx, int argc, char *argv[])
{
	int c;

	while (1) {
		/* getopt_long stores the option index here. */
		int option_index = 0;

		c = getopt_long(argc, argv, "hvd::l:f:s:c:p", long_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1)
			break;

		switch (c) {
			case 0:
				break;
			case 'h':		/* --help | -h */
				help_flag = 1;
				break;
			case 'v':		/* --version | -v */
				version_flag = 1;
				break;
			case 'f':		/* --file | -f */
			{
				char *command = alloc_printf("script {%s}", optarg);
				add_config_command(command);
				free(command);
				break;
			}
			case 's':		/* --search | -s */
				add_script_search_dir(optarg);
				break;
			case 'd':		/* --debug | -d */
			{
				char *command = alloc_printf("debug_level %s", optarg ? optarg : "3");
				command_run_line(cmd_ctx, command);
				free(command);
				break;
			}
			case 'l':		/* --log_output | -l */
				if (optarg) {
					char *command = alloc_printf("log_output %s", optarg);
					command_run_line(cmd_ctx, command);
					free(command);
				}
				break;
			case 'c':		/* --command | -c */
				if (optarg)
				    add_config_command(optarg);
				break;
			case 'p':
				/* to replicate the old syntax this needs to be synchronous
				 * otherwise the gdb stdin will overflow with the warning message */
				command_run_line(cmd_ctx, "gdb_port pipe; log_output openocd.log");
				LOG_WARNING("deprecated option: -p/--pipe. Use '-c \"gdb_port pipe; "
						"log_output openocd.log\"' instead.");
				break;
		}
	}

	if (help_flag) {
		LOG_OUTPUT("Open On-Chip Debugger\nLicensed under GNU GPL v2\n");
		LOG_OUTPUT("--help       | -h\tdisplay this help\n");
		LOG_OUTPUT("--version    | -v\tdisplay OpenOCD version\n");
		LOG_OUTPUT("--file       | -f\tuse configuration file <name>\n");
		LOG_OUTPUT("--search     | -s\tdir to search for config files and scripts\n");
		LOG_OUTPUT("--debug      | -d\tset debug level <0-3>\n");
		LOG_OUTPUT("--log_output | -l\tredirect log output to file <name>\n");
		LOG_OUTPUT("--command    | -c\trun <command>\n");
		exit(-1);
	}

	if (version_flag) {
		/* Nothing to do, version gets printed automatically. */
		/* It is not an error to request the VERSION number. */
		exit(0);
	}

	/* paths specified on the command line take precedence over these
	 * built-in paths
	 */
	add_default_dirs();

	return ERROR_OK;
}
