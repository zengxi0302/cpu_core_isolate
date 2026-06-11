// SPDX-License-Identifier: GPL-2.0
/*
 * Memory Fault Isolation (MFI) - Kernel-Context UCE Triage (Phase 2)
 *
 * With CFI's panic suppression active, a UCE consumed in kernel
 * context no longer reaches mce_panic(). That is only safe if someone
 * restores a correct verdict: either the poisoned page belongs to a
 * killable owner (user/guest data) and can be isolated, or the kernel
 * itself consumed corrupt data and the machine must come down NOW —
 * silently continuing is worse than the crash we suppressed.
 *
 * Whitelist policy (see mfi_policy.h for the pure decision):
 *   RECOVER only when PCC=0, RIPV=1 and the page is user/guest data
 *   (LRU / anon / hugetlb) or free. Everything else panics, which with
 *   kdump configured preserves the crash dump the suppressed panic
 *   would have produced.
 *
 * Page classification only reads struct page metadata (vmemmap), never
 * the poisoned page itself, and is careful about racing with frees:
 * the result is advisory and memory_failure() re-validates under
 * proper locks. A misclassification can only turn RECOVER into a
 * failed recovery (reported via MEM_PAGE_FAILED), never corrupt data.
 *
 * Gated by mem_triage=0 by default; enable per-fleet after grayscale
 * validation. x86 only in this phase (arm64 lacks an RIPV equivalent;
 * see the design document).
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/panic.h>
#include "cfi_internal.h"
#include "mfi_internal.h"
#include "mfi_policy.h"

static enum mfi_page_class mfi_classify_page(unsigned long pfn)
{
	struct page *page;
	struct folio *folio;

	page = pfn_to_online_page(pfn);
	if (!page)
		return MFI_PG_INVALID;

	if (PageReserved(page) || PageTable(page) || PageSlab(page))
		return MFI_PG_KERNEL;

	if (PageBuddy(page))
		return MFI_PG_FREE;

	folio = page_folio(page);
	if (folio_test_lru(folio) || folio_test_anon(folio) ||
	    folio_test_hugetlb(folio))
		return MFI_PG_USER;

	/* Unrecognized usage: treat as kernel data (conservative) */
	return MFI_PG_KERNEL;
}

static const char * const mfi_page_class_names[] = {
	[MFI_PG_INVALID]	= "invalid",
	[MFI_PG_USER]		= "user/guest",
	[MFI_PG_KERNEL]		= "kernel",
	[MFI_PG_FREE]		= "free",
};

/*
 * Triage a UCE consumed in kernel context.
 * Returns true when recovery was queued (caller tags the event
 * MFI_EVF_TRIAGED); does not return when the verdict is panic.
 */
bool mfi_triage_kernel_uce(unsigned long pfn, u64 ripv, u64 pcc,
			   unsigned int cpu)
{
	enum mfi_page_class class;
	struct mfi_mem_event ev = {};

	class = mfi_classify_page(pfn);

	if (mfi_triage_decide(class, !!ripv, !!pcc) == MFI_TRIAGE_PANIC) {
		atomic64_inc(&mfi_stats.triage_panic);
		pr_emerg("mem: UNRECOVERABLE kernel-context UCE: pfn=0x%lx class=%s ripv=%d pcc=%d cpu=%u\n",
			 pfn, mfi_page_class_names[class], (int)!!ripv,
			 (int)!!pcc, cpu);
		pr_emerg("mem: escalating to controlled panic (kdump will capture state)\n");
		panic("MFI: unrecoverable kernel memory UCE at pfn 0x%lx (%s page)",
		      pfn, mfi_page_class_names[class]);
		/* unreachable */
	}

	atomic64_inc(&mfi_stats.triage_saved);
	pr_warn("mem: triage recovering kernel-context UCE: pfn=0x%lx class=%s cpu=%u — isolating page, owner will be killed\n",
		pfn, mfi_page_class_names[class], cpu);

	mfi_page_hard_offline(pfn);

	/*
	 * The owning task is killed asynchronously by memory_failure()
	 * via rmap; we cannot name it from here (the decode chain runs
	 * in a kworker). The daemon resolves pfn -> VM and rebuilds the
	 * guest elsewhere.
	 */
	ev.pfn = pfn;
	ev.cpu = cpu;
	ev.err_type = MFI_MEM_UCE_CONSUMED;
	ev.page_state = MFI_PAGE_POISONED;
	ev.flags = MFI_EVF_KERNEL_CTX | MFI_EVF_TRIAGED;
	ev.timestamp_ns = ktime_get_ns();
	cfi_nl_send_mem_event(CFI_CMD_MEM_VM_KILLED, &ev);

	return true;
}
