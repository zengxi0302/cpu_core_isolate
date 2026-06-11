// SPDX-License-Identifier: GPL-2.0
/*
 * Memory Fault Isolation (MFI) - Page Accounting and State Machine
 *
 * Tracks per-page corrected error (CE) counts in a bounded hash table
 * and drives the page state machine:
 *
 *   WATCHED --[CE >= page_ce_threshold]--> PRE_ISO --> OFFLINED
 *                                                  \-> PRE_ISO_FAILED
 *   WATCHED --[async UCE]--> POISONED --> OFFLINED | FAILED
 *
 * Unlike the CPU domain there is no daemon-ACK handshake: poisoned
 * pages are isolated as fast as possible and the daemon only reacts
 * to the resulting events.
 *
 * Entry points:
 *   mfi_report_mem_error() - from arch backends / tracepoints / debugfs
 *   mfi_page_offline_done() - from mfi_page.c when offline concludes
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/kprobes.h>
#include "cfi_internal.h"
#include "mfi_internal.h"
#include "mfi_policy.h"

struct mfi_stats mfi_stats;

static DEFINE_SPINLOCK(mfi_page_lock);
static DEFINE_HASHTABLE(mfi_page_hash, MFI_PAGE_HASH_BITS);
static LIST_HEAD(mfi_page_lru);		/* least-recent at head */
static unsigned int mfi_page_count;

static const char * const mfi_page_state_names[] = {
	[MFI_PAGE_WATCHED]		= "watched",
	[MFI_PAGE_PRE_ISO]		= "pre_isolating",
	[MFI_PAGE_PRE_ISO_FAILED]	= "pre_isolate_failed",
	[MFI_PAGE_POISONED]		= "poisoned",
	[MFI_PAGE_OFFLINED]		= "offlined",
	[MFI_PAGE_FAILED]		= "offline_failed",
};

const char *mfi_page_state_name(enum mfi_page_state state)
{
	if (state < ARRAY_SIZE(mfi_page_state_names) &&
	    mfi_page_state_names[state])
		return mfi_page_state_names[state];
	return "unknown";
}

u64 mfi_pages_watched(void)
{
	return READ_ONCE(mfi_page_count);
}

void mfi_stats_snapshot(struct mfi_stats_rec *rec)
{
	rec->ce_total		= atomic64_read(&mfi_stats.ce_total);
	rec->uce_async		= atomic64_read(&mfi_stats.uce_async);
	rec->uce_consumed	= atomic64_read(&mfi_stats.uce_consumed);
	rec->pages_watched	= mfi_pages_watched();
	rec->pages_pre_offlined	= atomic64_read(&mfi_stats.pages_pre_offlined);
	rec->pages_offlined	= atomic64_read(&mfi_stats.pages_offlined);
	rec->pages_failed	= atomic64_read(&mfi_stats.pages_failed);
	rec->triage_saved	= atomic64_read(&mfi_stats.triage_saved);
	rec->triage_panic	= atomic64_read(&mfi_stats.triage_panic);
}

/* Caller must hold mfi_page_lock. */
static struct mfi_page_entry *mfi_page_lookup(unsigned long pfn)
{
	struct mfi_page_entry *e;

	hash_for_each_possible(mfi_page_hash, e, hash, pfn) {
		if (e->pfn == pfn)
			return e;
	}
	return NULL;
}

/*
 * Get or create the tracking entry for a PFN, evicting the LRU entry
 * if at capacity. Caller must hold mfi_page_lock.
 * Entries in flight (PRE_ISO/POISONED) are never evicted.
 */
static struct mfi_page_entry *mfi_page_get(unsigned long pfn)
{
	struct mfi_page_entry *e;

	e = mfi_page_lookup(pfn);
	if (e) {
		list_move_tail(&e->lru, &mfi_page_lru);
		return e;
	}

