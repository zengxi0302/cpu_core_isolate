#!/bin/bash
# Case 01 — Memory CE (correctable ECC)
#
#   path     : mce_inject sw  ->  x86_mce_decoder_chain  ->  EDAC + (cfi mfi)
#   bank     : 4 (Intel memory controller)
#   status   : 0x9c00000000000094  (VAL|EN|MISCV|ADDRV | mem 0x094)
#   pages    : a self-owned 4 KiB anonymous page, injected 3x
#
#   no  CFI  : EDAC counts; page stays in use; system unaffected.
#   has CFI  : mfi accounts each CE -> page_ce_threshold (default 3) triggers
#              soft_offline_page on that pfn; daemon receives MEM_PAGE_OFFLINED.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
status_banner "01 Memory CE (sw flag, bank=4, status=$STAT_MEM_CE)"
require_mce_inject

CE0=$(mfi_stat ce_total)
PRE0=$(mfi_stat pages_pre_offlined)
own_page mem_ce
dmesg_mark

for i in 1 2 3; do
    mce_submit sw 4 "$STAT_MEM_CE" "$PADDR" 0 0
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
