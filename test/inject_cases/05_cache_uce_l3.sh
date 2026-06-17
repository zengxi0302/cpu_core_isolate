#!/bin/bash
# Case 05 — L3 / LLC Cache UCE
#
#   path     : mce_inject sw -> x86_mce_decoder_chain (default)
#              --hw -> mce-inject(8) raise -> #MC NMI -> do_machine_check
#   bank     : 9 (CHA / LLC slice on Intel Skylake-SP / Cascade Lake-SP).
#              The Skylake-SP Bank-3 = MLC (L2). L3 errors are reported by
#              CHA banks (typically 9+). Using bank 3 with an L3 errcode
#              makes the kernel's hw raise path reject with "Invalid MCE
#              context" because bank semantics and errcode disagree.
#   status   : 0xb00000000000000f  (VAL|UC|EN | simple-cache 0x000F = L3/generic)
#   target   : (NCPU/2 + 1)
#
#   no  CFI  : decode chain runs, EDAC logs; no isolation.
#   has CFI  : cfi uce_count++ tagged as cache L3. Per the
#              `isolate_on_l3_uce` module param (default = 0, i.e. don't
#              isolate), the reporting CPU STAYS ONLINE — L3 is socket-
#              shared (CHA tiles on Intel, CCX on AMD), so isolating one
#              core does not remove the dependency on the bad LLC slice.
#              CFI logs:
#                  cpu%u: L3 cache UCE not isolating (shared LLC; set
#                                                     isolate_on_l3_uce=1 to override)
#              To verify the legacy "isolate anyway" behavior, run:
#                  echo 1 > /sys/kernel/cfi/isolate_on_l3_uce
#                  bash test/inject_cases/05_cache_uce_l3.sh
#                  echo 0 > /sys/kernel/cfi/isolate_on_l3_uce
#
#   This case replaces the earlier "auto_isolate=0 forced" hack — the
#   isolation skip is now a real kernel policy decision, not a script
#   workaround.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
TC=$(safe_cpu_alt)
status_banner "05 L3 Cache UCE on cpu$TC (bank=9 CHA/LLC, status=$STAT_CACHE_UCE_L3)"
require_mce_inject
hw_panic_warning

# Surface the current policy so the result is interpretable.
if cfi_loaded; then
    L3_ISOL=$(cat $CFI_SYSFS/isolate_on_l3_uce 2>/dev/null || echo "(n/a)")
    echo "  policy   : isolate_on_l3_uce=$L3_ISOL  (1 = isolate; 0 = account only)"
fi

UC0=$(cpu_attr "$TC" uce_count)
ST0=$(cpu_attr "$TC" state)
ON0=$(cpu_online "$TC")
dmesg_mark

mce_inject_dispatch "$TC" 9 "$STAT_CACHE_UCE_L3" 0 0
sleep 2

UC1=$(cpu_attr "$TC" uce_count)
ST1=$(cpu_attr "$TC" state)
ON1=$(cpu_online "$TC")

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi state"     "$ST0" "$ST1"
printf "    %-26s: %s -> %s\n" "online"        "$ON0" "$ON1"
printf "    %-26s: %s -> %s\n" "cfi uce_count" "$UC0" "$UC1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

# Re-online if isolate_on_l3_uce=1 took the cpu offline (full mode only).
if [[ "$ON1" == "0" && -w /sys/devices/system/cpu/cpu$TC/online ]]; then
    echo 1 > /sys/devices/system/cpu/cpu$TC/online 2>/dev/null
    sleep 1
fi
