#!/bin/bash
# Case 07 — TLB UCE (compound errcode)
#
#   path     : mce_inject sw  ->  x86_mce_decoder_chain
#   bank     : 2
#   status   : 0xb000000000000816  (VAL|UC|EN | compound errcode 0x0816)
#              errcode bits: 0x0800 compound + 0x0010 TLB + LL=L2 -> TLB-L2
#   target   : (NCPU/2 + 1)
#
#   no  CFI  : EDAC logs the bank-2 MCE.
#   has CFI  : cfi classifies as CFI_ERR_TLB; uce_count++; with auto_isolate=0
#              (test default) the cpu is NOT isolated — purpose is to verify
#              the TLB classification path is wired.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
TC=$(safe_cpu_alt)
status_banner "07 TLB UCE on cpu$TC (bank=2, status=$STAT_TLB_UCE)"
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

mce_inject_dispatch "$TC" 2 "$STAT_TLB_UCE" 0 0
sleep 2

UC1=$(cpu_attr "$TC" uce_count)
TY1=$(cpu_attr "$TC" error_types)

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi uce_count"   "$UC0" "$UC1"
printf "    %-26s: %s -> %s\n" "cfi error_types" "$TY0" "$TY1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'
