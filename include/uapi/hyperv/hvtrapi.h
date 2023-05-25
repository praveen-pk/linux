/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Type definitions to access Diagnostic Events from hypervsior
 */
#ifndef _HV_HVTRAPI_H
#define _HV_HVTRAPI_H

#ifdef __KERNEL__

/* Max number of pages in MSHV's Diagnostic Buffers */
#define HV_MAX_PAGES_IN_DIAG 512  /* Non-HyperV code */

struct hv_input_map_eventlog_buffer { /* HV_INPUT_MAP_EVENTLOG_BUFFER */
	u32 event_log_type; /* HV_EVENTLOG_TYPE */
	u32 buffer_index;
} __packed;

struct hv_output_map_eventlog_buffer { /* HV_OUTPUT_MAP_EVENTLOG_BUFFER */
	u64 gpa_numbers[HV_MAX_PAGES_IN_DIAG];
} __packed;

struct hv_input_unmap_eventlog_buffer { /* HV_INPUT_UNMAP_EVENTLOG_BUFFER */
	u32 event_log_type; /* HV_EVENTLOG_TYPE */
	u32 buffer_index;
} __packed;

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

#endif
