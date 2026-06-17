#!/bin/bash
# Case 23 — Processor CE via APEI EINJ (correctable, cache-level)
#
#   path     : EINJ -> firmware -> GHES/APEI -> MCE corrected handler
#              -> x86_mce_decoder_chain -> (cfi notifier)
#   type     : 0x00000001 (Processor Correctable)
#   target   : a "safe" CPU via APIC ID targeting
#
#   Equivalent to cases 06/14/17 (cache CE) but uses APEI EINJ.
#   EINJ Processor Correctable covers cache, TLB, and internal parity
#   CEs — the firmware decides which specific sub-unit to report.
#   Typically this results in a cache CE on the targeted core.
#
#   no  CFI  : corrected MCE handler logs; EDAC may count; no isolation.
#   has CFI  : cfi ce_count++ on the originating cpu; CE alone never
#              isolates (by design, CE only contributes to degraded-state
#              accounting).
#
#   Note: Unlike mce-inject CE which suffers FMA/SMM scrubbing on HCE2,
#   EINJ processor CE goes through firmware's native path and should be
#   visible on all EINJ-capable platforms.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env_einj.sh
TC=$(safe_cpu_alt)
einj_banner "23 EINJ Processor CE on cpu$TC (type=$EINJ_PROC_CE)"
require_einj
require_einj_type "$EINJ_PROC_CE" "Processor Correctable"

CE0=$(cpu_attr "$TC" ce_count)
ON0=$(cpu_online "$TC")
dmesg_mark

einj_inject_proc "$EINJ_PROC_CE" "$TC"
sleep 2

CE1=$(cpu_attr "$TC" ce_count)
ON1=$(cpu_online "$TC")

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi ce_count" "$CE0" "$CE1"
printf "    %-26s: %s -> %s\n" "online"       "$ON0" "$ON1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

if ! dmesg_since_mark | grep -qE 'Hardware Error|GHES|mce|corrected|cpu_fault_isolate'; then
    echo
    echo "  [INFO] No kernel-side trace for Processor CE injection."
    echo "  On FMA platforms, processor CE may be consumed by firmware."
    echo "  Without SET_ERROR_TYPE_WITH_ADDRESS support, CPU targeting"
    echo "  relies on taskset pinning — the error may have hit a"
    echo "  different CPU than cpu$TC."
fi
