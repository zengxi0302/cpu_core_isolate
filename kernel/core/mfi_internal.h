/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Memory Fault Isolation (MFI) - Internal Kernel Header
 *
 * Private definitions for the memory fault domain of the
 * cpu_fault_isolate module. Shares the panic-suppression layer,
 * netlink family and sysfs root with the CPU domain (CFI).
 */
#ifndef _MFI_INTERNAL_H
#define _MFI_INTERNAL_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include "../include/uapi/cfi.h"

#define MFI_PAGE_HASH_BITS	8	/* 256 buckets */
#define MFI_PAGE_MAX_ENTRIES	1024	/* bounded; LRU eviction beyond this */
#define MFI_DIMM_MAX_ENTRIES	64
#define MFI_DIMM_LABEL_LEN	32

/*
 * One tracked physical page. Entries live in a bounded hash table
 * keyed by PFN, with an LRU list for eviction.
 */
struct mfi_page_entry {
	struct hlist_node	hash;
	struct list_head	lru;
	unsigned long		pfn;
	enum mfi_page_state	state;
	u32			ce_count;
	u64			window_start_ns;
	u64			last_seen_ns;
};

/*
 * Per-DIMM accounting. The label comes from the EDAC mc_event
 * tracepoint; counters use the same sliding-window scheme as pages.
 */
struct mfi_dimm_entry {
	struct list_head	list;
	char			label[MFI_DIMM_LABEL_LEN];
	u32			ce_count;
	u32			uce_count;
	u64			window_start_ns;
	bool			advised;	/* MIGRATE_ADVISED already sent */
};

/* Global statistics (atomic; snapshotted into struct mfi_stats_rec) */
struct mfi_stats {
	atomic64_t	ce_total;
	atomic64_t	uce_async;
	atomic64_t	uce_consumed;
	atomic64_t	pages_pre_offlined;
	atomic64_t	pages_offlined;
	atomic64_t	pages_failed;
	atomic64_t	triage_saved;
	atomic64_t	triage_panic;
};

/*
 * Internal memory error descriptor, filled by event sources
 * (x86 MCE decode chain, EDAC tracepoint, debugfs injection) and
 * passed to mfi_report_mem_error().
 */
struct mfi_mem_error {
	unsigned long	pfn;
	u64		addr;
	unsigned int	cpu;
	enum mfi_mem_err_type	type;
	u8		flags;		/* MFI_EVF_* */
	const char	*dimm_label;	/* NULL if unknown */
};

/* --- Module parameters (defined in cfi_main.c) --- */
extern bool mfi_enable;
extern bool mfi_pre_isolate;
extern unsigned int mfi_page_ce_threshold;
extern unsigned int mfi_window_secs;
extern unsigned int mfi_dimm_ce_threshold;
extern unsigned int mfi_dimm_uce_threshold;
extern bool mfi_triage;

extern struct mfi_stats mfi_stats;

/* --- mfi_core.c --- */
/* Whole-domain init/exit, called from cfi_main.c */
int  mfi_init(void);
void mfi_exit(void);
int  mfi_core_init(void);
void mfi_core_exit(void);
void mfi_report_mem_error(const struct mfi_mem_error *err);
const char *mfi_page_state_name(enum mfi_page_state state);
u64  mfi_pages_watched(void);
void mfi_stats_snapshot(struct mfi_stats_rec *rec);
/* Called by mfi_page.c when an offline attempt concludes */
void mfi_page_offline_done(unsigned long pfn, bool pre_isolate, bool success);

/* --- mfi_page.c --- */
int  mfi_page_init(void);
void mfi_page_exit(void);
bool mfi_page_is_hwpoison(unsigned long pfn);
/* Queue proactive soft offline (returns false if mechanism unavailable) */
bool mfi_page_soft_offline(unsigned long pfn);
/* Queue hard offline via memory_failure_queue() */
void mfi_page_hard_offline(unsigned long pfn);
bool mfi_soft_offline_available(void);

/* --- mfi_dimm.c --- */
int  mfi_dimm_init(void);
void mfi_dimm_exit(void);
void mfi_dimm_account(const char *label, bool uce);
int  mfi_dimm_show(char *buf, size_t len);

/* --- mfi_sysfs.c --- */
int  mfi_sysfs_init(struct kobject *parent);
void mfi_sysfs_exit(void);

/* --- cfi_netlink.c (MEM event senders) --- */
int  cfi_nl_send_mem_event(u8 cmd, const struct mfi_mem_event *event);
int  cfi_nl_send_mem_migrate_advised(const char *dimm_label);

/* --- arch backends --- */
#ifdef CONFIG_X86
int  mfi_x86_init(void);
void mfi_x86_exit(void);
#endif

#endif /* _MFI_INTERNAL_H */
