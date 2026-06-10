// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - x86 MCA/MCE Backend
 *
 * Hooks into the kernel's x86 Machine Check Exception decode chain
 * via mce_register_decode_chain(). All machine check errors (both
 * corrected via CMCI and uncorrected via #MC exception) flow through
 * this notifier chain.
 *
 * This backend translates hardware-specific MCE records (struct mce)
 * into architecture-agnostic CFI error events and feeds them to the
 * core accounting engine.
 *
 * HCE3 adaptation:
 *   Also registers on fma_mce_do_chain (Huawei FMA framework) when
 *   available, providing an additional early notification path for
 *   MCE events during do_machine_check() processing.
 *
 * Coexistence:
 *   - Works alongside rasdaemon and mcelog
 *   - MCE decode chain supports multiple notifiers
 *   - We use high priority (above EDAC) to start isolation ASAP
 */

#define pr_fmt(fmt) CFI_MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/kprobes.h>
#include <asm/mce.h>
#include "../../core/cfi_internal.h"
#include "cfi_x86.h"

/* HCE3 FMA framework support (detected at runtime) */
static bool fma_chain_registered;

/*
 * MCE notifier callback.
 * Called for every machine check event, both corrected (CMCI) and
 * uncorrected (#MC exception). Note: may be called from NMI context
 * for uncorrected errors.
 */
static int cfi_x86_mce_notifier(struct notifier_block *nb,
				 unsigned long val, void *data)
{
	struct mce *m = (struct mce *)data;
	struct cfi_error_event event;

	if (cfi_x86_classify_mce(m, &event))
		return NOTIFY_DONE;

	/* Only process errors we understand (cache, TLB, bus, internal) */
	if (event.error_type == 0)
		return NOTIFY_DONE;

	pr_debug("cpu%u: MCE bank %d errcode=0x%04x type=0x%x sev=%d\n",
		 event.cpu, m->bank,
		 (unsigned int)(m->status & 0xFFFF),
		 event.error_type, event.severity);

	cfi_report_error(&event);

	/*
	 * Return NOTIFY_OK to let other handlers (mcelog, EDAC) also
	 * process this event. We never NOTIFY_STOP.
	 */
	return NOTIFY_OK;
}

static struct notifier_block cfi_x86_mce_nb = {
	.notifier_call	= cfi_x86_mce_notifier,
	/*
	 * Run at high priority to start isolation as early as possible.
	 * With MCE tolerant level raised by cfi_panic_suppress, fatal MCEs
	 * now flow through the decode chain instead of triggering mce_panic().
	 * We must act before other handlers to minimize the window where a
	 * CPU with corrupted context continues executing.
	 */
	.priority	= MCE_PRIO_EDAC + 1,
};

static int cfi_x86_describe_error(const struct cfi_error_event *rec,
				   char *buf, size_t len)
{
	const char *level = "unknown";
	const char *sev = "CE";

	switch (rec->error_type) {
	case CFI_ERR_CACHE_L1D: level = "L1D cache"; break;
	case CFI_ERR_CACHE_L1I: level = "L1I cache"; break;
	case CFI_ERR_CACHE_L2:  level = "L2 cache"; break;
	case CFI_ERR_CACHE_L3:  level = "L3 cache"; break;
	case CFI_ERR_TLB:       level = "TLB"; break;
	case CFI_ERR_BUS:       level = "Bus"; break;
	case CFI_ERR_INTERNAL:  level = "Internal"; break;
	default:                level = "Core"; break;
	}

	switch (rec->severity) {
	case CFI_SEV_CE:  sev = "CE"; break;
	case CFI_SEV_UCR: sev = "UCR"; break;
	case CFI_SEV_UCF: sev = "UCF"; break;
	}

	return snprintf(buf, len, "%s %s error on cpu%u (bank %u, status 0x%llx)",
			sev, level, rec->cpu,
			(unsigned int)rec->arch_data[0], rec->misc);
}

/*
 * Use kprobe to look up a kernel symbol address (same technique as
 * cfi_panic_suppress.c). Needed for runtime-detection of HCE3 symbols.
 */
static unsigned long cfi_x86_lookup_name(const char *name)
{
	struct kprobe kp = {};
	unsigned long addr;

	kp.symbol_name = name;
	if (register_kprobe(&kp) < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

/*
 * HCE3 FMA notifier callback — same logic as the standard decode chain
 * notifier. fma_mce_do_chain fires inside do_machine_check() and may
 * give us an earlier look at the MCE record on HCE3 kernels.
 */
static int cfi_x86_fma_notifier(struct notifier_block *nb,
				 unsigned long val, void *data)
{
	struct mce *m = (struct mce *)data;
	struct cfi_error_event event;

	if (!m)
		return NOTIFY_DONE;

	if (cfi_x86_classify_mce(m, &event))
		return NOTIFY_DONE;

	if (event.error_type == 0)
		return NOTIFY_DONE;

	pr_debug("cpu%u: FMA MCE bank %d errcode=0x%04x type=0x%x sev=%d\n",
		 event.cpu, m->bank,
		 (unsigned int)(m->status & 0xFFFF),
		 event.error_type, event.severity);

	cfi_report_error(&event);

	return NOTIFY_OK;
}

static struct notifier_block cfi_x86_fma_nb = {
	.notifier_call	= cfi_x86_fma_notifier,
	.priority	= INT_MAX,
};

/*
 * Try to register on HCE3's fma_mce_do_chain.
 * This is a best-effort operation — only available on HCE3 kernels.
 */
static void cfi_x86_fma_init(void)
{
	unsigned long addr;
	void (*reg_fn)(struct notifier_block *nb);

	addr = cfi_x86_lookup_name("fma_register_mce_do_chain");
	if (!addr)
		return;

	reg_fn = (void *)addr;
	reg_fn(&cfi_x86_fma_nb);
	fma_chain_registered = true;
	pr_info("registered on HCE3 fma_mce_do_chain\n");
}

static void cfi_x86_fma_exit(void)
{
	unsigned long addr;
	void (*unreg_fn)(struct notifier_block *nb);

	if (!fma_chain_registered)
		return;

	addr = cfi_x86_lookup_name("fma_unregister_mce_do_chain");
	if (!addr)
		return;

	unreg_fn = (void *)addr;
	unreg_fn(&cfi_x86_fma_nb);
	fma_chain_registered = false;
}

static int cfi_x86_init(void)
{
	pr_info("registering x86 MCA/MCE decode chain handler\n");
	mce_register_decode_chain(&cfi_x86_mce_nb);

	/* HCE3: also register on FMA chain for early MCE notification */
	cfi_x86_fma_init();

	return 0;
}

static void cfi_x86_exit(void)
{
	cfi_x86_fma_exit();
	mce_unregister_decode_chain(&cfi_x86_mce_nb);
	pr_info("unregistered x86 MCA/MCE handler\n");
}

const struct cfi_arch_ops cfi_x86_ops = {
	.init		= cfi_x86_init,
	.exit		= cfi_x86_exit,
	.describe_error	= cfi_x86_describe_error,
};
