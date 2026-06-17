#!/bin/bash
# Shared environment for test/inject_cases/*.sh
# Source from each case script. Provides:
#   - sysfs / debugfs paths
#   - MCi_STATUS preset constants (Intel SDM Vol 3 Ch 15)
#   - mce_submit / hwpoison_submit helpers
#   - own_page / own_page_release (allocate a private 4K page)
#   - status_banner / observe_* (uniform output format)
#
# Designed so the same case script runs identically with the CFI module
# loaded or unloaded — the helpers detect cfi sysfs presence and downgrade
# observations gracefully.

INJ_DIR=/sys/kernel/debug/mce-inject
HWP_DIR=/sys/kernel/debug/hwpoison
CFI_SYSFS=/sys/kernel/cfi
CFI_MEM=$CFI_SYSFS/mem
LOGDIR=${LOGDIR:-/home/cfi_logs/inject_cases}
mkdir -p "$LOGDIR"

# Status presets (VAL=63, UC=61, EN=60, MISCV=59, ADDRV=58, S=56, AR=55)
#   mem 0x094: memctl mask 0xff80 == 0x0080 -> mem domain
#   cache simple-code: 0x000D=L1, 0x000E=L2, 0x000F=L3
#   compound code 0x0816: bit 11 (compound) + 0x0010 TLB at L2
#   compound code 0x0E0F: bit 11 (compound) + bus
STAT_MEM_CE=0x9c00000000000094       # VAL|EN|MISCV|ADDRV
STAT_MEM_SRAO=0xbc00000000000094     # +UC
STAT_MEM_SRAR=0xbd80000000000094     # +UC|S|AR
STAT_CACHE_UCE_L2=0xb00000000000000e # VAL|UC|EN | L2
STAT_CACHE_UCE_L3=0xb00000000000000f # VAL|UC|EN | L3/generic
STAT_CACHE_CE_L2=0x900000000000000e  # VAL|EN | L2
STAT_TLB_UCE=0xb000000000000816      # VAL|UC|EN | compound TLB
STAT_BUS_UCE=0xb000000000000e0f      # VAL|UC|EN | compound bus

cfi_loaded() { lsmod | awk '{print $1}' | grep -qx cpu_fault_isolate; }

# ============================================================
# Injection mode + options. Per-case scripts call parse_inject_args "$@"
# at startup.
#
#   --sw          decode chain only (default; never panics)
#   --hw          real #MC via mce-inject(8) raise / debugfs flags=hw
#   --lmce        implies --hw; sets MCGSTATUS=0xF (MCIP|EIPV|RIPV|LMCE_S).
#                 Skips the Intel broadcast MCE sync; the kernel processes
#                 the event locally and panics (or recovers) based on
#                 mce_severity, matching real hardware-fault semantics.
#                 Without --lmce, hw injection only raises on one cpu and
#                 mce_reign times out -> mce_panic("Some CPUs didn't answer
#                 in synchronization") which is an mce-inject artifact, not
#                 a real-hardware panic mechanism. With --lmce, panic
#                 (when no CFI) reads "Fatal machine check on current CPU",
#                 the canonical severity-driven message.
#   --mcgstatus=X explicit MCGSTATUS hex (advanced).
# ============================================================
INJECT_MODE=sw
INJECT_LMCE=0
_INJECT_MCGSTATUS=""   # empty = kernel default; nonzero = override

parse_inject_args() {
    for a in "$@"; do
        case "$a" in
            --hw)            INJECT_MODE=hw ;;
            --sw)            INJECT_MODE=sw ;;
            --lmce)          INJECT_MODE=hw; INJECT_LMCE=1; _INJECT_MCGSTATUS=0xf ;;
            --mcgstatus=*)   _INJECT_MCGSTATUS="${a#--mcgstatus=}" ;;
        esac
    done
}

