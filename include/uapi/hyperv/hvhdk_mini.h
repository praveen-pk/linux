/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Type definitions for the hypervisor host interface to kernel.
 */
#ifndef _UAPI_HV_HVHDK_MINI_H
#define _UAPI_HV_HVHDK_MINI_H

#include "hvgdk_mini.h"

#define HVHVK_MINI_VERSION		(25294)

/*
 * Doorbell connection_info flags.
 */
#define HV_DOORBELL_FLAG_TRIGGER_SIZE_MASK  0x00000007
#define HV_DOORBELL_FLAG_TRIGGER_SIZE_ANY   0x00000000
#define HV_DOORBELL_FLAG_TRIGGER_SIZE_BYTE  0x00000001
#define HV_DOORBELL_FLAG_TRIGGER_SIZE_WORD  0x00000002
#define HV_DOORBELL_FLAG_TRIGGER_SIZE_DWORD 0x00000003
#define HV_DOORBELL_FLAG_TRIGGER_SIZE_QWORD 0x00000004
#define HV_DOORBELL_FLAG_TRIGGER_ANY_VALUE  0x80000000

/* Each generic set contains 64 elements */
#define HV_GENERIC_SET_SHIFT		(6)
#define HV_GENERIC_SET_MASK		(63)

enum hv_generic_set_format {
	HV_GENERIC_SET_SPARSE_4K,
	HV_GENERIC_SET_ALL,
};

enum hv_scheduler_type {
	HV_SCHEDULER_TYPE_LP = 1, /* Classic scheduler w/o SMT */
	HV_SCHEDULER_TYPE_LP_SMT = 2, /* Classic scheduler w/ SMT */
	HV_SCHEDULER_TYPE_CORE_SMT = 3, /* Core scheduler */
	HV_SCHEDULER_TYPE_ROOT = 4, /* Root / integrated scheduler */
	HV_SCHEDULER_TYPE_MAX
};

struct hv_vpset {		/* HV_VP_SET */
	__u64 format;
	__u64 valid_bank_mask;
	__u64 bank_contents[];
} __packed;

enum hv_stats_object_type {
	HV_STATS_OBJECT_HYPERVISOR		= 0x00000001,
	HV_STATS_OBJECT_LOGICAL_PROCESSOR	= 0x00000002,
	HV_STATS_OBJECT_PARTITION		= 0x00010001,
	HV_STATS_OBJECT_VP			= 0x00010002
};

union hv_stats_object_identity {
	/* hv_stats_hypervisor */
	struct {
		__u8 reserved[16];
	} __packed hv;

	/* hv_stats_logical_processor */
	struct {
		__u32 lp_index;
		__u8 reserved[12];
	} __packed lp;

	/* hv_stats_partition */
	struct {
		__u64 partition_id;
		__u8  reserved[4];
		__u16 flags;
		__u8  reserved1[2];
	} __packed partition;

	/* hv_stats_vp */
	struct {
		__u64 partition_id;
		__u32 vp_index;
		__u16 flags;
		__u8  reserved[2];
	} __packed vp;
};

enum hv_partition_property_code {
	/* Privilege properties */
	HV_PARTITION_PROPERTY_PRIVILEGE_FLAGS				= 0x00010000,
	HV_PARTITION_PROPERTY_SYNTHETIC_PROC_FEATURES			= 0x00010001,

	/* Scheduling properties */
	HV_PARTITION_PROPERTY_SUSPEND					= 0x00020000,
	HV_PARTITION_PROPERTY_CPU_RESERVE				= 0x00020001,
	HV_PARTITION_PROPERTY_CPU_CAP					= 0x00020002,
	HV_PARTITION_PROPERTY_CPU_WEIGHT				= 0x00020003,
	HV_PARTITION_PROPERTY_CPU_GROUP_ID				= 0x00020004,

	/* Time properties */
	HV_PARTITION_PROPERTY_TIME_FREEZE				= 0x00030003,

	/* Debugging properties */
	HV_PARTITION_PROPERTY_DEBUG_CHANNEL_ID				= 0x00040000,

