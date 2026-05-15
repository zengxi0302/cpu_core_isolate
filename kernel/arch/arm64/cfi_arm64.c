// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - ARM64 GHES/APEI Backend
 *
 * Hooks into the kernel's ARM64 RAS error reporting path via
 * tracepoint probes. ARM processor errors flow through the GHES
 * (Generic Hardware Error Source) driver, which emits tracepoints
 * for each error record.
 *
 * Two tracepoints are monitored:
 *   - ras:arm_event       -- Standard ARM processor error sections
 *   - ras:non_standard_event -- Vendor-specific error sections (HiSilicon)
 *
 * For Kunpeng 920/930:
 *   - L1/L2 per-core errors come through ras:arm_event with cache type
 *   - L3 errors come through ras:non_standard_event with HiSilicon GUIDs
 *   - SCCL topology is read from ACPI PPTT via sysfs
 *
 * Coexistence:
 *   - Works alongside rasdaemon (both register on same tracepoints)
 *   - Tracepoints support multiple probe registrations
 */

#define pr_fmt(fmt) CFI_MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/tracepoint.h>
#include <linux/topology.h>
#include <linux/uuid.h>
#include <ras/ras_event.h>
#include "../../core/cfi_internal.h"
#include "cfi_arm64.h"

/*
 * Tracepoint probe for ras:arm_event.
 *
 * This tracepoint is emitted by GHES when processing CPER ARM
 * Processor Error Sections. Each call provides error info including
 * error type (cache/TLB/bus/uarch), cache level, and whether the
 * error was corrected.
 *
 * The trace_arm_event prototype (from include/ras/ras_event.h):
 *   trace_arm_event(struct cper_sec_proc_arm *proc,
 *                   const __u8 *err_data)
 *
 * We parse the ARM processor error section to extract per-error-info
 * entries and classify each one.
 */
static void cfi_arm64_arm_event_probe(void *ignore,
				       const struct cper_sec_proc_arm *proc)
{
	struct cfi_error_event event = {};
	const struct cper_arm_err_info *err_info;
	unsigned int cpu;
	int i;

	if (!proc || !proc->err_info_num)
		return;

	/*
	 * MPIDR_EL1 identifies the PE (Processing Element).
	 * Map MPIDR to logical CPU number.
	 * The MPIDR is in proc->affinity.
	 */
	cpu = raw_smp_processor_id();  /* Approximate; refine from MPIDR if possible */

	event.cpu = cpu;
	event.socket = topology_physical_package_id(cpu);
	event.core_id = topology_core_id(cpu);
	event.thread_id = 0;  /* Kunpeng: no SMT */
	event.arch_vendor = CFI_VENDOR_ARM_GENERIC;
	event.timestamp_ns = ktime_get_ns();

	/*
	 * Process each error info structure in the ARM processor
	 * error section. A single event may contain multiple errors.
	 */
	err_info = (const struct cper_arm_err_info *)(proc + 1);

	for (i = 0; i < proc->err_info_num; i++) {
		u8 err_type;
		u8 cache_level = 0;
		bool corrected;

		err_type = err_info[i].type;
		corrected = !(err_info[i].flags & 0x1);  /* Bit 0: uncorrected */

		/* Cache level is in the error info validation bits */
		if (err_info[i].validation_bits & 0x4)  /* Cache level valid */
			cache_level = (err_info[i].flags >> 4) & 0x7;

		if (cfi_arm64_classify_error(err_type, cache_level,
					     corrected, &event))
			continue;

		/* Store error info index for reference */
		event.arch_data[0] = err_type;
		event.arch_data[1] = cache_level;
		event.arch_data[2] = corrected ? 1 : 0;

		pr_debug("cpu%u: ARM RAS type=%u level=%u corrected=%d -> cfi_type=0x%x\n",
			 cpu, err_type, cache_level, corrected,
			 event.error_type);

		cfi_report_error(&event);
	}
}

