// SPDX-License-Identifier: GPL-2.0
/*
 * Memory Fault Isolation (MFI) - Page Offline Mechanics
 *
 * Wraps the kernel's page isolation primitives for the MFI core:
 *
 *   Soft offline (proactive, lossless): soft_offline_page() migrates
 *   the page content away and marks the physical page HWPoison so it
 *   is never allocated again. The symbol is not exported to modules,
 *   so it is resolved at runtime via the same kprobe symbol-lookup
 *   technique cfi_panic_suppress.c uses for mce_tolerant.
 *
 *   Hard offline (reactive): memory_failure_queue() is exported (the
 *   GHES driver uses it from module context) and safe to call from
 *   atomic context. Results are observed asynchronously through the
 *   ras:memory_failure_event tracepoint, found at runtime with
 *   for_each_kernel_tracepoint() since its symbol is not exported.
 *
 * The tracepoint probe also sees memory_failure() outcomes initiated
 * elsewhere (GHES, the x86 SRAO path, madvise injection); only PFNs
 * tracked in POISONED state are attributed to MFI, which doubles as
 * dedup against the kernel's own handling.
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/kprobes.h>
#include <linux/tracepoint.h>
#include <linux/slab.h>
#include "cfi_internal.h"
#include "mfi_internal.h"

/* soft_offline_page() resolved at runtime; NULL if unavailable */
static int (*mfi_soft_offline_fn)(unsigned long pfn, int flags);

static struct workqueue_struct *mfi_offline_wq;

static struct tracepoint *mfi_mf_tracepoint;
static bool mfi_mf_probe_registered;

bool mfi_soft_offline_available(void)
{
	return mfi_soft_offline_fn != NULL;
}

bool mfi_page_is_hwpoison(unsigned long pfn)
{
	struct page *page;

	if (!IS_ENABLED(CONFIG_MEMORY_FAILURE))
		return false;

	page = pfn_to_online_page(pfn);
	if (!page)
		return false;
	return PageHWPoison(page);
}

/* --- Soft offline (deferred to process context; may sleep) --- */

struct mfi_offline_work {
	struct work_struct	work;
	unsigned long		pfn;
};

static void mfi_soft_offline_work_fn(struct work_struct *work)
{
	struct mfi_offline_work *ow =
		container_of(work, struct mfi_offline_work, work);
	int ret = -ENOENT;

	if (mfi_soft_offline_fn)
		ret = mfi_soft_offline_fn(ow->pfn, 0);

	if (ret)
		pr_warn("mem: soft_offline_page(0x%lx) failed: %d\n",
			ow->pfn, ret);

	mfi_page_offline_done(ow->pfn, true, ret == 0);
	kfree(ow);
}

bool mfi_page_soft_offline(unsigned long pfn)
{
	struct mfi_offline_work *ow;

	if (!mfi_soft_offline_fn || !mfi_offline_wq)
		return false;

	ow = kmalloc(sizeof(*ow), GFP_ATOMIC);
	if (!ow)
		return false;

	INIT_WORK(&ow->work, mfi_soft_offline_work_fn);
	ow->pfn = pfn;
	queue_work(mfi_offline_wq, &ow->work);
	return true;
}

/* --- Hard offline --- */

void mfi_page_hard_offline(unsigned long pfn)
{
	/*
	 * Safe from atomic context: memory_failure_queue() pushes into a
	 * per-CPU kfifo and schedules its own work. The outcome arrives
	 * via the memory_failure_event tracepoint probe below.
	 */
	memory_failure_queue(pfn, 0);
}

/* --- memory_failure result tracepoint --- */

/* Matches TP_PROTO(unsigned long pfn, int type, int result) */
static void mfi_mf_event_probe(void *ignore, unsigned long pfn, int type,
			       int result)
{
	bool success = (result == MF_RECOVERED || result == MF_DELAYED);

	pr_debug("mem: memory_failure pfn=0x%lx type=%d result=%d\n",
		 pfn, type, result);

	mfi_page_offline_done(pfn, false, success);
}

static void mfi_find_tracepoint(struct tracepoint *tp, void *priv)
{
	if (!strcmp(tp->name, "memory_failure_event"))
		mfi_mf_tracepoint = tp;
}

static int mfi_mf_probe_init(void)
{
	int ret;

	for_each_kernel_tracepoint(mfi_find_tracepoint, NULL);
	if (!mfi_mf_tracepoint) {
		pr_warn("mem: memory_failure_event tracepoint not found; "
			"hard offline results will not be tracked\n");
		return 0;	/* non-fatal */
	}

	ret = tracepoint_probe_register(mfi_mf_tracepoint,
					mfi_mf_event_probe, NULL);
	if (ret) {
		pr_warn("mem: failed to register memory_failure_event probe: %d\n",
			ret);
		return 0;	/* non-fatal */
	}

	mfi_mf_probe_registered = true;
	return 0;
}

/* --- Init / Exit --- */

/* Same kprobe symbol-lookup technique as cfi_panic_suppress.c */
static unsigned long mfi_lookup_name(const char *name)
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

int mfi_page_init(void)
{
	unsigned long addr;

	mfi_offline_wq = alloc_workqueue("mfi_offline", WQ_UNBOUND, 0);
	if (!mfi_offline_wq)
		return -ENOMEM;

	addr = mfi_lookup_name("soft_offline_page");
	if (addr) {
		mfi_soft_offline_fn = (void *)addr;
		pr_info("mem: soft_offline_page resolved, pre-isolation available\n");
	} else {
		pr_warn("mem: soft_offline_page not found; "
			"CE pre-isolation disabled (accounting only)\n");
	}

	mfi_mf_probe_init();
	return 0;
}

void mfi_page_exit(void)
{
	if (mfi_mf_probe_registered) {
		tracepoint_probe_unregister(mfi_mf_tracepoint,
					    mfi_mf_event_probe, NULL);
		tracepoint_synchronize_unregister();
		mfi_mf_probe_registered = false;
	}

	if (mfi_offline_wq) {
		destroy_workqueue(mfi_offline_wq);	/* flushes pending work */
		mfi_offline_wq = NULL;
	}

	mfi_soft_offline_fn = NULL;
}
