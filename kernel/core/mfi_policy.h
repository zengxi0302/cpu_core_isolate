/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Memory Fault Isolation (MFI) - Pure Policy Decisions
 *
 * Decision logic with no kernel dependencies, shared between the
 * kernel module and the host-side unit tests (test/unit/). Keep this
 * header free of kernel headers: only fixed-size integers and enums.
 */
#ifndef _MFI_POLICY_H
#define _MFI_POLICY_H

/*
 * Classification of the poisoned page, derived from struct page flags
 * by the kernel side (mfi_triage.c) or supplied directly by tests.
 */
enum mfi_page_class {
	MFI_PG_INVALID = 0,	/* no struct page / not online */
	MFI_PG_USER,		/* LRU, anon, or hugetlb: user/guest data */
	MFI_PG_KERNEL,		/* slab, page table, reserved, kernel data */
	MFI_PG_FREE,		/* in the buddy allocator */
};

enum mfi_triage_verdict {
	MFI_TRIAGE_RECOVER = 0,	/* queue memory_failure, kill owner, go on */
	MFI_TRIAGE_PANIC   = 1,	/* no safe recovery: controlled panic */
};

/*
 * Triage verdict for a UCE consumed in kernel context.
 *
 * Whitelist policy: recovery is only allowed when every condition
 * holds. Anything ambiguous escalates to panic, because continuing
 * past consumed poison without a recovery point is silent corruption.
 *
 * @page_class: classification of the poisoned page
 * @ripv:       MCG_STATUS.RIPV - the interrupted context can resume
 * @pcc:        MCI_STATUS.PCC - processor context corrupt
 */
static inline enum mfi_triage_verdict
mfi_triage_decide(enum mfi_page_class page_class, int ripv, int pcc)
{
	if (pcc)
		return MFI_TRIAGE_PANIC;
	if (!ripv)
		return MFI_TRIAGE_PANIC;

	switch (page_class) {
	case MFI_PG_USER:
		/* Owner can be killed, page can be isolated */
		return MFI_TRIAGE_RECOVER;
	case MFI_PG_FREE:
		/* Nothing consumed it from a live mapping; isolate it */
		return MFI_TRIAGE_RECOVER;
	case MFI_PG_KERNEL:
	case MFI_PG_INVALID:
	default:
		return MFI_TRIAGE_PANIC;
	}
}

/*
 * Sliding-window CE accounting decision: should this page be
 * proactively soft-offlined?
 *
 * @ce_count:   CE count after the current error was added
 * @threshold:  page_ce_threshold
 * @state_watched: page is in WATCHED state (not already in flight)
 * @pre_isolate_enabled: mem_pre_isolate policy switch
 * @mechanism_available: soft_offline_page() was resolved
 */
static inline int
mfi_pre_isolate_decide(unsigned int ce_count, unsigned int threshold,
		       int state_watched, int pre_isolate_enabled,
		       int mechanism_available)
{
	return state_watched && pre_isolate_enabled &&
	       mechanism_available && ce_count >= threshold;
}

/*
 * Window expiry check shared by page and DIMM accounting.
 * Returns 1 when the window must be reset before counting.
 */
static inline int
mfi_window_expired(unsigned long long now_ns,
		   unsigned long long window_start_ns,
		   unsigned int window_secs)
{
	return (now_ns - window_start_ns) >
	       (unsigned long long)window_secs * 1000000000ULL;
}

#endif /* _MFI_POLICY_H */
