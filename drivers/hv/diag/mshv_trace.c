// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, Microsoft Corporation.
 *
 * Functions used to read events from Microsoft Hypervisor's Local Diagnostics
 *
 * Author:
 *   Stanislav Kinsburskii <skinsburskii@linux.microsoft.com>
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/anon_inodes.h>

#include "mshv_diag.h"

struct mshv_trace {
};

static int mshv_trace_release(struct inode *inode, struct file *filp)
{
	struct mshv_trace *trace = filp->private_data;

	kfree(trace);

	return 0;
}

static const struct file_operations mshv_trace_fops = {
	.owner = THIS_MODULE,
	.release = mshv_trace_release,
};

static struct mshv_trace *mshv_trace_create(void)
{
	struct mshv_trace *trace;

	trace = kzalloc(sizeof(struct mshv_trace), GFP_KERNEL);
	if (!trace)
		return ERR_PTR(-ENOMEM);

	return trace;
}

int mshv_trace_get_fd(void)
{
	struct mshv_trace *trace;
	int fd;

	trace = mshv_trace_create();
	if (IS_ERR(trace))
		return PTR_ERR(trace);

	fd = anon_inode_getfd("mshv_trace",
			      &mshv_trace_fops, trace,
			      O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		kfree(trace);

	return fd;
}
