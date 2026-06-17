#!/bin/bash
# Case 21 — Memory UCE non-fatal via APEI EINJ (SRAO equivalent)
#
#   path     : EINJ -> firmware -> GHES/APEI -> memory_failure (async)
#   type     : 0x00000010 (Memory Uncorrectable non-fatal)
#   target   : a self-owned 4 KiB anonymous page
#
#   Equivalent to cases 02/11 but uses APEI EINJ.
#   EINJ Memory UCE non-fatal maps to the kernel's "action optional" (AO)
#   severity — the firmware signals an uncorrectable error that doesn't
#   require immediate action. The kernel queues memory_failure asynchronously.
#
#   no  CFI  : kernel's GHES handler calls memory_failure on the poisoned pfn;
#              page hard-offlined. No panic (non-fatal).
#   has CFI  : same, plus cfi mfi uce_async++ + netlink event.
#
#   Key difference from mce-inject: EINJ goes through firmware's native
#   error reporting (GHES → CPER → memory_failure), bypassing the MCE
#   broadcast sync mechanism entirely. No sync timeout artifact.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env_einj.sh
einj_banner "21 EINJ Memory UCE non-fatal (type=$EINJ_MEM_UCE_NONFATAL)"
require_einj
require_einj_type "$EINJ_MEM_UCE_NONFATAL" "Memory Uncorrectable non-fatal"

UA0=$(mfi_stat uce_async)
OFF0=$(mfi_stat pages_offlined)
own_page einj_mem_uce_nonfatal
dmesg_mark

einj_inject_mem "$EINJ_MEM_UCE_NONFATAL" "$PADDR"
sleep 3

UA1=$(mfi_stat uce_async)
OFF1=$(mfi_stat pages_offlined)
OWNER_ALIVE=$(kill -0 "$PFN_OWNER_PID" 2>/dev/null && echo "alive" || echo "killed")

echo
echo "  observations:"
observe_row "mfi uce_async"      "$UA0"  "$UA1"
observe_row "mfi pages_offlined" "$OFF0" "$OFF1"
echo "    owner process           : $OWNER_ALIVE"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

own_page_release
