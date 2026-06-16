#!/bin/bash
# Case 08 — Bus / interconnect UCE (compound errcode)
#
#   path     : mce_inject sw  ->  x86_mce_decoder_chain
#   bank     : 5
#   status   : 0xb000000000000e0f  (VAL|UC|EN | compound errcode 0x0E0F)
#              errcode bits: compound bit 11 set + bus class
#   target   : (NCPU/2 + 1)
#
#   no  CFI  : EDAC logs.
#   has CFI  : cfi classifies as CFI_ERR_BUS; uce_count++. Some bus errors
#              are not per-cpu and may be invisible on the chosen target —
#              SKIP is expected, not a failure.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
TC=$(( $(safe_cpu) + 1 ))
[[ $TC -ge $(nproc) ]] && TC=$(( $(safe_cpu) - 1 ))
status_banner "08 Bus UCE on cpu$TC (bank=5, status=$STAT_BUS_UCE)"
require_mce_inject
hw_panic_warning

RESTORE_AUTO=
if cfi_loaded; then
    RESTORE_AUTO=$(cat $CFI_SYSFS/auto_isolate 2>/dev/null || echo 1)
    echo 0 > $CFI_SYSFS/auto_isolate 2>/dev/null
fi
trap '[[ -n "$RESTORE_AUTO" ]] && echo "$RESTORE_AUTO" > $CFI_SYSFS/auto_isolate 2>/dev/null' EXIT

UC0=$(cpu_attr "$TC" uce_count)
TY0=$(cpu_attr "$TC" error_types)
dmesg_mark

mce_inject_dispatch "$TC" 5 "$STAT_BUS_UCE" 0 0
sleep 2

UC1=$(cpu_attr "$TC" uce_count)
TY1=$(cpu_attr "$TC" error_types)

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi uce_count"   "$UC0" "$UC1"
printf "    %-26s: %s -> %s\n" "cfi error_types" "$TY0" "$TY1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'
