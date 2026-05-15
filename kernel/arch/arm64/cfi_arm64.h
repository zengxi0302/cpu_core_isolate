/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CPU Fault Isolation (CFI) - ARM64 Architecture Backend Header
 */
#ifndef _CFI_ARM64_H
#define _CFI_ARM64_H

#ifdef CONFIG_ARM64

#include <linux/cper.h>

/*
 * ARM processor error section types (from UEFI/CPER specification).
 * Used to classify errors reported via GHES/APEI.
 */
#define CPER_ARM_ERR_TYPE_CACHE		0
#define CPER_ARM_ERR_TYPE_TLB		1
#define CPER_ARM_ERR_TYPE_BUS		2
#define CPER_ARM_ERR_TYPE_MICRO_ARCH	3

/*
 * HiSilicon Kunpeng vendor-specific CPER section GUID.
 * Used to identify Kunpeng-specific error records from GHES.
 *
 * Kunpeng 920/930 reports platform errors through vendor-specific
 * CPER sections, including L3 cache errors from the HHA (Hydra
 * Home Agent) and SCCL (Super CPU Cluster) errors.
 */
#define HISI_COMMON_GUID \
	GUID_INIT(0xc8b328a8, 0x9917, 0x4af6, \
		  0x9a, 0x13, 0x2e, 0x08, 0xab, 0x46, 0x0e, 0x6c)

/* HiSilicon HHA (Hydra Home Agent) section type */
#define HISI_HHA_GUID \
	GUID_INIT(0xd0709026, 0x814e, 0x11e9, \
		  0x82, 0x77, 0xf0, 0x59, 0x2c, 0x6b, 0x78, 0x50)

/* HiSilicon L3 Tag section type */
#define HISI_L3TAG_GUID \
	GUID_INIT(0xd0709028, 0x814e, 0x11e9, \
		  0x82, 0x77, 0xf0, 0x59, 0x2c, 0x6b, 0x78, 0x50)

/*
 * Classify an ARM processor error info structure into cfi_error_event fields.
 */
int cfi_arm64_classify_error(u8 err_type, u8 cache_level, bool corrected,
			     struct cfi_error_event *event);

/*
 * Parse HiSilicon vendor-specific error section.
 */
int cfi_arm64_classify_hisi(const guid_t *sec_type, const void *data,
			    size_t len, struct cfi_error_event *event);

#endif /* CONFIG_ARM64 */
#endif /* _CFI_ARM64_H */