	if (mfi_page_count >= MFI_PAGE_MAX_ENTRIES) {
		struct mfi_page_entry *victim;

		list_for_each_entry(victim, &mfi_page_lru, lru) {
			if (victim->state != MFI_PAGE_PRE_ISO &&
			    victim->state != MFI_PAGE_POISONED) {
				hash_del(&victim->hash);
				list_del(&victim->lru);
				mfi_page_count--;
				e = victim;
				break;
			}
		}
		if (!e)
			return NULL;	/* everything in flight; drop event */
	} else {
		e = kzalloc(sizeof(*e), GFP_ATOMIC);
		if (!e)
			return NULL;
	}

	memset(e, 0, sizeof(*e));
	e->pfn = pfn;
	e->state = MFI_PAGE_WATCHED;
	e->window_start_ns = ktime_get_ns();
	hash_add(mfi_page_hash, &e->hash, pfn);
	list_add_tail(&e->lru, &mfi_page_lru);
	mfi_page_count++;
	return e;
}

/* Caller must hold mfi_page_lock. */
static void mfi_page_remove(struct mfi_page_entry *e)
{
	hash_del(&e->hash);
	list_del(&e->lru);
	mfi_page_count--;
	kfree(e);
}

/*
 * Build the userspace event record from an internal error descriptor.
 * Captures the current task when the error was consumed in-context so
 * the daemon can map a killed task back to a VM.
 */
static void mfi_fill_event(struct mfi_mem_event *ev,
			   const struct mfi_mem_error *err,
			   enum mfi_page_state state, u32 ce_count)
{
	memset(ev, 0, sizeof(*ev));
	ev->pfn = err->pfn;
	ev->addr = err->addr;
	ev->cpu = err->cpu;
	ev->err_type = err->type;
	ev->page_state = state;
	ev->flags = err->flags;
	ev->ce_count = ce_count;
	ev->timestamp_ns = ktime_get_ns();
	/*
	 * Attribute the interrupted task only when it is plausibly the
	 * victim: the decode chain runs in a kworker (no mm), where
	 * current is meaningless.
	 */
	if (err->type == MFI_MEM_UCE_CONSUMED && !in_interrupt() &&
	    current->mm) {
		ev->pid = current->pid;
		strscpy(ev->comm, current->comm, sizeof(ev->comm));
	}
	if (err->dimm_label)
		strscpy(ev->dimm_label, err->dimm_label,
			sizeof(ev->dimm_label));
}

/*
 * Handle a corrected error on a page: count it within the sliding
 * window and queue proactive soft offline once the threshold is hit.
 */
static void mfi_handle_ce(const struct mfi_mem_error *err)
{
	struct mfi_mem_event ev;
	struct mfi_page_entry *e;
	enum mfi_page_state state = MFI_PAGE_WATCHED;
	bool offline = false;
	u64 now;
	u32 ce_count = 1;
	unsigned long flags;

	atomic64_inc(&mfi_stats.ce_total);

	spin_lock_irqsave(&mfi_page_lock, flags);
	e = mfi_page_get(err->pfn);
	if (e) {
		now = ktime_get_ns();
		if (mfi_window_expired(now, e->window_start_ns,
				       mfi_window_secs)) {
			e->ce_count = 0;
			e->window_start_ns = now;
		}
		e->ce_count++;
		e->last_seen_ns = now;
		ce_count = e->ce_count;

		if (mfi_pre_isolate_decide(e->ce_count, mfi_page_ce_threshold,
					   e->state == MFI_PAGE_WATCHED,
					   mfi_pre_isolate,
					   mfi_soft_offline_available())) {
			e->state = MFI_PAGE_PRE_ISO;
			offline = true;
		}
		state = e->state;
	}
	spin_unlock_irqrestore(&mfi_page_lock, flags);

	mfi_fill_event(&ev, err, state, ce_count);
	cfi_nl_send_mem_event(CFI_CMD_MEM_ERROR_EVENT, &ev);

	if (offline) {
		pr_info("mem: pfn 0x%lx hit %u CEs in window, queueing soft offline\n",
			err->pfn, ce_count);
		if (!mfi_page_soft_offline(err->pfn))
			mfi_page_offline_done(err->pfn, true, false);
	}
}

