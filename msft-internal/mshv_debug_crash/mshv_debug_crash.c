// SPDX-License-Identifier: GPL-2.0-only
/*
 * A simple module to crash the system
 *
 * This module is used to test the crash dump mechanism. It invokes the
 * hypervisor test framework to voluntarily crash the system. This only works
 * on a checked build hypervisor.
 *
 * This module is NOT intended to be used in production or upstreamed.
 */

#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/mm.h>
#include <linux/module.h>

#include <asm/mshyperv.h>

/*
 * Search for the exact match of the following definitions in the test
 * framework source code if you're curious.
 *
 * File is onecore/hv/hvx/inc/HvTfApi.h.
 */

#define _TF_COMPONENT_KD_ 2
#define HV_TEST_KD_TRIGGER_EXCEPTION (_TF_COMPONENT_KD_ << 16 | 0x00000001)

/* Call code for the test framework */
#define HVCALL_INVOKE_TEST_FRAMEWORK 0x00CB

/* HV_TF_INPUT_COMMAND */
struct hv_tf_input_command {
	__u32 type; /* 2 TfTypeTestcase */
	__u32 command; /* 2 TfCmdMethod */
} __packed;

/* HV_INPUT_TF_TESTCASE */
struct hv_input_tf_testcase {
	__u32 id;
	__u32 padding;
} __packed;

/* HV_TF_INPUT_KD_TRIGGER_EXCEPTION */
struct hv_tf_input_kd_trigger_exception {
	__u32 exception_type; /* 0 bugcheck, 1 exception, 2 assert */
	__u32 padding;
	__u64 parameter1;
	__u64 parameter2;
	__u64 parameter3;
} __packed;

/* The input buffer for the test framework */
struct input_buffer {
	struct hv_tf_input_command command;
	struct hv_input_tf_testcase testcase;
	struct hv_tf_input_kd_trigger_exception trigger_exception;
} __packed;

/* HV_INPUT_INVOKE_TF */
struct hv_input_invoke_tf {
	__u64 input_buffer_gva;
	__u64 output_buffer_gva;
	__u32 input_buffer_size;
	__u32 output_buffer_size;
} __packed;

static void try_crash(void)
{
	struct hv_input_invoke_tf *input;
	struct input_buffer *buffer, b = { 0 };
	unsigned long flags;
	u64 status;

	/*
	 * Construct the input buffer for the test framework. See
	 * DbgHvCrashTests::CrashHypervisor.
	 */
	buffer = &b;
	buffer->command.type = 2; /* TfTypeTestcase */;
	buffer->command.command = 2; /* TfCmdMethod */
	buffer->testcase.id = HV_TEST_KD_TRIGGER_EXCEPTION;
	buffer->trigger_exception.exception_type = 0; /* bugcheck */
	buffer->trigger_exception.parameter1 = 0xffff;

	pr_err("mshv_debug_crash: crashing the system 0xdeadbeef\n");

	/* See TfpInvokeTestFrameworkHyperCall */
	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);

	memset(input, 0, sizeof(*input));

	input->input_buffer_gva = (__u64)buffer;
	input->input_buffer_size = sizeof(*buffer);

	status = hv_do_hypercall(HVCALL_INVOKE_TEST_FRAMEWORK, input, NULL);

	local_irq_restore(flags);

	pr_err("mshv_debug_crash: failed to crash the hypervisor 0xbadc0ffee\n");
	pr_err("mshv_debug_crash: hypercall status %s\n", hv_status_to_string(hv_result(status)));
}

static int __init mshv_debug_crash_init(void)
{
	if (!hv_is_hyperv_initialized())
		return -ENODEV;

	try_crash();

	/* Failed to crash the system for some reason, just retun an error. */
	return -EIO;
}

module_init(mshv_debug_crash_init);

MODULE_AUTHOR("Microsoft");
MODULE_LICENSE("GPL");