	/* Resource properties */
	HV_PARTITION_PROPERTY_VIRTUAL_TLB_PAGE_COUNT			= 0x00050000,
	HV_PARTITION_PROPERTY_VSM_CONFIG				= 0x00050001,
	HV_PARTITION_PROPERTY_ZERO_MEMORY_ON_RESET			= 0x00050002,
	HV_PARTITION_PROPERTY_PROCESSORS_PER_SOCKET			= 0x00050003,
	HV_PARTITION_PROPERTY_NESTED_TLB_SIZE				= 0x00050004,
	HV_PARTITION_PROPERTY_GPA_PAGE_ACCESS_TRACKING			= 0x00050005,
	HV_PARTITION_PROPERTY_VSM_PERMISSIONS_DIRTY_SINCE_LAST_QUERY	= 0x00050006,
	HV_PARTITION_PROPERTY_SGX_LAUNCH_CONTROL_CONFIG			= 0x00050007,
	HV_PARTITION_PROPERTY_DEFAULT_SGX_LAUNCH_CONTROL0		= 0x00050008,
	HV_PARTITION_PROPERTY_DEFAULT_SGX_LAUNCH_CONTROL1		= 0x00050009,
	HV_PARTITION_PROPERTY_DEFAULT_SGX_LAUNCH_CONTROL2		= 0x0005000a,
	HV_PARTITION_PROPERTY_DEFAULT_SGX_LAUNCH_CONTROL3		= 0x0005000b,
	HV_PARTITION_PROPERTY_ISOLATION_STATE				= 0x0005000c,
	HV_PARTITION_PROPERTY_ISOLATION_CONTROL				= 0x0005000d,
	HV_PARTITION_PROPERTY_ALLOCATION_ID				= 0x0005000e,
	HV_PARTITION_PROPERTY_MONITORING_ID				= 0x0005000f,
	HV_PARTITION_PROPERTY_IMPLEMENTED_PHYSICAL_ADDRESS_BITS		= 0x00050010,
	HV_PARTITION_PROPERTY_NON_ARCHITECTURAL_CORE_SHARING		= 0x00050011,
	HV_PARTITION_PROPERTY_HYPERCALL_DOORBELL_PAGE			= 0x00050012,
	HV_PARTITION_PROPERTY_ISOLATION_POLICY				= 0x00050014,
	HV_PARTITION_PROPERTY_UNIMPLEMENTED_MSR_ACTION                  = 0x00050017,
	HV_PARTITION_PROPERTY_SEV_VMGEXIT_OFFLOADS			= 0x00050022,

	/* Compatibility properties */
	HV_PARTITION_PROPERTY_PROCESSOR_VENDOR				= 0x00060000,
	HV_PARTITION_PROPERTY_PROCESSOR_FEATURES_DEPRECATED		= 0x00060001,
	HV_PARTITION_PROPERTY_PROCESSOR_XSAVE_FEATURES			= 0x00060002,
	HV_PARTITION_PROPERTY_PROCESSOR_CL_FLUSH_SIZE			= 0x00060003,
	HV_PARTITION_PROPERTY_ENLIGHTENMENT_MODIFICATIONS		= 0x00060004,
	HV_PARTITION_PROPERTY_COMPATIBILITY_VERSION			= 0x00060005,
	HV_PARTITION_PROPERTY_PHYSICAL_ADDRESS_WIDTH			= 0x00060006,
	HV_PARTITION_PROPERTY_XSAVE_STATES				= 0x00060007,
	HV_PARTITION_PROPERTY_MAX_XSAVE_DATA_SIZE			= 0x00060008,
	HV_PARTITION_PROPERTY_PROCESSOR_CLOCK_FREQUENCY			= 0x00060009,
	HV_PARTITION_PROPERTY_PROCESSOR_FEATURES0			= 0x0006000a,
	HV_PARTITION_PROPERTY_PROCESSOR_FEATURES1			= 0x0006000b,

	/* Guest software properties */
	HV_PARTITION_PROPERTY_GUEST_OS_ID				= 0x00070000,

	/* Nested virtualization properties */
	HV_PARTITION_PROPERTY_PROCESSOR_VIRTUALIZATION_FEATURES		= 0x00080000,
};

enum hv_sleep_state {
	HV_SLEEP_STATE_S1 = 1,
	HV_SLEEP_STATE_S2 = 2,
	HV_SLEEP_STATE_S3 = 3,
	HV_SLEEP_STATE_S4 = 4,
	HV_SLEEP_STATE_S5 = 5,
	/*
	 * After hypervisor has reseived this, any follow up sleep
	 * state registration requests will be rejected.
	 */
	HV_SLEEP_STATE_LOCK = 6
};

enum hv_system_property {
	/* Add more values when needed */
	HV_SYSTEM_PROPERTY_SLEEP_STATE = 3,
	HV_SYSTEM_PROPERTY_SCHEDULER_TYPE = 15,
	HV_DYNAMIC_PROCESSOR_FEATURE_PROPERTY = 21,
	HV_SYSTEM_PROPERTY_DIAGOSTICS_LOG_BUFFERS = 28,
};

