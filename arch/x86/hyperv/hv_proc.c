// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/clockchips.h>
#include <linux/hyperv.h>
#include <linux/slab.h>
#include <linux/cpuhotplug.h>
#include <linux/minmax.h>
#include <asm/hypervisor.h>
#include <asm/mshyperv.h>
#include <asm/apic.h>

#include <asm/trace/hyperv.h>
#include <asm-generic/hyperv-defs.h>

#define HV_SET_REGISTER_BATCH_SIZE	\
	((HV_HYP_PAGE_SIZE - sizeof(struct hv_input_set_vp_registers)) \
		/ sizeof(struct hv_register_assoc))

int hv_call_add_logical_proc(int node, u32 lp_index, u32 apic_id)
{
	struct hv_input_add_logical_processor *input;
	struct hv_output_add_logical_processor *output;
	u64 status;
	unsigned long flags;
	int ret = HV_STATUS_SUCCESS;

	/*
	 * When adding a logical processor, the hypervisor may return
	 * HV_STATUS_INSUFFICIENT_MEMORY. When that happens, we deposit more
	 * pages and retry.
	 */
	do {
		local_irq_save(flags);

		input = *this_cpu_ptr(hyperv_pcpu_input_arg);
		/* We don't do anything with the output right now */
		output = *this_cpu_ptr(hyperv_pcpu_output_arg);

		input->lp_index = lp_index;
		input->apic_id = apic_id;
		input->proximity_domain_info =
			numa_node_to_proximity_domain_info(node);
		status = hv_do_hypercall(HVCALL_ADD_LOGICAL_PROCESSOR,
					 input, output);
		local_irq_restore(flags);

		if (hv_result(status) != HV_STATUS_INSUFFICIENT_MEMORY) {
			if (!hv_result_success(status)) {
				pr_err("%s: cpu %u apic ID %u, %s\n", __func__,
				       lp_index, apic_id, hv_status_to_string(status));
				ret = hv_status_to_errno(status);
			}
			break;
		}
		ret = hv_call_deposit_pages(node, hv_current_partition_id, 1);
	} while (!ret);

	return ret;
}

int hv_call_notify_all_processors_started(void)
{
	struct hv_input_notify_partition_event *input;
	u64 status;
	unsigned long irq_flags;

	local_irq_save(irq_flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	input->event = HV_PARTITION_ALL_LOGICAL_PROCESSORS_STARTED;

	status = hv_do_hypercall(HVCALL_NOTIFY_PARTITION_EVENT, input, NULL);

	local_irq_restore(irq_flags);

	if (!hv_result_success(status)) {
		pr_err("%s: Failed to notify all processors started, %s\n",
		       __func__, hv_status_to_string(status));
	}

	return hv_status_to_errno(status);
}

int hv_call_set_vp_registers(
		u32 vp_index,
		u64 partition_id,
		u16 count,
		union hv_input_vtl input_vtl,
		struct hv_register_assoc *registers)
{
	struct hv_input_set_vp_registers *input_page;
	u16 completed;
	unsigned long remaining = count;
	int rep_count;
	u64 status;
	unsigned long flags;

	local_irq_save(flags);
	input_page = *this_cpu_ptr(hyperv_pcpu_input_arg);

	input_page->partition_id = partition_id;
	input_page->vp_index = vp_index;
	input_page->input_vtl.as_uint8 = input_vtl.as_uint8;
	input_page->rsvd_z8 = 0;
	input_page->rsvd_z16 = 0;

	while (remaining) {
		rep_count = min(remaining, HV_SET_REGISTER_BATCH_SIZE);
		memcpy(input_page->elements, registers,
			sizeof(struct hv_register_assoc) * rep_count);

		status = hv_do_rep_hypercall(HVCALL_SET_VP_REGISTERS, rep_count,
					     0, input_page, NULL);
		if (!hv_result_success(status)) {
			pr_err("%s: completed %li out of %u, %s\n",
			       __func__,
			       count - remaining, count,
			       hv_status_to_string(status));
			break;
		}
		completed = hv_repcomp(status);
		registers += completed;
		remaining -= completed;
	}

	local_irq_restore(flags);

	return hv_status_to_errno(status);
}
EXPORT_SYMBOL_GPL(hv_call_set_vp_registers);

int hv_set_sev_control_register(u32 vp_index, u64 partition_id,
				u64 enable_encrypted_state,
				u64 vmsa_gpa_page_number)
{
	union hv_input_vtl input_vtl;
	struct hv_register_assoc sev_control = {
		.name = HV_X64_REGISTER_SEV_CONTROL,
	};
	union hv_x64_register_sev_control *sc;

	sc = &sev_control.value.sev_control;
	sc->enable_encrypted_state = enable_encrypted_state;
	sc->vmsa_gpa_page_number = vmsa_gpa_page_number;

	input_vtl.as_uint8 = 0;
	return hv_call_set_vp_registers(vp_index, partition_id, 1, input_vtl,
					&sev_control);
}
EXPORT_SYMBOL_GPL(hv_set_sev_control_register);

bool hv_lp_exists(u32 lp_index)
{
	struct hv_input_get_logical_processor_run_time *input;
	struct hv_output_get_logical_processor_run_time *out_page;
	unsigned long flags;
	u64 status;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	out_page = *this_cpu_ptr(hyperv_pcpu_output_arg);

	input->lp_index = lp_index;
	status = hv_do_hypercall(HVCALL_GET_LOGICAL_PROCESSOR_RUN_TIME, input,
			out_page);

	local_irq_restore(flags);

	/*
	 * This method is called early in boot before adding the LPs.
	 *
	 * HV_STATUS_SUCCESS and HV_STATUS_INVALID_LP_INDEX are the only
	 * expected return codes here. Anything else means the system is
	 * in some sort of an indeterminate state and we can't say for sure
	 * whether the LP is added or not.
	 */
	if (status != HV_STATUS_SUCCESS && status != HV_STATUS_INVALID_LP_INDEX) {
		pr_err("%s: unexpected status %llu\n", __func__, status);
		BUG();
	}

	return hv_result_success(status);
}
