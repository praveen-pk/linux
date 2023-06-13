// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, Microsoft Corporation.
 *
 * Authors:
 *   Shubhangi Agrawal <t-shuagrawal@microsoft.com>
 */

#define CREATE_TRACE_POINTS
#include <trace/events/mshv.h>

EXPORT_TRACEPOINT_SYMBOL_GPL(mshv_create_partition);
EXPORT_TRACEPOINT_SYMBOL_GPL(mshv_hvcall_create_partition);
EXPORT_TRACEPOINT_SYMBOL_GPL(mshv_hvcall_initialize_partition);
EXPORT_TRACEPOINT_SYMBOL_GPL(mshv_hvcall_set_partition_property);
EXPORT_TRACEPOINT_SYMBOL_GPL(mshv_destroy_partition);
EXPORT_TRACEPOINT_SYMBOL_GPL(mshv_hvcall_finalize_partition);
EXPORT_TRACEPOINT_SYMBOL_GPL(mshv_hvcall_delete_partition);
EXPORT_TRACEPOINT_SYMBOL_GPL(mshv_hvcall_withdraw_memory);
