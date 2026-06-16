#!/bin/bash
# Case 14 — Real #MC L2 Cache CE (hw mode)
#
#   path     : mce_inject hw  ->  do_machine_check (CMC/corrected handler)
#              -> mce_severity = MCE_NO_SEVERITY
#              -> x86_mce_decoder_chain
#   bank     : 3
#   status   : 0x900000000000000e  (VAL|EN | L2)
#
#   no  CFI  : kernel CMC handler logs; no panic; no isolation.
#   has CFI  : cfi ce_count++ on the originating cpu; CE alone never
#              isolates (matches case 06 for the real-#MC path).
#
#   prereq   : hw-mode delivery functional.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
TC=$(( $(safe_cpu) + 1 ))
[[ $TC -ge $(nproc) ]] && TC=$(( $(safe_cpu) - 1 ))
status_banner "14 hw L2 Cache CE on cpu$TC (hw flag, bank=3, status=$STAT_CACHE_CE_L2)"
require_mce_inject

CE0=$(cpu_attr "$TC" ce_count)
ON0=$(cpu_online "$TC")
dmesg_mark

mce_submit hw 3 "$STAT_CACHE_CE_L2" 0 0 "$TC"
sleep 2

CE1=$(cpu_attr "$TC" ce_count)
ON1=$(cpu_online "$TC")

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi ce_count" "$CE0" "$CE1"
printf "    %-26s: %s -> %s\n" "online"       "$ON0" "$ON1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'
