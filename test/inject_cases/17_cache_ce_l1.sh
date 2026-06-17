#!/bin/bash
# Case 17 — L1 Cache CE accumulation (expect no isolation)
#
#   path     : mce_inject sw -> x86_mce_decoder_chain (default)
#              --hw -> mce-inject(8) raise -> do_machine_check (CMC path)
#   bank     : 1 (DCU)
#   status   : 0x900000000000000d  (VAL|EN | L1, no UC)
#
#   no  CFI  : EDAC may log.
#   has CFI  : cfi ce_count++ on the reporting CPU. CE alone never isolates
#              (matches case 06 for L2 CE). Verifies that L1 CE classification
#              path is wired without triggering isolation.
#
#   PLATFORM LIMITATION (--hw mode only): HCE2 + FMA routes CE injection
#   through CMC IRQ -> machine_check_poll, where FMA / SMM scrubs MCi_STATUS
#   before the poll reads it. Expected on HCE2: "cfi ce_count: 0 -> 0  [delta=0]"
#   under --hw — not a script bug, firmware-first interception. Default sw
#   mode works.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
TC=$(safe_cpu_alt)
status_banner "17 L1 Cache CE on cpu$TC (bank=1 DCU, status=$STAT_CACHE_CE_L1)"
require_mce_inject

CE0=$(cpu_attr "$TC" ce_count)
ON0=$(cpu_online "$TC")
dmesg_mark

for i in 1 2; do
    mce_inject_dispatch "$TC" 1 "$STAT_CACHE_CE_L1" 0 0
    sleep 1
done
sleep 1

CE1=$(cpu_attr "$TC" ce_count)
ON1=$(cpu_online "$TC")

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi ce_count" "$CE0" "$CE1"
printf "    %-26s: %s -> %s\n" "online"       "$ON0" "$ON1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'
