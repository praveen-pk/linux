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

enum hv_stats_partition_counters {					/* HV_PROCESS_COUNTER */
	PartitionVirtualProcessors		= 1,
	PartitionTlbSize			= 3,
	PartitionAddressSpaces			= 4,
	PartitionDepositedPages			= 5,
	PartitionGpaPages			= 6,
	PartitionGpaSpaceModifications		= 7,
	PartitionVirtualTlbFlushEntires		= 8,
	PartitionRecommendedTlbSize		= 9,
	PartitionGpaPages4K			= 10,
	PartitionGpaPages2M			= 11,
	PartitionGpaPages1G			= 12,
	PartitionGpaPages512G			= 13,
	PartitionDevicePages4K			= 14,
	PartitionDevicePages2M			= 15,
	PartitionDevicePages1G			= 16,
	PartitionDevicePages512G		= 17,
	PartitionAttachedDevices		= 18,
	PartitionDeviceInterruptMappings	= 19,
	PartitionIoTlbFlushes			= 20,
	PartitionIoTlbFlushCost			= 21,
	PartitionDeviceInterruptErrors		= 22,
	PartitionDeviceDmaErrors		= 23,
	PartitionDeviceInterruptThrottleEvents	= 24,
	PartitionSkippedTimerTicks		= 25,
	PartitionPartitionId			= 26,
	PartitionNestedTlbSize			= 27,
	PartitionRecommendedNestedTlbSize	= 28,
	PartitionNestedTlbFreeListSize		= 29,
	PartitionNestedTlbTrimmedPages		= 30,
	PartitionPagesShattered			= 31,
	PartitionPagesRecombined		= 32,
	PartitionHwpRequestValue		= 33,
};

#endif /* _HV_HVHDK_H */
