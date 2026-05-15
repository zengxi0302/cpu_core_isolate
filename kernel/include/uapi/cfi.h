/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * CPU Fault Isolation (CFI) - UAPI Header
 *
 * Shared definitions between kernel module and userspace daemon/tools.
 * This header defines the generic netlink protocol, error types, and
 * state enumerations used for CPU core/cache fault isolation.
 *
 * Copyright (c) 2026 openEuler Community
 */
#ifndef _UAPI_CFI_H
#define _UAPI_CFI_H

#include <linux/types.h>

/* Generic netlink family name */
#define CFI_GENL_NAME		"CFI"
#define CFI_GENL_VERSION	1

/* Multicast group for unsolicited events (errors, state changes) */
#define CFI_MCGRP_EVENTS	"events"

/*
 * Error source type - architecture-agnostic classification.
 * Arch backends map hardware-specific error codes to these types.
 */
enum cfi_error_type {
	CFI_ERR_CACHE_L1D	= 0x01,	/* L1 data cache error */
	CFI_ERR_CACHE_L1I	= 0x02,	/* L1 instruction cache error */
	CFI_ERR_CACHE_L2	= 0x04,	/* L2 unified cache error */
	CFI_ERR_CACHE_L3	= 0x08,	/* L3 / last-level cache error */
	CFI_ERR_TLB		= 0x10,	/* TLB error */
	CFI_ERR_BUS		= 0x20,	/* Bus/interconnect error */
	CFI_ERR_INTERNAL	= 0x40,	/* Micro-architectural internal parity */
	CFI_ERR_GENERIC_CORE	= 0x80,	/* Unclassified core-scoped error */
};

/*
 * Error severity levels.
 * CE: hardware corrected automatically, logged for trending.
 * UCR: uncorrected but recoverable (e.g., data can be re-fetched).
 * UCF: uncorrected fatal, processor context may be corrupt.
 */
enum cfi_error_severity {
	CFI_SEV_CE	= 0,	/* Corrected Error */
	CFI_SEV_UCR	= 1,	/* Uncorrected Recoverable */
	CFI_SEV_UCF	= 2,	/* Uncorrected Fatal */
};

/*
 * Per-CPU isolation state.
 * Transitions: ONLINE -> DEGRADED -> ISOLATING -> ISOLATED
 *                                               -> FAILED
 */
enum cfi_cpu_state {
	CFI_STATE_ONLINE	= 0,	/* Normal operation */
	CFI_STATE_DEGRADED	= 1,	/* Errors accumulating, under watch */
	CFI_STATE_ISOLATING	= 2,	/* Isolation in progress (draining) */
	CFI_STATE_ISOLATED	= 3,	/* CPU offlined by this module */
	CFI_STATE_FAILED	= 4,	/* Isolation attempted but failed */
};

/* Architecture vendor identifiers */
enum cfi_arch_vendor {
	CFI_VENDOR_INTEL	= 0,
	CFI_VENDOR_AMD		= 1,
	CFI_VENDOR_ARM_GENERIC	= 2,
	CFI_VENDOR_HISI		= 3,	/* HiSilicon Kunpeng */
};

/*
 * Error record exchanged between kernel and userspace via netlink.
 * Arch backends fill this from hardware-specific structures.
 */
struct cfi_error_event {
	__u32	cpu;		/* Logical CPU that reported the error */
	__u32	socket;		/* Physical package / socket id */
	__u32	core_id;	/* Physical core within package */
	__u32	thread_id;	/* SMT thread id (0 for non-SMT) */
	__u32	error_type;	/* Bitmask of enum cfi_error_type */
	__u8	severity;	/* enum cfi_error_severity */
	__u8	arch_vendor;	/* enum cfi_arch_vendor */
	__u8	reserved[2];
	__u64	misc;		/* Arch-specific auxiliary data */
	__u64	addr;		/* Fault address (0 if not available) */
	__u64	timestamp_ns;	/* Kernel timestamp (CLOCK_MONOTONIC) */
	__u8	arch_data[64];	/* Opaque arch-specific blob */
} __attribute__((packed));

/*
 * Generic netlink commands.
 */
enum cfi_nl_cmd {
	CFI_CMD_UNSPEC		= 0,
	CFI_CMD_ERROR_EVENT	= 1,	/* K->U: new error record */
	CFI_CMD_STATE_CHANGE	= 2,	/* K->U: CPU state transition */
	CFI_CMD_GET_STATUS	= 3,	/* U->K: query CPU state */
	CFI_CMD_GET_STATUS_REPLY = 4,	/* K->U: reply to GET_STATUS */
	CFI_CMD_SET_POLICY	= 5,	/* U->K: update thresholds */
	CFI_CMD_ISOLATE		= 6,	/* U->K: force isolate a CPU */
	CFI_CMD_UNISOLATE	= 7,	/* U->K: bring CPU back online */
	CFI_CMD_ACK_ISOLATE	= 8,	/* U->K: daemon confirms pre-isolation done */
	__CFI_CMD_MAX,
};
#define CFI_CMD_MAX	(__CFI_CMD_MAX - 1)

/*
 * Generic netlink attributes.
 */
enum cfi_nl_attr {
	CFI_ATTR_UNSPEC		= 0,
	CFI_ATTR_CPU		= 1,	/* u32: logical CPU id */
	CFI_ATTR_STATE		= 2,	/* u8: enum cfi_cpu_state */
	CFI_ATTR_ERROR_REC	= 3,	/* binary: struct cfi_error_event */
	CFI_ATTR_CE_THRESH	= 4,	/* u32: corrected error threshold */
	CFI_ATTR_UCE_THRESH	= 5,	/* u32: uncorrected error threshold */
	CFI_ATTR_WINDOW_SEC	= 6,	/* u32: counting window in seconds */
	CFI_ATTR_CE_COUNT	= 7,	/* u32: current CE count */
	CFI_ATTR_UCE_COUNT	= 8,	/* u32: current UCE count */
	CFI_ATTR_CE_TOTAL	= 9,	/* u32: lifetime CE count */
	CFI_ATTR_ERR_TYPES	= 10,	/* u32: bitmask of error types seen */
	CFI_ATTR_USER_PINNED	= 11,	/* u8: operator prevented isolation */
	CFI_ATTR_SOCKET		= 12,	/* u32: socket id */
	CFI_ATTR_CORE_ID	= 13,	/* u32: physical core id */
	CFI_ATTR_PAD		= 14,
	__CFI_ATTR_MAX,
};
#define CFI_ATTR_MAX	(__CFI_ATTR_MAX - 1)

#endif /* _UAPI_CFI_H */
