// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - x86 Cache Error Classification
 *
 * Parses MCA error codes from IA32_MCi_STATUS to classify cache errors
 * into the architecture-agnostic cfi_error_type taxonomy.
 *
 * MCA error code format (bits [15:0] of MCi_STATUS):
 *   Compound errors (bit 15..11 = 00001):
 *     LL  [1:0]  = Cache level (00=L0, 01=L1, 10=L2, 11=L3/generic)
 *     TT  [3:2]  = Transaction (00=Instr, 01=Data, 10=Generic)
 *     RRRR[7:4]  = Request type
 *   Simple errors:
 *     0x000C-0x000F = Cache hierarchy errors by level
 *
 * Intel additional:
 *   Cache "yellow/green" states based on affected line count trends.
 *   Green = below threshold, Yellow = above threshold (cache degraded).
 *
 * AMD additional:
 *   EC-TED (Error Correction - Table Entry Decoder) for L2/L3.
 *   Data poisoning for uncorrectable cache errors.
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <asm/mce.h>
#include "../../core/cfi_internal.h"
#include "cfi_x86.h"

/*
 * Map MCA cache level code to cfi_error_type.
 *
 * @ll: Cache level field from MCA error code (bits [1:0])
 * @tt: Transaction type field (bits [3:2])
 *
 * Returns: cfi_error_type bitmask, or 0 if not a cache error.
 */
static u32 cfi_x86_ll_to_type(unsigned int ll, unsigned int tt)
{
	switch (ll) {
	case MCI_ERR_LL_L0:
		/* L0 is rarely used; treat as L1 */
		/* fallthrough */
	case MCI_ERR_LL_L1:
		if (tt == MCI_ERR_TT_INSTR)
			return CFI_ERR_CACHE_L1I;
		return CFI_ERR_CACHE_L1D;
	case MCI_ERR_LL_L2:
		return CFI_ERR_CACHE_L2;
	case MCI_ERR_LL_LG:
		return CFI_ERR_CACHE_L3;
	default:
		return 0;
	}
}

/*
 * Classify an MCA error code into cfi_error_type.
 *
 * Handles both compound error codes (cache/TLB/bus/memory) and
 * simple cache hierarchy error codes.
 *
 * Returns: cfi_error_type bitmask.
 */
static u32 cfi_x86_classify_errcode(u16 errcode)
{
	/* Simple cache hierarchy errors: 0x000C-0x000F */
	if (MCI_ERR_IS_CACHE_SIMPLE(errcode))
		return cfi_x86_ll_to_type(MCI_ERR_LL(errcode),
					  MCI_ERR_TT_GENERIC);

	/* Compound errors */
	if (MCI_ERR_IS_COMPOUND(errcode)) {
		unsigned int ll = MCI_ERR_LL(errcode);
		unsigned int tt = MCI_ERR_TT(errcode);
		unsigned int rrrr = MCI_ERR_RRRR(errcode);

		/*
		 * RRRR field indicates the type of request:
		 * 0000 = Generic error
		 * 0001 = Generic read
		 * 0010 = Generic write
		 * 0011 = Data read
		 * 0100 = Data write
		 * 0101 = Instruction fetch
		 * 0110 = Prefetch
		 * 0111 = Eviction
		 * 1000 = Snoop
		 */
		(void)rrrr;  /* Used for detailed logging, not classification */

		/* TLB errors: LL=11 and specific RRRR patterns */
		if (errcode & 0x0010)
			return CFI_ERR_TLB;

		/* Bus/interconnect errors */
		if (errcode & 0x0800)
			return CFI_ERR_BUS;

		/* Cache-related compound errors */
		return cfi_x86_ll_to_type(ll, tt);
	}

	/* Internal unclassified / micro-architectural error */
	if (errcode >= 0x0400 && errcode <= 0x0FFF)
		return CFI_ERR_INTERNAL;

	return CFI_ERR_GENERIC_CORE;
}

/*
 * Classify a full MCE record into a cfi_error_event.
 *
 * Extracts error type, severity, CPU topology, and address
 * from the kernel's struct mce.
 *
 * Returns 0 on success, -1 if the MCE should be ignored.
 */
int cfi_x86_classify_mce(const struct mce *m, struct cfi_error_event *event)
{
	u64 status = m->status;
	u16 errcode;

	/* Only process valid status entries */
	if (!(status & MCI_STATUS_VAL))
		return -1;

	memset(event, 0, sizeof(*event));

	/* CPU topology */
	event->cpu = m->extcpu;
	event->socket = m->socketid;
	event->core_id = m->cpuid;  /* Note: this is CPUID, map via topology */
	event->timestamp_ns = ktime_get_ns();

	/* Vendor identification */
	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL)
		event->arch_vendor = CFI_VENDOR_INTEL;
	else if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD)
		event->arch_vendor = CFI_VENDOR_AMD;

	/* Error classification from MCA error code */
	errcode = MCI_STATUS_ERRCODE(status);
	event->error_type = cfi_x86_classify_errcode(errcode);

	/* Severity */
	if (status & MCI_STATUS_UC) {
		if (status & MCI_STATUS_PCC)
			event->severity = CFI_SEV_UCF;
		else
			event->severity = CFI_SEV_UCR;
	} else {
		event->severity = CFI_SEV_CE;
	}

	/* Fault address */
	if (status & MCI_STATUS_ADDRV)
		event->addr = m->addr;

	/* Store raw MCA status in misc field for detailed logging */
	event->misc = status;

	/* Copy bank number and raw data for arch-specific analysis */
	if (sizeof(event->arch_data) >= sizeof(u8) + sizeof(u64)) {
		event->arch_data[0] = m->bank;
		memcpy(&event->arch_data[1], &m->status, sizeof(u64));
	}

	return 0;
}
