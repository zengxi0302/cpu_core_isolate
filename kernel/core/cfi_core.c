// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - Core Error Accounting and State Machine
 *
 * Central logic for receiving architecture-agnostic error records,
 * maintaining per-CPU error counters within a sliding time window,
 * and making isolation decisions based on configurable thresholds.
 *
 * State machine:
 *   ONLINE --[CE >= threshold]--> DEGRADED
 *   DEGRADED --[UCE >= threshold]--> ISOLATING --> ISOLATED | FAILED
 *   ONLINE --[UCE >= threshold]--> ISOLATING --> ISOLATED | FAILED
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include "cfi_internal.h"

static const char * const cfi_state_names[] = {
	[CFI_STATE_ONLINE]	= "online",
	[CFI_STATE_DEGRADED]	= "degraded",
	[CFI_STATE_ISOLATING]	= "isolating",
	[CFI_STATE_ISOLATED]	= "isolated",
	[CFI_STATE_FAILED]	= "failed",
};

const char *cfi_state_name(enum cfi_cpu_state state)
{
	if (state < ARRAY_SIZE(cfi_state_names) && cfi_state_names[state])
		return cfi_state_names[state];
	return "unknown";
}

/*
 * Reset the error counters for a new counting window.
 * Caller must hold ci->lock.
 */
void cfi_reset_cpu_window(struct cfi_cpu_info *ci)
{
	ci->ce_count = 0;
	ci->uce_count = 0;
	ci->error_types_seen = 0;
	ci->window_start_ns = ktime_get_ns();
}

/*
 * Add an error record to the per-CPU ring buffer.
 * Evicts the oldest entry if the ring is full.
 * Caller must hold ci->lock.
 */
static void cfi_log_error(struct cfi_cpu_info *ci,
			   const struct cfi_error_event *event)
{
	struct cfi_error_log_entry *entry;

	/* Evict oldest if at capacity */
	if (ci->error_log_count >= CFI_ERROR_LOG_MAX) {
		entry = list_first_entry(&ci->error_log,
					 struct cfi_error_log_entry, list);
		list_del(&entry->list);
		ci->error_log_count--;
	} else {
		entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
		if (!entry)
			return;
	}

	memcpy(&entry->event, event, sizeof(*event));
	list_add_tail(&entry->list, &ci->error_log);
	ci->error_log_count++;
}

/*
 * Transition a CPU to a new state with logging.
 * Caller must hold ci->lock.
 */
static void cfi_transition(struct cfi_cpu_info *ci, unsigned int cpu,
			    enum cfi_cpu_state new_state)
{
	enum cfi_cpu_state old_state = ci->state;

	if (old_state == new_state)
		return;

	ci->state = new_state;

	pr_info("cpu%u: %s -> %s (ce=%u uce=%u types=0x%x)\n",
		cpu, cfi_state_name(old_state), cfi_state_name(new_state),
		ci->ce_count, ci->uce_count, ci->error_types_seen);

	/* Notify userspace via netlink (best-effort, don't block) */
	cfi_nl_send_state_change(cpu, new_state);
}

/*
 * Main error reporting entry point.
 * Called by architecture backends (x86, arm64) from notifier/tracepoint context.
 * This may be called from NMI context on x86, so must be lock-safe.
 */
void cfi_report_error(struct cfi_error_event *event)
{
	struct cfi_cpu_info *ci;
	unsigned int cpu = event->cpu;
	u64 now, window_ns;
	unsigned long flags;

	if (cpu >= nr_cpu_ids) {
		pr_warn_ratelimited("error on invalid cpu %u\n", cpu);
		return;
	}

	ci = &cfi_cpus[cpu];
	spin_lock_irqsave(&ci->lock, flags);

	/* Skip CPUs already isolated or in isolation process */
	if (ci->state == CFI_STATE_ISOLATED ||
	    ci->state == CFI_STATE_ISOLATING) {
		spin_unlock_irqrestore(&ci->lock, flags);
		return;
	}

	/* Check window expiry */
	now = ktime_get_ns();
	window_ns = (u64)cfi_window_secs * NSEC_PER_SEC;
	if (now - ci->window_start_ns > window_ns)
		cfi_reset_cpu_window(ci);

	/* Update counters */
	switch (event->severity) {
	case CFI_SEV_CE:
		ci->ce_count++;
		ci->ce_count_total++;
		break;
	case CFI_SEV_UCR:
	case CFI_SEV_UCF:
		ci->uce_count++;
		ci->uce_count_total++;
		break;
	}
	ci->error_types_seen |= event->error_type;

	/* Add to per-CPU error log */
	cfi_log_error(ci, event);

	/* Notify userspace daemon of the error */
	cfi_nl_send_error(event);

	/* State machine transitions */
	if (!cfi_auto_isolate)
		goto out;

	if (ci->user_pinned) {
		pr_info_ratelimited("cpu%u: error detected but user-pinned, skipping isolation\n",
				    cpu);
		goto out;
	}

	/*
	 * L3 / LLC is socket-shared (CHA tiles on Intel, CCX on AMD): isolating
	 * the reporting CPU doesn't remove the dependency on the bad cache slice,
	 * so by default we account the UCE and emit a netlink event but skip the
	 * automatic CPU isolation. A userspace daemon can do address-level
	 * handling (hwpoison affected pages) or escalate to socket-level drain.
	 * Set isolate_on_l3_uce=1 to fall back to the conservative behavior.
	 * L1 / L2 are per-core, so any non-L3 cache UCE keeps the original
	 * "isolate reporting CPU" path.
	 */
	if (event->error_type == CFI_ERR_CACHE_L3 && !cfi_isolate_on_l3_uce &&
	    (ci->state == CFI_STATE_ONLINE || ci->state == CFI_STATE_DEGRADED) &&
	    ci->uce_count >= cfi_uce_threshold) {
		pr_info_ratelimited("cpu%u: L3 cache UCE not isolating (shared LLC; set isolate_on_l3_uce=1 to override)\n",
				    cpu);
		goto out;
	}

	switch (ci->state) {
	case CFI_STATE_ONLINE:
		if (ci->uce_count >= cfi_uce_threshold) {
			/* UCE from ONLINE: go straight to isolation */
			cfi_transition(ci, cpu, CFI_STATE_ISOLATING);
			spin_unlock_irqrestore(&ci->lock, flags);
			cfi_begin_isolation(cpu, event->severity == CFI_SEV_UCF);
			return;
		}
		if (ci->ce_count >= cfi_ce_threshold)
			cfi_transition(ci, cpu, CFI_STATE_DEGRADED);
		break;

	case CFI_STATE_DEGRADED:
		if (ci->uce_count >= cfi_uce_threshold) {
			cfi_transition(ci, cpu, CFI_STATE_ISOLATING);
			spin_unlock_irqrestore(&ci->lock, flags);
			cfi_begin_isolation(cpu, event->severity == CFI_SEV_UCF);
			return;
		}
		break;

	default:
		break;
	}

out:
	spin_unlock_irqrestore(&ci->lock, flags);
}
