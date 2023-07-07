/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Type definitions to access Diagnostic Events from hypervsior
 */
#ifndef _HV_HVTRAPI_H
#define _HV_HVTRAPI_H

#include <hyperv/hvgdk_mini.h>

#ifdef __KERNEL__

/* Max number of pages in MSHV's Diagnostic Buffers */
#define HV_MAX_PAGES_IN_DIAG 512  /* Non-HyperV code */

struct hv_input_map_eventlog_buffer { /* HV_INPUT_MAP_EVENTLOG_BUFFER */
	u32 type; /* HV_EVENTLOG_TYPE */
	u32 buffer_index;
} __packed;

struct hv_output_map_eventlog_buffer { /* HV_OUTPUT_MAP_EVENTLOG_BUFFER */
	u64 gpa_numbers[HV_MAX_PAGES_IN_DIAG];
} __packed;

union hv_input_unmap_eventlog_buffer { /* HV_INPUT_UNMAP_EVENTLOG_BUFFER */
	u64 as_uint64;
	struct {
		u32 type; /* HV_EVENTLOG_TYPE */
		u32 buffer_index;
	} __packed;
};

enum hv_eventlog_buffer_state { /* HV_EVENTLOG_BUFFER_STATE */
	HV_EVENT_LOG_BUFFER_STATE_STANDBY = 0,
	HV_EVENT_LOG_BUFFER_STATE_FREE = 1,
	HV_EVENT_LOG_BUFFER_STATE_IN_USE = 2,
	HV_EVENT_LOG_BUFFER_STATE_COMPLETE = 3,
	HV_EVENT_LOG_BUFFER_STATE_READY = 4,
};

struct hv_eventlog_buffer_header { /* HV_EVENTLOG_BUFFER_HEADER */
	u32 buffer_size;
	u32 buffer_index;
	u32 events_lost;
	u32 reference_count;
	union {
		u64 time_stamp;
		hv_nano100_time_t reference_time;
	};
	u64 reserved1;
	u64 reserved2;
	struct {
		u16 logical_processor;
		u16 logger_id;
	} __packed;
	u32 buffer_state; /* HV_EVENTLOG_BUFFER_STATE */
	u32 next_buffer_offset;
	union {
		u32 type; /* HV_EVENTLOG_TYPE */
		struct {
			u16 buffer_flag;
			u16 buffer_type;
		} __packed;
	};
	u32 next_buffer_index;
	u32 lp_sequence_number;
	u32 reserved4[2];
} __packed;

struct hv_input_initialize_eventlog_buffer_group {
	struct hv_eventlog_init_type {
		__u16 type; /* enum hv_eventlog_type */
		__u16 mode; /* enum hv_eventlog_mode */
	} __packed init;
	__u32 maximum_buffer_count;
	__u32 buffer_size_in_bytes;
	__u32 threshold;
	__u32 time_basis; /* enum hv_eventlog_entry_time_basis */
	hv_nano100_time_t system_time;
} __packed;

union hv_input_finalize_eventlog_buffer_group {
	__u64 as_uint64;
	struct {
		__u32 type; /* enum hv_eventlog_type */
	} __packed;
};

union hv_input_create_eventlog_buffer {
	__u64 as_uint64[2];
	struct {
		__u32 type; /* enum hv_eventlog_type */
		__u32 buffer_index;
		union hv_proximity_domain_info proximity_info;
	} __packed;
};

union hv_input_delete_eventlog_buffer {
	__u64 as_uint64;
	struct {
		__u32 type; /* enum hv_eventlog_type */
		__u32 buffer_index;
	} __packed;
};
#endif

struct hv_eventlog_entry_header { /* HV_EVENTLOG_ENTRY_HEADER */
	__u32 context;
	__u16 size;
	__u16 type;
	union {
		__u64 time_stamp;
		hv_nano100_time_t reference_time; /* HV_NANO100_TIME */
	};
} __attribute__((packed, aligned(sizeof(__u64))));

enum hv_eventlog_mode {
	HV_EVENT_LOG_MODE_REGULAR  = 0,
	HV_EVENT_LOG_MODE_CIRCULAR = 1,
	HV_EVENT_LOG_MODE_MAX      = 2
};

enum hv_eventlog_entry_time_basis {
	HV_EVENT_LOG_ENTRY_TIME_REFERENCE = 0,
	HV_EVENT_LOG_ENTRY_TIME_TSC       = 1,
	HV_EVENT_LOG_ENTRY_TIME_QPC       = 2,
	HV_EVENT_LOG_ENTRY_TIME_MAX       = 3
};

#endif
