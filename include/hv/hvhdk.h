/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _HV_HVHDK_H
#define _HV_HVHDK_H

enum hv_stats_hypervisor_counters {					/* HV_HYPERVISOR_COUNTER */
	HvLogicalProcessors         =  1,
	HvPartitions                =  2,
	HvTotalPages                =  3,
	HvVirtualProcessors         =  4,
	HvMonitoredNotifications    =  5,
	HvModernStandbyEntries      =  6,
	HvPlatformIdleTransitions   =  7,
	HvHypervisorStartupCost     =  8,
	HvIOSpacePages              =  10,
	HvNonEssentialPagesForDump  =  11,
	HvSubsumedPages             =  12,
};

#endif /* _HV_HVHDK_H */
