# 物理机 Panic 抑制实证 — 同一注入：无 CFI 宕机 / 有 CFI 隔离

## TL;DR

在真实 HCE2 + Huawei 2288H V5 物理机上，**完全相同的故障注入**：

| 状态 | 结果 |
|------|------|
| 未加载 `cpu_fault_isolate` 模块 | 内核 `mce_panic` → "Fatal machine check" → kdump → 整机重启 |
| 加载 `cpu_fault_isolate` (`isolation_mode=inactive`) | 目标 CPU 被 inactive-isolated，整机存活，业务不可调度到该 CPU |

这是物理机上 CFI 价值的最直接证据，对外汇报应作为核心 demo。

## 实验环境

| 项 | 值 |
|----|----|
| 整机型号 | Huawei 2288H V5 / BC11SPSCB0 |
| BIOS | 7.99 / 2021-03-11 |
| 内核 | `5.10.0-182.0.0.95.r2673_211_284.hce2.x86_64` |
| OS | HCE-2.0.2503_x86_64 |
| CPU | Intel Skylake/Cascade Lake (microcode 0x2000069) |
| 内存 | 186 GiB |
| RAS 架构 | FMA (Firmware-First) — `fma_memory_offline_notify` 编译进内核 |
| 注入工具 | 内核 `mce_inject` 模块 + 用户态 `mce-inject(8)` |

## 故障注入

单个 `.mce` 描述文件：

```
CPU 3
BANK 1
STATUS 0xB80000000000000E    # VAL|UC|EN|MISCV | simple-cache L2 (0x000E)
ADDR 0xDEADBEEF000
MISC 0x0
```

触发：

```bash
mce-inject mce_l2_uce_direct.mce
```

`mce-inject(8)` 内部会把 `/sys/kernel/debug/mce-inject/flags` 改写为 `raise`，然后 `WRMSR MSR_IA32_MCx_STATUS(1)` + `IPI-NMI` 到 CPU 3，使其进入 `do_machine_check` 主流程。

> ⚠️ 注意：`cat /sys/kernel/debug/mce-inject/flags` 看到 `sw` 只是静态默认值。`mce-inject` 工具运行期间会临时改写此节点，因此**这条注入实际走的是 hw / raise 路径，不是 sw 路径**。早期对"sw 注入永远不触发 panic"的描述只适用于直接 `echo $bank > bank` 的 debugfs 调用，不适用于 `mce-inject(8)` 工具调用。

## 实验 A：未加载 CFI → 整机 panic

注入后内核立即 panic（来自 `vmcore-dmesg.txt`）：

```
[ 2711.891575] mce: [Hardware Error]: CPU 3: Machine Check: 0 Bank 1: bc0000000000000e
[ 2711.891576] mce: [Hardware Error]: TSC 7f96455bd86 ADDR deadbeef000
[ 2711.891578] mce: [Hardware Error]: PROCESSOR 0:50654 TIME 1781609342 SOCKET 0 APIC 6 microcode 2000069
[ 2711.891579] mce: [Hardware Error]: Some CPUs didn't answer in synchronization
[ 2711.891579] mce: [Hardware Error]: Machine check: MCIP not set in MCA handler
[ 2711.891579] Kernel panic - not syncing: Fatal machine check
[ 2711.891580] CPU: 56 PID: 0 Comm: swapper/56 Kdump: loaded
...
Call Trace:
 <NMI>
  panic+0x1b4/0x3f0
  mce_panic+0x1be/0x2e0
  mce_reign+0x1d0/0x1f0
  mce_end+0xff/0x110
  do_machine_check+0x3df/0x590
  raise_exception+0x37/0xa0 [mce_inject]
  mce_raise_notify+0xaf/0xb6 [mce_inject]
  nmi_handle+0x58/0x100
  default_do_nmi+0x42/0x140
 </NMI>
```

