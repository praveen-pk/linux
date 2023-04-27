# SPDX-License-Identifier: GPL-2.0-only
obj-y			:= hv_init.o mmu.o nested.o irqdomain.o
CFLAGS_hv_proc.o	= -DHV_HYPERV_DEFS
obj-$(CONFIG_X86_64)	+= hv_apic.o hv_proc.o

ifdef CONFIG_X86_64
obj-$(CONFIG_PARAVIRT_SPINLOCKS)	+= hv_spinlock.o
endif
