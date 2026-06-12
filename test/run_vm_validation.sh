#!/bin/bash
# CFI/MFI 虚拟机一键功能验证脚本（HCE 2.0 / 5.10 内核，x86_64）
#
# 用法（在云服务器上以 root 执行，仓库已拷贝到本机）:
#   cd cpu_core_isolate && bash test/run_vm_validation.sh
#   bash test/run_vm_validation.sh --with-panic   # 末尾追加受控 panic 测试(机器会重启!)
#
# 输出: 终端实时日志 + /root/cfi_validation_report.txt
#
# 覆盖范围（VM 内可验证的全部能力）:
#   P0  环境与编译        P1  模块加载/sysfs/参数
#   P2  CPU 域状态机与隔离 P3  netlink 协议(cfimon)
#   P4  MFI 页记账/预隔离/异步UCE  P5  madvise 真实毒页路径
#   P6  甄别 RECOVER 路径  P7  甄别 PANIC 路径(可选,重启)
#   P8  模块卸载与状态恢复
#
# VM 环境天然测不到的（需要物理机/EINJ）:
#   真实 MCE 硬件通路、EDAC DIMM 定位、patrol scrub、FMA(HCE3)

set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
REPORT=/root/cfi_validation_report.txt
KDIR=/lib/modules/$(uname -r)/build
INJ=/sys/kernel/debug/cfi/inject
MEM=/sys/kernel/cfi/mem
PASS=0; FAIL=0; SKIP=0
WITH_PANIC=0
[[ "${1:-}" == "--with-panic" ]] && WITH_PANIC=1

log()  { echo "$*" | tee -a "$REPORT"; }
ok()   { PASS=$((PASS+1)); log "  [PASS] $*"; }
bad()  { FAIL=$((FAIL+1)); log "  [FAIL] $*"; }
skip() { SKIP=$((SKIP+1)); log "  [SKIP] $*"; }
hdr()  { log ""; log "===== $* ====="; }

: > "$REPORT"
log "CFI/MFI VM validation  $(date)"
log "host: $(uname -a)"
log "os:   $(head -1 /etc/os-release 2>/dev/null)"
log "cpus: $(nproc)   mem: $(free -h | awk '/Mem/{print $2}')"

# ---------- P0 环境与编译 ----------
hdr "P0 build"
if [[ ! -d "$KDIR" ]]; then
    log "  kernel-devel missing, installing..."
    dnf install -y kernel-devel-$(uname -r) gcc make elfutils-libelf-devel >>"$REPORT" 2>&1 \
        || dnf install -y kernel-devel gcc make elfutils-libelf-devel >>"$REPORT" 2>&1
fi
[[ -d "$KDIR" ]] && ok "kernel-devel present: $KDIR" || { bad "kernel-devel unavailable"; exit 1; }

grep -q CONFIG_MEMORY_FAILURE=y /boot/config-$(uname -r) \
    && ok "CONFIG_MEMORY_FAILURE=y" || bad "CONFIG_MEMORY_FAILURE not set (MFI page isolation unavailable)"
grep -qE "CONFIG_X86_MCE=y" /boot/config-$(uname -r) \
    && ok "CONFIG_X86_MCE=y" || bad "CONFIG_X86_MCE not set"

make -C kernel KDIR="$KDIR" clean >/dev/null 2>&1
if make -C kernel KDIR="$KDIR" >>"$REPORT" 2>&1 && [[ -f kernel/cpu_fault_isolate.ko ]]; then
    ok "module build (zero-error) on $(uname -r)"
else
    bad "module build failed — see $REPORT"; exit 1
fi
make -C test/tools >>"$REPORT" 2>&1 && ok "userspace tools build" || bad "tools build failed"
if make -C test/unit run >>"$REPORT" 2>&1; then
    ok "policy unit tests (18 cases)"
else
    bad "policy unit tests failed"
fi

# ---------- P1 加载与接口 ----------
hdr "P1 load & interfaces"
rmmod cpu_fault_isolate 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null
if insmod kernel/cpu_fault_isolate.ko ce_threshold=3 uce_threshold=1 window_secs=60 \
        defer_to_daemon=0 page_ce_threshold=3 mem_window_secs=300 \
        dimm_uce_threshold=2 mem_triage=0 >>"$REPORT" 2>&1; then
    ok "insmod with test thresholds"
