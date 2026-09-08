// SPDX-License-Identifier: GPL-2.0
/*
 * Mali KMD version selector
 *
 * Parses mali.version= cmdline param into the version string
 * defined in init/main.c for Mali KMD modules to check.
 */

#include <linux/init.h>
#include <linux/string.h>

extern char mali_selected_version[];

static int __init mali_version_setup(char *str)
{
	if (str)
		strscpy(mali_selected_version, str, 8);
	return 1;
}
__setup("mali.version=", mali_version_setup);