# Real #MC injection (no upfront probe — picks path purely by tool presence).
#
# Why no probe: an earlier version tried to detect delivery by injecting a
# memory CE and watching for ce_total / dmesg signals. That mis-fires on
# HCE2 physical hosts: CE delivery goes through CMC IRQ -> machine_check_poll,
# and FMA / vendor SMM scrubs MCi_STATUS before the poll reads it back, so
# the probe sees "no delivery" even though UC delivery via NMI works fine
# (the manual mce-inject .mce experiments prove that). Probing without
# isolating a CPU as a side effect requires choosing a signal that's safe,
# and there isn't one that's also universal. Better to just dispatch and
# let each case observe its own result.
#
# Usage: mce_inject_hw <cpu> <bank> <status> <addr> <misc>
mce_inject_hw() {
    local cpu=$1 bank=$2 status=$3 addr=${4:-0} misc=${5:-0}
    require_mce_inject
    if command -v mce-inject >/dev/null 2>&1; then
        mce_inject_file "$cpu" "$bank" "$status" "$addr" "$misc"
    else
        mce_submit hw "$bank" "$status" "$addr" "$misc" "$cpu"
    fi
}

# Dispatch sw or hw based on INJECT_MODE.
# Usage: mce_inject_dispatch <cpu> <bank> <status> <addr> <misc>
mce_inject_dispatch() {
    if [[ "$INJECT_MODE" == hw ]]; then
        mce_inject_hw "$@"
    else
        local cpu=$1 bank=$2 status=$3 addr=${4:-0} misc=${5:-0}
        mce_submit sw "$bank" "$status" "$addr" "$misc" "$cpu"
    fi
}

# Warn before injecting a UC event in hw mode without CFI loaded — that
# combination is expected to panic on most kernels:
#   - default hw (no LMCE): broadcast MCE sync timeout -> "Some CPUs didn't
#     answer in synchronization" -> mce_panic (mce-inject single-CPU raise
#     artifact, not a real-hardware failure mode)
#   - --lmce: kernel handles locally, mce_severity drives the decision;
#     AR / PCC=1 with mce_tolerant<3 -> mce_panic("Fatal machine check on
#     current CPU") — this matches the actual real-hardware panic path
# UC cases should call this before mce_inject_dispatch.
hw_panic_warning() {
    [[ "$INJECT_MODE" == hw ]] || return 0
    cfi_loaded && return 0
    echo
    if [[ $INJECT_LMCE == 1 ]]; then
        echo "  !! --lmce + CFI NOT LOADED — severity-driven mce_panic expected."
    else
        echo "  !! --hw + CFI NOT LOADED — broadcast-sync-timeout mce_panic expected."
    fi
    echo "  !! Confirm kdump configured and kernel.panic > 0. Ctrl-C within 5s to abort."
    sleep 5
}

# Pick a safe target cpu: avoid cpu0 (housekeeping) and the last cpu.
safe_cpu() {
    local n=$(nproc)
    local t=$(( n / 2 ))
    [[ $t -le 0 ]] && t=1
    echo $t
}

# Banner. $1 = short case description.
status_banner() {
    echo "================================================================"
    if cfi_loaded; then
        echo "  CFI status : LOADED  (cpu_fault_isolate present)"
    else
        echo "  CFI status : NOT LOADED"
    fi
    echo "  kernel     : $(uname -r)"
    echo "  case       : $1"
    echo "  inject mode: $INJECT_MODE (override with --hw / --sw / --lmce)"
    if [[ $INJECT_LMCE == 1 ]]; then
        echo "  lmce       : ON  (MCGSTATUS=$_INJECT_MCGSTATUS — local MCE, no broadcast sync)"
    elif [[ -n "$_INJECT_MCGSTATUS" ]]; then
        echo "  mcgstatus  : $_INJECT_MCGSTATUS (explicit override)"
    fi
    echo "  artifacts  : $LOGDIR/"
    echo "================================================================"
}

