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

#include <asm/mshyperv.h>

#include <hv/hvhdk.h>

#include "mshv.h"
#include "mshv_root.h"

static struct dentry *mshv_debugfs;
static struct dentry *mshv_debugfs_partition;

static int partition_stats_show(struct seq_file *m, void *v)
{
	const u64 *stats = m->private;

#define PARTITION_SEQ_PRINTF(cnt)		\
	seq_printf(m, "%-29s: %llu\n", __stringify(cnt), stats[Partition##cnt])

	PARTITION_SEQ_PRINTF(VirtualProcessors);
	PARTITION_SEQ_PRINTF(TlbSize);
	PARTITION_SEQ_PRINTF(AddressSpaces);
	PARTITION_SEQ_PRINTF(DepositedPages);
	PARTITION_SEQ_PRINTF(GpaPages);
	PARTITION_SEQ_PRINTF(GpaSpaceModifications);
	PARTITION_SEQ_PRINTF(VirtualTlbFlushEntires);
	PARTITION_SEQ_PRINTF(RecommendedTlbSize);
	PARTITION_SEQ_PRINTF(GpaPages4K);
	PARTITION_SEQ_PRINTF(GpaPages2M);
	PARTITION_SEQ_PRINTF(GpaPages1G);
	PARTITION_SEQ_PRINTF(GpaPages512G);
	PARTITION_SEQ_PRINTF(DevicePages4K);
	PARTITION_SEQ_PRINTF(DevicePages2M);
	PARTITION_SEQ_PRINTF(DevicePages1G);
	PARTITION_SEQ_PRINTF(DevicePages512G);
	PARTITION_SEQ_PRINTF(AttachedDevices);
	PARTITION_SEQ_PRINTF(DeviceInterruptMappings);
	PARTITION_SEQ_PRINTF(IoTlbFlushes);
	PARTITION_SEQ_PRINTF(IoTlbFlushCost);
	PARTITION_SEQ_PRINTF(DeviceInterruptErrors);
	PARTITION_SEQ_PRINTF(DeviceDmaErrors);
	PARTITION_SEQ_PRINTF(DeviceInterruptThrottleEvents);
	PARTITION_SEQ_PRINTF(SkippedTimerTicks);
	PARTITION_SEQ_PRINTF(PartitionId);
	PARTITION_SEQ_PRINTF(NestedTlbSize);
	PARTITION_SEQ_PRINTF(RecommendedNestedTlbSize);
	PARTITION_SEQ_PRINTF(NestedTlbFreeListSize);
	PARTITION_SEQ_PRINTF(NestedTlbTrimmedPages);
	PARTITION_SEQ_PRINTF(PagesShattered);
	PARTITION_SEQ_PRINTF(PagesRecombined);
	PARTITION_SEQ_PRINTF(HwpRequestValue);

#undef PARTITION_SEQ_PRINTF

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(partition_stats);

static void mshv_partition_stats_unmap(u64 partition_id)
{
	union hv_stats_object_identity identity = {
		.partition.partition_id = partition_id,
	};
	int err;

	err = hv_call_unmap_stat_page(HV_STATS_OBJECT_PARTITION,
				      &identity);
	if (err)
		pr_err("%s: failed to unmap partition %lld stats, err: %d\n",
			__func__, partition_id, err);
}

static void *mshv_partition_stats_map(u64 partition_id)
{
	union hv_stats_object_identity identity = {
		.partition.partition_id = partition_id,
	};
	void *stats;
	int err;

	err = hv_call_map_stat_page(HV_STATS_OBJECT_PARTITION,
				    &identity, &stats);
	if (err) {
		pr_err("%s: failed to map partition %lld stats, err: %d\n",
				__func__, partition_id, err);
		return ERR_PTR(err);
	}
	return stats;
}

static int mshv_debugfs_partition_stats_create(u64 partition_id, struct dentry *parent)
{
	struct dentry *dentry;
	void *stats;
	int err;

	stats = mshv_partition_stats_map(partition_id);
	if (IS_ERR(stats))
		return PTR_ERR(stats);

	dentry = debugfs_create_file("stats", 0400, parent,
				     stats, &partition_stats_fops);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		goto unmap_partition_stats;
	}

	return 0;

unmap_partition_stats:
	mshv_partition_stats_unmap(partition_id);
	return err;
}

static void partition_debugfs_remove(u64 partition_id, struct dentry *dentry)
{
	debugfs_remove_recursive(dentry);

	mshv_partition_stats_unmap(partition_id);
}

static struct dentry *partition_debugfs_create(u64 partition_id, struct dentry *parent)
{
	char part_id[21]; /* sizeof(u64) + 1 */
	struct dentry *id;
	int err;

	sprintf(part_id, "%llu", partition_id);

	id = debugfs_create_dir(part_id, parent);
	if (IS_ERR(id))
		return id;

	err = mshv_debugfs_partition_stats_create(partition_id, id);
	if (err)
		goto remove_debugfs_partition_id;

	return id;

remove_debugfs_partition_id:
	debugfs_remove_recursive(id);
	return ERR_PTR(err);
}

static int __init mshv_debugfs_root_partition_create(void)
{
	struct dentry *id;
	int err;

	mshv_debugfs_partition = debugfs_create_dir("partition",
						     mshv_debugfs);
	if (IS_ERR(mshv_debugfs_partition))
		return PTR_ERR(mshv_debugfs_partition);

	id = partition_debugfs_create(hv_current_partition_id,
				      mshv_debugfs_partition);
	if (IS_ERR(id)) {
		err = PTR_ERR(id);
		goto remove_debugfs_partition;
	}

	return 0;

remove_debugfs_partition:
	debugfs_remove_recursive(mshv_debugfs_partition);
	return err;
}

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

static void mshv_hv_stats_unmap(void)
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

int mshv_debugfs_partition_create(struct mshv_partition *partition)
{
	struct dentry *id;

	id = partition_debugfs_create(partition->id, mshv_debugfs_partition);
	if (IS_ERR(id))
		return PTR_ERR(id);

	partition->debugfs_dentry = id;

	return 0;
}

void mshv_debugfs_partition_remove(struct mshv_partition *partition)
{
	partition_debugfs_remove(partition->id, partition->debugfs_dentry);
}

int __init mshv_debugfs_init(void)
{
	int err;

	mshv_debugfs = debugfs_create_dir("mshv", NULL);
	if (IS_ERR(mshv_debugfs)) {
		pr_err("mshv: failed to create debugfs directory\n");
		return PTR_ERR(mshv_debugfs);
	}

	err = mshv_debugfs_hv_stats_create(mshv_debugfs);
	if (err)
		goto remove_mshv_dir;

	err = mshv_debugfs_root_partition_create();
	if (err)
		goto unmap_hv_stats;

	return 0;

unmap_hv_stats:
	mshv_hv_stats_unmap();
remove_mshv_dir:
	debugfs_remove_recursive(mshv_debugfs);
	return err;
}

void __exit mshv_debugfs_exit(void)
{
	partition_debugfs_remove(hv_current_partition_id, NULL);

	debugfs_remove_recursive(mshv_debugfs);

	mshv_hv_stats_unmap();
}