/*
 * Tracepoint probe for ras:non_standard_event.
 *
 * This handles vendor-specific error sections (HiSilicon Kunpeng).
 * The GHES driver emits this when it encounters a CPER section
 * with a vendor-specific GUID.
 *
 * Prototype:
 *   trace_non_standard_event(const guid_t *sec_type,
 *                            const guid_t *fru_id,
 *                            const char *fru_text,
 *                            u8 sev, const u8 *err, u32 len)
 */
static void cfi_arm64_non_standard_probe(void *ignore,
					  const guid_t *sec_type,
					  const guid_t *fru_id,
					  const char *fru_text,
					  const u8 sev,
					  const u8 *err,
					  const u32 len)
{
	struct cfi_error_event event = {};
	unsigned int cpu = raw_smp_processor_id();

	event.cpu = cpu;
	event.socket = topology_physical_package_id(cpu);
	event.core_id = topology_core_id(cpu);
	event.timestamp_ns = ktime_get_ns();

	/* Map GHES severity to CFI severity */
	switch (sev) {
	case 0:  /* Recoverable / Corrected */
		event.severity = CFI_SEV_CE;
		break;
	case 1:  /* Fatal */
		event.severity = CFI_SEV_UCF;
		break;
	case 2:  /* Corrected */
		event.severity = CFI_SEV_CE;
		break;
	default:
		event.severity = CFI_SEV_UCR;
		break;
	}

	/* Try HiSilicon vendor-specific parsing */
	if (cfi_arm64_classify_hisi(sec_type, err, len, &event) == 0) {
		pr_debug("cpu%u: HiSilicon vendor error type=0x%x sev=%d\n",
			 cpu, event.error_type, event.severity);
		cfi_report_error(&event);
	}
}

static int cfi_arm64_describe_error(const struct cfi_error_event *rec,
				     char *buf, size_t len)
{
	const char *level = "unknown";
	const char *sev = "CE";
	const char *vendor = "ARM";

	switch (rec->error_type) {
	case CFI_ERR_CACHE_L1D: level = "L1 cache"; break;
	case CFI_ERR_CACHE_L1I: level = "L1I cache"; break;
	case CFI_ERR_CACHE_L2:  level = "L2 cache"; break;
	case CFI_ERR_CACHE_L3:  level = "L3 cache"; break;
	case CFI_ERR_TLB:       level = "TLB"; break;
	case CFI_ERR_BUS:       level = "Bus"; break;
	case CFI_ERR_INTERNAL:  level = "Micro-arch"; break;
	default:                level = "Core"; break;
	}

	if (rec->severity == CFI_SEV_UCR)
		sev = "UCR";
	else if (rec->severity == CFI_SEV_UCF)
		sev = "UCF";

	if (rec->arch_vendor == CFI_VENDOR_HISI)
		vendor = "HiSilicon";

	return snprintf(buf, len, "%s %s %s error on cpu%u (socket %u core %u)",
			vendor, sev, level, rec->cpu, rec->socket, rec->core_id);
}

static int cfi_arm64_init(void)
{
	int ret;

	pr_info("registering ARM64 RAS tracepoint probes\n");

	ret = register_trace_arm_event(cfi_arm64_arm_event_probe, NULL);
	if (ret) {
		pr_err("failed to register arm_event tracepoint: %d\n", ret);
		return ret;
	}

	ret = register_trace_non_standard_event(cfi_arm64_non_standard_probe,
						NULL);
	if (ret) {
		pr_warn("failed to register non_standard_event tracepoint: %d "
			"(HiSilicon vendor errors will not be tracked)\n", ret);
		/* Non-fatal: standard ARM events still work */
	}

	return 0;
}

static void cfi_arm64_exit(void)
{
	unregister_trace_arm_event(cfi_arm64_arm_event_probe, NULL);
	unregister_trace_non_standard_event(cfi_arm64_non_standard_probe, NULL);
	tracepoint_synchronize_unregister();

	pr_info("unregistered ARM64 RAS tracepoint probes\n");
}

const struct cfi_arch_ops cfi_arm64_ops = {
	.init		= cfi_arm64_init,
	.exit		= cfi_arm64_exit,
	.describe_error	= cfi_arm64_describe_error,
};
