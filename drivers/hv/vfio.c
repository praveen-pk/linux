// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO-MSHV bridge pseudo device
 *
 * Heavily inspired by the VFIO-KVM bridge pseudo device.
 * Copyright (C) 2013 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 */

#include <linux/errno.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/anon_inodes.h>
#include <linux/nospec.h>

#include "mshv.h"
#include "mshv_root.h"
#include "vfio.h"

struct mshv_device {
	const struct mshv_device_ops *ops;
	struct mshv_partition *partition;
	void *private;
	struct hlist_node partition_node;

};

/* create, destroy, and name are mandatory */
struct mshv_device_ops {
	const char *name;

	/*
	 * create is called holding partition->mutex and any operations not suitable
	 * to do while holding the lock should be deferred to init (see
	 * below).
	 */
	int (*create)(struct mshv_device *dev, u32 type);

	/*
	 * init is called after create if create is successful and is called
	 * outside of holding partition->mutex.
	 */
	void (*init)(struct mshv_device *dev);

	/*
	 * Destroy is responsible for freeing dev.
	 *
	 * Destroy may be called before or after destructors are called
	 * on emulated I/O regions, depending on whether a reference is
	 * held by a vcpu or other mshv component that gets destroyed
	 * after the emulated I/O.
	 */
	void (*destroy)(struct mshv_device *dev);

	/*
	 * Release is an alternative method to free the device. It is
	 * called when the device file descriptor is closed. Once
	 * release is called, the destroy method will not be called
	 * anymore as the device is removed from the device list of
	 * the VM. partition->mutex is held.
	 */
	void (*release)(struct mshv_device *dev);

	int (*set_attr)(struct mshv_device *dev, struct mshv_device_attr *attr);
	int (*get_attr)(struct mshv_device *dev, struct mshv_device_attr *attr);
	int (*has_attr)(struct mshv_device *dev, struct mshv_device_attr *attr);
	long (*ioctl)(struct mshv_device *dev, unsigned int ioctl,
		      unsigned long arg);
	int (*mmap)(struct mshv_device *dev, struct vm_area_struct *vma);
};

struct mshv_vfio_group {
	struct list_head node;
	struct vfio_group *vfio_group;
};

struct mshv_vfio {
	struct list_head group_list;
	struct mutex lock;
};

static struct vfio_group *mshv_vfio_group_get_external_user(struct file *filep)
{
	struct vfio_group *vfio_group;
	struct vfio_group *(*fn)(struct file *);

	fn = symbol_get(vfio_group_get_external_user);
	if (!fn)
		return ERR_PTR(-EINVAL);

	vfio_group = fn(filep);

	symbol_put(vfio_group_get_external_user);

	return vfio_group;
}

static bool mshv_vfio_external_group_match_file(struct vfio_group *group,
						struct file *filep)
{
	bool ret, (*fn)(struct vfio_group *, struct file *);

	fn = symbol_get(vfio_external_group_match_file);
	if (!fn)
		return false;

	ret = fn(group, filep);

	symbol_put(vfio_external_group_match_file);

	return ret;
}

static void mshv_vfio_group_put_external_user(struct vfio_group *vfio_group)
{
	void (*fn)(struct vfio_group *);

	fn = symbol_get(vfio_group_put_external_user);
	if (!fn)
		return;

	fn(vfio_group);

	symbol_put(vfio_group_put_external_user);
}

