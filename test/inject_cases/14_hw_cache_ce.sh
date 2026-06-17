#!/bin/bash
# Case 14 — Real #MC L2 Cache CE (mce-inject userspace tool)
#
#   path     : mce-inject(8) -> raise -> do_machine_check
#              -> mce_severity = MCE_NO_SEVERITY -> x86_mce_decoder_chain
#   bank     : 1
#   status   : 0x900000000000000e  (VAL|EN | L2 simple-cache)
#
#   no  CFI  : CMC corrected handler logs; no panic; no isolation.
#   has CFI  : cfi ce_count++ on the originating cpu; CE alone never
#              isolates (matches case 06 for the real-#MC path).
#
#   PLATFORM LIMITATION — HCE2 + 2288H V5 (and FMA-firmware-first kernels
#   in general): CE via hw raise goes through CMC IRQ -> machine_check_poll,
#   and FMA / vendor SMM scrubs MCi_STATUS before the poll reads it back.
#   Expected on HCE2: "cfi ce_count: 0 -> 0  [delta=0]" — not a script bug,
#   firmware-first interception of the CMC path. For cache CE testing on
#   HCE2 use case 06 (sw flag).

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
INJECT_MODE=hw
TC=$(( $(safe_cpu) + 1 ))
[[ $TC -ge $(nproc) ]] && TC=$(( $(safe_cpu) - 1 ))
status_banner "14 hw L2 Cache CE on cpu$TC (mce-inject userspace tool, bank=1)"
require_mce_inject

CE0=$(cpu_attr "$TC" ce_count)
ON0=$(cpu_online "$TC")
dmesg_mark

mce_inject_file "$TC" 1 "$STAT_CACHE_CE_L2" 0x0 0x0
sleep 2

CE1=$(cpu_attr "$TC" ce_count)
ON1=$(cpu_online "$TC")

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi ce_count" "$CE0" "$CE1"
printf "    %-26s: %s -> %s\n" "online"       "$ON0" "$ON1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'