**panic 真正成因 — Intel MCE 广播同步超时**：Intel MCE 默认是广播事件，所有 CPU 应同时进入 `do_machine_check`。但 `mce-inject(8)` 只在目标 CPU (3) 上 raise；其它 67 个 CPU 看不到 MCIP，`mce_reign` 等待同步超时 → `mce_panic("Some CPUs didn't answer in synchronization")` → kdump 拉起，整机重启。

**关键点**：这条 panic 路径**与注入事件的严重性档位（PCC/AR/S 位）无关**。只要 `mce-inject` 触发了 #MC 而没有 panic 抑制器，必然走到这里。

## 实验 B：加载 CFI inactive 模式 → CPU 隔离

模块加载：

```bash
insmod kernel/cpu_fault_isolate.ko \
    ce_threshold=3 uce_threshold=1 \
    window_secs=60 defer_to_daemon=0 \
    page_ce_threshold=3 mem_window_secs=300 \
    mem_triage=0 isolation_mode=inactive
```

**相同**的 `.mce` 文件，相同的 `mce-inject` 命令：

```
[  100.954959] Disabling lock debugging due to kernel taint
[  100.954990] mce: Triggering MCE exception on CPU 3
[  100.954991] mce: MCE exception done on CPU 3                       ← 没有 sync timeout panic
[  100.955037] mce: [Hardware Error]: Machine check events logged
[  100.955045] cpu_fault_isolate: cpu3: online -> isolating (ce=0 uce=1 types=0x4)
[  100.955052] cpu_fault_isolate: cpu3: inactive-isolating (sched_active=false + IRQ migration)
[  100.955311] cpu_fault_isolate: cpu3: inactive-isolated (cleared from cpu_active_mask + migrated 462 IRQs; CPU stays online)
[  100.955313] cpu_fault_isolate: cpu3: isolated successfully via inactive (total isolations: 1)
```

整机存活，cpu3 被 inactive-isolated（462 个 IRQ 被迁出，CPU 仍 online 但调度器不再派活）。

## CFI 抑制 panic 的机制

两层防护：

### 1. `mce_tolerant` 抬到 3（主防线）

`cfi_init()` 在加载时把 `/proc/sys/kernel/mce_tolerant` 从默认值（通常 1）抬到 3。Linux 5.10 内核 `arch/x86/kernel/cpu/mce/core.c` 中，**所有触发 `mce_panic` 的分支**都被 `cfg->tolerant < 3` 门控，包括：

- "Some CPUs didn't answer in synchronization"（实验 A 的成因）
- "MCIP not set in MCA handler"
- "Fatal machine check on current CPU"

`tolerant=3` 之后这些分支变成"记录 + 继续"，`do_machine_check` 干净返回，`mce_log()` 把事件投递到 `x86_mce_decoder_chain`。

### 2. `mce_panic_notifier` 兜底（次防线）

模块还注册了 `cfi_hce3_mce_panic_fn` 到 `panic_notifier_list`。万一 tolerant 门控没拦住的 panic，notifier 还能在 panic 入口做最后一次检查与日志记录。卸载时 dmesg 能看到：

```
mce_unregister_panic_notifier_chain success. nb: ffffffffc0d713a0,
nb_call: cfi_hce3_mce_panic_fn+0x0/0x50 [cpu_fault_isolate]
```

### 3. 隔离接力

panic 被抑制后，decode chain 继续投递。CFI 注册的 MCE notifier (`cfi_mce_chain_notifier`) 看到 bank 1 的 UC + L2 simple-cache，分类为 cache UCE，调用 `cfi_isolate_cpu(3)`：

- **inactive 模式**：`set_cpu_active(3, false)` 清掉调度器 active_mask；遍历目标 CPU 的中断亲和性，把所有可迁移的 IRQ 迁到其它 CPU（本次共 462 个）；不调 `cpu_down`，**完全绕开 CPU 热插拔的死锁面**。
- **full 模式**：调 `cpu_device_down`，真把 CPU 摘掉。这条路在本物理机上还会撞 vendor kernel 的 `percpu_counter_cpu_dead` 硬死锁（见下节），需要 livepatch 才能用。

