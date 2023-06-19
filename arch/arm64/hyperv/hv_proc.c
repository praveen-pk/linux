// SPDX-License-Identifier: GPL-2.0

#include <asm/mshyperv.h>
#include <asm-generic/hyperv-defs.h>

int hv_call_set_vp_registers(u32 vp_index, u64 partition_id, u16 count,
			     union hv_input_vtl input_vtl,
			     struct hv_register_assoc *registers)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(hv_call_set_vp_registers);

int hv_set_sev_control_register(u32 vp_index, u64 partition_id,
				u64 sev_control_val)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(hv_set_sev_control_register);
