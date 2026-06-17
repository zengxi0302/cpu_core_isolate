// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - Module Entry Point
 *
 * Initializes the CFI subsystem: allocates per-CPU tracking structures,
 * suppresses default kernel panic behavior for CPU faults, registers
 * the architecture-specific error backend, starts lockup detection,
 * and sets up netlink and sysfs interfaces.
 *
 * Copyright (c) 2026 openEuler Community
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include "cfi_internal.h"
#include "mfi_internal.h"

/* --- Module parameters --- */

unsigned int cfi_ce_threshold = 10;
module_param_named(ce_threshold, cfi_ce_threshold, uint, 0644);
MODULE_PARM_DESC(ce_threshold,
	"Corrected errors per window to enter DEGRADED state (default: 10)");

unsigned int cfi_uce_threshold = 1;
module_param_named(uce_threshold, cfi_uce_threshold, uint, 0644);
MODULE_PARM_DESC(uce_threshold,
	"Uncorrected errors per window to trigger isolation (default: 1)");

unsigned int cfi_window_secs = 3600;
module_param_named(window_secs, cfi_window_secs, uint, 0644);
MODULE_PARM_DESC(window_secs,
	"Error counting window duration in seconds (default: 3600)");

bool cfi_auto_isolate = true;
module_param_named(auto_isolate, cfi_auto_isolate, bool, 0644);
MODULE_PARM_DESC(auto_isolate,
	"Automatically offline CPUs that exceed error threshold (default: Y)");

bool cfi_defer_to_daemon = true;
module_param_named(defer_to_daemon, cfi_defer_to_daemon, bool, 0644);
MODULE_PARM_DESC(defer_to_daemon,
	"Wait for userspace daemon ACK before CPU offline (default: Y)");

unsigned int cfi_defer_timeout_ms = 30000;
module_param_named(defer_timeout_ms, cfi_defer_timeout_ms, uint, 0644);
MODULE_PARM_DESC(defer_timeout_ms,
	"Max time to wait for daemon ACK in milliseconds (default: 30000)");

unsigned int cfi_mce_tolerant = 3;
module_param_named(mce_tolerant, cfi_mce_tolerant, uint, 0644);
MODULE_PARM_DESC(mce_tolerant,
	"MCE tolerant level override: 1=recover SRAR, 3=never panic (default: 3)");

bool cfi_offline_bypass = true;
module_param_named(offline_bypass, cfi_offline_bypass, bool, 0644);
MODULE_PARM_DESC(offline_bypass,
	"If remove_cpu() is blocked (e.g. cpu_subsys_offline stubbed by a host "
	"health agent returning -EINVAL), fall back to cpu_device_down()/"
	"cpu_down() resolved via kprobe (default: Y)");

bool cfi_protect_cpu0 = true;
module_param_named(protect_cpu0, cfi_protect_cpu0, bool, 0644);
MODULE_PARM_DESC(protect_cpu0,
	"Never auto-isolate CPU0: it is the boot CPU and, on some vendor "
	"kernels, the housekeeping CPU that runs the hotplug teardown "
	"(work_on_cpu); offlining it mid-teardown deadlocks (default: Y)");

bool cfi_isolate_on_l3_uce = false;
module_param_named(isolate_on_l3_uce, cfi_isolate_on_l3_uce, bool, 0644);
MODULE_PARM_DESC(isolate_on_l3_uce,
	"Whether a Last-Level (L3 / LLC) cache UCE should trigger CPU "
	"isolation. L3 is socket-shared (CHA tiles on Intel, CCX on AMD), so "
	"isolating the reporting CPU does not remove the dependency on the "
	"bad LLC slice — other CPUs sharing the same L3 can still hit the "
	"same corrupted line. Default N: account the UCE and emit netlink "
	"event so a userspace daemon can decide (hwpoison affected pages, "
	"alert for socket drain, etc.); CPU stays online. Set Y to fall back "
	"to the conservative \"isolate the reporting CPU anyway\" behavior, "
	"which matches earlier releases. L1/L2 UCE always isolates (those "
	"caches are per-core; isolating the core is the right answer).");

bool cfi_soft_isolation = false;
module_param_named(soft_isolation, cfi_soft_isolation, bool, 0644);
MODULE_PARM_DESC(soft_isolation,
	"DEPRECATED. Equivalent to isolation_mode=soft. Kept for backwards "
	"compatibility; please switch to isolation_mode=. When set to 1 it "
	"forces SOFT regardless of isolation_mode= (default: N)");