## 验证：CPU 是真"不可调度"

inactive 模式下 cpu3 仍然 `online=1`，但任何尝试把任务绑过去都会失败：

```bash
$ taskset -c 3 yes
taskset: failed to set pid 31777's affinity: Invalid argument
```

`top` 实时观察 cpu3 负载（隔离后约 5 分钟）：

```
%Cpu3  :  0.0 us,  0.0 sy,  0.0 ni, 99.7 id,  0.0 wa,  0.3 hi,  0.0 si,  0.0 st
```

- `us` (user) = 0.0：业务进程不可能踩到
- `sy` (kernel) = 0.0：内核线程也基本不来（per-cpu kworker 因 active_mask 被排除）
- `hi` (hardirq) = 0.3：仅剩 timer / CMCI 等无法迁移的 per-cpu 硬中断
- `id` (idle) = 99.7：CPU 实际在等

**这是 inactive 模式最重要的卖点**：CPU 保留物理在线状态（无 hotplug 副作用），但调度器与中断子系统对它形同"已下线"。业务永远拿不到这个核。

## Full 模式边界条件

相同注入用 `isolation_mode=full`：

```
[  893.438402] cpu_fault_isolate: cpu3: online -> isolating
[  893.438408] cpu_fault_isolate: cpu3: taking offline
[  893.438414] cpu_fault_isolate: cpu3: remove_cpu() failed (-22), CPU still online — trying bypass
[  893.464602] smpboot: CPU 3 is now offline    ← bypass 路径成功下线
......
[  921.643230] NMI watchdog: Watchdog detected hard LOCKUP on cpu 0
  ...
  ? _raw_spin_lock+0x10/0x30
  percpu_counter_cpu_dead+0x44/0x90
  cpuhp_invoke_callback+0x268/0x3d0
  ...
  _cpu_down+0x118/0x200
  __cpu_down_maps_locked+0x11/0x20
  work_for_cpu_fn+0x16/0x20
  ...
```

下线本身成功，但 28 秒后 cpu0 在执行 `percpu_counter_cpu_dead` 回调时硬死锁。**这是 HCE2 r2673 内核的 vendor bug，跟 CFI 无关**：vendor patch 在 `cpu_dead` 回调里对一个全局 spinlock 的争用没正确处理，导致死等。

打上 `livepatch_percpu_counter` 热补丁后，full 模式即可正常完成：

```
[  645.864565] cpu_fault_isolate: cpu2: offlined via cpu_device_down() bypass
[  645.864567] cpu_fault_isolate: cpu2: isolated successfully via full (total isolations: 1)
```

**结论**：在打了 livepatch 的物理机上 full 与 inactive 都能跑；没打补丁的现场只能用 inactive。CFI 默认就是 inactive，正确处理了这个差异。

## 重现脚本

仓库中可重复的脚本化版本：

- `test/inject_cases/13_hw_cache_uce.sh` — 程序化版本，使用 `mce-inject(8)` 写同一份 `.mce` 文件。
  - 未加载 CFI 时给 5 秒 Ctrl-C 窗口，提示确认 kdump 已配置。
  - 加载 CFI 时观测 `cfi state / online / uce_count` + dmesg + taskset 排除性。
- `test/inject_cases/12_hw_mem_srar.sh` — 同样路径，注入 SRAR 内存事件。
- `test/inject_cases/README.md` — 全部 14 个 case 的对照矩阵与运行方式。

操作流程（**警告**：未加载 CFI 时**必定**触发 kdump，确保 kdump 服务正常 + `kernel.panic > 0`）：

