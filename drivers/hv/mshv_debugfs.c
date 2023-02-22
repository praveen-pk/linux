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
#include <linux/stringify.h>

#include <hv/hvhdk.h>

#include "mshv.h"

static struct dentry *mshv_debugfs;

static int hv_stats_show(struct seq_file *m, void *v)
{
	const u64 *stats = m->private;

#define HV_SEQ_PRINTF(cnt)		\
	seq_printf(m, "%-24s: %llu\n", __stringify(cnt), stats[Hv##cnt])

	HV_SEQ_PRINTF(LogicalProcessors);
	HV_SEQ_PRINTF(Partitions);
	HV_SEQ_PRINTF(TotalPages);
	HV_SEQ_PRINTF(VirtualProcessors);
	HV_SEQ_PRINTF(MonitoredNotifications);
	HV_SEQ_PRINTF(ModernStandbyEntries);
	HV_SEQ_PRINTF(PlatformIdleTransitions);
	HV_SEQ_PRINTF(HypervisorStartupCost);

	HV_SEQ_PRINTF(IOSpacePages);
	HV_SEQ_PRINTF(NonEssentialPagesForDump);
	HV_SEQ_PRINTF(SubsumedPages);

#undef HV_SEQ_PRINTF

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hv_stats);

static void __exit mshv_hv_stats_unmap(void)
{
	union hv_stats_object_identity identity = { };
	int err;

	err = hv_call_unmap_stat_page(HV_STATS_OBJECT_HYPERVISOR,
				      &identity);
	if (err)
		pr_err("%s: failed to unmap hypervisor stats: %d\n",
				__func__, err);
}

static void * __init mshv_hv_stats_map(void)
{
	union hv_stats_object_identity identity = { };
	void *stats;
	int err;

	err = hv_call_map_stat_page(HV_STATS_OBJECT_HYPERVISOR,
				    &identity, &stats);
	if (err) {
		pr_err("%s: failed to map hypervisor stats: %d\n",
				__func__, err);
		return ERR_PTR(err);
	}
	return stats;
}

static int __init mshv_debugfs_hv_stats_create(struct dentry *parent)
{
	struct dentry *dentry;
	void *stats;
	int err;

	stats = mshv_hv_stats_map();
	if (IS_ERR(stats))
		return PTR_ERR(stats);

	dentry = debugfs_create_file("stats", 0400, parent,
				     stats, &hv_stats_fops);
	if (IS_ERR(dentry)) {
		pr_err("%s: failed to create hypervisor stats dentry: %d\n",
				__func__, err);
		err = PTR_ERR(dentry);
		goto unmap_hv_stats;
	}

	return 0;

unmap_hv_stats:
	mshv_hv_stats_unmap();
	return err;
}

int __init mshv_debugfs_init(void)
{
	mshv_debugfs = debugfs_create_dir("mshv", NULL);
	if (IS_ERR(mshv_debugfs)) {
		pr_err("mshv: failed to create debugfs directory\n");
		return PTR_ERR(mshv_debugfs);
	}

	return mshv_debugfs_hv_stats_create(mshv_debugfs);
}

void __exit mshv_debugfs_exit(void)
{
	debugfs_remove_recursive(mshv_debugfs);

	mshv_hv_stats_unmap();
}
