// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - CPU Hotplug and Isolation
 *
 * Handles the actual CPU offline/online operations via the kernel's
 * CPU hotplug infrastructure. Implements the deferred isolation protocol
 * that allows the userspace daemon to migrate VMs before CPU offline.
 *
 * Key constraint: cpu_down()/remove_cpu() must be called from process
 * context. When isolation is triggered from MCE/NMI context, we defer
 * to a workqueue.
 *
 * Offline path robustness (physical host vs VM):
 *   The normal exported entry remove_cpu() goes
 *     remove_cpu() -> device_offline() -> bus->offline()
 *                  -> cpu_subsys_offline() -> cpu_device_down() -> cpu_down()
 *   On some hosts a health/guard agent stubs cpu_subsys_offline() to return
 *   -EINVAL, so remove_cpu() fails and the CPU never actually goes down (this
 *   does not happen inside a plain VM). When that is detected we escalate to
 *   the next layer down, resolved via kprobe since they are not exported:
 *     level 1: remove_cpu()        (normal, keeps the device model in sync)
 *     level 2: cpu_device_down(dev) (bypasses the cpu_subsys_offline stub)
 *     level 3: cpu_down(cpu, CPUHP_OFFLINE) (bypasses cpu_device_down too)
 *   After a level 2/3 success we set dev->offline ourselves and emit the
 *   uevent, because we skipped device_offline()'s bookkeeping — without it a
 *   later add_cpu()/device_online() would think the CPU is still online and
 *   never bring it back. The online path mirrors this.
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpuhotplug.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/device.h>
#include <linux/kprobes.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqnr.h>
#include "cfi_internal.h"

/* CPU hotplug state handle, used for cleanup on module exit */
static enum cpuhp_state cfi_hp_state;

/*
 * Dedicated workqueue for the actual cpu_down work. It must NOT be
 * system_wq: some vendor kernels (HCE 2.0) run _cpu_down via
 * work_on_cpu(), which itself queues onto system_wq and flushes. If our
 * offline work also sat on system_wq, the outer work would block a
 * system_wq worker while waiting for the inner work_on_cpu item — and
 * under a burst of isolations (e.g. our own lockup detector reacting to
 * the stalls a teardown causes) system_wq saturates and deadlocks.
 * An ordered (max_active=1) unbound wq also serializes offlines so two
 * CPUs are never torn down concurrently, and never runs on a wedged CPU.
 */
static struct workqueue_struct *cfi_hotplug_wq;

/* >0 while an offline is executing; read by the lockup detector. */
static atomic_t cfi_offline_inflight = ATOMIC_INIT(0);

bool cfi_offline_in_progress(void)
{
	return atomic_read(&cfi_offline_inflight) > 0;
}

bool cfi_cpu_is_protected(unsigned int cpu)
{
	return cfi_protect_cpu0 && cpu == 0;
}

/*
 * Lower-level hotplug entry points, resolved at init via kprobe because
 * they are not exported to modules. Any may be NULL if not found, in
 * which case that escalation level is skipped.
 */
typedef int (*cfi_cpu_dev_fn)(struct device *dev);
typedef int (*cfi_cpu_down_fn)(unsigned int cpu, enum cpuhp_state target);

static cfi_cpu_dev_fn  cfi_cpu_device_down;	/* bypasses cpu_subsys_offline */
static cfi_cpu_dev_fn  cfi_cpu_device_up;	/* bypasses cpu_subsys_online */
static cfi_cpu_down_fn cfi_cpu_down;		/* bypasses cpu_device_down */
static cfi_cpu_down_fn cfi_cpu_up;		/* bypasses cpu_device_up */