```bash
# 1. 准备
modprobe mce_inject
dnf install -y mce-inject   # 或 zypper install -y mce-inject

# 2. 未加载 CFI - 这一次必定 kdump
rmmod cpu_fault_isolate 2>/dev/null
bash test/inject_cases/13_hw_cache_uce.sh
# (机器重启后, 收集 /var/crash/.../vmcore-dmesg.txt 作为证据)

# 3. 加载 CFI (inactive 模式) - 同样注入, 整机存活
insmod kernel/cpu_fault_isolate.ko ce_threshold=3 uce_threshold=1 \
    window_secs=60 defer_to_daemon=0 page_ce_threshold=3 \
    mem_window_secs=300 mem_triage=0 isolation_mode=inactive
bash test/inject_cases/13_hw_cache_uce.sh

# 4. 验证 cpu 被调度器排除
taskset -c $(($(nproc)/2)) yes        # 期望: Invalid argument
top -d 1                              # 期望: 目标 cpu idle ≈100%
```

## 对外汇报建议

这一对实验是物理机里程碑最直接的可视化材料。报告页里建议保留四样东西：

1. **同一份 `.mce` 文件** —— 截图，证明输入相同；
2. **vmcore-dmesg.txt 的 `mce_panic` 栈** —— 证明无 CFI 时整机宕；
3. **加载 CFI 后 dmesg 的 `cpu3: inactive-isolated ... 462 IRQs migrated` 行** —— 证明同样事件被吸收；
4. **`taskset -c 3 yes` 失败 + `top` cpu3 = 0%** —— 证明 CPU 真正"对业务不可见"。

不要把这条 panic 的成因泛化成"任何 UC 注入都会 panic"。它是"Intel 广播 MCE 同步超时" 这条具体路径，跟 `mce_tolerant` 门控直接相关。CFI 的价值不是"消除 #MC"，而是"接管 #MC 后续处置"。

## 补充实验：`--lmce` 揭示的真实硬件路径

默认 `--hw` 注入（mce-inject 单 CPU raise）触发的 panic 信息是 `Some CPUs didn't answer in synchronization` —— 这条路径是 **mce-inject 工艺**，不是真实硬件常见模式。真实硬件下 MCA 微码会同步广播给所有核，sync 必过，panic 由 `mce_severity` 决策。

加 `--lmce` 后 `MCGSTATUS=0xF`（含 `LMCE_S` 位），告诉内核这是 Local MCE，绕过广播 sync，走 severity-driven 路径。物理机实测：

```
mce: [Hardware Error]: CPU 0: Machine Check Exception: f Bank 4: bd80000000000094
                                                      ^
                                          MCGSTATUS=0xf, LMCE_S 起作用

mce: [Hardware Error]: Machine check: Action required: unknown MCACOD
Kernel panic - not syncing: Fatal local machine check
                                  ^^^^^
                              这就是真实硬件 panic 的标准信息
```

panic 信息从 `Fatal machine check`（广播版本，mce-inject 工艺）变成 `Fatal local machine check`（LMCE 版本，对应真实硬件路径）。对外讲故事时，`--lmce` 的 vmcore 比默认 `--hw` 的 vmcore **更接近真实硬件故障**。

## CFI 的真实覆盖边界

`--lmce + AR 严重性` 实验暴露了 CFI 的一条真实覆盖边界 —— **加载 CFI 之后这条 panic 仍然发生**：

```
cpu_fault_isolate: PANIC on cpu0: Fatal local machine check
cpu_fault_isolate: CFI was unable to prevent this panic.
                   Check if the fault source is covered by CFI.
```

原因在内核 MCE 处理本身：`do_machine_check` 在 `no_way_out` 路径调 `mce_panic("Fatal local machine check")` 时是**无条件触发**，不受 `mce_tolerant` 门控。内核态遇到 AR 严重性错误，没有用户进程可 kill 来恢复，内核就只能死。这是真实硬件下也无解的边界。

CFI 的覆盖矩阵：