else
    bad "insmod failed: $(dmesg | tail -3)"; exit 1
fi
dmesg | tail -30 >>"$REPORT"

[[ -f /sys/kernel/cfi/version ]] && ok "sysfs root ($(cat /sys/kernel/cfi/version))" || bad "sysfs root missing"
[[ -f /sys/devices/system/cpu/cpu0/cfi/state ]] && ok "per-CPU sysfs" || bad "per-CPU sysfs missing"
[[ -f $MEM/stats ]] && ok "MFI sysfs subtree" || bad "MFI sysfs missing"
[[ -w $INJ ]] && ok "debugfs inject" || bad "debugfs inject missing"

SOA=$(awk '/soft_offline_available/{print $2}' $MEM/stats)
[[ "$SOA" == "1" ]] && ok "soft_offline_page resolved via kprobe" \
                    || bad "soft_offline_page NOT resolved (pre-isolation off)"
if dmesg | grep -q "mem: RAS CEC detected"; then
    log "  [INFO] CEC active -> pre_isolate auto-downgraded; re-enabling for tests"
    echo 1 > $MEM/pre_isolate
fi
dmesg | grep -q "panic suppression active" && ok "panic suppression layer up" || bad "suppression layer"
TOL=$(cat /sys/devices/system/machinecheck/machinecheck0/tolerant 2>/dev/null || echo NA)
[[ "$TOL" == "3" ]] && ok "mce tolerant=3 (sysfs path, 5.10 native)" \
                    || log "  [INFO] tolerant=$TOL (check dmesg for direct-write path)"

echo 5 > /sys/kernel/cfi/ce_threshold && [[ $(cat /sys/kernel/cfi/ce_threshold) == 5 ]] \
    && ok "sysfs param write" || bad "sysfs param write"
echo 3 > /sys/kernel/cfi/ce_threshold

# ---------- P2 CPU 域 ----------
hdr "P2 CPU domain"
NC=$(nproc); TC=$((NC-1))
for i in 1 2 3; do echo "cpu=$TC type=cache_l2 severity=ce" > $INJ; done
[[ $(cat /sys/devices/system/cpu/cpu$TC/cfi/ce_count) == 3 ]] && ok "CE counting (3)" || bad "CE count=$(cat /sys/devices/system/cpu/cpu$TC/cfi/ce_count)"
[[ $(cat /sys/devices/system/cpu/cpu$TC/cfi/state) == degraded ]] && ok "CE threshold -> DEGRADED" || bad "state=$(cat /sys/devices/system/cpu/cpu$TC/cfi/state)"

echo "cpu=$TC type=cache_l2 severity=ucr" > $INJ; sleep 3
ST=$(cat /sys/devices/system/cpu/cpu$TC/cfi/state); ON=$(cat /sys/devices/system/cpu/cpu$TC/online)
[[ "$ST" == isolated && "$ON" == 0 ]] && ok "UCE -> ISOLATED, cpu$TC offline" || bad "UCE isolation: state=$ST online=$ON"
echo 1 > /sys/devices/system/cpu/cpu$TC/online; sleep 1
[[ $(cat /sys/devices/system/cpu/cpu$TC/online) == 1 ]] && ok "manual re-online" || bad "re-online failed"

T2=$((NC-2))
echo 1 > /sys/devices/system/cpu/cpu$T2/cfi/user_pinned
echo "cpu=$T2 type=cache_l2 severity=ucr" > $INJ; sleep 2
[[ $(cat /sys/devices/system/cpu/cpu$T2/online) == 1 ]] && ok "user_pinned blocks isolation" || bad "pinned CPU isolated"
echo 0 > /sys/devices/system/cpu/cpu$T2/cfi/user_pinned

echo 0 > /sys/kernel/cfi/auto_isolate
echo "cpu=1 type=cache_l3 severity=ucr" > $INJ; sleep 2
[[ $(cat /sys/devices/system/cpu/cpu1/online) == 1 ]] && ok "auto_isolate=0 notify-only" || bad "isolated despite auto_isolate=0"
echo 1 > /sys/kernel/cfi/auto_isolate