/*
 * Handle an uncorrected error. Async (not yet consumed) UCEs are the
 * clean isolation window: queue memory_failure() unless the kernel has
 * already poisoned the page. Consumed UCEs in user context are killed
 * by the kernel's own recovery; we account and report. Consumed UCEs
 * in kernel context go to the Phase-2 triage path (if enabled).
 */
static void mfi_handle_uce(const struct mfi_mem_error *err)
{
	struct mfi_mem_event ev;
	struct mfi_page_entry *e;
	enum mfi_page_state state = MFI_PAGE_POISONED;
	bool already_poisoned;
	bool queue_offline = false;
	unsigned long flags;
	u32 ce_count = 0;

	if (err->type == MFI_MEM_UCE_DEFERRED)
		atomic64_inc(&mfi_stats.uce_async);
	else
		atomic64_inc(&mfi_stats.uce_consumed);

	/*
	 * Dedup against the kernel's own handling (GHES, x86 SRAO path,
	 * CEC): if the page already carries HWPoison we only account.
	 */
	already_poisoned = mfi_page_is_hwpoison(err->pfn);

	spin_lock_irqsave(&mfi_page_lock, flags);
	e = mfi_page_get(err->pfn);
	if (e) {
		ce_count = e->ce_count;
		e->last_seen_ns = ktime_get_ns();
		if (e->state == MFI_PAGE_WATCHED ||
		    e->state == MFI_PAGE_PRE_ISO_FAILED) {
			e->state = MFI_PAGE_POISONED;
			queue_offline = !already_poisoned;
		}
		state = e->state;
	} else {
		queue_offline = !already_poisoned;
	}
	spin_unlock_irqrestore(&mfi_page_lock, flags);

	mfi_fill_event(&ev, err, state, ce_count);
	cfi_nl_send_mem_event(CFI_CMD_MEM_ERROR_EVENT, &ev);

	if (already_poisoned) {
		pr_info_ratelimited("mem: pfn 0x%lx UCE, page already hwpoisoned (handled elsewhere)\n",
				    err->pfn);
		return;
	}

	/* Triage already queued recovery for this page */
	if (err->flags & MFI_EVF_TRIAGED)
		return;

	if (queue_offline) {
		pr_warn("mem: pfn 0x%lx %s UCE, queueing memory_failure\n",
			err->pfn,
			err->type == MFI_MEM_UCE_DEFERRED ? "async" : "consumed");
		mfi_page_hard_offline(err->pfn);
	}
}

/*
 * Main entry point for memory error reports from all sources.
 * Sources may run in atomic context (tracepoints, MCE decode chain
 * work); everything heavy is deferred to workqueues by mfi_page.c.
 */
void mfi_report_mem_error(const struct mfi_mem_error *err)
{
	if (!mfi_enable)
		return;

	if (!pfn_valid(err->pfn)) {
		pr_warn_ratelimited("mem: error report with invalid pfn 0x%lx\n",
				    err->pfn);
		return;
	}

	if (err->dimm_label)
		mfi_dimm_account(err->dimm_label, err->type != MFI_MEM_CE);

	if (err->type == MFI_MEM_CE)
		mfi_handle_ce(err);
	else
		mfi_handle_uce(err);
}

/*
 * Completion callback from mfi_page.c. Updates the entry state, emits
 * the result event, and drops OFFLINED entries from the table (the
 * kernel's HWPoison flag is now the source of truth).
 */
void mfi_page_offline_done(unsigned long pfn, bool pre_isolate, bool success)
{
	struct mfi_mem_event ev = {};
	struct mfi_page_entry *e;
	enum mfi_page_state state;
	unsigned long flags;
	u32 ce_count = 0;

	if (success)
		state = MFI_PAGE_OFFLINED;
	else
		state = pre_isolate ? MFI_PAGE_PRE_ISO_FAILED : MFI_PAGE_FAILED;

	spin_lock_irqsave(&mfi_page_lock, flags);
	e = mfi_page_lookup(pfn);
	if (e) {
		ce_count = e->ce_count;
		if (success)
			mfi_page_remove(e);
		else
			e->state = state;
	}
	spin_unlock_irqrestore(&mfi_page_lock, flags);

	if (success) {
		if (pre_isolate)
			atomic64_inc(&mfi_stats.pages_pre_offlined);
		else
			atomic64_inc(&mfi_stats.pages_offlined);
	} else {
		atomic64_inc(&mfi_stats.pages_failed);
	}

	pr_info("mem: pfn 0x%lx %s offline %s\n", pfn,
		pre_isolate ? "soft" : "hard",
		success ? "succeeded" : "FAILED (page still live)");

	ev.pfn = pfn;
	ev.page_state = state;
	ev.ce_count = ce_count;
	ev.flags = pre_isolate ? MFI_EVF_PRE_ISOLATE : 0;
	ev.timestamp_ns = ktime_get_ns();
	cfi_nl_send_mem_event(success ? CFI_CMD_MEM_PAGE_OFFLINED :
				        CFI_CMD_MEM_PAGE_FAILED, &ev);
}