/*
 * INACTIVE isolation backend.
 *
 * set_cpu_active(cpu, false) is the upstream API to make the scheduler
 * stop picking a CPU. On HCE 2.0 / 5.10 (and several other vendor 5.x
 * kernels) set_cpu_active is inlined and not present in kallsyms, so we
 * cannot resolve it via kprobe. But the data it manipulates,
 * __cpu_active_mask, is EXPORT_SYMBOL — and the bit it flips is the only
 * state the scheduler actually consults. Doing the bit flip directly
 * with cpumask_clear_cpu() is equivalent for our purposes (skipping only
 * the sched_smt_active accounting, which controls SMT-aware sibling
 * scheduling heuristics, not whether a CPU can be picked at all).
 */

/* Resolve an unexported symbol's address via a throwaway kprobe. */
static unsigned long cfi_lookup_name(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long addr;

	if (register_kprobe(&kp) < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

static void cfi_resolve_offline_bypass(void)
{
	if (!cfi_offline_bypass)
		return;

	cfi_cpu_device_down = (cfi_cpu_dev_fn)cfi_lookup_name("cpu_device_down");
	cfi_cpu_device_up   = (cfi_cpu_dev_fn)cfi_lookup_name("cpu_device_up");
	cfi_cpu_down        = (cfi_cpu_down_fn)cfi_lookup_name("cpu_down");
	cfi_cpu_up          = (cfi_cpu_down_fn)cfi_lookup_name("cpu_up");

	pr_info("offline bypass resolved: cpu_device_down=%s cpu_device_up=%s cpu_down=%s cpu_up=%s\n",
		cfi_cpu_device_down ? "ok" : "no",
		cfi_cpu_device_up ? "ok" : "no",
		cfi_cpu_down ? "ok" : "no",
		cfi_cpu_up ? "ok" : "no");
}

/*
 * Resolve isolation_mode= string to enum. INACTIVE is always available
 * because it uses the exported __cpu_active_mask directly.
 */
static void cfi_select_isolation_mode(const char *requested)
{
	if (cfi_soft_isolation) {
		pr_warn("soft_isolation=1 is deprecated; use isolation_mode=soft\n");
		cfi_isolation_mode = CFI_ISOL_SOFT;
	} else if (requested && !strcasecmp(requested, "inactive")) {
		cfi_isolation_mode = CFI_ISOL_INACTIVE;
	} else if (requested && !strcasecmp(requested, "soft")) {
		cfi_isolation_mode = CFI_ISOL_SOFT;
	} else {
		cfi_isolation_mode = CFI_ISOL_FULL;
	}
}

/*
 * Reconcile the device-model "offline" flag after a bypass down/up that
 * skipped device_offline()/device_online(). Keeps the cpuX/online sysfs
 * file truthful and, crucially, lets a later add_cpu()/remove_cpu() act
 * instead of short-circuiting on a stale flag. Runs in process context.
 */
static void cfi_sync_dev_offline(unsigned int cpu, bool offline)
{
	struct device *dev = get_cpu_device(cpu);

	if (!dev)
		return;

	device_lock(dev);
	dev->offline = offline;
	device_unlock(dev);

	kobject_uevent(&dev->kobj, offline ? KOBJ_OFFLINE : KOBJ_ONLINE);
}

/*
 * Take a CPU offline, escalating past a stubbed cpu_subsys_offline if the
 * normal path is blocked. Returns 0 on success.
 */
static int cfi_cpu_do_offline(unsigned int cpu)
{
	struct device *dev;
	int ret;

	ret = remove_cpu(cpu);
	if (ret == 0)
		return 0;

	/*
	 * remove_cpu() failed. If the CPU genuinely went offline anyway, or
	 * the bypass is disabled, just report the original outcome.
	 */
	if (!cpu_online(cpu))
		return 0;
	if (!cfi_offline_bypass)
		return ret;

	pr_warn("cpu%u: remove_cpu() failed (%d), CPU still online — trying bypass\n",
		cpu, ret);

	dev = get_cpu_device(cpu);

	if (dev && cfi_cpu_device_down) {
		int ret2 = cfi_cpu_device_down(dev);

		if (ret2 == 0 || !cpu_online(cpu)) {
			cfi_sync_dev_offline(cpu, true);
			pr_info("cpu%u: offlined via cpu_device_down() bypass\n", cpu);
			return 0;
		}
		pr_warn("cpu%u: cpu_device_down() also failed (%d)\n", cpu, ret2);
	}

	if (cfi_cpu_down) {
		int ret3 = cfi_cpu_down(cpu, CPUHP_OFFLINE);

		if (ret3 == 0 || !cpu_online(cpu)) {
			cfi_sync_dev_offline(cpu, true);
			pr_info("cpu%u: offlined via cpu_down() bypass\n", cpu);
			return 0;
		}
		pr_warn("cpu%u: cpu_down() bypass failed (%d)\n", cpu, ret3);
	}

	return ret;
}

/*
 * Bring a CPU back online, mirroring the offline escalation in case
 * cpu_subsys_online is stubbed the same way. Returns 0 on success.
 */
static int cfi_cpu_do_online(unsigned int cpu)
{
	struct device *dev;
	int ret;

	ret = add_cpu(cpu);
	if (ret == 0)
		return 0;

	if (cpu_online(cpu))
		return 0;
	if (!cfi_offline_bypass)
		return ret;

	pr_warn("cpu%u: add_cpu() failed (%d), CPU still offline — trying bypass\n",
		cpu, ret);

	dev = get_cpu_device(cpu);

	if (dev && cfi_cpu_device_up) {
		int ret2 = cfi_cpu_device_up(dev);

		if (ret2 == 0 || cpu_online(cpu)) {
			cfi_sync_dev_offline(cpu, false);
			pr_info("cpu%u: onlined via cpu_device_up() bypass\n", cpu);
			return 0;
		}
		pr_warn("cpu%u: cpu_device_up() also failed (%d)\n", cpu, ret2);
	}

	if (cfi_cpu_up) {
		int ret3 = cfi_cpu_up(cpu, CPUHP_ONLINE);

		if (ret3 == 0 || cpu_online(cpu)) {
			cfi_sync_dev_offline(cpu, false);
			pr_info("cpu%u: onlined via cpu_up() bypass\n", cpu);
			return 0;
		}
		pr_warn("cpu%u: cpu_up() bypass failed (%d)\n", cpu, ret3);
	}

	return ret;
}

/*
 * CPU hotplug callback: track externally initiated CPU state changes.
 * If someone offlines a CPU via sysfs directly (not through us),
 * we update our state tracking.
 */
static int cfi_cpu_online(unsigned int cpu)
{
	struct cfi_cpu_info *ci = &cfi_cpus[cpu];
	unsigned long flags;

	spin_lock_irqsave(&ci->lock, flags);
	if (ci->state == CFI_STATE_ISOLATED) {
		/*
		 * CPU came back online externally (e.g., operator wrote
		 * to sysfs). Reset our state.
		 */
		ci->state = CFI_STATE_ONLINE;
		cfi_reset_cpu_window(ci);
		pr_info("cpu%u: externally brought online, resetting state\n", cpu);
	}
	spin_unlock_irqrestore(&ci->lock, flags);

	return 0;
}

static int cfi_cpu_offline(unsigned int cpu)
{
	/* Nothing to do - we only care about online transitions */
	return 0;
}

int cfi_hotplug_init(void)
{
	int ret;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
				CFI_MODULE_NAME ":online",
				cfi_cpu_online, cfi_cpu_offline);
	if (ret < 0)
		return ret;

	cfi_hp_state = ret;

	/*
	 * Ordered (single in-flight), independent of system_wq. See the
	 * comment on cfi_hotplug_wq above for why this matters on vendor
	 * kernels that route _cpu_down through work_on_cpu(). Note: NOT
	 * WQ_MEM_RECLAIM — cpu_device_down() flushes the non-reclaim
	 * system_wq internally (work_on_cpu), and a reclaim wq flushing a
	 * non-reclaim one trips check_flush_dependency().
	 */
	cfi_hotplug_wq = alloc_ordered_workqueue("cfi_hotplug", 0);
	if (!cfi_hotplug_wq) {
		cpuhp_remove_state(cfi_hp_state);
		cfi_hp_state = 0;
		return -ENOMEM;
	}

	/* Resolve the deeper offline entry points for the stub-bypass path */
	cfi_resolve_offline_bypass();
	cfi_select_isolation_mode(cfi_isolation_mode_str);

	pr_info("isolation mode: %s\n", cfi_isolation_mode_name(cfi_isolation_mode));
	switch (cfi_isolation_mode) {
	case CFI_ISOL_FULL:
		pr_info("  (cpu hotplug down — VM / non-stubbed hosts only; may deadlock on HCE2 physical)\n");
		break;
	case CFI_ISOL_INACTIVE:
		pr_info("  (set_cpu_active(false) + IRQ migration — recommended for HCE2 physical hosts)\n");
		break;
	case CFI_ISOL_SOFT:
		pr_info("  (IRQ migration only — daemon must migrate tasks/vCPUs)\n");
		break;
	}
	return 0;
}

void cfi_hotplug_exit(void)
{
	if (cfi_hotplug_wq) {
		destroy_workqueue(cfi_hotplug_wq);	/* drains pending offlines */
		cfi_hotplug_wq = NULL;
	}
	if (cfi_hp_state)
		cpuhp_remove_state(cfi_hp_state);
}

/* Queue the offline work on the dedicated wq (never system_wq). */
static void cfi_queue_offline(struct cfi_cpu_info *ci)
{
	queue_work(cfi_hotplug_wq, &ci->offline_work);
}

/*
 * Steer every movable IRQ off @cpu. Returns count of IRQs successfully
 * migrated (purely informational). Shared by the SOFT and INACTIVE modes.
 * Best-effort: managed/per-CPU IRQs that cannot be moved are skipped.
 */
static unsigned int cfi_migrate_irqs_off_cpu(unsigned int cpu)
{
	cpumask_var_t mask;
	unsigned int irq, moved = 0;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		return 0;

	for (irq = 0; irq < nr_irqs; irq++) {
		struct irq_data *d = irq_get_irq_data(irq);
		const struct cpumask *aff;

		if (!d)
			continue;
		aff = irq_data_get_affinity_mask(d);
		if (!aff || !cpumask_test_cpu(cpu, aff))
			continue;

		/* Preserve the IRQ's policy, just drop the faulty CPU. */
		cpumask_and(mask, aff, cpu_online_mask);
		cpumask_clear_cpu(cpu, mask);
		if (cpumask_empty(mask)) {
			/* Affinity was only the faulty CPU: spread to all others */
			cpumask_copy(mask, cpu_online_mask);
			cpumask_clear_cpu(cpu, mask);
		}
		if (cpumask_empty(mask))
			continue;	/* would be the last CPU; leave it */

		if (irq_set_affinity(irq, mask) == 0)
			moved++;
	}

	free_cpumask_var(mask);
	return moved;
}

/*
 * Soft isolation: IRQ migration only. The CPU stays online; the daemon
 * is responsible for moving tasks/vCPUs off it. Never touches cpus_rwsem
 * or the hotplug state machine, so it cannot deadlock. Weakest of the
 * three modes; kept for kernels where set_cpu_active is not resolvable.
 */
static int cfi_soft_isolate_cpu(unsigned int cpu)
{
	unsigned int moved = cfi_migrate_irqs_off_cpu(cpu);

	pr_info("cpu%u: soft-isolated (migrated %u IRQs; CPU stays online, daemon migrates tasks/vCPUs)\n",
		cpu, moved);
	return 0;
}

/*
 * Inactive isolation: clear @cpu from cpu_active_mask + migrate its IRQs.
 * The CPU stays in cpu_online_mask, but the scheduler stops picking it
 * for new tasks and the load balancer drifts existing migratable tasks
 * off it within a few rebalance ticks (~ms). Per-CPU kthreads (idle,
 * ksoftirqd, migration/N, cpuhp/N) remain bound but they are short-burst
 * / mostly idle, so the faulty cache effectively stops being used.
 *
 * Why this is safe vs the FULL-hotplug deadlock seen on HCE2:
 *   - We never call any function that takes cpus_write_lock.
 *   - We never enter the CPU-hotplug state machine, so we never call
 *     cpuset_hotplug_workfn-scheduling helpers.
 *   - We never use work_on_cpu(target_cpu, ...), so we never queue work
 *     to the "being-isolated" CPU's events pool.
 * The known HCE2 deadlock chain (work_on_cpu(target)+cpuset_hotplug_workfn
 * +cpus_rwsem ABBA) requires all three of those; we trip none.
 *
 * Trade-off: we skip the sched_smt_active accounting that the inlined
 * set_cpu_active() also updates. That counter affects SMT-aware sibling
 * scheduling heuristics, not whether a CPU is eligible at all — so this
 * is non-load-bearing for the "stop dispatching to the faulty CPU" goal.
 */
static int cfi_inactive_isolate_cpu(unsigned int cpu)
{
	unsigned int moved;

	/*
	 * cpu_active_mask is declared `const`; the underlying storage
	 * __cpu_active_mask is writable and gets mutated by set_cpu_active()
	 * upstream the same way. Cast off const at the API boundary.
	 */
	cpumask_clear_cpu(cpu, (struct cpumask *)cpu_active_mask);

	moved = cfi_migrate_irqs_off_cpu(cpu);

	pr_info("cpu%u: inactive-isolated (cleared from cpu_active_mask + migrated %u IRQs; CPU stays online)\n",
		cpu, moved);
	return 0;
}

/*
 * Reverse of cfi_inactive_isolate_cpu: re-add to cpu_active_mask. IRQs
 * are not restored — irqbalance / the kernel rebalances affinity over
 * time, matching SOFT-mode unisolate behaviour.
 */
static void cfi_inactive_unisolate_cpu(unsigned int cpu)
{
	cpumask_set_cpu(cpu, (struct cpumask *)cpu_active_mask);
	pr_info("cpu%u: inactive isolation cleared (re-added to cpu_active_mask)\n",
		cpu);
}

/*
 * Workqueue function: isolate the CPU. Runs in process context (kworker).
 * Two modes: full hotplug offline (default; VM and non-stubbed hosts) or
 * soft isolation (IRQ migration; required where cpu_down() deadlocks).
 */
void cfi_offline_work_fn(struct work_struct *work)
{
	struct cfi_cpu_info *ci = container_of(work, struct cfi_cpu_info,
					       offline_work);
	unsigned int cpu = ci - cfi_cpus;
	unsigned long flags;
	int ret;

	/* Last-line guard: never tear down a protected CPU. */
	if (cfi_cpu_is_protected(cpu)) {
		spin_lock_irqsave(&ci->lock, flags);
		ci->state = CFI_STATE_ONLINE;
		spin_unlock_irqrestore(&ci->lock, flags);
		pr_warn("cpu%u: protected, refusing to isolate\n", cpu);
		cfi_nl_send_state_change(cpu, CFI_STATE_ONLINE);
		return;
	}

	if (cfi_isolation_mode == CFI_ISOL_SOFT) {
		pr_info("cpu%u: soft-isolating (IRQ migration)\n", cpu);
		ret = cfi_soft_isolate_cpu(cpu);
		goto report_mark;
	}

	if (cfi_isolation_mode == CFI_ISOL_INACTIVE) {
		pr_info("cpu%u: inactive-isolating (sched_active=false + IRQ migration)\n",
			cpu);
		ret = cfi_inactive_isolate_cpu(cpu);
		goto report_mark;
	}

	pr_info("cpu%u: taking offline\n", cpu);

	/*
	 * remove_cpu() calls cpu_down() which triggers the hotplug state
	 * machine. This automatically:
	 * - Migrates all runnable tasks off the CPU
	 * - Moves IRQs to other CPUs
	 * - Stops per-CPU kernel threads
	 * - Drains the CPU's run queue
	 *
	 * Mark an offline in flight so the lockup detector does not react to
	 * the transient stalls a teardown causes by scheduling more offlines.
	 */
	atomic_inc(&cfi_offline_inflight);
	ret = cfi_cpu_do_offline(cpu);
	atomic_dec(&cfi_offline_inflight);

report_mark:
	spin_lock_irqsave(&ci->lock, flags);
	if (ret == 0) {
		ci->state = CFI_STATE_ISOLATED;
		ci->isolation_count++;
		pr_info("cpu%u: isolated successfully via %s (total isolations: %u)\n",
			cpu, cfi_isolation_mode_name(cfi_isolation_mode),
			ci->isolation_count);
	} else {
		ci->state = CFI_STATE_FAILED;
		pr_err("cpu%u: isolation failed (%s): %d\n",
		       cpu, cfi_isolation_mode_name(cfi_isolation_mode), ret);
	}
	spin_unlock_irqrestore(&ci->lock, flags);

	cfi_nl_send_state_change(cpu, ci->state);
}

/*
 * Timer callback: fires when daemon doesn't ACK within timeout.
 * Forces the CPU offline even without daemon cooperation.
 */
void cfi_defer_timer_fn(struct timer_list *t)
{
	struct cfi_cpu_info *ci = from_timer(ci, t, defer_timer);
	unsigned int cpu = ci - cfi_cpus;

	pr_warn("cpu%u: daemon ACK timeout (%u ms), forcing offline\n",
		cpu, cfi_defer_timeout_ms);

	cfi_queue_offline(ci);
}

/*
 * Begin the isolation process for a CPU.
 *
 * @cpu: logical CPU number to isolate
 * @urgent: if true (lockup/UCE fatal), skip daemon deferral and offline
 *          immediately. Also ensures work is scheduled on a different CPU,
 *          since the faulting CPU may be non-functional (lockup).
 *
 * When defer_to_daemon is enabled and not urgent, we start a timer
 * and wait for the daemon to acknowledge (via netlink ACK_ISOLATE).
 * The daemon uses this time to migrate VM vCPUs off the CPU.
 *
 * When the timer expires or if urgent, we proceed directly to cpu_down().
 */
void cfi_begin_isolation(unsigned int cpu, bool urgent)
{
	struct cfi_cpu_info *ci = &cfi_cpus[cpu];
	unsigned long flags;

	/*
	 * Single chokepoint for the protected-CPU policy: covers every
	 * caller (core state machine, lockup detector, netlink ISOLATE).
	 * Revert the ISOLATING transition the caller just made.
	 */
	if (cfi_cpu_is_protected(cpu)) {
		spin_lock_irqsave(&ci->lock, flags);
		ci->state = CFI_STATE_ONLINE;
		spin_unlock_irqrestore(&ci->lock, flags);
		pr_warn_ratelimited("cpu%u: protected, isolation refused\n", cpu);
		cfi_nl_send_state_change(cpu, CFI_STATE_ONLINE);
		return;
	}

	/* Never isolate the last online CPU. */
	if (cpumask_weight(cpu_online_mask) <= 1) {
		spin_lock_irqsave(&ci->lock, flags);
		ci->state = CFI_STATE_FAILED;
		spin_unlock_irqrestore(&ci->lock, flags);
		pr_err("cpu%u: last online CPU, cannot isolate\n", cpu);
		cfi_nl_send_state_change(cpu, CFI_STATE_FAILED);
		return;
	}

	ci->daemon_acked = false;

	if (cfi_defer_to_daemon && !urgent) {
		pr_info("cpu%u: waiting for daemon ACK (timeout %u ms)\n",
			cpu, cfi_defer_timeout_ms);
		mod_timer(&ci->defer_timer,
			  jiffies + msecs_to_jiffies(cfi_defer_timeout_ms));
	} else {
		if (urgent)
			pr_info("cpu%u: urgent isolation, skipping daemon deferral\n",
				cpu);
		/*
		 * The ordered unbound cfi_hotplug_wq runs the teardown on a
		 * healthy CPU (never the faulting one — an unbound pool will
		 * not pick a wedged CPU) and serializes offlines, so there is
		 * no need to hand-pick a target as before.
		 */
		cfi_queue_offline(ci);
	}
}

/*
 * Called when daemon sends ACK_ISOLATE via netlink.
 * Cancels the defer timer and proceeds with offline.
 */
void cfi_daemon_ack_isolate(unsigned int cpu)
{
	struct cfi_cpu_info *ci;
	unsigned long flags;

	if (cpu >= nr_cpu_ids)
		return;

	ci = &cfi_cpus[cpu];
	spin_lock_irqsave(&ci->lock, flags);

	if (ci->state != CFI_STATE_ISOLATING) {
		pr_warn("cpu%u: daemon ACK but state is %s, ignoring\n",
			cpu, cfi_state_name(ci->state));
		spin_unlock_irqrestore(&ci->lock, flags);
		return;
	}

	ci->daemon_acked = true;
	spin_unlock_irqrestore(&ci->lock, flags);

	/* Cancel the timeout timer and proceed */
	del_timer_sync(&ci->defer_timer);

	pr_info("cpu%u: daemon ACK received, proceeding with offline\n", cpu);
	cfi_queue_offline(ci);
}

/*
 * Bring a previously isolated CPU back online.
 * Called from netlink (daemon/CLI) or sysfs.
 *
 * Returns 0 on success, negative errno on failure.
 */
int cfi_unisolate_cpu(unsigned int cpu)
{
	struct cfi_cpu_info *ci;
	unsigned long flags;
	int ret;

	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	ci = &cfi_cpus[cpu];
	spin_lock_irqsave(&ci->lock, flags);

	if (ci->state != CFI_STATE_ISOLATED &&
	    ci->state != CFI_STATE_FAILED) {
		pr_warn("cpu%u: cannot unisolate, state is %s\n",
			cpu, cfi_state_name(ci->state));
		spin_unlock_irqrestore(&ci->lock, flags);
		return -EINVAL;
	}

	spin_unlock_irqrestore(&ci->lock, flags);

	/*
	 * Restore depends on isolation mode:
	 *   SOFT     — only IRQs were steered; nothing to undo besides
	 *              bookkeeping (irqbalance will rebalance affinity).
	 *   INACTIVE — flip set_cpu_active back to true; IRQs left as-is
	 *              for the same reason.
	 *   FULL     — bring the CPU back via add_cpu()/bypass.
	 */
	switch (cfi_isolation_mode) {
	case CFI_ISOL_SOFT:
		pr_info("cpu%u: clearing soft isolation\n", cpu);
		break;
	case CFI_ISOL_INACTIVE:
		cfi_inactive_unisolate_cpu(cpu);
		break;
	case CFI_ISOL_FULL:
		pr_info("cpu%u: bringing back online\n", cpu);
		ret = cfi_cpu_do_online(cpu);
		if (ret) {
			pr_err("cpu%u: failed to bring online: %d\n", cpu, ret);
			return ret;
		}
		break;
	}

	spin_lock_irqsave(&ci->lock, flags);
	ci->state = CFI_STATE_ONLINE;
	cfi_reset_cpu_window(ci);
	spin_unlock_irqrestore(&ci->lock, flags);

	cfi_nl_send_state_change(cpu, CFI_STATE_ONLINE);

	pr_info("cpu%u: back online\n", cpu);
	return 0;
}
