#!/bin/bash
# Case 05 — L3/generic Cache UCE (accounting only)
#
#   path     : mce_inject sw  ->  x86_mce_decoder_chain
#   bank     : 3
#   status   : 0xb00000000000000f  (VAL|UC|EN | simple-cache 0x000F = L3/generic)
#   target   : (NCPU/2 + 1)
#
#   no  CFI  : decode chain runs, EDAC logs; no isolation.
#   has CFI  : cfi uce_count++ tagged as cache L3. With auto_isolate=0 (default
#              for this case to avoid isolating the only "spare" cpu) the cpu
#              stays online — purpose is to verify the *classification* path.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
TC=$(( $(safe_cpu) + 1 ))
[[ $TC -ge $(nproc) ]] && TC=$(( $(safe_cpu) - 1 ))
status_banner "05 L3 Cache UCE accounting on cpu$TC (status=$STAT_CACHE_UCE_L3)"
require_mce_inject
hw_panic_warning

# Park auto_isolate=0 only when cfi is loaded, restore at exit.
RESTORE_AUTO=
if cfi_loaded; then
    RESTORE_AUTO=$(cat $CFI_SYSFS/auto_isolate 2>/dev/null || echo 1)
    echo 0 > $CFI_SYSFS/auto_isolate 2>/dev/null
fi
trap '[[ -n "$RESTORE_AUTO" ]] && echo "$RESTORE_AUTO" > $CFI_SYSFS/auto_isolate 2>/dev/null' EXIT

UC0=$(cpu_attr "$TC" uce_count)
ON0=$(cpu_online "$TC")
dmesg_mark

mce_inject_dispatch "$TC" 3 "$STAT_CACHE_UCE_L3" 0 0
sleep 2

UC1=$(cpu_attr "$TC" uce_count)
ON1=$(cpu_online "$TC")

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi uce_count" "$UC0" "$UC1"
printf "    %-26s: %s -> %s\n" "online"        "$ON0" "$ON1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'