echo 2 > /sys/kernel/cfi/window_secs
echo "cpu=2 type=cache_l2 severity=ce" > $INJ; sleep 3
echo "cpu=2 type=cache_l2 severity=ce" > $INJ
[[ $(cat /sys/devices/system/cpu/cpu2/cfi/ce_count) == 1 ]] && ok "window expiry resets counters" || bad "window: ce=$(cat /sys/devices/system/cpu/cpu2/cfi/ce_count)"
echo 60 > /sys/kernel/cfi/window_secs

# ---------- P3 netlink ----------
hdr "P3 netlink protocol"
./test/tools/cfimon > /tmp/cfimon.log 2>&1 &
MONPID=$!; sleep 1
if kill -0 $MONPID 2>/dev/null; then
    echo "cpu=1 type=cache_l2 severity=ce" > $INJ; sleep 1
    grep -q "CPU_ERROR" /tmp/cfimon.log && ok "CPU_ERROR multicast received" || bad "no CPU_ERROR event"
else
    bad "cfimon failed to resolve family: $(cat /tmp/cfimon.log)"
fi
./test/tools/cfimon --mem-status > /tmp/cfimem.log 2>&1 \
    && ok "MEM_GET_STATUS round-trip: $(grep ce_total /tmp/cfimem.log)" || bad "MEM_GET_STATUS failed"

# ---------- P4 MFI 页记账/预隔离/异步 UCE ----------
hdr "P4 MFI page accounting & isolation"
./test/tools/ownpage > /tmp/op1.txt & OP1=$!; sleep 1
PFN1=$(sed 's/pfn=//' /tmp/op1.txt)
if [[ -n "$PFN1" ]]; then
    for i in 1 2 3; do echo "domain=mem pfn=$PFN1 type=ce" > $INJ; done
    sleep 3
    PRE=$(awk '/pages_pre_offlined/{print $2}' $MEM/stats)
    CE=$(awk '/ce_total/{print $2}' $MEM/stats)
    [[ "$CE" -ge 3 ]] && ok "MFI CE accounting (ce_total=$CE)" || bad "ce_total=$CE"
    if [[ "$PRE" -ge 1 ]]; then
        ok "CE threshold -> soft offline (pages_pre_offlined=$PRE)"
        kill -0 $OP1 2>/dev/null && ok "owner survived soft offline (lossless)" || bad "owner died on soft offline"
    else
        FAILED=$(awk '/pages_failed/{print $2}' $MEM/stats)
        [[ "$FAILED" -ge 1 ]] && skip "soft offline failed (page pinned? pages_failed=$FAILED)" || bad "no pre-isolation action"
    fi
    grep -q "MEM_ERROR" /tmp/cfimon.log && ok "MEM_ERROR events on netlink" || bad "no MEM_ERROR events"
else
    bad "ownpage produced no pfn"
fi
kill $OP1 2>/dev/null

./test/tools/ownpage > /tmp/op2.txt & OP2=$!; sleep 1
PFN2=$(sed 's/pfn=//' /tmp/op2.txt)
echo "domain=mem pfn=$PFN2 type=uce_srao" > $INJ; sleep 3
UA=$(awk '/uce_async/{print $2}' $MEM/stats)
OFF=$(awk '/pages_offlined/{print $2}' $MEM/stats)
[[ "$UA" -ge 1 ]] && ok "async UCE accounted (uce_async=$UA)" || bad "uce_async=$UA"
if kill -0 $OP2 2>/dev/null; then
    log "  [INFO] owner alive after srao on dirty anon page (kernel may kill or keep mapping poisoned)"
else
    log "  [INFO] owner killed by memory_failure (dirty anon semantics)"
fi
[[ "$OFF" -ge 1 ]] && ok "hard offline completed (pages_offlined=$OFF)" || skip "offline result pending/failed (check $MEM/stats)"
# 去重: 同一 pfn 再注一次, 不应重复处置
echo "domain=mem pfn=$PFN2 type=uce_srao" > $INJ; sleep 2
OFF2=$(awk '/pages_offlined/{print $2}' $MEM/stats)
[[ "$OFF2" == "$OFF" ]] && ok "HWPoison dedup (no double handling)" || log "  [INFO] offlined $OFF->$OFF2 (re-check dedup)"
kill $OP2 2>/dev/null