static int mshv_vfio_set_group(struct mshv_device *dev, long attr, u64 arg)
{
	struct mshv_vfio *mv = dev->private;
	struct vfio_group *vfio_group;
	struct mshv_vfio_group *mvg;
	int32_t __user *argp = (int32_t __user *)(unsigned long)arg;
	struct fd f;
	int32_t fd;
	int ret;

	switch (attr) {
	case MSHV_DEV_VFIO_GROUP_ADD:
		if (get_user(fd, argp))
			return -EFAULT;

		f = fdget(fd);
		if (!f.file)
			return -EBADF;

		vfio_group = mshv_vfio_group_get_external_user(f.file);
		fdput(f);

		if (IS_ERR(vfio_group))
			return PTR_ERR(vfio_group);

		mutex_lock(&mv->lock);

		list_for_each_entry(mvg, &mv->group_list, node) {
			if (mvg->vfio_group == vfio_group) {
				mutex_unlock(&mv->lock);
				mshv_vfio_group_put_external_user(vfio_group);
				return -EEXIST;
			}
		}

		mvg = kzalloc(sizeof(*mvg), GFP_KERNEL_ACCOUNT);
		if (!mvg) {
			mutex_unlock(&mv->lock);
			mshv_vfio_group_put_external_user(vfio_group);
			return -ENOMEM;
		}

		list_add_tail(&mvg->node, &mv->group_list);
		mvg->vfio_group = vfio_group;

		mutex_unlock(&mv->lock);

		return 0;

	case MSHV_DEV_VFIO_GROUP_DEL:
		if (get_user(fd, argp))
			return -EFAULT;

		f = fdget(fd);
		if (!f.file)
			return -EBADF;

		ret = -ENOENT;

		mutex_lock(&mv->lock);

		list_for_each_entry(mvg, &mv->group_list, node) {
			if (!mshv_vfio_external_group_match_file(mvg->vfio_group,
								 f.file))
				continue;

			list_del(&mvg->node);
			mshv_vfio_group_put_external_user(mvg->vfio_group);
			kfree(mvg);
			ret = 0;
			break;
		}

		mutex_unlock(&mv->lock);

		fdput(f);

		return ret;
	}

	return -ENXIO;
}

static int mshv_vfio_set_attr(struct mshv_device *dev,
			      struct mshv_device_attr *attr)
{
	switch (attr->group) {
	case MSHV_DEV_VFIO_GROUP:
		return mshv_vfio_set_group(dev, attr->attr, attr->addr);
	}

	return -ENXIO;
}

static int mshv_vfio_has_attr(struct mshv_device *dev,
			      struct mshv_device_attr *attr)
{
	switch (attr->group) {
	case MSHV_DEV_VFIO_GROUP:
		switch (attr->attr) {
		case MSHV_DEV_VFIO_GROUP_ADD:
		case MSHV_DEV_VFIO_GROUP_DEL:
			return 0;
		}

		break;
	}

	return -ENXIO;
}

static void mshv_vfio_destroy(struct mshv_device *dev)
{
	struct mshv_vfio *mv = dev->private;
	struct mshv_vfio_group *mvg, *tmp;

	list_for_each_entry_safe(mvg, tmp, &mv->group_list, node) {
		mshv_vfio_group_put_external_user(mvg->vfio_group);
		list_del(&mvg->node);
		kfree(mvg);
	}

	kfree(mv);
	kfree(dev);
}

static int mshv_vfio_create(struct mshv_device *dev, u32 type);

static struct mshv_device_ops mshv_vfio_ops = {
	.name = "mshv-vfio",
	.create = mshv_vfio_create,
	.destroy = mshv_vfio_destroy,
	.set_attr = mshv_vfio_set_attr,
	.has_attr = mshv_vfio_has_attr,
};

static int mshv_vfio_create(struct mshv_device *dev, u32 type)
{
	struct mshv_device *tmp;
	struct mshv_vfio *mv;

	/* Only one VFIO "device" per VM */
	hlist_for_each_entry(tmp, &dev->partition->devices, partition_node)
		if (tmp->ops == &mshv_vfio_ops)
			return -EBUSY;

	mv = kzalloc(sizeof(*mv), GFP_KERNEL_ACCOUNT);
	if (!mv)
		return -ENOMEM;

	INIT_LIST_HEAD(&mv->group_list);
	mutex_init(&mv->lock);

	dev->private = mv;

	return 0;
}

static int mshv_device_release(struct inode *inode, struct file *filp);
static long mshv_device_ioctl(struct file *filp, unsigned int ioctl,
			      unsigned long arg);

static const struct file_operations mshv_device_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mshv_device_ioctl,
	.release = mshv_device_release,
};

static const struct mshv_device_ops *mshv_device_ops_table[MSHV_DEV_TYPE_MAX];

static int mshv_device_ioctl_attr(struct mshv_device *dev,
				 int (*accessor)(struct mshv_device *dev,
						 struct mshv_device_attr *attr),
				 unsigned long arg)
{
	struct mshv_device_attr attr;

	if (!accessor)
		return -EPERM;

	if (copy_from_user(&attr, (void __user *)arg, sizeof(attr)))
		return -EFAULT;

	return accessor(dev, &attr);
}

