/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CPU Fault Isolation (CFI) - Internal Kernel Header
 *
 * Private definitions shared among CFI kernel module components.
 * Not exposed to userspace.
 */
#ifndef _CFI_INTERNAL_H
#define _CFI_INTERNAL_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/cpumask.h>
#include <linux/list.h>
#include "../include/uapi/cfi.h"

#define CFI_MODULE_NAME		"cpu_fault_isolate"
#define CFI_VERSION		"0.4.0"
#define CFI_ERROR_LOG_MAX	64	/* Max recent errors per CPU */

/* Forward declarations */
struct cfi_cpu_info;

/*
 * Architecture backend operations.
 * Each arch (x86, arm64) registers one of these at module init.
 */
struct cfi_arch_ops {
	int  (*init)(void);
	void (*exit)(void);
	/* Human-readable description of an error record */
	int  (*describe_error)(const struct cfi_error_event *rec,
			       char *buf, size_t len);
};

/*
 * Per-CPU error log entry (ring buffer element).
 */
struct cfi_error_log_entry {
	struct list_head	list;
	struct cfi_error_event	event;
};

/*
 * Per-CPU isolation tracking structure.
 * One instance per logical CPU, allocated as a flat array at init.
 */
struct cfi_cpu_info {
	spinlock_t		lock;
	enum cfi_cpu_state	state;

	/* Error counters for current window */
	u32			ce_count;
	u32			uce_count;

	/* Lifetime counters */
	u32			ce_count_total;
	u32			uce_count_total;

	/* Sliding window */
	u64			window_start_ns;

	/* Bitmask of cfi_error_type seen in current window */
	u32			error_types_seen;

	/* Operator override: if true, automatic isolation is suppressed */
	bool			user_pinned;

	/* Recent error log (bounded ring) */
	struct list_head	error_log;
	u32			error_log_count;

	/* Deferred isolation work (cpu_down must run in process context) */
	struct work_struct	offline_work;

	/* Timer for daemon ACK timeout */
	struct timer_list	defer_timer;

	/* Set when daemon has ACK'd pre-isolation steps */
	bool			daemon_acked;

	/* Number of times this CPU has been isolated and unisolated */
	u32			isolation_count;
};

/*
 * Module parameters (defined in cfi_main.c, used everywhere).
 */
extern unsigned int cfi_ce_threshold;
extern unsigned int cfi_uce_threshold;
extern unsigned int cfi_window_secs;
extern bool cfi_auto_isolate;
extern bool cfi_defer_to_daemon;
extern unsigned int cfi_defer_timeout_ms;
extern unsigned int cfi_mce_tolerant;
extern bool cfi_offline_bypass;
extern bool cfi_protect_cpu0;
extern bool cfi_soft_isolation;	/* deprecated alias for isolation_mode=soft */

/*
 * Isolation mechanism. Selected by the isolation_mode= modparam at load.
 * Resolved once in cfi_hotplug_init().
 *
 *   FULL     — cpu hotplug offline via remove_cpu() / cpu_device_down.
 *              Strongest. Works in plain VMs and bare-metal kernels that
 *              haven't patched the offline path. Deadlocks on HCE2
 *              physical-host kernels (work_on_cpu-wrapped _cpu_down +
 *              cgroup-v1 cpuset_hotplug_workfn vs cpus_rwsem ABBA).
 *
 *   INACTIVE — set_cpu_active(cpu, false) + IRQ migration. The CPU stays
 *              in cpu_online_mask but the scheduler stops picking it for
 *              new tasks; existing migratable tasks drift off via load
 *              balance. Never enters the hotplug state machine, so it
 *              cannot hit the HCE2 deadlocks. Strength close to FULL for
 *              the "fault cache stops being used" goal.
 *
 *   SOFT     — IRQ migration only (legacy). Weakest; daemon must drive
 *              task/vCPU migration. Kept for environments where
 *              set_cpu_active cannot be resolved.
 */
enum cfi_isol_mode {
	CFI_ISOL_FULL = 0,
	CFI_ISOL_INACTIVE,
	CFI_ISOL_SOFT,
};
extern enum cfi_isol_mode cfi_isolation_mode;
extern char *cfi_isolation_mode_str;	/* raw modparam string, pre-resolve */
const char *cfi_isolation_mode_name(enum cfi_isol_mode m);

/*
 * Global per-CPU info array (allocated in cfi_main.c).
 */
extern struct cfi_cpu_info *cfi_cpus;

/*
 * Architecture backend (set at init by cfi_main.c).
 */
extern const struct cfi_arch_ops *cfi_arch;

/* --- cfi_core.c --- */
void cfi_report_error(struct cfi_error_event *event);
void cfi_reset_cpu_window(struct cfi_cpu_info *ci);
const char *cfi_state_name(enum cfi_cpu_state state);

/* --- cfi_hotplug.c --- */
int  cfi_hotplug_init(void);
void cfi_hotplug_exit(void);
void cfi_begin_isolation(unsigned int cpu, bool urgent);
int  cfi_unisolate_cpu(unsigned int cpu);
void cfi_offline_work_fn(struct work_struct *work);
void cfi_defer_timer_fn(struct timer_list *t);
/* True if this CPU must never be auto-isolated (e.g. CPU0 when protected) */
bool cfi_cpu_is_protected(unsigned int cpu);

/* --- cfi_netlink.c --- */
int  cfi_netlink_init(void);
void cfi_netlink_exit(void);
int  cfi_nl_send_error(const struct cfi_error_event *event);
int  cfi_nl_send_state_change(unsigned int cpu, enum cfi_cpu_state state);

/* --- cfi_sysfs.c --- */
int  cfi_sysfs_init(void);
void cfi_sysfs_exit(void);
/* Root kobject (/sys/kernel/cfi), parent for the MFI "mem" subtree */
struct kobject *cfi_sysfs_root(void);

/* --- cfi_debugfs.c --- */
int  cfi_debugfs_init(void);
void cfi_debugfs_exit(void);

/* --- cfi_hotplug.c (daemon ACK handler, called from netlink) --- */
void cfi_daemon_ack_isolate(unsigned int cpu);

/* --- cfi_panic_suppress.c --- */
int  cfi_suppress_init(void);
void cfi_suppress_exit(void);

/* --- Arch backends (cfi_x86.c / cfi_arm64.c) --- */
#ifdef CONFIG_X86
extern const struct cfi_arch_ops cfi_x86_ops;
#endif
#ifdef CONFIG_ARM64
extern const struct cfi_arch_ops cfi_arm64_ops;
#endif

#endif /* _CFI_INTERNAL_H */
