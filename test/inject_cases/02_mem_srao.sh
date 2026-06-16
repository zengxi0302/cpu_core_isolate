#!/bin/bash
# Case 02 — Memory SRAO (UC, action-optional)
#
#   path     : mce_inject sw  ->  x86_mce_decoder_chain
#   bank     : 4
#   status   : 0xbc00000000000094  (VAL|UC|EN|MISCV|ADDRV | mem 0x094)
#
#   no  CFI  : decode chain runs; in sw mode no severity check / no #MC,
#              so the kernel does NOT call memory_failure on its own.
#              Page stays mapped. System unaffected.
#   has CFI  : cfi notifier sees UC|S=0 -> async UCE; queues memory_failure;
#              page hard-offlined, MEM_PAGE_OFFLINED netlink to daemon.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
status_banner "02 Memory SRAO (bank=4, status=$STAT_MEM_SRAO)"
require_mce_inject
hw_panic_warning

UA0=$(mfi_stat uce_async)
OFF0=$(mfi_stat pages_offlined)
own_page mem_srao
dmesg_mark

mce_inject_dispatch 0 4 "$STAT_MEM_SRAO" "$PADDR" 0
sleep 3

UA1=$(mfi_stat uce_async)
OFF1=$(mfi_stat pages_offlined)
OWNER_ALIVE=$(kill -0 "$PFN_OWNER_PID" 2>/dev/null && echo "alive" || echo "killed")

echo
echo "  observations:"
observe_row "mfi uce_async"      "$UA0"  "$UA1"
observe_row "mfi pages_offlined" "$OFF0" "$OFF1"
echo "    owner process           : $OWNER_ALIVE"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

own_page_release