# Preflight: mce-inject debugfs must be present (modprobe mce_inject).
require_mce_inject() {
    if [[ ! -d $INJ_DIR ]]; then
        modprobe mce_inject 2>/dev/null
    fi
    [[ -d $INJ_DIR ]] || { echo "[ABORT] $INJ_DIR missing — modprobe mce_inject failed"; exit 1; }
}
require_hwpoison_inject() {
    if [[ ! -f $HWP_DIR/corrupt-pfn ]]; then
        modprobe hwpoison_inject 2>/dev/null
    fi
    [[ -f $HWP_DIR/corrupt-pfn ]] || { echo "[ABORT] $HWP_DIR/corrupt-pfn missing"; exit 1; }
}
require_mce_inject_userspace() {
    command -v mce-inject >/dev/null 2>&1 || {
        echo "[ABORT] userspace tool 'mce-inject' missing (try: dnf install mce-inject)"
        exit 1
    }
}

# Submit one mce-inject event via the debugfs interface.
# Use this for the *sw* path (flags=sw); decode chain only, no #MC.
#
# Usage: mce_submit <flags=sw|hw> <bank> <status_hex> <addr_hex> <misc_hex> <cpu>
#
# CAVEAT: do not rely on flags=hw here for a "real #MC" demo on physical hosts.
# Empirically on HCE2 + Purley (Huawei 2288H V5) the userspace mce-inject(8)
# tool is more reliable for hw-equivalent delivery — see mce_inject_file().
mce_submit() {
    local flags=$1 bank=$2 status=$3 addr=${4:-0} misc=${5:-0} cpu=${6:-0}
    local onf=/sys/devices/system/cpu/cpu$cpu/online
    if [[ -f $onf && "$(cat $onf)" == "0" ]]; then
        echo "[ABORT] cpu$cpu offline; mce_submit would deadlock"
        return 1
    fi
    echo "$status" > $INJ_DIR/status
    echo "$addr"   > $INJ_DIR/addr
    echo "$misc"   > $INJ_DIR/misc
    echo 0         > $INJ_DIR/synd
    echo "$cpu"    > $INJ_DIR/cpu
    # MCGSTATUS override (for --lmce or explicit --mcgstatus). Only meaningful
    # for the hw path (sw path bypasses do_machine_check entirely).
    if [[ "$flags" == "hw" && -n "${_INJECT_MCGSTATUS:-}" && -w $INJ_DIR/mcgstatus ]]; then
        echo "$_INJECT_MCGSTATUS" > $INJ_DIR/mcgstatus
    fi
    echo "$flags"  > $INJ_DIR/flags
    echo "$bank"   > $INJ_DIR/bank
    local mcgs_note=""
    [[ "$flags" == "hw" && -n "${_INJECT_MCGSTATUS:-}" ]] && mcgs_note=" mcgstatus=$_INJECT_MCGSTATUS"
    echo "  injected (debugfs): flags=$flags bank=$bank status=$status addr=$addr cpu=$cpu$mcgs_note"
}

# Submit a real #MC via the mce-inject(8) userspace tool with a .mce file.
# This is the path verified to actually deliver #MC on HCE2 physical hosts:
# the tool internally writes flags="raise" to /sys/kernel/debug/mce-inject
# before triggering, so the debugfs flags node's static value (sw|hw) does
# NOT govern the actual injection mechanism.
#
# Usage: mce_inject_file <cpu> <bank> <status_hex> [addr_hex] [misc_hex]
mce_inject_file() {
    local cpu=$1 bank=$2 status=$3 addr=${4:-0x0} misc=${5:-0x0}
    local onf=/sys/devices/system/cpu/cpu$cpu/online
    if [[ -f $onf && "$(cat $onf)" == "0" ]]; then
        echo "[ABORT] cpu$cpu offline; mce_inject_file would deadlock"
        return 1
    fi
    require_mce_inject_userspace
    local f=$LOGDIR/.mce.$$.txt
    cat > "$f" <<MCE
CPU $cpu
BANK $bank
STATUS $status
ADDR $addr
MISC $misc
MCE
    # Inline MCGSTATUS line if --lmce or --mcgstatus is set. The mce-inject(8)
    # tool parses this and writes it to debugfs/mcgstatus before triggering.
    if [[ -n "${_INJECT_MCGSTATUS:-}" ]]; then
        echo "MCGSTATUS $_INJECT_MCGSTATUS" >> "$f"
    fi
    echo "  injecting (mce-inject userspace tool, raise mode):"
    sed 's/^/    /' "$f"
    mce-inject "$f"
    local rc=$?
    rm -f "$f"
    return $rc
}

