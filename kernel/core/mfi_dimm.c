// SPDX-License-Identifier: GPL-2.0
/*
 * Memory Fault Isolation (MFI) - DIMM/Rank Media Accounting
 *
 * Page isolation treats symptoms; a degrading DIMM needs hardware
 * replacement. This component keeps per-DIMM error counters (keyed by
 * the EDAC label from the ras:mc_event tracepoint) and emits a single
 * MEM_MIGRATE_ADVISED netlink event when a DIMM crosses its CE or UCE
 * threshold, advising the daemon to evacuate the host and file repair.
 *
 * MFI takes no host-level action itself: unlike a CPU core, a DIMM
 * cannot be taken offline at runtime, so the media-level decision
 * belongs entirely to the daemon / upper scheduler.
 *
 * The mc_event tracepoint also feeds page-level CE accounting with
 * physical addresses on platforms where EDAC decodes them (notably
 * arm64/GHES, where the CPER memory section arrives via ghes_edac).
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/edac.h>
#include <ras/ras_event.h>
#include "cfi_internal.h"
#include "mfi_internal.h"
#include "mfi_policy.h"

static DEFINE_SPINLOCK(mfi_dimm_lock);
static LIST_HEAD(mfi_dimm_list);
static unsigned int mfi_dimm_count;

static bool mfi_mc_probe_registered;

/* Caller must hold mfi_dimm_lock. */
static struct mfi_dimm_entry *mfi_dimm_get(const char *label)
{
	struct mfi_dimm_entry *d;

	list_for_each_entry(d, &mfi_dimm_list, list) {
		if (!strncmp(d->label, label, MFI_DIMM_LABEL_LEN))
			return d;
	}

	if (mfi_dimm_count >= MFI_DIMM_MAX_ENTRIES)
		return NULL;

	d = kzalloc(sizeof(*d), GFP_ATOMIC);
	if (!d)
		return NULL;

	strscpy(d->label, label, sizeof(d->label));
	d->window_start_ns = ktime_get_ns();
	list_add_tail(&d->list, &mfi_dimm_list);
	mfi_dimm_count++;
	return d;
}

void mfi_dimm_account(const char *label, bool uce)
{
	struct mfi_dimm_entry *d;
	bool advise = false;
	u64 now;
	unsigned long flags;
	char label_copy[MFI_DIMM_LABEL_LEN];

	if (!label || !label[0])
		return;

	spin_lock_irqsave(&mfi_dimm_lock, flags);
	d = mfi_dimm_get(label);
	if (d) {
		now = ktime_get_ns();
		if (mfi_window_expired(now, d->window_start_ns,
				       mfi_window_secs)) {
			d->ce_count = 0;
			d->uce_count = 0;
			d->advised = false;
			d->window_start_ns = now;
		}

		if (uce)
			d->uce_count++;
		else
			d->ce_count++;

		if (!d->advised &&
		    (d->uce_count >= mfi_dimm_uce_threshold ||
		     d->ce_count >= mfi_dimm_ce_threshold)) {
			d->advised = true;
			advise = true;
			strscpy(label_copy, d->label, sizeof(label_copy));
		}
	}
	spin_unlock_irqrestore(&mfi_dimm_lock, flags);

	if (advise) {
		pr_warn("mem: DIMM '%s' crossed error threshold, advising host evacuation\n",
			label_copy);
		cfi_nl_send_mem_migrate_advised(label_copy);
	}
}

/* Render the DIMM table for sysfs (returns bytes written). */
int mfi_dimm_show(char *buf, size_t len)
{
	struct mfi_dimm_entry *d;
	unsigned long flags;
	int pos = 0;

	spin_lock_irqsave(&mfi_dimm_lock, flags);
	list_for_each_entry(d, &mfi_dimm_list, list) {
		pos += scnprintf(buf + pos, len - pos, "%s ce=%u uce=%u%s\n",
				 d->label, d->ce_count, d->uce_count,
				 d->advised ? " [migrate-advised]" : "");
		if (pos >= len - 1)
			break;
	}
	spin_unlock_irqrestore(&mfi_dimm_lock, flags);
	return pos;
}

/*
 * ras:mc_event tracepoint probe. Fired by the EDAC core for every
 * decoded memory-controller error on both x86 (driver EDAC) and arm64
 * (ghes_edac). Provides the DIMM label and, when the driver decodes
 * it, the physical address.
 *
 * Must match TP_PROTO in include/ras/ras_event.h exactly.
 */
static void mfi_mc_event_probe(void *ignore,
			       const unsigned int err_type,
			       const char *error_msg,
			       const char *label,
			       const int error_count,
			       const u8 mc_index,
			       const s8 top_layer,
			       const s8 mid_layer,
			       const s8 low_layer,
			       unsigned long address,
			       const u8 grain_bits,
			       unsigned long syndrome,
			       const char *driver_detail)
{
	struct mfi_mem_error err = {};
	int i;

	switch (err_type) {
	case HW_EVENT_ERR_CORRECTED:
		err.type = MFI_MEM_CE;
		break;
	case HW_EVENT_ERR_UNCORRECTED:
	case HW_EVENT_ERR_FATAL:
		/*
		 * EDAC does not tell us whether the UCE was consumed;
		 * consumption is observed by the arch backend (MCE/SEA).
		 * Treat EDAC-reported UCEs as the async flavor — the
		 * HWPoison dedup in mfi_core avoids double handling.
		 */
		err.type = MFI_MEM_UCE_DEFERRED;
		break;
	default:
		return;		/* informational */
	}

	err.cpu = raw_smp_processor_id();
	err.dimm_label = label;

	if (!address) {
		/* No address decoded: media accounting only */
		mfi_dimm_account(label, err.type != MFI_MEM_CE);
		return;
	}

	err.addr = address;
	err.pfn = address >> PAGE_SHIFT;

	for (i = 0; i < error_count; i++)
		mfi_report_mem_error(&err);
}

int mfi_dimm_init(void)
{
	int ret;

	ret = register_trace_mc_event(mfi_mc_event_probe, NULL);
	if (ret) {
		pr_warn("mem: failed to register mc_event probe: %d "
			"(DIMM accounting disabled)\n", ret);
		return 0;	/* non-fatal: MCE/GHES paths still work */
	}

	mfi_mc_probe_registered = true;
	pr_info("mem: EDAC mc_event probe registered for DIMM accounting\n");
	return 0;
}

void mfi_dimm_exit(void)
{
	struct mfi_dimm_entry *d, *tmp;
	unsigned long flags;

	if (mfi_mc_probe_registered) {
		unregister_trace_mc_event(mfi_mc_event_probe, NULL);
		tracepoint_synchronize_unregister();
		mfi_mc_probe_registered = false;
	}

	spin_lock_irqsave(&mfi_dimm_lock, flags);
	list_for_each_entry_safe(d, tmp, &mfi_dimm_list, list) {
		list_del(&d->list);
		kfree(d);
	}
	mfi_dimm_count = 0;
	spin_unlock_irqrestore(&mfi_dimm_lock, flags);
}
