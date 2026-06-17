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
#   !! DANGER !! — Without address targeting, the firmware picks the
#   victim page. On HCE2 physical hosts this has been observed to hit
#   kernel pages or critical application memory, causing system hang or
#   panic. This case should ONLY be run when:
#     - kdump is configured and working
#     - kernel.panic > 0 (auto-reboot after crash)
#     - You are prepared to lose the SSH session
#   Use case 21 (address-targeted) for safe, repeatable testing.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env_einj.sh
einj_banner "28 EINJ Memory UCE non-fatal (no address targeting)"
require_einj
require_einj_type "$EINJ_MEM_UCE_NONFATAL" "Memory Uncorrectable non-fatal"

echo
echo "  !! DANGER: No address targeting — firmware picks the victim page."
echo "  !! On HCE2 this has caused system hang (firmware hit kernel page)."
echo "  !! Use case 21 (address-targeted) for safe, repeatable testing."
echo "  !! This case is for boundary/stress testing ONLY."
echo
echo "  pre-state:"
echo "    /proc/sys/kernel/panic = $(cat /proc/sys/kernel/panic 2>/dev/null || echo '?')"
echo "    kdump: $(systemctl is-active kdump 2>/dev/null || echo 'unknown')"
echo
echo "  Press Ctrl-C within 10s to abort."
sleep 10

UA0=$(mfi_stat uce_async)
OFF0=$(mfi_stat pages_offlined)
dmesg_mark

einj_inject "$EINJ_MEM_UCE_NONFATAL"
sleep 5

UA1=$(mfi_stat uce_async)
OFF1=$(mfi_stat pages_offlined)

echo
echo "  observations:"
observe_row "mfi uce_async"      "$UA0"  "$UA1"
observe_row "mfi pages_offlined" "$OFF0" "$OFF1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'
