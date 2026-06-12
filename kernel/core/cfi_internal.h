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
extern unsigned int cfi_lockup_thresh_secs;
extern bool cfi_offline_bypass;
extern bool cfi_protect_cpu0;

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
/* True while a CPU offline is executing; lockup isolation must not pile on */
bool cfi_offline_in_progress(void);
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

/* --- cfi_lockup.c --- */
int  cfi_lockup_init(void);
void cfi_lockup_exit(void);

/* --- Arch backends (cfi_x86.c / cfi_arm64.c) --- */
#ifdef CONFIG_X86
extern const struct cfi_arch_ops cfi_x86_ops;
#endif
#ifdef CONFIG_ARM64
extern const struct cfi_arch_ops cfi_arm64_ops;
#endif

#endif /* _CFI_INTERNAL_H */