struct hv_sleep_state_info {
	__u32 sleep_state; /* enum hv_sleep_state */
	__u8 pm1a_slp_typ;
	__u8 pm1b_slp_typ;
} __packed;

enum hv_snp_status {
	HV_SNP_STATUS_NONE = 0,
	HV_SNP_STATUS_AVAILABLE = 1,
	HV_SNP_STATUS_INCOMPATIBLE = 2,
	HV_SNP_STATUS_PSP_UNAVAILABLE = 3,
	HV_SNP_STATUS_PSP_INIT_FAILED = 4,
	HV_SNP_STATUS_PSP_BAD_FW_VERSION = 5,
	HV_SNP_STATUS_BAD_CONFIGURATION = 6,
	HV_SNP_STATUS_PSP_FW_UPDATE_IN_PROGRESS = 7,
	HV_SNP_STATUS_PSP_RB_INIT_FAILED = 8,
	HV_SNP_STATUS_PSP_PLATFORM_STATUS_FAILED = 9,
	HV_SNP_STATUS_PSP_INIT_LATE_FAILED = 10,
};

enum hv_dynamic_processor_feature_property {
	/* Add more values when needed */
	HV_X64_DYNAMIC_PROCESSOR_FEATURE_MAX_ENCRYPTED_PARTITIONS = 13,
	HV_X64_DYNAMIC_PROCESSOR_FEATURE_SNP_STATUS = 16,
};

struct hv_input_get_system_property {
	__u32 property_id; /* enum hv_system_property */
	__u32 reserved;
	union {
		__u64 as_uint64;
#if defined(__x86_64__)
		__u32 hv_processor_feature; /* enum hv_dynamic_processor_feature_property */
#endif
		/* More fields to be filled in when needed */
	};
} __packed;

/* HV_SYSTEM_DIAG_LOG_BUFFER_CONFIG */
struct  hv_system_diag_log_buffer_config {
	__u32 buffer_count;
	__u32 buffer_size_in_pages;
} __packed;

struct hv_output_get_system_property { /* HV_OUTPUT_GET_SYSTEM_PROPERTY */
	union {
		__u32 scheduler_type; /* HV_SCHEDULER_TYPE */
		struct hv_system_diag_log_buffer_config hv_diagbuf_info;
#if defined(__x86_64__)
		__u64 hv_processor_feature_value;
#endif
	};
} __packed;

struct hv_input_set_system_property {
	__u32 property_id; /* enum hv_system_property */
	union {
		/* More fields to be filled in when needed */
		struct hv_sleep_state_info set_sleep_state_info;
	};
} __packed;

struct hv_input_map_stats_page {
	__u32 type; /* enum hv_stats_object_type */
	__u32 padding;
	union hv_stats_object_identity identity;
} __packed;

struct hv_output_map_stats_page {
	__u64 map_location;
} __packed;

struct hv_input_unmap_stats_page {
	__u32 type; /* enum hv_stats_object_type */
	__u32 padding;
	union hv_stats_object_identity identity;
} __packed;



struct hv_proximity_domain_flags {
	__u32 proximity_preferred : 1;
	__u32 reserved : 30;
	__u32 proximity_info_valid : 1;
} __packed;

/* Not a union in windows but useful for zeroing */
union hv_proximity_domain_info {
	struct {
		__u32 domain_id;
		struct hv_proximity_domain_flags flags;
	};
	__u64 as_uint64;
} __packed;

struct hv_input_withdraw_memory {
	__u64 partition_id;
	union hv_proximity_domain_info proximity_domain_info;
} __packed;

struct hv_output_withdraw_memory {
	/* Hack - compiler doesn't like empty array size
	 * in struct with no other members
	 */
	__u64 gpa_page_list[0];
} __packed;

/* HV Map GPA (Guest Physical Address) Flags */
#define HV_MAP_GPA_PERMISSIONS_NONE     0x0
#define HV_MAP_GPA_READABLE             0x1
#define HV_MAP_GPA_WRITABLE             0x2
#define HV_MAP_GPA_KERNEL_EXECUTABLE    0x4
#define HV_MAP_GPA_USER_EXECUTABLE      0x8
#define HV_MAP_GPA_EXECUTABLE           0xC
#define HV_MAP_GPA_PERMISSIONS_MASK     0xF
#define HV_MAP_GPA_ADJUSTABLE           0x8000
#define HV_MAP_GPA_NOT_CACHED           0x200000
#define HV_MAP_GPA_LARGE_PAGE           0x80000000

struct hv_input_map_gpa_pages {
	__u64 target_partition_id;
	__u64 target_gpa_base;
	__u32 map_flags;
	__u32 padding;
	__u64 source_gpa_page_list[];
} __packed;

