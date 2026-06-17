#!/bin/bash
# Case 16 — L1I (instruction cache) UCE -> CPU isolation
#
#   path     : mce_inject sw -> x86_mce_decoder_chain (default)
#              --hw -> mce-inject(8) raise -> #MC NMI -> do_machine_check
#   bank     : 0 (IFU = instruction fetch / L1I on Intel Skylake-SP)
#   status   : 0xb000000000000801
#              Compound errcode (bit 11 = compound flag) + LL=01 (L1) +
#              TT=00 (instruction transaction). Cfi classifier sees this
#              and returns CFI_ERR_CACHE_L1I (bit 1 in error_types).
#              For an L1D-specific test see case 15.
#   target   : (NCPU/2 + 1) — separate from case 15 to avoid stepping on
#              each other's isolation
#
#   no  CFI  : decode chain runs; no isolation.
#   has CFI  : cfi classifies as L1I; error_types gains 0x02 bit.
#              L1I is per-core, isolation applies the same way as L1D.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
TC=$(safe_cpu_alt)
status_banner "16 L1I Cache UCE on cpu$TC (bank=0 IFU, status=$STAT_CACHE_UCE_L1I)"
require_mce_inject
hw_panic_warning

ST0=$(cpu_attr "$TC" state)
ON0=$(cpu_online "$TC")
UC0=$(cpu_attr "$TC" uce_count)
TY0=$(cpu_attr "$TC" error_types)
dmesg_mark

mce_inject_dispatch "$TC" 0 "$STAT_CACHE_UCE_L1I" 0 0
sleep 3

ST1=$(cpu_attr "$TC" state)
ON1=$(cpu_online "$TC")
UC1=$(cpu_attr "$TC" uce_count)
TY1=$(cpu_attr "$TC" error_types)

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi state"       "$ST0" "$ST1"
printf "    %-26s: %s -> %s\n" "online"          "$ON0" "$ON1"
printf "    %-26s: %s -> %s\n" "cfi uce_count"   "$UC0" "$UC1"
printf "    %-26s: %s -> %s\n" "cfi error_types" "$TY0" "$TY1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

# Re-online if full mode took it offline.
if [[ "$ON1" == "0" && -w /sys/devices/system/cpu/cpu$TC/online ]]; then
    echo 1 > /sys/devices/system/cpu/cpu$TC/online 2>/dev/null
    sleep 1
fi
