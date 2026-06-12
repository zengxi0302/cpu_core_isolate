// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - Lockup Detection and Isolation
 *
 * Implements CFI's own CPU lockup detection, independent of the kernel's
 * watchdog. This is necessary because after we suppress softlockup_panic
 * and hardlockup_panic, the kernel only logs lockups without taking action.
 * This module detects them and triggers CPU isolation.
 *
 * Detection mechanism:
 *   - Per-CPU hrtimer ("heartbeat"): fires every 4 seconds, increments
 *     an atomic counter. If the counter stops incrementing, the CPU is
 *     not processing interrupts → hardlockup.
 *   - Per-CPU kthread ("watchdog"): runs every 4 seconds, updates a
 *     timestamp. The hrtimer checks if the kthread ran recently. If not,
 *     the CPU is not scheduling → softlockup.
 *   - Global delayed_work ("monitor"): periodically checks all CPUs'
 *     heartbeat counters from a known-good CPU. Triggers isolation for
 *     any CPU whose heartbeat is stale.
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include "cfi_internal.h"

#define HEARTBEAT_INTERVAL_NS	(4ULL * NSEC_PER_SEC)
#define MONITOR_INTERVAL_MS	5000
#define STALE_CYCLES_HARDLOCKUP	6	/* 6 * 5s = 30s */

static DEFINE_PER_CPU(struct hrtimer, cfi_hb_timer);
static DEFINE_PER_CPU(atomic_t, cfi_hb_count);
static DEFINE_PER_CPU(struct task_struct *, cfi_wd_thread);
static DEFINE_PER_CPU(unsigned long, cfi_sched_ts);

/* CPUs whose cfi_hb_timer has been hrtimer_init()'d + started. We can only
 * safely hrtimer_cancel() those — calling hrtimer_active() (or _cancel())
 * on a per-CPU slot that was never hrtimer_init()'d hits a NULL timer->base
 * on 5.10 (observed on HCE 2.0 during the rmmod after a reload where one
 * CPU was still isolated/offline when the second cfi_lockup_init() ran).
 */
static cpumask_var_t cfi_hb_init_mask;

struct cfi_mon_cpu {
	int last_hb;
	int stale_cycles;
	bool lockup_reported;
};

static struct cfi_mon_cpu *cfi_mon;
static struct delayed_work cfi_monitor_dwork;
static bool cfi_lockup_active;

static void cfi_report_lockup(unsigned int cpu, u32 error_type)
{
	struct cfi_error_event event = {};

	event.cpu = cpu;
	event.error_type = error_type;
	event.severity = CFI_SEV_UCF;
	event.timestamp_ns = ktime_get_ns();

	pr_emerg("cpu%u: %s detected by CFI, triggering isolation\n",
		 cpu,
		 (error_type == CFI_ERR_HARDLOCKUP) ? "hardlockup" : "softlockup");

	cfi_report_error(&event);
}

/*
 * Per-CPU hrtimer callback.
 * Runs in hard IRQ context on the local CPU.
 * Increments heartbeat counter and checks for softlockup.
 */
static enum hrtimer_restart cfi_heartbeat_fn(struct hrtimer *timer)
{
	unsigned int cpu = raw_smp_processor_id();
	unsigned long sched_ts;

	atomic_inc(this_cpu_ptr(&cfi_hb_count));

	/* Check softlockup: has our kthread been scheduled recently? */
	sched_ts = __this_cpu_read(cfi_sched_ts);
	if (sched_ts != 0 &&
	    time_after(jiffies, sched_ts + cfi_lockup_thresh_secs * HZ)) {
		struct cfi_cpu_info *ci = &cfi_cpus[cpu];

		if ((ci->state == CFI_STATE_ONLINE ||
		     ci->state == CFI_STATE_DEGRADED) &&
		    !cfi_cpu_is_protected(cpu) && !cfi_offline_in_progress()) {
			cfi_report_lockup(cpu, CFI_ERR_SOFTLOCKUP);
			/*
			 * Clear timestamp to avoid re-reporting every
			 * hrtimer cycle until isolation completes.
			 */
			__this_cpu_write(cfi_sched_ts, 0);
		}
	}

	hrtimer_forward_now(timer, ns_to_ktime(HEARTBEAT_INTERVAL_NS));
	return HRTIMER_RESTART;
}

/*
 * Per-CPU kthread function.
 * Periodically updates a timestamp to prove the scheduler is running.
 */
static int cfi_watchdog_fn(void *data)
{
	while (!kthread_should_stop()) {
		__this_cpu_write(cfi_sched_ts, jiffies);
		schedule_timeout_interruptible(msecs_to_jiffies(4000));
	}
	return 0;
}

/*
 * Global monitor: checks all CPUs' heartbeat counters.
 * Runs on system workqueue (any healthy CPU).
 */
