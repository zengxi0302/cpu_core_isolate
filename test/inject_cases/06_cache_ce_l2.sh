#!/bin/bash
# Case 06 — L2 Cache CE (accumulation, expect no isolation)
#
#   path     : mce_inject sw  ->  x86_mce_decoder_chain
#   bank     : 3
#   status   : 0x900000000000000e  (VAL|EN | simple-cache 0x000E = L2)
#   target   : (NCPU/2 + 1)
#
#   no  CFI  : EDAC counts. Nothing happens.
#   has CFI  : cfi ce_count++ — by design CE alone never isolates a CPU
#              (only repeated UCE does). This case verifies the CE counter
#              is wired and that CE does NOT cause isolation.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
TC=$(safe_cpu_alt)
status_banner "06 L2 Cache CE on cpu$TC (bank=3, status=$STAT_CACHE_CE_L2)"
require_mce_inject

CE0=$(cpu_attr "$TC" ce_count)
ON0=$(cpu_online "$TC")
dmesg_mark

for i in 1 2; do
    mce_inject_dispatch "$TC" 3 "$STAT_CACHE_CE_L2" 0 0
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