char *cfi_isolation_mode_str = "full";
module_param_named(isolation_mode, cfi_isolation_mode_str, charp, 0644);
MODULE_PARM_DESC(isolation_mode,
	"CPU isolation mechanism. 'full' = cpu hotplug offline (default; works "
	"in VMs and bare-metal kernels without offline-path patches). 'inactive' "
	"= set_cpu_active(false) + IRQ migration; CPU stays in cpu_online_mask "
	"but the scheduler stops picking it. RECOMMENDED on HCE2 physical hosts "
	"where full cpu_down() deadlocks (work_on_cpu-wrapped _cpu_down + "
	"cgroup-v1 cpuset vs cpus_rwsem). 'soft' = IRQ migration only; weakest, "
	"daemon must migrate tasks. Falls back from 'inactive' to 'soft' if "
	"set_cpu_active is not resolvable on the running kernel.");

enum cfi_isol_mode cfi_isolation_mode = CFI_ISOL_FULL;

const char *cfi_isolation_mode_name(enum cfi_isol_mode m)
{
	switch (m) {
	case CFI_ISOL_FULL:	return "full";
	case CFI_ISOL_INACTIVE:	return "inactive";
	case CFI_ISOL_SOFT:	return "soft";
	default:		return "unknown";
	}
}

/* --- Memory fault domain (MFI) parameters --- */

bool mfi_enable = true;
module_param_named(mem_enable, mfi_enable, bool, 0644);
MODULE_PARM_DESC(mem_enable,
	"Enable the memory fault isolation domain (default: Y)");

bool mfi_pre_isolate = true;
module_param_named(mem_pre_isolate, mfi_pre_isolate, bool, 0644);
MODULE_PARM_DESC(mem_pre_isolate,
	"Proactively soft-offline pages with recurring CEs (default: Y; "
	"auto-disabled when RAS CEC is active)");

unsigned int mfi_page_ce_threshold = 8;
module_param_named(page_ce_threshold, mfi_page_ce_threshold, uint, 0644);
MODULE_PARM_DESC(page_ce_threshold,
	"Per-page CEs in window to trigger pre-isolation (default: 8)");

unsigned int mfi_window_secs = 86400;
module_param_named(mem_window_secs, mfi_window_secs, uint, 0644);
MODULE_PARM_DESC(mem_window_secs,
	"Page/DIMM CE counting window in seconds (default: 86400)");

unsigned int mfi_dimm_ce_threshold = 1000;
module_param_named(dimm_ce_threshold, mfi_dimm_ce_threshold, uint, 0644);
MODULE_PARM_DESC(dimm_ce_threshold,
	"Per-DIMM CEs in window to advise host evacuation (default: 1000)");

unsigned int mfi_dimm_uce_threshold = 3;
module_param_named(dimm_uce_threshold, mfi_dimm_uce_threshold, uint, 0644);
MODULE_PARM_DESC(dimm_uce_threshold,
	"Per-DIMM UCEs in window to advise host evacuation (default: 3)");

bool mfi_triage = false;
module_param_named(mem_triage, mfi_triage, bool, 0644);
MODULE_PARM_DESC(mem_triage,
	"Phase-2 kernel-context UCE triage: recover user/guest page UCEs, "
	"controlled panic otherwise (default: N, enable after grayscale)");

/* --- Global state --- */

struct cfi_cpu_info *cfi_cpus;
const struct cfi_arch_ops *cfi_arch;

static int cfi_alloc_cpus(void)
{
	unsigned int cpu;

	cfi_cpus = kvcalloc(nr_cpu_ids, sizeof(*cfi_cpus), GFP_KERNEL);
	if (!cfi_cpus)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct cfi_cpu_info *ci = &cfi_cpus[cpu];

		spin_lock_init(&ci->lock);
		ci->state = cpu_online(cpu) ? CFI_STATE_ONLINE : CFI_STATE_ISOLATED;
		ci->window_start_ns = ktime_get_ns();
		INIT_LIST_HEAD(&ci->error_log);
		INIT_WORK(&ci->offline_work, cfi_offline_work_fn);
		timer_setup(&ci->defer_timer, cfi_defer_timer_fn, 0);
	}

	return 0;
}

