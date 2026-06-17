#!/bin/bash
# Case 03 — Memory SRAR (UC, action-required, synchronous)
#
#   path     : mce_inject sw  ->  x86_mce_decoder_chain
#   bank     : 4
#   status   : 0xbd80000000000094  (VAL|UC|EN|MISCV|ADDRV|S|AR | mem 0x094)
#
#   no  CFI  : sw injection bypasses do_machine_check; chain runs but no
#              severity-driven action. NO PANIC even though bits say AR.
#              (For the real "panic without CFI" demo see 12_hw_mem_srar.sh.)
#   has CFI  : cfi notifier sees UC|S|AR -> sync memory_failure(MF_ACTION_REQUIRED)
#              on owner; SIGBUS or async-kill; page hard-offlined.
#              Note: 5.10 kernel often demotes SRAR -> SRAO when injected
#              outside user-mode context (uce_async++ instead of uce_sync++).

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
status_banner "03 Memory SRAR (bank=4, status=$STAT_MEM_SRAR)"
require_mce_inject
hw_panic_warning

US0=$(mfi_stat uce_consumed)
UA0=$(mfi_stat uce_async)
OFF0=$(mfi_stat pages_offlined)
own_page mem_srar
dmesg_mark

mce_inject_dispatch 0 4 "$STAT_MEM_SRAR" "$PADDR" 0
sleep 3

US1=$(mfi_stat uce_consumed)
UA1=$(mfi_stat uce_async)
OFF1=$(mfi_stat pages_offlined)
OWNER_ALIVE=$(kill -0 "$PFN_OWNER_PID" 2>/dev/null && echo "alive (async kill may still be pending)" || echo "killed")

echo
echo "  observations:"
observe_row "mfi uce_consumed"   "$US0"  "$US1"
observe_row "mfi uce_async"      "$UA0"  "$UA1"
observe_row "mfi pages_offlined" "$OFF0" "$OFF1"
echo "    owner process           : $OWNER_ALIVE"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

own_page_release
