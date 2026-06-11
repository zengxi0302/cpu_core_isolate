// SPDX-License-Identifier: GPL-2.0
/*
 * Memory Fault Isolation (MFI) - x86 MCA/MCE Memory Error Backend
 *
 * Registers a second notifier on the MCE decode chain (alongside the
 * CPU domain's cfi_x86.c) and extracts memory errors:
 *
 *   - Memory controller errors: MCACOD pattern 0000_0001_MMMM_CCCC
 *     (0x0080-0x00FF), including patrol scrub (0x00C0-0x00CF)
 *   - Poison consumption reported through cache banks: the canonical
 *     SRAR signatures data load (0x0134) / instruction fetch (0x0150)
 *     and the SRAO L3 explicit writeback (0x017A)
 *
 * Severity mapping:
 *   !UC                          -> MFI_MEM_CE
 *   UC && (AR=0 or AMD deferred) -> MFI_MEM_UCE_DEFERRED (not consumed)
 *   UC && AR=1                   -> MFI_MEM_UCE_CONSUMED (SRAR)
 *
 * Kernel-context consumed UCEs are handed to the Phase-2 triage path
 * first (when mem_triage=1). With CFI's panic suppression active these
 * MCEs reach the decode chain instead of mce_panic(); without triage
 * the kernel would silently continue past corrupted data, so triage is
 * the component that restores a safe verdict (recover or panic).
 *
 * The decode chain notifier runs in process context (mce_gen_pool
 * work), so it is safe to take locks and call memory_failure_queue().
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <linux/notifier.h>
#include <asm/mce.h>
#include "../../core/cfi_internal.h"
#include "../../core/mfi_internal.h"

/* True if the MCACOD signature identifies a memory error with an address */
static bool mfi_x86_is_memory_error(u16 mcacod)
{
	/* Memory controller errors: 0000_0001_MMMM_CCCC */
	if ((mcacod & 0xff80) == 0x0080)
		return true;

	/* Patrol scrub (subset of the above, kept explicit for clarity) */
	if ((mcacod & MCACOD_SCRUBMSK) == MCACOD_SCRUB)
		return true;

	/* Poison consumption signatures reported via cache banks */
	if (mcacod == MCACOD_DATA || mcacod == MCACOD_INSTR ||
	    mcacod == MCACOD_L3WB)
		return true;

	return false;
}

static int mfi_x86_mce_notifier(struct notifier_block *nb,
				unsigned long val, void *data)
{
	struct mce *m = (struct mce *)data;
	struct mfi_mem_error err = {};
	u16 mcacod;
	bool kernel_ctx;

	if (!m || !mfi_enable)
		return NOTIFY_DONE;

	if (!(m->status & MCI_STATUS_VAL))
		return NOTIFY_DONE;

	mcacod = m->status & MCACOD;
	if (!mfi_x86_is_memory_error(mcacod))
		return NOTIFY_DONE;

	/* Page accounting needs a physical address */
	if (!(m->status & MCI_STATUS_ADDRV))
		return NOTIFY_DONE;

	err.addr = m->addr;
	err.pfn = m->addr >> PAGE_SHIFT;
	err.cpu = m->extcpu;

	kernel_ctx = !(m->cs & 3);
	if (kernel_ctx)
		err.flags |= MFI_EVF_KERNEL_CTX;

	if (!(m->status & MCI_STATUS_UC)) {
		err.type = MFI_MEM_CE;
	} else if ((m->status & MCI_STATUS_DEFERRED) ||
		   !(m->status & MCI_STATUS_AR)) {
		/* Not yet consumed: SRAO / patrol scrub / AMD deferred */
		err.type = MFI_MEM_UCE_DEFERRED;
	} else {
		err.type = MFI_MEM_UCE_CONSUMED;
	}

	pr_debug("mem: MCE bank %d mcacod=0x%04x pfn=0x%lx type=%d kctx=%d\n",
		 m->bank, mcacod, err.pfn, err.type, kernel_ctx);

	/*
	 * Phase-2 triage: a UCE consumed in kernel context has no safe
	 * default once panic suppression is active. Hand the verdict to
	 * the triage engine before regular accounting; it either queues
	 * recovery (we tag the event TRIAGED so mfi_core does not queue
	 * a duplicate) or escalates to a controlled panic and never
	 * returns.
	 */
	if (err.type == MFI_MEM_UCE_CONSUMED && kernel_ctx && mfi_triage) {
		if (mfi_triage_kernel_uce(err.pfn,
					  !!(m->mcgstatus & MCG_STATUS_RIPV),
					  !!(m->status & MCI_STATUS_PCC),
					  err.cpu))
			err.flags |= MFI_EVF_TRIAGED;
	}

	mfi_report_mem_error(&err);

	/* Never NOTIFY_STOP: let EDAC/mcelog also see the record */
	return NOTIFY_OK;
}

static struct notifier_block mfi_x86_mce_nb = {
	.notifier_call	= mfi_x86_mce_notifier,
	/* Just below the CPU domain handler; both must precede EDAC */
	.priority	= MCE_PRIO_EDAC + 1,
};

int mfi_x86_init(void)
{
	mce_register_decode_chain(&mfi_x86_mce_nb);
	pr_info("mem: x86 MCE memory error backend registered\n");
	return 0;
}

void mfi_x86_exit(void)
{
	mce_unregister_decode_chain(&mfi_x86_mce_nb);
}
