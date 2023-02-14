// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, Microsoft Corporation.
 *
 * The /sys/kernel/debug/mshv directory contents.
 * Contains various statistics data, provided by the hypervisor.
 *
 * Authors:
 *   Stanislav Kinsburskii <skinsburskii@linux.microsoft.com>
 */

#include <linux/debugfs.h>

#include "mshv.h"

static struct dentry *mshv_debugfs;

int __init mshv_debugfs_init(void)
{
	mshv_debugfs = debugfs_create_dir("mshv", NULL);
	if (IS_ERR(mshv_debugfs)) {
		pr_err("mshv: failed to create debugfs directory\n");
		return PTR_ERR(mshv_debugfs);
	}

	return 0;
}

void __exit mshv_debugfs_exit(void)
{
	debugfs_remove_recursive(mshv_debugfs);
}