# Run the ownpage helper in the background. Sets PFN_OWNER_PID, PFN, PADDR.
# Caller must invoke own_page_release after observations.
own_page() {
    local case_tag=${1:-page}
    local out=$LOGDIR/.ownpage.$case_tag.$$
    if [[ ! -x test/tools/ownpage ]]; then
        echo "[ABORT] test/tools/ownpage not built — run: make -C test/tools"
        exit 1
    fi
    test/tools/ownpage > "$out" &
    PFN_OWNER_PID=$!
    sleep 1
    PFN=$(awk -F= '/^pfn/{print $2}' "$out")
    PADDR=$(printf "0x%x" $((PFN * 4096)))
    if [[ -z "$PFN" || "$PFN" == "0x0" ]]; then
        echo "[ABORT] failed to acquire pfn"
        kill $PFN_OWNER_PID 2>/dev/null
        exit 1
    fi
    echo "  owned page : pfn=$PFN paddr=$PADDR (owner pid=$PFN_OWNER_PID)"
}
own_page_release() {
    [[ -n "${PFN_OWNER_PID:-}" ]] && kill "$PFN_OWNER_PID" 2>/dev/null
    PFN_OWNER_PID=
}

# Read a single mfi stat key. Returns "(n/a)" if cfi not loaded.
mfi_stat() {
    [[ -r $CFI_MEM/stats ]] || { echo "(n/a)"; return; }
    awk -v k=$1 '$1==k{print $2}' "$CFI_MEM/stats"
}

# Print a single observation row: "<label>: <v0> -> <v1>  [delta=N]"
# or "(n/a — CFI not loaded)" when applicable.
observe_row() {
    local label=$1 v0=$2 v1=$3
    if [[ "$v0" == "(n/a)" || "$v1" == "(n/a)" ]]; then
        printf "    %-26s: (n/a — CFI not loaded)\n" "$label"
    else
        local d=$((v1 - v0))
        local sign=""
        [[ $d -gt 0 ]] && sign="+"
        printf "    %-26s: %s -> %s   [delta=%s%d]\n" "$label" "$v0" "$v1" "$sign" "$d"
    fi
}

# Per-cpu cfi attribute reader.
cpu_attr() { cat "/sys/devices/system/cpu/cpu$1/cfi/$2" 2>/dev/null || echo "(n/a)"; }
cpu_online() { cat "/sys/devices/system/cpu/cpu$1/online" 2>/dev/null || echo "?"; }

# Dump the dmesg lines we care about, since a 'mark' line written before injection.
DMESG_TAG=
dmesg_mark() {
    DMESG_TAG="CASE-$$-$(date +%H%M%S%N)"
    echo "$DMESG_TAG" >/dev/kmsg 2>/dev/null
}
dmesg_since_mark() {
    [[ -z "$DMESG_TAG" ]] && { dmesg | tail -10; return; }
    dmesg | awk -v tag="$DMESG_TAG" 'p{print} $0~tag{p=1}'
}

# Filter dmesg digest to the lines relevant to memory/cache errors.
dmesg_relevant_grep='cpu_fault_isolate|memory_failure|Memory failure|EDAC MC|EDAC.*MEMORY ERROR|mce: \[Hardware Error\]|soft offline|hwpoison|Injecting memory failure|x86_mce'
dmesg_digest() { dmesg_since_mark | grep -E "$dmesg_relevant_grep" | head -30; }