static void cfi_monitor_fn(struct work_struct *work)
{
	unsigned int cpu;

	if (!cfi_lockup_active)
		return;

	/*
	 * While a CPU offline is executing, heartbeats stall across the
	 * machine for reasons that are not lockups (stop-machine, IRQ
	 * migration, vendor work_on_cpu teardown). Pause detection for this
	 * cycle rather than misread it and pile on more isolations.
	 */
	if (cfi_offline_in_progress())
		goto rearm;

	for_each_online_cpu(cpu) {
		struct cfi_mon_cpu *mc = &cfi_mon[cpu];
		struct cfi_cpu_info *ci = &cfi_cpus[cpu];
		int cur;

		/* Skip CPUs already handled or that we will never isolate */
		if (ci->state == CFI_STATE_ISOLATING ||
		    ci->state == CFI_STATE_ISOLATED ||
		    mc->lockup_reported ||
		    cfi_cpu_is_protected(cpu))
			continue;

		cur = atomic_read(per_cpu_ptr(&cfi_hb_count, cpu));

		if (cur == mc->last_hb) {
			mc->stale_cycles++;
			if (mc->stale_cycles >= STALE_CYCLES_HARDLOCKUP) {
				mc->lockup_reported = true;
				cfi_report_lockup(cpu, CFI_ERR_HARDLOCKUP);
			}
		} else {
			mc->stale_cycles = 0;
			mc->last_hb = cur;
		}
	}

rearm:

	if (cfi_lockup_active)
		schedule_delayed_work(&cfi_monitor_dwork,
				      msecs_to_jiffies(MONITOR_INTERVAL_MS));
}

static void cfi_start_heartbeat_on_cpu(void *data)
{
	struct hrtimer *timer = this_cpu_ptr(&cfi_hb_timer);
	int cpu = smp_processor_id();

	atomic_set(this_cpu_ptr(&cfi_hb_count), 0);
	__this_cpu_write(cfi_sched_ts, jiffies);

	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	timer->function = cfi_heartbeat_fn;
	hrtimer_start(timer, ns_to_ktime(HEARTBEAT_INTERVAL_NS),
		      HRTIMER_MODE_REL_PINNED);
	cpumask_set_cpu(cpu, cfi_hb_init_mask);
}

/*
 * Cancel exactly the per-CPU heartbeat hrtimers we started in this module
 * incarnation (tracked in cfi_hb_init_mask). Iterating for_each_possible_cpu
 * is unsafe on 5.10: hrtimer_active() / hrtimer_cancel() dereference
 * timer->base, which is NULL for a per-CPU slot that was never hrtimer_init'd
 * (e.g. a CPU that was offline at init time). on_each_cpu is also unsafe in
 * the other direction: a CPU that was online at init but isolated later had
 * its hrtimer migrated by the hotplug code to a surviving CPU's timerqueue,
 * and on_each_cpu won't visit that CPU. The init bitmap captures exactly
 * the right set; hrtimer_cancel handles migrated timers correctly because
 * the timer carries its current base pointer.
 */
static void cfi_cancel_all_heartbeats(void)
{
	unsigned int cpu;

	for_each_cpu(cpu, cfi_hb_init_mask)
		hrtimer_cancel(per_cpu_ptr(&cfi_hb_timer, cpu));
	cpumask_clear(cfi_hb_init_mask);
}

int cfi_lockup_init(void)
{
	unsigned int cpu;
	int ret = 0;

	cfi_mon = kvcalloc(nr_cpu_ids, sizeof(*cfi_mon), GFP_KERNEL);
	if (!cfi_mon)
		return -ENOMEM;

	if (!zalloc_cpumask_var(&cfi_hb_init_mask, GFP_KERNEL)) {
		kvfree(cfi_mon);
		cfi_mon = NULL;
		return -ENOMEM;
	}

	cfi_lockup_active = true;

	/* Start heartbeat hrtimers on all online CPUs (init_mask records them) */
	on_each_cpu(cfi_start_heartbeat_on_cpu, NULL, 1);

	/* Create per-CPU watchdog kthreads */
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct task_struct *t;

		t = kthread_create(cfi_watchdog_fn, NULL,
				   "cfi_watchdog/%u", cpu);
		if (IS_ERR(t)) {
			pr_warn("failed to create watchdog for cpu%u: %ld\n",
				cpu, PTR_ERR(t));
			continue;
		}
		kthread_bind(t, cpu);
		per_cpu(cfi_wd_thread, cpu) = t;
		wake_up_process(t);
	}
	cpus_read_unlock();

	/* Initialize monitor heartbeat baselines */
	for_each_possible_cpu(cpu) {
		cfi_mon[cpu].last_hb = atomic_read(per_cpu_ptr(&cfi_hb_count, cpu));
		cfi_mon[cpu].stale_cycles = 0;
		cfi_mon[cpu].lockup_reported = false;
	}

	/* Start global monitor */
	INIT_DELAYED_WORK(&cfi_monitor_dwork, cfi_monitor_fn);
	schedule_delayed_work(&cfi_monitor_dwork,
			      msecs_to_jiffies(MONITOR_INTERVAL_MS));

	pr_info("lockup detection active (threshold=%u s)\n",
		cfi_lockup_thresh_secs);
	return ret;
}

void cfi_lockup_exit(void)
{
	unsigned int cpu;

	cfi_lockup_active = false;

	cancel_delayed_work_sync(&cfi_monitor_dwork);

	/* Stop kthreads */
	for_each_possible_cpu(cpu) {
		struct task_struct *t = per_cpu(cfi_wd_thread, cpu);

		if (t) {
			kthread_stop(t);
			per_cpu(cfi_wd_thread, cpu) = NULL;
		}
	}

	/* Cancel only the hrtimers we actually started (init_mask). */
	cfi_cancel_all_heartbeats();
	free_cpumask_var(cfi_hb_init_mask);

	kvfree(cfi_mon);
	cfi_mon = NULL;

	pr_info("lockup detection stopped\n");
}
