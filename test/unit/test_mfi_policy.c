// SPDX-License-Identifier: GPL-2.0
/*
 * Host-side unit tests for the MFI pure policy decisions
 * (kernel/core/mfi_policy.h). No kernel needed:
 *
 *   make && ./test_mfi_policy
 *
 * The triage table below is the contract reviewed in the design
 * document (section 5): recovery only with PCC=0, RIPV=1 and a
 * user/guest or free page; everything else must panic.
 */

#include <stdio.h>

#include "../../kernel/core/mfi_policy.h"

static int failures;

#define CHECK(cond, name) do {						\
	if (cond) {							\
		printf("  ok   %s\n", name);				\
	} else {							\
		printf("  FAIL %s\n", name);				\
		failures++;						\
	}								\
} while (0)

static void test_triage(void)
{
	printf("triage verdicts:\n");

	/* The only recoverable combinations */
	CHECK(mfi_triage_decide(MFI_PG_USER, 1, 0) == MFI_TRIAGE_RECOVER,
	      "user page, RIPV=1, PCC=0 -> recover");
	CHECK(mfi_triage_decide(MFI_PG_FREE, 1, 0) == MFI_TRIAGE_RECOVER,
	      "free page, RIPV=1, PCC=0 -> recover");

	/* PCC always wins */
	CHECK(mfi_triage_decide(MFI_PG_USER, 1, 1) == MFI_TRIAGE_PANIC,
	      "user page but PCC=1 -> panic");
	CHECK(mfi_triage_decide(MFI_PG_FREE, 1, 1) == MFI_TRIAGE_PANIC,
	      "free page but PCC=1 -> panic");

	/* No restart IP: cannot resume the interrupted context */
	CHECK(mfi_triage_decide(MFI_PG_USER, 0, 0) == MFI_TRIAGE_PANIC,
	      "user page but RIPV=0 -> panic");

	/* Kernel-owned or unidentifiable pages are never recoverable */
	CHECK(mfi_triage_decide(MFI_PG_KERNEL, 1, 0) == MFI_TRIAGE_PANIC,
	      "kernel page -> panic");
	CHECK(mfi_triage_decide(MFI_PG_INVALID, 1, 0) == MFI_TRIAGE_PANIC,
	      "invalid pfn -> panic");
	CHECK(mfi_triage_decide(MFI_PG_KERNEL, 0, 1) == MFI_TRIAGE_PANIC,
	      "kernel page, RIPV=0, PCC=1 -> panic");
}

static void test_pre_isolate(void)
{
	printf("pre-isolation gate:\n");

	CHECK(mfi_pre_isolate_decide(8, 8, 1, 1, 1) == 1,
	      "at threshold, all gates open -> offline");
	CHECK(mfi_pre_isolate_decide(9, 8, 1, 1, 1) == 1,
	      "above threshold -> offline");
	CHECK(mfi_pre_isolate_decide(7, 8, 1, 1, 1) == 0,
	      "below threshold -> no action");
	CHECK(mfi_pre_isolate_decide(100, 8, 0, 1, 1) == 0,
	      "page already in flight -> no action");
	CHECK(mfi_pre_isolate_decide(100, 8, 1, 0, 1) == 0,
	      "pre_isolate disabled (e.g. CEC active) -> no action");
	CHECK(mfi_pre_isolate_decide(100, 8, 1, 1, 0) == 0,
	      "soft_offline_page unavailable -> no action");
}

static void test_window(void)
{
	const unsigned long long S = 1000000000ULL;

	printf("window expiry:\n");

	CHECK(mfi_window_expired(100 * S, 0, 60) == 1,
	      "100s elapsed, 60s window -> expired");
	CHECK(mfi_window_expired(59 * S, 0, 60) == 0,
	      "59s elapsed, 60s window -> live");
	CHECK(mfi_window_expired(60 * S, 0, 60) == 0,
	      "exactly 60s -> live (strict >)");
	CHECK(mfi_window_expired(86401ULL * S, 0, 86400) == 1,
	      "default 24h window expires");
}

int main(void)
{
	test_triage();
	test_pre_isolate();
	test_window();

	if (failures) {
		printf("\n%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("\nall tests passed\n");
	return 0;
}
