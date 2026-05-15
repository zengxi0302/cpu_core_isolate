/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CPU Fault Isolation (CFI) - x86 Architecture Backend Header
 */
#ifndef _CFI_X86_H
#define _CFI_X86_H

#ifdef CONFIG_X86

#include <asm/mce.h>

/*
 * MCA error code field definitions (IA32_MCi_STATUS bits [15:0]).
 *
 * The MCA error code is a standardized encoding:
 *
 * Simple errors (bit 15 = 0):
 *   Bits [3:0] = Type-specific info
 *
 * Compound errors (bits [15:11] = 00001):
 *   Bits [1:0] = LL (Cache Level): 00=L0, 01=L1, 10=L2, 11=LG(generic/L3)
 *   Bits [3:2] = TT (Transaction Type): 00=Instruction, 01=Data, 10=Generic
 *   Bits [7:4] = RRRR (Request Type)
 *   Bits [8]   = PP (Participation)
 *   Bits [9]   = T (Timeout)
 *   Bits [10]  = II (Memory/IO)
 *
 * Cache hierarchy errors: 000F 000C-000F
 *   0x000C = L0 cache (reserved on most CPUs)
 *   0x000D = L1 cache
 *   0x000E = L2 cache
 *   0x000F = L3/generic cache
 */

/* Compound error type check */
#define MCI_ERR_IS_COMPOUND(ec)		(((ec) & 0xF800) == 0x0800)

/* Cache level from compound error code */
#define MCI_ERR_LL(ec)			((ec) & 0x3)
#define MCI_ERR_LL_L0			0x0
#define MCI_ERR_LL_L1			0x1
#define MCI_ERR_LL_L2			0x2
#define MCI_ERR_LL_LG			0x3	/* L3 or generic */

/* Transaction type from compound error code */
#define MCI_ERR_TT(ec)			(((ec) >> 2) & 0x3)
#define MCI_ERR_TT_INSTR		0x0
#define MCI_ERR_TT_DATA		0x1
#define MCI_ERR_TT_GENERIC		0x2

/* Request type from compound error code */
#define MCI_ERR_RRRR(ec)		(((ec) >> 4) & 0xF)

/* Simple cache error codes (bits [15:2] = 0, bits [1:0] = LL) */
#define MCI_ERR_IS_CACHE_SIMPLE(ec)	(((ec) & 0xFFFC) == 0x000C)

/*
 * IA32_MCi_STATUS register bit definitions.
 */
#define MCI_STATUS_VAL		BIT_ULL(63)	/* MCi_STATUS valid */
#define MCI_STATUS_OVER		BIT_ULL(62)	/* Error overflow */
#define MCI_STATUS_UC		BIT_ULL(61)	/* Uncorrected error */
#define MCI_STATUS_EN		BIT_ULL(60)	/* Error enabled */
#define MCI_STATUS_MISCV	BIT_ULL(59)	/* MCi_MISC valid */
#define MCI_STATUS_ADDRV	BIT_ULL(58)	/* MCi_ADDR valid */
#define MCI_STATUS_PCC		BIT_ULL(57)	/* Processor context corrupt */
#define MCI_STATUS_S		BIT_ULL(56)	/* Signaling (UCR capability) */
#define MCI_STATUS_AR		BIT_ULL(55)	/* Action Required */

/* MCA error code (bits [15:0]) */
#define MCI_STATUS_ERRCODE(s)	((s) & 0xFFFF)

/* Model-specific error code (bits [31:16]) */
#define MCI_STATUS_MSCODE(s)	(((s) >> 16) & 0xFFFF)

/* Classify an x86 MCE into cfi_error_event fields */
int cfi_x86_classify_mce(const struct mce *m, struct cfi_error_event *event);

#endif /* CONFIG_X86 */
#endif /* _CFI_X86_H */