static long mshv_device_ioctl(struct file *filp, unsigned int ioctl,
			      unsigned long arg)
{
	struct mshv_device *dev = filp->private_data;

	switch (ioctl) {
	case MSHV_SET_DEVICE_ATTR:
		return mshv_device_ioctl_attr(dev, dev->ops->set_attr, arg);
	case MSHV_GET_DEVICE_ATTR:
		return mshv_device_ioctl_attr(dev, dev->ops->get_attr, arg);
	case MSHV_HAS_DEVICE_ATTR:
		return mshv_device_ioctl_attr(dev, dev->ops->has_attr, arg);
	default:
		if (dev->ops->ioctl)
			return dev->ops->ioctl(dev, ioctl, arg);

		return -ENOTTY;
	}
}

static int mshv_device_release(struct inode *inode, struct file *filp)
{
	struct mshv_device *dev = filp->private_data;
	struct mshv_partition *partition = dev->partition;

	if (dev->ops->release) {
		mutex_lock(&partition->mutex);
		hlist_del(&dev->partition_node);
		dev->ops->release(dev);
		mutex_unlock(&partition->mutex);
	}

	mshv_partition_put(partition);
	return 0;
}

long mshv_partition_ioctl_create_device(struct mshv_partition *partition,
					void __user *user_args)
{
	long r;
	struct mshv_create_device tmp, *cd;
	struct mshv_device *dev;
	const struct mshv_device_ops *ops;
	int type;

	if (copy_from_user(&tmp, user_args, sizeof(tmp))) {
		r = -EFAULT;
		goto out;
	}

	cd = &tmp;

	if (cd->type >= ARRAY_SIZE(mshv_device_ops_table)) {
		r = -ENODEV;
		goto out;
	}

	type = array_index_nospec(cd->type, ARRAY_SIZE(mshv_device_ops_table));
	ops = mshv_device_ops_table[type];
	if (ops == NULL) {
		r = -ENODEV;
		goto out;
	}

	if (cd->flags & MSHV_CREATE_DEVICE_TEST) {
		r = 0;
		goto out;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL_ACCOUNT);
	if (!dev) {
		r = -ENOMEM;
		goto out;
	}

	dev->ops = ops;
	dev->partition = partition;

	r = ops->create(dev, type);
	if (r < 0) {
		kfree(dev);
		goto out;
	}

	hlist_add_head(&dev->partition_node, &partition->devices);

	if (ops->init)
		ops->init(dev);

	mshv_partition_get(partition);
	r = anon_inode_getfd(ops->name, &mshv_device_fops, dev, O_RDWR | O_CLOEXEC);
	if (r < 0) {
		mshv_partition_put(partition);
		hlist_del(&dev->partition_node);
		ops->destroy(dev);
		goto out;
	}

	cd->fd = r;
	r = 0;

	if (copy_to_user(user_args, &tmp, sizeof(tmp))) {
		r = -EFAULT;
		goto out;
	}
out:
	return r;
}

void mshv_destroy_devices(struct mshv_partition *partition)
{
	struct mshv_device *dev;
	struct hlist_node *n;

	/*
	 * No need to take any lock since at this point nobody else can
	 * reference this partition.
	 */
	hlist_for_each_entry_safe(dev, n, &partition->devices, partition_node) {
		hlist_del(&dev->partition_node);
		dev->ops->destroy(dev);
	}
}

static int mshv_register_device_ops(const struct mshv_device_ops *ops, u32 type)
{
	if (type >= ARRAY_SIZE(mshv_device_ops_table))
		return -ENOSPC;

	if (mshv_device_ops_table[type] != NULL)
		return -EEXIST;

	mshv_device_ops_table[type] = ops;
	return 0;
}

static void mshv_unregister_device_ops(u32 type)
{
	if (type >= ARRAY_SIZE(mshv_device_ops_table))
		return;
	mshv_device_ops_table[type] = NULL;
}

int mshv_vfio_ops_init(void)
{
	return mshv_register_device_ops(&mshv_vfio_ops, MSHV_DEV_TYPE_VFIO);
}

void mshv_vfio_ops_exit(void)
{
	mshv_unregister_device_ops(MSHV_DEV_TYPE_VFIO);
}
