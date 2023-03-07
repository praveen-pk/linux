/* SPDX-License-Identifier: GPL-2.0 */

/*
 * This file contains definitions from the Hyper-V Hypervisor Top-Level
 * Functional Specification (TLFS):
 * https://docs.microsoft.com/en-us/virtualization/hyper-v-on-windows/reference/tlfs
 *
 * Copyright (C) 2021, Microsoft, Inc.
 *
 * Author : Michael Kelley <mikelley@microsoft.com>
 */

#ifndef _ASM_HYPERV_TLFS_H
#define _ASM_HYPERV_TLFS_H

#include <linux/types.h>
#include <asm-generic/hyperv-common-types.h>

/*
 * All data structures defined in the TLFS that are shared between Hyper-V
 * and a guest VM use Little Endian byte ordering.  This matches the default
 * byte ordering of Linux running on ARM64, so no special handling is required.
 */

/*
 * These Hyper-V registers provide information equivalent to the CPUID
 * instruction on x86/x64.
 */
#define HV_REGISTER_FEATURES			0x00000200 /*CPUID 0x40000003 */
#define HV_REGISTER_ENLIGHTENMENTS		0x00000201 /*CPUID 0x40000004 */

/*
 * Group C Features. See the asm-generic version of hyperv-tlfs.h
 * for a description of Feature Groups.
 */

/* Crash MSRs available */
#define HV_FEATURE_GUEST_CRASH_MSR_AVAILABLE	BIT(8)

/* STIMER direct mode is available */
#define HV_STIMER_DIRECT_MODE_AVAILABLE		BIT(13)

/*
 * Synthetic register definitions name differently in hyperv-common-types.h
 */
#define HV_REGISTER_SIMP		HV_REGISTER_SIPP
#define HV_REGISTER_SIEFP		HV_REGISTER_SIFP

/*
 * To support non-arch-specific code calling hv_set/get_register:
 * - On x86,   HV_SYN_REG_ indicates an MSR accessed via rdmsrl/wrmsrl
 * - On ARM64, HV_SYN_REG_ indicates a VP register accessed via hypercall
 */
#define HV_SYN_REG_VP_INDEX		(HV_REGISTER_VP_INDEX)
#define HV_SYN_REG_TIME_REF_COUNT	(HV_REGISTER_TIME_REF_COUNT)
#define HV_SYN_REG_REFERENCE_TSC	(HV_REGISTER_REFERENCE_TSC)
#define HV_SYN_REG_STIMER0_CONFIG	(HV_REGISTER_STIMER0_CONFIG)
#define HV_SYN_REG_STIMER0_COUNT	(HV_REGISTER_STIMER0_COUNT)

#define HV_SYN_REG_SCONTROL		(HV_REGISTER_SCONTROL)
#define HV_SYN_REG_SIEFP		(HV_REGISTER_SIFP)
#define HV_SYN_REG_SIMP			(HV_REGISTER_SIMP)
#define HV_SYN_REG_SIRBP		(HV_REGISTER_SIRBP)
#define HV_SYN_REG_EOM			(HV_REGISTER_EOM)
#define HV_SYN_REG_SINT0		(HV_REGISTER_SINT0)

#define HV_SYN_REG_CRASH_P0		(HV_REGISTER_GUEST_CRASH_P0)
#define HV_SYN_REG_CRASH_P1		(HV_REGISTER_GUEST_CRASH_P1)
#define HV_SYN_REG_CRASH_P2		(HV_REGISTER_GUEST_CRASH_P2)
#define HV_SYN_REG_CRASH_P3		(HV_REGISTER_GUEST_CRASH_P3)
#define HV_SYN_REG_CRASH_P4		(HV_REGISTER_GUEST_CRASH_P4)
#define HV_SYN_REG_CRASH_CTL		(HV_REGISTER_GUEST_CRASH_CTL)

union hv_msi_entry {
	u64 as_uint64[2];
	struct {
		u64 address;
		u32 data;
		u32 reserved;
	} __packed;
};

#include <uapi/asm/hyperv-tlfs.h>
#include <asm-generic/hyperv-tlfs.h>

#endif