static void cfi_free_cpus(void)
{
	unsigned int cpu;

	if (!cfi_cpus)
		return;

	for_each_possible_cpu(cpu) {
		struct cfi_cpu_info *ci = &cfi_cpus[cpu];
		struct cfi_error_log_entry *entry, *tmp;

		cancel_work_sync(&ci->offline_work);
		del_timer_sync(&ci->defer_timer);

		list_for_each_entry_safe(entry, tmp, &ci->error_log, list) {
			list_del(&entry->list);
			kfree(entry);
		}
	}

	kvfree(cfi_cpus);
	cfi_cpus = NULL;
}

static int __init cfi_init(void)
{
	int ret;

	pr_info("initializing (ce_thresh=%u uce_thresh=%u window=%us "
		"mce_tolerant=%u)\n",
		cfi_ce_threshold, cfi_uce_threshold, cfi_window_secs,
		cfi_mce_tolerant);

	ret = cfi_alloc_cpus();
	if (ret) {
		pr_err("failed to allocate per-CPU structures: %d\n", ret);
		return ret;
	}

	ret = cfi_netlink_init();
	if (ret) {
		pr_err("failed to register netlink family: %d\n", ret);
		goto err_free_cpus;
	}

	ret = cfi_sysfs_init();
	if (ret) {
		pr_err("failed to create sysfs entries: %d\n", ret);
		goto err_netlink;
	}

	ret = cfi_hotplug_init();
	if (ret) {
		pr_err("failed to register hotplug callbacks: %d\n", ret);
		goto err_sysfs;
	}

	/* debugfs inject interface (non-fatal if it fails) */
	cfi_debugfs_init();

	/*
	 * Suppress default panic behavior BEFORE registering the arch
	 * backend. This ensures that when our MCE handler starts receiving
	 * events, fatal MCEs flow through the decode chain instead of
	 * triggering mce_panic().
	 */
	ret = cfi_suppress_init();
	if (ret) {
		pr_err("failed to initialize panic suppression: %d\n", ret);
		goto err_debugfs;
	}

	/* Select and initialize architecture backend */
#ifdef CONFIG_X86
	cfi_arch = &cfi_x86_ops;
#elif defined(CONFIG_ARM64)
	cfi_arch = &cfi_arm64_ops;
#else
	pr_err("unsupported architecture\n");
	ret = -ENODEV;
	goto err_suppress;
#endif

	if (cfi_arch->init) {
		ret = cfi_arch->init();
		if (ret) {
			pr_err("arch backend init failed: %d\n", ret);
			cfi_arch = NULL;
			goto err_suppress;
		}
	}

	/*
	 * Memory fault domain. Needs panic suppression (fatal memory
	 * MCEs must reach the decode chain) and the sysfs root.
	 */
	ret = mfi_init();
	if (ret) {
		pr_err("memory fault domain init failed: %d\n", ret);
		goto err_arch;
	}

	pr_info("initialized successfully — panic suppression active\n");
	return 0;

err_arch:
	if (cfi_arch && cfi_arch->exit)
		cfi_arch->exit();
	cfi_arch = NULL;
err_suppress:
	cfi_suppress_exit();
err_debugfs:
	cfi_debugfs_exit();
	cfi_hotplug_exit();
err_sysfs:
	cfi_sysfs_exit();
err_netlink:
	cfi_netlink_exit();
err_free_cpus:
	cfi_free_cpus();
	return ret;
}

static void __exit cfi_exit(void)
{
	pr_info("unloading\n");

	/* Memory domain: unregister notifiers/probes, flush offline work */
	mfi_exit();

	/* Unregister arch handler before restoring panic behavior */
	if (cfi_arch && cfi_arch->exit)
		cfi_arch->exit();
	cfi_arch = NULL;

	/* Restore original panic settings */
	cfi_suppress_exit();

	cfi_debugfs_exit();
	cfi_hotplug_exit();
	cfi_sysfs_exit();
	cfi_netlink_exit();
	cfi_free_cpus();

	pr_info("unloaded\n");
}

module_init(cfi_init);
module_exit(cfi_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("openEuler Community");
MODULE_DESCRIPTION("CPU core/cache and memory fault isolation for improved system reliability");
MODULE_VERSION(CFI_VERSION);
