#!/bin/bash
# Case 20 — Memory CE via APEI EINJ (correctable ECC)
#
#   path     : EINJ -> firmware error injection -> GHES/APEI -> EDAC + (cfi mfi)
#   type     : 0x00000008 (Memory Correctable)
#   target   : a self-owned 4 KiB anonymous page, injected 3x
#
#   Equivalent to cases 01/10 but uses APEI EINJ instead of mce-inject.
#   EINJ goes through firmware — the error record is a proper CPER
#   (Common Platform Error Record) processed by GHES, so the entire
#   EDAC + memory_failure + CFI notification chain is exercised.
#
#   no  CFI  : EDAC counts; page stays in use; system unaffected.
#   has CFI  : mfi accounts each CE -> page_ce_threshold (default 3) triggers
#              soft_offline_page on that pfn; daemon receives MEM_PAGE_OFFLINED.
#
#   Advantage over mce-inject: no FMA/SMM scrub issue — EINJ CE injection
#   is firmware-native and not subject to the CMC IRQ polling race that
#   makes hw CE invisible on HCE2 platforms.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env_einj.sh
einj_banner "20 EINJ Memory CE (type=$EINJ_MEM_CE)"
require_einj
require_einj_type "$EINJ_MEM_CE" "Memory Correctable"

CE0=$(mfi_stat ce_total)
PRE0=$(mfi_stat pages_pre_offlined)
own_page einj_mem_ce
dmesg_mark

for i in 1 2 3; do
    einj_inject_mem "$EINJ_MEM_CE" "$PADDR"
    sleep 1
done
sleep 2

CE1=$(mfi_stat ce_total)
PRE1=$(mfi_stat pages_pre_offlined)

echo
echo "  observations:"
observe_row "mfi ce_total"           "$CE0"  "$CE1"
observe_row "mfi pages_pre_offlined" "$PRE0" "$PRE1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

own_page_release