int mfi_core_init(void)
{
	hash_init(mfi_page_hash);
	return 0;
}

void mfi_core_exit(void)
{
	struct mfi_page_entry *e, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&mfi_page_lock, flags);
	list_for_each_entry_safe(e, tmp, &mfi_page_lru, lru)
		mfi_page_remove(e);
	spin_unlock_irqrestore(&mfi_page_lock, flags);
}

/* --- Whole-domain init/exit --- */

/* Same kprobe symbol-lookup technique as cfi_panic_suppress.c */
static unsigned long mfi_core_lookup_name(const char *name)
{
	struct kprobe kp = {};
	unsigned long addr;

	kp.symbol_name = name;
	if (register_kprobe(&kp) < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

/*
 * The kernel's Correctable Error Collector (CONFIG_RAS_CEC) already
 * soft-offlines CE-heavy pages on x86. When it is present, default to
 * accounting-only so the two mechanisms do not race on the same pages;
 * operators can re-enable via /sys/kernel/cfi/mem/pre_isolate.
 */
static void mfi_detect_cec(void)
{
	if (mfi_core_lookup_name("cec_add_elem")) {
		if (mfi_pre_isolate) {
			mfi_pre_isolate = false;
			pr_info("mem: RAS CEC detected, pre-isolation downgraded to accounting-only\n");
		}
	}
}

/* Set when init completed; mem_enable may be toggled at runtime */
static bool mfi_active;

int mfi_init(void)
{
	int ret;

	if (!mfi_enable) {
		pr_info("mem: memory fault domain disabled (mem_enable=0)\n");
		return 0;
	}

	ret = mfi_core_init();
	if (ret)
		return ret;

	ret = mfi_page_init();
	if (ret)
		goto err_core;

	mfi_detect_cec();

	ret = mfi_dimm_init();
	if (ret)
		goto err_page;

	ret = mfi_sysfs_init(cfi_sysfs_root());
	if (ret) {
		pr_warn("mem: sysfs init failed: %d (continuing without)\n",
			ret);
		ret = 0;	/* non-fatal */
	}

#ifdef CONFIG_X86
	ret = mfi_x86_init();
	if (ret)
		goto err_sysfs;
#endif
	/*
	 * arm64 needs no dedicated backend in Phase 1: memory errors
	 * arrive via the common EDAC mc_event probe (ghes_edac) and the
	 * memory_failure_event probe; GHES itself already queues
	 * memory_failure() for poisoned pages.
	 */

	mfi_active = true;
	pr_info("mem: memory fault domain active (pre_isolate=%d triage=%d "
		"page_ce_thresh=%u window=%us)\n",
		mfi_pre_isolate, mfi_triage, mfi_page_ce_threshold,
		mfi_window_secs);
	return 0;

#ifdef CONFIG_X86
err_sysfs:
	mfi_sysfs_exit();
	mfi_dimm_exit();
#endif
err_page:
	mfi_page_exit();
err_core:
	mfi_core_exit();
	return ret;
}

void mfi_exit(void)
{
	if (!mfi_active)
		return;
	mfi_active = false;

#ifdef CONFIG_X86
	mfi_x86_exit();
#endif
	mfi_sysfs_exit();
	mfi_dimm_exit();
	mfi_page_exit();
	mfi_core_exit();
}
