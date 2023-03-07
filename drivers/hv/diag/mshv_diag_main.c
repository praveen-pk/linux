// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, Microsoft Corporation.
 *
 * This module exposes Diagnostic Logs, Performance Tracing and other
 * telemetry data from hyp to userspace via special device /dev/mshv_diag
 *
 * Authors:
 *	Praveen K Paladugu <prapal@linux.microsoft.com>
 *	Mukesh Rathor <mrathor@linux.microsoft.com>
 */

#include <asm/mshyperv.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <uapi/linux/mshv.h>

#include "mshv_diag.h"

static long mshv_diag_ioctl(struct file *filp, unsigned int ioctl,
			    unsigned long arg)
{
	int rc;

	switch (ioctl) {
	case MSHV_GET_DIAGLOG_FD:
		rc = mshv_diaglog_get_fd();
		break;
	default:
		return -ENOTTY;
	}

	return rc;
}

static const struct file_operations mshv_diag_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mshv_diag_ioctl,
};

static struct miscdevice mshv_diag_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mshv_diag",
	.fops = &mshv_diag_fops,
	.mode = 0400,
};

static int __init mshv_diag_init(void)
{
	int ret;

	if (!hv_root_partition)
		return -EPERM;

	ret = misc_register(&mshv_diag_dev);
	if (ret) {
		pr_err("%s: misc device register failed\n", __func__);
		return ret;
	}

	ret = mshv_diaglog_init();
	if (ret < 0) {
		misc_deregister(&mshv_diag_dev);
		return ret;
	}

	return ret;
}

static void __exit mshv_diag_exit(void)
{
	misc_deregister(&mshv_diag_dev);
	mshv_diaglog_exit();
}

module_init(mshv_diag_init);
module_exit(mshv_diag_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Microsoft");
