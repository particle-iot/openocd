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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "configuration.h"
#include "log.h"
#include "command.h"

#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/sysctl.h>
#include <errno.h>
#endif

static int help_flag, version_flag;

static const struct option long_options[] = {
	{"help",                no_argument,                    &help_flag,             1},
	{"version",             no_argument,                    &version_flag,  1},
	{"debug",               optional_argument,              0,
	 'd'},
	{"file",                required_argument,              0,
	 'f'},
	{"search",              required_argument,              0,
	 's'},
	{"log_output",  required_argument,              0,                              'l'},
	{"command",             required_argument,              0,
	 'c'},
	{"pipe",                no_argument,                    0,
	 'p'},
	{0, 0, 0, 0}
};

int configuration_output_handler(struct command_context *context, const char *line)
{
	LOG_USER_N("%s", line);

	return ERROR_OK;
}

static char *find_suffix(const char *text, const char *suffix)
{
	size_t text_len = strlen(text);
	size_t suffix_len = strlen(suffix);

	if (suffix_len == 0)
		return (char *)text + text_len;

	if (suffix_len > text_len || strncmp(text + text_len - suffix_len, suffix, suffix_len) != 0)
		return NULL;	/* Not a suffix of text */

	return (char *)text + text_len - suffix_len;
}

/** Concatenate strings, and add script search dir. */
static void add_script_dir(char *prefix, const char *suffix)
{
	/* Never build a suffix only path to the root directory. */
	if ((prefix == NULL) || (strlen(prefix) == 0))
		return;

	/* Convert path separators to UNIX style, should work on Windows also. */
	for (char *p = prefix; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}

	char *path = alloc_printf("%s%s", prefix, suffix);
	if (path) {
		add_script_search_dir(path);
		free(path);
	}
}

static void add_script_dirs(char *prefix)
{
	/* Don't add /site or /scripts */
	if ((prefix == NULL) || (strlen(prefix) == 0))
		return;
	add_script_dir(prefix, "/site");
	add_script_dir(prefix, "/scripts");
}

static void add_default_dirs(void)
{
	char *run_prefix = "";

#ifdef _WIN32
	char strExePath[MAX_PATH];
	GetModuleFileName(NULL, strExePath, MAX_PATH);

	/* Convert path separators to UNIX style, should work on Windows also. */
	for (char *p = strExePath; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}
#else
	char strExePath[PATH_MAX];
	char *ret;
	/* Linux/Cygwin */
	ret = realpath("/proc/self/exe", strExePath);
	if (ret == NULL)
		/* FreeBSD (if procfs installed) */
		ret = realpath("/proc/curproc/file", strExePath);
	if (ret == NULL)
		/* Solaris */
		ret = realpath("/proc/self/path/a.out", strExePath);
#if defined(CTL_KERN) && defined(KERN_PROC) && defined(KERN_PROC_PATHNAME)
	/* FreeBSD */
	int iret = 0;
	if (ret == NULL) {
		int mib[4];
		mib[0] = CTL_KERN;
		mib[1] = KERN_PROC;
		mib[2] = KERN_PROC_PATHNAME;
		mib[3] = -1;
		size_t cb = sizeof(strExePath);
		iret = sysctl(mib, 4, strExePath, &cb, NULL, 0);
		if (!iret)
			printf("buf:%s\n", strExePath);
	}
	if (iret)
		LOG_ERROR("Freebsd exe search failed.");
#else
	if (ret == NULL)
		LOG_ERROR("Executable path not found: %s.", strerror(errno));
#endif
#endif

	/* Strip executable file name, leaving path */
	*strrchr(strExePath, '/') = '\0';


	char *end_of_prefix = find_suffix(strExePath, BINDIR);
	if (end_of_prefix != NULL)
		*end_of_prefix = '\0';

	/* In some cases, BINDIR is a path that doesn't match.
	The executable path suffix is 'bin' in this case. */
	end_of_prefix = find_suffix(strExePath, "bin");
	if (end_of_prefix != NULL)
		*end_of_prefix = '\0';

	run_prefix = strExePath;

	LOG_DEBUG("bindir=%s", BINDIR);
	LOG_DEBUG("pkgdatadir=%s", PKGDATADIR);
	LOG_DEBUG("run_prefix=%s", run_prefix);

	/*
	 * The directory containing OpenOCD-supplied scripts should be
	 * listed last in the built-in search order, so the user can
	 * override these scripts with site-specific customizations.
	 */

	add_script_dir(getenv("HOME"), "/.openocd");

	add_script_dir(getenv("OPENOCD_SCRIPTS"), "");

#ifdef _WIN32
	char *appdata = getenv("APPDATA");

	if (appdata)
		add_script_dir(appdata, "/OpenOCD");
#endif

	add_script_dirs(run_prefix);
	add_script_dirs(PKGDATADIR);
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
				char *command = alloc_printf("debug_level %s",
					optarg ? optarg : "3");
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
				LOG_WARNING(
				"deprecated option: -p/--pipe. Use '-c \"gdb_port pipe; "
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
		/* Nothing to do, version gets printed automatically.
		 * It is not an error to request the VERSION number. */
		exit(0);
	}

	/* paths specified on the command line take precedence over these
	 * built-in paths
	 */
	add_default_dirs();

	return ERROR_OK;
}
