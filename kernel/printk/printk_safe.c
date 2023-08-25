// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * printk_safe.c - Safe printk for printk-deadlock-prone contexts
 */

#include <linux/preempt.h>
#include <linux/kdb.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/printk.h>
#include <linux/kprobes.h>

#include "internal.h"

static DEFINE_PER_CPU(int, printk_context);

/* Can be preempted by NMI. */
void __printk_safe_enter(void)
{
	this_cpu_inc(printk_context);
}

/* Can be preempted by NMI. */
void __printk_safe_exit(void)
{
	this_cpu_dec(printk_context);
}

static DEFINE_SPINLOCK(printk_lock);

int hv_sev_printf(const char *fmt, va_list ap)
{
	char buf[1024];
	int len;
	int idx;
	int left;
	unsigned long flags;
	u32 orig_low, orig_high;
	u32 low, high;

	len = vsnprintf(buf, sizeof(buf), fmt, ap);

	local_irq_save(flags);
	spin_lock(&printk_lock);
	asm volatile ("rdmsr" : "=a" (orig_low), "=d" (orig_high) : "c" (MSR_AMD64_SEV_ES_GHCB));
	for (idx = 0; idx < len; idx += 6) {
		left = len - idx;
		if (left > 6) left = 6;
		low = 0xf03;
		high = 0;
		memcpy((char *)&low+2, &buf[idx], left == 1 ? 1 : 2);
		if (left > 2)
			memcpy((char *)&high, &buf[idx+2], left-2);
		asm volatile ("wrmsr\n\r"
				"rep; vmmcall\n\r"
				:: "c" (MSR_AMD64_SEV_ES_GHCB), "a" (low), "d" (high));
	}
	asm volatile ("wrmsr" :: "c" (MSR_AMD64_SEV_ES_GHCB), "a" (orig_low), "d" (orig_high));
	spin_unlock(&printk_lock);
	local_irq_restore(flags);

	return len;
}

void hv_sev_debugbreak(u32 val)
{
	u32 low, high;
	val = ((val & (u32)0xf) << 12) | (u32)0xf03;
	asm volatile ("rdmsr" : "=a" (low), "=d" (high) : "c" (MSR_AMD64_SEV_ES_GHCB));
	asm volatile ("wrmsr\n\r"
		      "rep; vmmcall\n\r"
		      :: "c" (MSR_AMD64_SEV_ES_GHCB), "a" (val), "d" (0x0));
	asm volatile ("wrmsr" :: "c" (MSR_AMD64_SEV_ES_GHCB), "a" (low), "d" (high));
}
EXPORT_SYMBOL_GPL(hv_sev_debugbreak);

asmlinkage int vprintk(const char *fmt, va_list args)
{
	va_list args2;

#ifdef CONFIG_KGDB_KDB
	/* Allow to pass printk() to kdb but avoid a recursion. */
	if (unlikely(kdb_trap_printk && kdb_printf_cpu < 0))
		return vkdb_printf(KDB_MSGSRC_PRINTK, fmt, args);
#endif

	// can't use hv_isolation_type_en_snp() yet, it is not initialized
	if (cc_platform_has(CC_ATTR_GUEST_SEV_SNP)) {
		va_copy(args2, args);
		hv_sev_printf(fmt, args2);
		va_end(args2);
	}

	/*
	 * Use the main logbuf even in NMI. But avoid calling console
	 * drivers that might have their own locks.
	 */
	if (this_cpu_read(printk_context) || in_nmi()) {
		int len;

		len = vprintk_store(0, LOGLEVEL_DEFAULT, NULL, fmt, args);
		defer_console_output();
		return len;
	}

	/* No obstacles. */
	return vprintk_default(fmt, args);
}
EXPORT_SYMBOL(vprintk);
