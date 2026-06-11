#!/bin/bash
# Memory Fault Isolation (MFI) - Error Injection Test Script
#
# Drives the MFI software injection path end to end on a page owned by
# a helper process (test/tools/ownpage), so real page isolation only
# affects the helper.
#
# Usage:
#   ./mem_inject.sh --type <ce|uce_srao|uce_srar> [--count <N>] [--pfn <0xN>]
#
# Examples:
#   ./mem_inject.sh --type ce --count 8      # cross page_ce_threshold
#   ./mem_inject.sh --type uce_srao          # async UCE -> memory_failure
#
# Hardware injection (EINJ) is available via inject_common.sh-style
# flows on physical machines:
#   error_type 0x00000008 = Memory Correctable
#   error_type 0x00000010 = Memory Uncorrectable non-fatal (SRAO-like)
#   error_type 0x00000020 = Memory Uncorrectable fatal

set -euo pipefail

CFI_DEBUGFS="/sys/kernel/debug/cfi/inject"
MFI_SYSFS="/sys/kernel/cfi/mem"
TOOLS_DIR="$(cd "$(dirname "$0")/../tools" && pwd)"

TYPE="ce"
COUNT=1
PFN=""
OWNPAGE_PID=""

usage() {
    echo "Usage: $0 --type <ce|uce_srao|uce_srar> [--count <N>] [--pfn <0xN>]"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --type)  TYPE="$2"; shift 2;;
        --count) COUNT="$2"; shift 2;;
        --pfn)   PFN="$2"; shift 2;;
        *)       usage;;
    esac
done

case "$TYPE" in
    ce|uce_srao|uce_srar) ;;
    *) usage;;
esac

if [[ ! -w "$CFI_DEBUGFS" ]]; then
    echo "ERROR: $CFI_DEBUGFS not available. Is cpu_fault_isolate loaded?"
    exit 1
fi

cleanup() {
    [[ -n "$OWNPAGE_PID" ]] && kill "$OWNPAGE_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Acquire a target page owned by a sacrificial helper unless given one
if [[ -z "$PFN" ]]; then
    if [[ ! -x "$TOOLS_DIR/ownpage" ]]; then
        echo "Building ownpage helper..."
        make -C "$TOOLS_DIR" ownpage
    fi
    coproc OWNPAGE { "$TOOLS_DIR/ownpage"; }
    OWNPAGE_PID=$!
    read -r line <&"${OWNPAGE[0]}"
    PFN="${line#pfn=}"
    echo "Helper pid=$OWNPAGE_PID owns pfn=$PFN"
fi

echo "=== MFI injection: type=$TYPE pfn=$PFN count=$COUNT ==="
[[ -f "$MFI_SYSFS/stats" ]] && { echo "--- stats before ---"; cat "$MFI_SYSFS/stats"; }

for ((i=1; i<=COUNT; i++)); do
    echo "domain=mem pfn=$PFN type=$TYPE" > "$CFI_DEBUGFS"
    echo "  Injected $i/$COUNT"
done

# Page isolation is asynchronous (workqueues); give it a moment
sleep 2

echo "--- stats after ---"
[[ -f "$MFI_SYSFS/stats" ]] && cat "$MFI_SYSFS/stats"

if [[ -n "$OWNPAGE_PID" ]]; then
    if kill -0 "$OWNPAGE_PID" 2>/dev/null; then
        echo "Helper still alive (expected for ce / clean isolation)"
    else
        echo "Helper was killed (expected for consumed UCE on dirty page)"
        OWNPAGE_PID=""
    fi
fi

echo "Done. Watch events with: $TOOLS_DIR/cfimon"
