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

static long mshv_ioctl_dummy(void __user *user_arg)
{
	return -ENOTTY;
}

static long mshv_check_ext_dummy(u32 arg)
{
	return -EOPNOTSUPP;
}

static struct mshv {
	struct mutex		mutex;
	mshv_create_func_t	create_vtl;
	mshv_create_func_t	create_partition;
	mshv_check_ext_func_t	check_extension;
} mshv = {
	.create_vtl		= mshv_ioctl_dummy,
	.create_partition	= mshv_ioctl_dummy,
	.check_extension	= mshv_check_ext_dummy,
};

static int mshv_register_dev(void);
static void mshv_deregister_dev(void);

int mshv_setup_vtl_func(const mshv_create_func_t create_vtl,
			const mshv_check_ext_func_t check_ext)
{
	int ret;

	mutex_lock(&mshv.mutex);
	if (create_vtl && check_ext) {
		ret = mshv_register_dev();
		if (ret)
			goto unlock;
		mshv.create_vtl = create_vtl;
		mshv.check_extension = check_ext;
	} else {
		mshv.create_vtl = mshv_ioctl_dummy;
		mshv.check_extension = mshv_check_ext_dummy;
		mshv_deregister_dev();
		ret = 0;
	}

unlock:
	mutex_unlock(&mshv.mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mshv_setup_vtl_func);

int mshv_set_create_partition_func(const mshv_create_func_t func)
{
	int ret;

	mutex_lock(&mshv.mutex);
	if (func) {
		ret = mshv_register_dev();
		if (ret)
			goto unlock;
		mshv.create_partition = func;
	} else {
		mshv.create_partition = mshv_ioctl_dummy;
		mshv_deregister_dev();
		ret = 0;
	}
	mshv.check_extension = mshv_check_ext_dummy;

unlock:
	mutex_unlock(&mshv.mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mshv_set_create_partition_func);

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

static int mshv_register_dev(void)
{
	int ret;

	if (mshv_dev.this_device &&
	    device_is_registered(mshv_dev.this_device)) {
		pr_err("%s: mshv device already registered\n", __func__);
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
mshv_ioctl_check_extension(void __user *user_arg)
{
	u32 arg;

	if (copy_from_user(&arg, user_arg, sizeof(arg)))
		return -EFAULT;

	switch (arg) {
	case MSHV_CAP_CORE_API_STABLE:
		return 0;
	}

	return mshv.check_extension(arg);
}

static long
mshv_dev_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
	switch (ioctl) {
	case MSHV_CHECK_EXTENSION:
		return mshv_ioctl_check_extension((void __user *)arg);
	case MSHV_CREATE_PARTITION:
		return mshv.create_partition((void __user *)arg);
	case MSHV_CREATE_VTL:
		return mshv.create_vtl((void __user *)arg);
	}

	return -ENOTTY;
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

	mutex_init(&mshv.mutex);

	return 0;
}

static void
__exit mshv_exit(void)
{
}

module_init(mshv_init);
module_exit(mshv_exit);
