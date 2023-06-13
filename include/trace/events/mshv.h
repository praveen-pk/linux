/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023, Microsoft Corporation.
 *
 * Tracepoint definitions for tracepoints in mshv driver.
 *
 * Authors:
 *   Shubhangi Agrawal <t-shuagrawal@microsoft.com>
 */

#if !defined(_TRACE_MSHV_MAIN_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MSHV_MAIN_H

#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM mshv

TRACE_EVENT(mshv_create_partition,
	    TP_PROTO(long ret, u64 partition_id, int vm_fd),
	    TP_ARGS(ret, partition_id, vm_fd),

	TP_STRUCT__entry(
		__field(long, ret)
		__field(u64, partition_id)
		__field(int, vm_fd)
	),

	TP_fast_assign(
		__entry->ret = ret;
		__entry->partition_id = partition_id;
		__entry->vm_fd = vm_fd;
	),

	TP_printk("ret=%ld partition_id=%llu vm_fd=%d",
		__entry->ret,
		__entry->partition_id,
		__entry->vm_fd
	)
);

TRACE_EVENT(mshv_hvcall_create_partition,
		TP_PROTO(u64 status, u64 partition_id, u64 flags),
		TP_ARGS(status, partition_id, flags),

	TP_STRUCT__entry(
		__field(u64, status)
		__field(u64, partition_id)
		__field(u64, flags)
	),

	TP_fast_assign(
		__entry->status = status;
		__entry->partition_id = partition_id;
		__entry->flags = flags;
	),

	TP_printk("status=0x%llx partition_id=%llu flags=0x%llx",
		__entry->status,
		__entry->partition_id,
		__entry->flags
	)
);

TRACE_EVENT(mshv_hvcall_set_partition_property,
		TP_PROTO(u64 status, u64 partition_id, u64 pcode, u64 pvalue),
		TP_ARGS(status, partition_id, pcode, pvalue),

	TP_STRUCT__entry(
		__field(u64, status)
		__field(u64, partition_id)
		__field(u64, pcode)
		__field(u64, pvalue)
	),

	TP_fast_assign(
		__entry->status = status;
		__entry->partition_id = partition_id;
		__entry->pcode = pcode;
		__entry->pvalue = pvalue;
	),

	TP_printk("status=0x%llx partition_id=%llu property_code=0x%llx property_value=0x%llx",
		__entry->status,
		__entry->partition_id,
		__entry->pcode,
		__entry->pvalue
	)
);

TRACE_EVENT(mshv_hvcall_initialize_partition,
		TP_PROTO(u64 status, u64 partition_id),
		TP_ARGS(status, partition_id),

	TP_STRUCT__entry(
		__field(u64, status)
		__field(u64, partition_id)
	),

	TP_fast_assign(
		__entry->status = status;
		__entry->partition_id = partition_id;
	),

	TP_printk("status=0x%llx partition_id=%llu",
		__entry->status,
		__entry->partition_id
	)
);

TRACE_EVENT(mshv_destroy_partition,
		TP_PROTO(u64 partition_id),
		TP_ARGS(partition_id),

	TP_STRUCT__entry(
		__field(u64, partition_id)
	),

	TP_fast_assign(
		__entry->partition_id = partition_id;
	),

	TP_printk("partition_id=%llu",
		__entry->partition_id
	)
);

TRACE_EVENT(mshv_hvcall_finalize_partition,
		TP_PROTO(u64 status, u64 partition_id),
		TP_ARGS(status, partition_id),

	TP_STRUCT__entry(
		__field(u64, status)
		__field(u64, partition_id)
	),

	TP_fast_assign(
		__entry->status = status;
		__entry->partition_id = partition_id;
	),

	TP_printk("status=0x%llx partition_id=%llu",
		__entry->status,
		__entry->partition_id
	)
);

TRACE_EVENT(mshv_hvcall_withdraw_memory,
		TP_PROTO(u64 status, u64 partition_id),
		TP_ARGS(status, partition_id),

	TP_STRUCT__entry(
		__field(u64, status)
		__field(u64, partition_id)
	),

	TP_fast_assign(
		__entry->status = status;
		__entry->partition_id = partition_id;
	),

	TP_printk("status=0x%llx partition_id=%llu",
		__entry->status,
		__entry->partition_id
	)
);

TRACE_EVENT(mshv_hvcall_delete_partition,
		TP_PROTO(u64 status, u64 partition_id),
		TP_ARGS(status, partition_id),

	TP_STRUCT__entry(
		__field(u64, status)
		__field(u64, partition_id)
	),

	TP_fast_assign(
		__entry->status = status;
		__entry->partition_id = partition_id;
	),

	TP_printk("status=0x%llx partition_id=%llu",
		__entry->status,
		__entry->partition_id
	)
);

#endif /* _TRACE_MSHV_MAIN_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
