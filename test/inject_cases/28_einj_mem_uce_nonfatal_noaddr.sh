#!/bin/bash
# Case 28 — Memory UCE non-fatal via EINJ WITHOUT address targeting
#
#   path     : EINJ -> firmware -> GHES/APEI -> memory_failure (async)
#   type     : 0x00000010 (Memory Uncorrectable non-fatal)
#   target   : no address targeting — firmware picks the victim page
#
#   This is a variant of case 21 that injects without specifying a
#   physical address. The firmware chooses which memory location to
#   corrupt. This is useful for:
#     1. Platforms that don't support address-targeted EINJ
#     2. Testing CFI's behavior when the error address comes entirely
#        from firmware (more realistic — real hardware errors don't
#        target pre-allocated test pages)
#
#   no  CFI  : kernel's GHES handler calls memory_failure; page offlined.
#   has CFI  : cfi mfi accounts the error; page offlined + netlink.
#
#   CAUTION: the firmware picks the victim — it may hit a kernel page,
#   an in-use application page, or a free page. Impact is unpredictable.
#   On most platforms, firmware picks a "safe" sacrificial page, but
#   this is not guaranteed.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env_einj.sh
einj_banner "28 EINJ Memory UCE non-fatal (no address targeting)"
require_einj
require_einj_type "$EINJ_MEM_UCE_NONFATAL" "Memory Uncorrectable non-fatal"

UA0=$(mfi_stat uce_async)
OFF0=$(mfi_stat pages_offlined)
dmesg_mark

echo "  [INFO] No address targeting — firmware chooses victim page."
echo "  [INFO] Impact depends on which page firmware picks."
einj_inject "$EINJ_MEM_UCE_NONFATAL"
sleep 3

UA1=$(mfi_stat uce_async)
OFF1=$(mfi_stat pages_offlined)

echo
echo "  observations:"
observe_row "mfi uce_async"      "$UA0"  "$UA1"
observe_row "mfi pages_offlined" "$OFF0" "$OFF1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'