union hv_gpa_page_access_state_flags {
	struct {
		__u64 clear_accessed : 1;
		__u64 set_access : 1;
		__u64 clear_dirty : 1;
		__u64 set_dirty : 1;
		__u64 reserved : 60;
	} __packed;
	__u64 as_uint64;
};

struct hv_input_get_gpa_pages_access_state {
	__u64  partition_id;
	union hv_gpa_page_access_state_flags flags;
	__u64 hv_gpa_page_number;
} __packed;

union hv_gpa_page_access_state {
	struct {
		__u8 accessed : 1;
		__u8 dirty : 1;
		__u8 reserved: 6;
	};
	__u8 as_uint8;
} __packed;

union hv_snp_guest_policy {
	struct {
		__u64 minor_version : 8;
		__u64 major_version : 8;
		__u64 smt_allowed : 1;
		__u64 vmpls_required : 1;
		__u64 migration_agent_allowed : 1;
		__u64 debug_allowed : 1;
		__u64 reserved : 44;
	} __packed;
	__u64 as_uint64;
};

struct hv_snp_id_block {
	__u8 launch_digest[48];
	__u8 family_id[16];
	__u8 image_id[16];
	__u32 version;
	__u32 guest_svn;
	union hv_snp_guest_policy policy;
} __packed;

struct hv_snp_id_auth_info {
	__u32 id_key_algorithm;
	__u32 auth_key_algorithm;
	__u8 reserved0[56];
	__u8 id_block_signature[512];
	__u8 id_key[1028];
	__u8 reserved1[60];
	__u8 id_key_signature[512];
	__u8 author_key[1028];
} __packed;

struct hv_psp_launch_finish_data {
	struct hv_snp_id_block id_block;
	struct hv_snp_id_auth_info id_auth_info;
	__u8 host_data[32];
	__u8 id_block_enabled;
	__u8 author_key_enabled;
} __packed;

union hv_partition_complete_isolated_import_data {
	__u64 reserved;
	struct hv_psp_launch_finish_data psp_parameters;
} __packed;

struct hv_input_complete_isolated_import {
	__u64 partition_id;
	union hv_partition_complete_isolated_import_data import_data;
} __packed;

enum hv_crashdump_action {
	HV_CRASHDUMP_NONE = 0,
	HV_CRASHDUMP_SUSPEND_ALL_VPS,
	HV_CRASHDUMP_PREPARE_FOR_STATE_SAVE,
	HV_CRASHDUMP_STATE_SAVED,
	HV_CRASHDUMP_ENTRY,
};

struct hv_partition_event_root_crashdump_input {
	__u32 crashdump_action; /* enum hv_crashdump_action */
} __packed;

struct hv_partition_event_commit_processor_indices_input {
	__u32 schedulable_processor_count;
} __packed;

union hv_partition_event_input {
	struct hv_partition_event_root_crashdump_input crashdump_input;
	struct hv_partition_event_commit_processor_indices_input
		commit_lp_indices_input;
};

enum hv_partition_event {
	HV_PARTITION_EVENT_DEBUG_DEVICE_AVAILABLE = 1,
	HV_PARTITION_EVENT_ROOT_CRASHDUMP = 2,
	HV_PARTITION_EVENT_ACPI_REENABLED = 3,
	HV_PARTITION_ALL_LOGICAL_PROCESSORS_STARTED = 4,
	HV_PARTITION_COMMIT_LP_INDICES = 5,
};

struct hv_input_notify_partition_event {
	__u32 event; /* enum hv_partition_event */
	union hv_partition_event_input input;
} __packed;

struct hv_lp_startup_status {
	__u64 hv_status;
	__u64 substatus1;
	__u64 substatus2;
	__u64 substatus3;
	__u64 substatus4;
	__u64 substatus5;
	__u64 substatus6;
} __packed;

struct hv_input_add_logical_processor {
	__u32 lp_index;
	__u32 apic_id;
	union hv_proximity_domain_info proximity_domain_info;
} __packed;

struct hv_output_add_logical_processor {
	struct hv_lp_startup_status startup_status;
} __packed;

/* HV_INPUT_GET_LOGICAL_PROCESSOR_RUN_TIME */
struct hv_input_get_logical_processor_run_time {
	__u32 lp_index;
} __packed;

/* HV_OUTPUT_GET_LOGICAL_PROCESSOR_RUN_TIME */
struct hv_output_get_logical_processor_run_time {
	__u64 global_time;
	__u64 local_run_time;
	__u64 rsvdz0;
	__u64 hypervisor_time;
} __packed;

#endif /* _UAPI_HV_HVHDK_MINI_H */