| 场景 | 内核默认（无 CFI）| CFI 加载后 | 机制 |
|------|-------------------|------------|------|
| 广播 MCE sync 超时（mce-inject 单核 raise） | `mce_panic("Fatal machine check")` | **不 panic**，事件经 decode chain 处理 | `mce_tolerant=3` 门控 sync 超时分支 |
| 内存 CE / SRAO（pfn 可识别）| 内核 memory_failure 处理或忽略 | CFI 软/硬下线 page；机器活 | mfi page handler + memory_failure |
| L1 / L2 cache UCE（per-core）| 各自轻处理或忽略 | CFI notifier **隔离上报 CPU** | decode chain；cache 本地，单核隔离对 |
| TLB / Bus UCE（per-core）| 各自轻处理或忽略 | CFI notifier **隔离上报 CPU** | 同上 |
| L3 / LLC cache UCE（socket 共享）| 各自轻处理或忽略 | CFI **仅记账 + netlink**（默认）；**不**隔离 CPU | `isolate_on_l3_uce=0` 默认；L3 共享，隔离单核不解决根因 |
| 内核态 AR 严重性事件（如 `--lmce` + SRAR） | `mce_panic("Fatal local machine check")` | **同样 panic**，CFI panic_notifier 只能 logging | 内核无条件路径，tolerant 无能为力 |

**Cache 层级策略说明**：CFI 按物理 cache 拓扑分流隔离决策。Intel Skylake-SP / Cascade Lake-SP（你这台 2288H V5）+ AMD Zen 都是同一拓扑——L1（per-core，分 L1I 指令 + L1D 数据）+ L2（per-core，统一）+ L3（socket-shared，Intel 是 CHA tile 分布式，AMD 是 CCX-shared）。L1/L2 坏的话只影响那一个物理核，隔离它正确；L3 坏的话所有核共享同一片 LLC slice，隔离上报核**不解决根因**，需要在 userspace daemon 层做地址级 hwpoison 或者 socket-level drain。所以 CFI 模块参数 `isolate_on_l3_uce` 默认 **0**——L3 UCE 走"记账 + netlink，CPU 留在线让 daemon 决策"路径。要恢复早期"L3 UCE 一律隔离上报核"行为：`echo 1 > /sys/kernel/cfi/isolate_on_l3_uce`。

**汇报时这一条建议主动讲**：CFI 的卖点不是"消灭所有 panic"，而是"消灭那些原本不该 panic 的 panic（mce-inject 工艺、广播误判等不必要的整机宕机），让真正不可恢复的故障走 kdump 兜底，可控地保留现场。"

## 已知限制

1. **Hardware-first MCE 路径**：本物理机 RAS 用 FMA (firmware-first)，**真正的硬件 #MC**（真坏 DIMM）会被 BIOS SMM 先接走，再以 APEI/GHES 形式喂给 OS。本实验展示的是 OS 注入路径，FMA 路径下 CFI 走的是另一组通知器（`fma_memory_offline_notify` → cfi mfi page handler）。两条路径都有覆盖。
2. **`--lmce` 仅对带 S+AR 位的注入有明显效果**：不带 S/AR 的 UC（SRAO、cache UCE simple-code、TLB UCE、Bus UCE、CE）在 LMCE 模式下走 polling 路径，MCi_STATUS 又会被 FMA scrub，最终既不 panic 也不投递到 decode chain。这些 case 想走真 #MC 链路应该用默认 `--hw`（广播路径），CFI notifier 反而能在 sync 后接管 decode chain。
3. **CFI 拦不住内核态 AR severity panic**：见上节"CFI 的真实覆盖边界"。
4. **Full 模式依赖 livepatch**：见前面"Full 模式边界条件"节。
5. **Inactive 模式不下线 CPU**：cpu 仍然占资源（电、缓存、内存控制器配置），只是不再调度任务。如果客户要求"故障 CPU 彻底下电"，需要走 full 模式 + livepatch，或者人工 `echo 0 > /sys/devices/system/cpu/cpuN/online`。
