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
#include "cfi_internal.h"

/* CPU hotplug state handle, used for cleanup on module exit */
static enum cpuhp_state cfi_hp_state;

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

	/* Resolve the deeper offline entry points for the stub-bypass path */
	cfi_resolve_offline_bypass();
	return 0;
}

void cfi_hotplug_exit(void)
{
	if (cfi_hp_state)
		cpuhp_remove_state(cfi_hp_state);
}

/*
 * Workqueue function: actually take the CPU offline.
 * Runs in process context (kworker).
 */
void cfi_offline_work_fn(struct work_struct *work)
{
	struct cfi_cpu_info *ci = container_of(work, struct cfi_cpu_info,
					       offline_work);
	unsigned int cpu = ci - cfi_cpus;
	unsigned long flags;
	int ret;

	pr_info("cpu%u: taking offline\n", cpu);

	/*
	 * remove_cpu() calls cpu_down() which triggers the hotplug state
	 * machine. This automatically:
	 * - Migrates all runnable tasks off the CPU
	 * - Moves IRQs to other CPUs
	 * - Stops per-CPU kernel threads
	 * - Drains the CPU's run queue
	 */
	ret = cfi_cpu_do_offline(cpu);

	spin_lock_irqsave(&ci->lock, flags);
	if (ret == 0) {
		ci->state = CFI_STATE_ISOLATED;
		ci->isolation_count++;
		pr_info("cpu%u: isolated successfully (total isolations: %u)\n",
			cpu, ci->isolation_count);
	} else {
		ci->state = CFI_STATE_FAILED;
		pr_err("cpu%u: failed to offline: %d\n", cpu, ret);
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

	schedule_work(&ci->offline_work);
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

	ci->daemon_acked = false;

	if (cfi_defer_to_daemon && !urgent) {
		pr_info("cpu%u: waiting for daemon ACK (timeout %u ms)\n",
			cpu, cfi_defer_timeout_ms);
		mod_timer(&ci->defer_timer,
			  jiffies + msecs_to_jiffies(cfi_defer_timeout_ms));
	} else {
		unsigned int target;

		if (urgent)
			pr_info("cpu%u: urgent isolation, skipping daemon deferral\n",
				cpu);

		/*
		 * For urgent isolation (lockup, fatal MCE), ensure the
		 * offline work runs on a healthy CPU. The faulting CPU may
		 * not be processing its workqueue (hardlockup) or may have
		 * corrupted context (MCE PCC).
		 */
		target = raw_smp_processor_id();
		if (urgent && target == cpu) {
			target = cpumask_any_but(cpu_online_mask, cpu);
			if (target >= nr_cpu_ids) {
				pr_err("cpu%u: last online CPU, cannot self-isolate\n",
				       cpu);
				return;
			}
			queue_work_on(target, system_wq, &ci->offline_work);
		} else {
			schedule_work(&ci->offline_work);
		}
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
	schedule_work(&ci->offline_work);
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

	pr_info("cpu%u: bringing back online\n", cpu);
	ret = cfi_cpu_do_online(cpu);
	if (ret) {
		pr_err("cpu%u: failed to bring online: %d\n", cpu, ret);
		return ret;
	}

	spin_lock_irqsave(&ci->lock, flags);
	ci->state = CFI_STATE_ONLINE;
	cfi_reset_cpu_window(ci);
	spin_unlock_irqrestore(&ci->lock, flags);

	cfi_nl_send_state_change(cpu, CFI_STATE_ONLINE);

	pr_info("cpu%u: back online\n", cpu);
	return 0;
}
