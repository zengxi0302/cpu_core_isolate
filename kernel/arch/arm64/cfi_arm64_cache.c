// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - ARM64 Cache Error Classification
 *
 * Parses ARM processor error information from CPER (Common Platform
 * Error Record) sections reported by GHES/APEI. Classifies errors
 * into the architecture-agnostic CFI error type taxonomy.
 *
 * ARM RAS error types (from UEFI CPER ARM Processor Error Section):
 *   Type 0: Cache error     (with cache level)
 *   Type 1: TLB error       (with TLB level)
 *   Type 2: Bus error
 *   Type 3: Micro-architectural error
 *
 * HiSilicon Kunpeng-specific:
 *   Kunpeng 920/930 reports L3/HHA errors through vendor-specific
 *   CPER sections identified by HiSilicon GUIDs.
 */

#define pr_fmt(fmt) CFI_MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/uuid.h>
#include "../../core/cfi_internal.h"
#include "cfi_arm64.h"

/*
 * Map ARM cache level to cfi_error_type.
 *
 * ARM CPER defines cache level as 0-based (0=L1, 1=L2, 2=L3).
 * If the level field is not valid, we treat it as generic core error.
 */
static u32 cfi_arm64_cache_level_to_type(u8 cache_level)
{
	switch (cache_level) {
	case 0:
		/* L1 - we default to L1D since ARM CPER doesn't always
		 * distinguish I/D cache at this layer */
		return CFI_ERR_CACHE_L1D;
	case 1:
		return CFI_ERR_CACHE_L2;
	case 2:
		return CFI_ERR_CACHE_L3;
	default:
		/* Higher levels or unknown - treat as L3/LLC */
		return CFI_ERR_CACHE_L3;
	}
}

/*
 * Classify an ARM processor error into cfi_error_event fields.
 *
 * @err_type: CPER ARM error info type (0=cache, 1=TLB, 2=bus, 3=uarch)
 * @cache_level: Cache/TLB level (0-based), only valid for cache/TLB types
 * @corrected: True if the error was corrected by hardware
 * @event: Output event structure (partially filled by caller)
 *
 * Returns 0 on success, -1 if the error should be ignored.
 */
int cfi_arm64_classify_error(u8 err_type, u8 cache_level, bool corrected,
			     struct cfi_error_event *event)
{
	switch (err_type) {
	case CPER_ARM_ERR_TYPE_CACHE:
		event->error_type = cfi_arm64_cache_level_to_type(cache_level);
		break;
	case CPER_ARM_ERR_TYPE_TLB:
		event->error_type = CFI_ERR_TLB;
		break;
	case CPER_ARM_ERR_TYPE_BUS:
		event->error_type = CFI_ERR_BUS;
		break;
	case CPER_ARM_ERR_TYPE_MICRO_ARCH:
		event->error_type = CFI_ERR_INTERNAL;
		break;
	default:
		event->error_type = CFI_ERR_GENERIC_CORE;
		break;
	}

	event->severity = corrected ? CFI_SEV_CE : CFI_SEV_UCR;

	return 0;
}

/*
 * Parse HiSilicon vendor-specific CPER section.
 *
 * Kunpeng 920/930 uses vendor-specific CPER sections to report
 * platform errors including:
 *   - L3 Tag errors (HISI_L3TAG_GUID)
 *   - HHA (Hydra Home Agent) errors (HISI_HHA_GUID)
 *   - Common platform errors (HISI_COMMON_GUID)
 *
 * These sections contain SCCL (Super CPU Cluster) ID and error
 * syndrome data specific to the Kunpeng architecture.
 *
 * @sec_type: GUID of the vendor-specific section
 * @data: Raw section data
 * @len: Length of section data
 * @event: Output event structure
 *
 * Returns 0 on success, -1 if not a recognized HiSilicon section.
 */
int cfi_arm64_classify_hisi(const guid_t *sec_type, const void *data,
			    size_t len, struct cfi_error_event *event)
{
	/*
	 * HiSilicon error section format (simplified):
	 * Offset 0x00: u32 val_bits    - Valid field bitmask
	 * Offset 0x04: u8  soc_id      - SoC ID (for multi-chip)
	 * Offset 0x05: u8  socket_id   - Socket ID
	 * Offset 0x06: u8  nimbus_id   - NIMBUS ID (TOTEM in Kunpeng)
	 * Offset 0x07: u8  module_id   - Module ID within SCCL
	 * Offset 0x08: u8  sub_module  - Sub-module ID
	 * Offset 0x09: u8  err_severity - Error severity
	 * ...
	 * Actual format varies by section type. We extract what we can.
	 */

	if (len < 10)
		return -1;

	event->arch_vendor = CFI_VENDOR_HISI;

	if (guid_equal(sec_type, &(const guid_t)HISI_L3TAG_GUID)) {
		/* L3 Tag error - affects entire SCCL sharing this L3 */
		event->error_type = CFI_ERR_CACHE_L3;
		pr_debug("HiSilicon L3 Tag error detected\n");
	} else if (guid_equal(sec_type, &(const guid_t)HISI_HHA_GUID)) {
		/* HHA error - Home Agent, treat as bus/interconnect */
		event->error_type = CFI_ERR_BUS;
		pr_debug("HiSilicon HHA error detected\n");
	} else if (guid_equal(sec_type, &(const guid_t)HISI_COMMON_GUID)) {
		/* Generic HiSilicon platform error */
		event->error_type = CFI_ERR_GENERIC_CORE;
		pr_debug("HiSilicon common platform error detected\n");
	} else {
		return -1;
	}

	/* Extract socket/module info from the section header */
	{
		const u8 *hdr = data;

		event->socket = hdr[5];  /* socket_id */
		/* Store raw section data for daemon analysis */
		memcpy(event->arch_data, data,
		       min_t(size_t, len, sizeof(event->arch_data)));
	}

	return 0;
}