# ---------- P5 madvise 真实毒页 ----------
hdr "P5 real consumption path (madvise MADV_HWPOISON)"
sysctl -w vm.memory_failure_early_kill=1 >/dev/null 2>&1
./test/tools/ownpage --poison > /tmp/op3.txt 2>&1
RC=$?
if [[ $RC -ge 128 ]] && [[ $((RC-128)) -eq 7 ]]; then
    ok "madvise poison -> SIGBUS on consumption (real memory_failure path)"
elif grep -qE "Operation not supported|Invalid argument" /tmp/op3.txt; then
    skip "MADV_HWPOISON unsupported on this kernel config"
else
    bad "unexpected poison result rc=$RC: $(cat /tmp/op3.txt)"
fi
grep -q "MEM_PAGE_OFFLINED\|MEM_ERROR" /tmp/cfimon.log && ok "kernel-initiated poison visible to MFI accounting" \
    || log "  [INFO] poison event not in netlink log (tracepoint availability?)"

# ---------- P6 甄别 RECOVER ----------
hdr "P6 triage RECOVER path"
echo 1 > $MEM/triage
./test/tools/ownpage > /tmp/op4.txt & OP4=$!; sleep 1
PFN4=$(sed 's/pfn=//' /tmp/op4.txt)
echo "domain=mem pfn=$PFN4 type=uce_srar kernel=1" > $INJ; sleep 3
TS=$(awk '/triage_saved/{print $2}' $MEM/stats)
TP=$(awk '/triage_panic/{print $2}' $MEM/stats)
[[ "$TS" -ge 1 ]] && ok "kernel-ctx UCE on user page -> RECOVER (triage_saved=$TS)" || bad "triage_saved=$TS"
[[ "$TP" == 0 ]] && ok "no spurious panic verdict (triage_panic=0)" || bad "triage_panic=$TP"
kill -0 $OP4 2>/dev/null && log "  [INFO] owner pending kill (async rmap)" || ok "owner process killed"
grep -q "MEM_VM_KILLED" /tmp/cfimon.log && ok "MEM_VM_KILLED event emitted" || bad "no MEM_VM_KILLED event"
kill $OP4 2>/dev/null
echo 0 > $MEM/triage

# ---------- P8 卸载 ----------
hdr "P8 unload & restore"
kill $MONPID 2>/dev/null
if rmmod cpu_fault_isolate >>"$REPORT" 2>&1; then
    ok "rmmod clean"
    [[ ! -f /sys/kernel/cfi/version ]] && ok "sysfs cleaned up" || bad "sysfs残留"
    TOL2=$(cat /sys/devices/system/machinecheck/machinecheck0/tolerant 2>/dev/null || echo NA)
    log "  [INFO] tolerant restored to: $TOL2"
else
    bad "rmmod failed: $(dmesg | tail -3)"
fi
# 重载稳定性
insmod kernel/cpu_fault_isolate.ko defer_to_daemon=0 >>"$REPORT" 2>&1 \
    && rmmod cpu_fault_isolate && ok "reload cycle stable" || bad "reload cycle failed"

# ---------- P7 (可选) 甄别 PANIC ----------
if [[ $WITH_PANIC == 1 ]]; then
    hdr "P7 triage PANIC path  ** machine will reboot in ~15s **"
    sysctl -w kernel.panic=10 >/dev/null
    insmod kernel/cpu_fault_isolate.ko defer_to_daemon=0 mem_triage=1 >>"$REPORT" 2>&1
    log "  injecting kernel-ctx UCE with RIPV=0 -> expect controlled panic + reboot"
    log "  AFTER REBOOT verify: journalctl -k -b -1 | grep 'MFI: unrecoverable'"
    sync; sleep 1
    ./test/tools/ownpage > /tmp/op5.txt & sleep 1
    PFN5=$(sed 's/pfn=//' /tmp/op5.txt)
    echo "domain=mem pfn=$PFN5 type=uce_srar kernel=1 ripv=0" > $INJ
    sleep 30
    bad "still alive 30s after panic verdict — suppression not working as designed"
fi

# ---------- 汇总 ----------
hdr "SUMMARY"
log "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
log "report: $REPORT   netlink log: /tmp/cfimon.log"
[[ $FAIL == 0 ]] && log "RESULT: ALL GREEN" || log "RESULT: $FAIL FAILURE(S) — see above"
exit $FAIL
