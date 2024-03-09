// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, Microsoft Corporation.
 *
 * The /dev/mshv device.
 * This is the core module mshv_root and mshv_vtl depend on.
 *
 * Authors:
 *   Nuno Das Neves <nudasnev@microsoft.com>
 *   Lillian Grassin-Drake <ligrassi@microsoft.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/cpuhotplug.h>
#include <linux/random.h>
#include <linux/nospec.h>
#include <asm/mshyperv.h>

#include "mshv_eventfd.h"
#include "mshv.h"

MODULE_AUTHOR("Microsoft");
MODULE_LICENSE("GPL");

static struct mutex mshv_ops_mutex;
static const struct mshv_ops *module_ops;

static int mshv_register_dev(void);
static void mshv_deregister_dev(void);

static int mshv_dev_open(struct inode *inode, struct file *filp);
static int mshv_dev_release(struct inode *inode, struct file *filp);
static long mshv_dev_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg);

static const struct file_operations mshv_dev_fops = {
	.owner = THIS_MODULE,
	.open = mshv_dev_open,
	.release = mshv_dev_release,
	.unlocked_ioctl = mshv_dev_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice mshv_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mshv",
	.fops = &mshv_dev_fops,
	.mode = 0600,
};

int mshv_set_ops(const struct mshv_ops *ops, struct device **dev)
{
	int ret = 0;

	mutex_lock(&mshv_ops_mutex);
	if (ops && dev) {
		*dev = mshv_dev.this_device;
		ret = mshv_register_dev();
	} else {
		mshv_deregister_dev();
	}

	if (!ret)
		module_ops = ops;
	mutex_unlock(&mshv_ops_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mshv_set_ops);

static int mshv_register_dev(void)
{
	int ret;

	if (mshv_dev.this_device &&
	    device_is_registered(mshv_dev.this_device)) {
		dev_err(mshv_dev.this_device, "mshv device already registered\n");
		return -ENODEV;
	}

	ret = misc_register(&mshv_dev);
	if (ret)
		pr_err("%s: mshv device register failed\n", __func__);

	return ret;
}

static void mshv_deregister_dev(void)
{
	misc_deregister(&mshv_dev);
}

static long
mshv_ioctl_get_api_version(void __user *user_arg)
{
	long ret;
	struct mshv_version_info arg;

	if (copy_from_user(&arg, user_arg, sizeof(arg)))
		return -EFAULT;

	if (memchr_inv(&arg.rsvd_0, 0,
		       sizeof(arg) - offsetof(struct mshv_version_info, rsvd_0)))
		return -EINVAL;

	ret = module_ops->get_version_info(&arg);
	if (ret)
		return ret;

	if (copy_to_user(user_arg, &arg, sizeof(arg)))
		return -EFAULT;

	return 0;
}

static long
mshv_dev_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
	if (!module_ops)
		return -ENODEV;

	switch (ioctl) {
	case MSHV_GET_VERSION_INFO:
		return mshv_ioctl_get_api_version((void __user *)arg);
	}

	return module_ops->ioctl(filp, ioctl, arg);
}

static int
mshv_dev_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int
mshv_dev_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static int
__init mshv_init(void)
{
	if (!hv_is_hyperv_initialized())
		return -ENODEV;

	mutex_init(&mshv_ops_mutex);

	return 0;
}

static void
__exit mshv_exit(void)
{
}

module_init(mshv_init);
module_exit(mshv_exit);
