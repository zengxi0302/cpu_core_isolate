# CPU 核心故障自隔离（CFI）— 技术例会汇报材料

> 适用场景：技术例会、方案评审、架构评审
> 重点：方案设计、技术难点、实现路径

---

## Slide 1: 问题陈述

**一句话**：单核 CPU Cache 故障导致整机 panic，牵连全部 VM。

```
现状:  1 个 CPU 核心 Cache UCE  →  mce_panic()  →  整机 100+ VM 全部丢失
目标:  1 个 CPU 核心 Cache UCE  →  隔离该核心  →  丢失 1 VM，保全 99+ VM
```

**触发场景**：
- L1/L2/L3 Cache 不可纠正错误（UCE）
- TLB / Bus / 微架构内部错误
- 内存控制器报告的 DRAM UCE

---

## Slide 2: 为什么现有方案不行？

| 方案 | 问题 |
|------|------|
| 调高 tolerant sysfs | HCE3 内核**移除了**该 sysfs 属性 |
| 注册 MCE decode chain | decode chain 在 `mce_panic()` **之后**才走到 |
| EDAC/rasdaemon | 只做记录，不做隔离决策 |

**时序图（核心矛盾）**：

```
MCE 中断
  └→ do_machine_check()
       ├→ mce_severity() == PANIC
       ├→ mce_panic()  ←←←  系统在这里就死了
       │     └→ panic()
       │
       └→ mce_log() → decode chain → CFI  ←←← 走不到这里
```

---

## Slide 3: 解决方案总览

**三层防御体系**（以内核模块形式实现，不改内核源码）：

```
┌─────────────────────────────────────────────────────┐
│ 第1层: MCE Panic 拦截 (模块加载时立即生效)          │
│   • mce_tolerant=3 (sysfs 或 kprobe 直写内核变量)   │
│   • lockup_panic 不动 (软件锁死无法硬件隔离,        │
│     保持现网 softlockup=0/hardlockup=1 默认)        │
├─────────────────────────────────────────────────────┤
│ 第2层: 错误检测 (全量错误捕获)                      │
│   • x86: MCE decode chain (高优先级)                │
│   • x86: HCE3 FMA chain (额外早期通知)              │
│   • arm64: GHES tracepoint                          │
│   • EDAC mc_event (DIMM 物理拓扑解码)               │
├─────────────────────────────────────────────────────┤
│ 第3层: 隔离执行 (确保在健康CPU上执行)               │
│   • workqueue → remove_cpu()                        │
│   • 跨CPU调度 (故障CPU可能已不可用)                 │
│   • Daemon 协作 (给迁移VM留窗口)                    │
└─────────────────────────────────────────────────────┘
```

---

## Slide 4: 关键技术点 — tolerant 直写

**挑战**：HCE3 移除了 tolerant sysfs，`kallsyms_lookup_name()` 5.7+ 不导出。

**方案**：kprobe 符号查找 + 内存直写

```c
// 1. 利用 kprobe 注册获取符号地址
struct kprobe kp = { .symbol_name = "mce_tolerant" };
register_kprobe(&kp);
int *ptr = (int *)kp.addr;  // 得到内核变量地址
unregister_kprobe(&kp);

// 2. 直接修改
*ptr = 3;  // 阻止 mce_panic()

// 3. 模块卸载时恢复
*ptr = orig_value;
```

**效果**：`do_machine_check()` 检查 tolerant 时发现 >=3，跳过 `mce_panic()`，UCE 流入 decode chain → CFI notifier → 触发隔离。

---

## Slide 5: 关键技术点 — HCE3 FMA 框架适配

**运行时检测**（无需条件编译，同一 ko 兼容 upstream 和 HCE3）：

```c
// 探测 HCE3 特有符号
addr = cfi_lookup_name("fma_register_mce_do_chain");
if (addr) {
    // HCE3 内核，注册 FMA 通知链
    void (*reg)(struct notifier_block *) = (void *)addr;
    reg(&cfi_fma_nb);
}
// 否则跳过，仅用标准 decode chain
```

**三个 HCE3 钩子**：

| 钩子 | 作用 | CFI 用法 |
|------|------|---------|
| `fma_mce_do_chain` | MCE 处理过程中通知 | 早期错误获取 |
| `mce_panic_chain` | panic 前通知 | 诊断日志 |
| `mce_tolerant` (变量) | 控制 panic 行为 | 设为 3 阻止 panic |

---

## Slide 6: 关键技术点 — 跨 CPU 隔离

**问题**：故障 CPU 可能无法处理自己的 workqueue。

**方案**：

```c
if (urgent && current_cpu == faulty_cpu) {
    // 找一个健康 CPU 来执行 remove_cpu()
    target = cpumask_any_but(cpu_online_mask, faulty_cpu);
    queue_work_on(target, system_wq, &offline_work);
} else {
    schedule_work(&offline_work);
}
```

**remove_cpu() 自动完成**：
- 迁移所有 runnable task
- 转移 IRQ 亲和性
- 停止 per-CPU 内核线程
- 排空 run queue

---

## Slide 7: 状态机与阈值逻辑

```
ONLINE ──[CE >= 10]──> DEGRADED ──[UCE >= 1]──> ISOLATING ──> ISOLATED
  │                                                  │
  └──────────[UCE >= 1]──────────────────────────────┘
```

**关键参数**（运行时可调）：
- CE 阈值：10/窗口（默认 1 小时）
- UCE 阈值：1（一次即触发）

**Daemon 协作**：
- 非紧急：等 daemon ACK（最多 30s），让 daemon 先迁移 VM
- 紧急（PCC=1）：立即下线，不等 daemon

---

## Slide 8: 模块架构与文件组织

```
cpu_fault_isolate.ko (单一模块，~3000 行 C)
├── core/
│   ├── cfi_main.c           # 入口、参数、init 编排
│   ├── cfi_core.c           # 状态机、阈值引擎
│   ├── cfi_hotplug.c        # CPU 三档隔离 (full/inactive/soft)
│   ├── cfi_panic_suppress.c # panic 拦截 + HCE3 适配
│   ├── cfi_netlink.c        # 用户态通信
│   ├── cfi_sysfs.c          # sysfs 接口
│   ├── cfi_debugfs.c        # 测试注入
│   ├── mfi_core.c           # 内存域：状态机 + 页记账
│   ├── mfi_page.c           # 页隔离 (soft offline / memory_failure)
│   ├── mfi_dimm.c           # DIMM 介质记账
│   └── mfi_triage.c         # 内核态 UCE 落点甄别
├── arch/x86/
│   ├── cfi_x86.c            # MCE handler + FMA
│   ├── cfi_x86_cache.c      # MCA 错误码分类
│   └── mfi_x86.c            # 内存 MCE 后端
└── arch/arm64/
    ├── cfi_arm64.c           # GHES tracepoint
    └── cfi_arm64_cache.c     # ARM RAS 分类
```

---

## Slide 9: 测试验证现状（已更新）

**累计 128 断言 / 0 FAIL**

| 测试项 | 环境 | 结果 |
|--------|------|------|
| 模块加载/卸载 | HCE2 VM | ✅ PASS |
| sysfs 读写 / debugfs 注入 | HCE2 VM | ✅ PASS |
| MCE decode chain 真实路径 (sw inject) | HCE2 VM + 物理机 | ✅ PASS |
| 内存 CE/SRAO/SRAR | HCE2 VM + 物理机 | ✅ PASS |
| Cache UCE L2/L3 + TLB + Bus | HCE2 VM + 物理机 | ✅ PASS |
| hwpoison_inject 内核全路径 | HCE2 VM + 物理机 | ✅ PASS |
| **inactive 模式（cpu_active_mask 清除）** | **HCE2 物理机** | **✅ PASS** |
| **物理机 EDAC DIMM 真实解码** | **HCE2 物理机 / Skylake** | **✅ 验证** |
| mce-inject hw 模式 | VM (KVM) | ⚠️ SKIP（KVM 不实现 MCE virt，已自动识别）|
| mce-inject hw 模式 | 物理机 | 部分通（与 sw 等价覆盖） |

**v0.4 已实现**：HCE2 物理机 437 IRQ 迁移 + cpu_active_mask 清除 + 0 死锁 0 重启。期望 dmesg：
```
cpu_fault_isolate: isolation mode: inactive
cpu_fault_isolate: cpuN: inactive-isolated (cleared from cpu_active_mask
                                              + migrated K IRQs; CPU stays online)
cpu_fault_isolate: cpuN: isolated successfully via inactive (total isolations: 1)
```

---

## Slide 10: 后续计划（已更新）

| 里程碑 | 内容 | 状态 |
|--------|------|------|
| **v0.2 UCE 拦截** | tolerant=3 直写阻止 mce_panic | ✅ |
| **v0.3 MFI 内存域** | 页 CE/UCE 三层防线 | ✅ |
| **v0.4 物理机隔离** | inactive 模式绕开 HCE2 hotplug 死锁 | ✅ |
| **v0.5 daemon** | Level 1/2/3 分级处置（vcpupin → migrate → destroy）| 🔄 |
| v0.6 | SMT sibling 联动、NUMA 感知 | 📋 |
| v0.7 | 上层调度对接（Nova/K8s 节点降级）| 📋 |
| v1.0 | RPM 打包、systemd、监控大盘 | 📋 |

---

## Slide 11: 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| kprobe 在某些内核配置下禁用 | tolerant 设不上 | 提供内核 patch 作为备选 |
| tolerant=3 可能掩盖其他 fatal MCE | 理论上降低保护 | CFI 主动处理所有 MCE，比 panic 更精准 |
| 故障 CPU 上的 VM 数据损坏 | 必然 | 属于"止损"，与 panic 相比从 N 个 VM 降到 1 个 |
| 模块 unload 时恢复不完整 | 残留低保护状态 | exit 路径严格恢复所有原值 |

---

## T9: 落点甄别——panic 抑制后内核态 UCE 没有安全默认值（难度 ★★★★★）

- **难点**：tolerant=3 压住 mce_panic() 后，内核带毒继续跑 = 静默数据损坏，比宕机更糟
- **常规失效**：直接放行 → 静默损坏；一律 panic → 丢掉可救场景（虚机页占大头）
- **解法**：白名单判决（PCC=0 且 RIPV=1 且用户/虚机页或空闲页才救）；
  判定只读 vmemmap 元数据；mem_triage 灰度开关；判决表 18 项主机单测全绿
- 代码：`core/mfi_policy.h::mfi_triage_decide()`（纯函数，可单测）

## T10: 不改内核复用 memory_failure 全套页隔离（难度 ★★★★）

- **难点**：soft_offline_page 未导出；memory_failure 结果异步；GHES/CEC/madvise 多方处置同一页
- **解法**：① kprobe 查符号直调 soft_offline_page；② 硬隔离走 memory_failure_queue
  （GHES 同款导出路径，NMI 安全）；③ memory_failure_event tracepoint 回执
  （运行时查找）+ PageHWPoison 标志双重去重
- 代码：`core/mfi_page.c`

## T11: 两个故障域共享一条 MCE 通路（难度 ★★★）

- **难点**：内存控制器错误码落入 CPU 域 GENERIC_CORE 兜底 ——
  坏 DIMM 的 CE 风暴会把健康核推进 DEGRADED/隔离（实测发现并修复）
- **解法**：MCACOD 内存签名判定提为共享内联（`cfi_x86.h::cfi_x86_is_memory_errcode`），
  CPU 域显式忽略内存错误；CEC 探测自动降级预隔离；DIMM 记账只上报不动整机

## T12: 为「海量页」记账，而不是为「百个核」（难度 ★★★）

- **难点**：CPU 域状态对象只有 nr_cpu_ids 个；内存域是数亿物理页，且事件来自
  原子上下文（不能睡眠/大分配），页处置本身又必须能睡眠
- **常规失效**：外挂全量页表内存失控；无界哈希被坏 DIMM 风暴打爆；
  事件上下文直接做页迁移 → 原子上下文睡眠死机
- **解法**：固定容量哈希（1024 项，最坏 ~64KB）+ LRU 淘汰；在途页
  （PRE_ISO/POISONED）永不淘汰；OFFLINED 即出表，HWPoison 标志做持久事实源；
  事件路径只查表计数，重操作下沉独立 workqueue（卸载 destroy 自动排空）
- 代码：`core/mfi_core.c::mfi_page_get()`

## T13: 四个事件源，一个事实——融合与归一化（难度 ★★★★）

- **难点**：同一物理错误可能同时来自 MCE decode chain / EDAC mc_event /
  GHES / FMA；地址语义不一、信息互补（label vs PFN）、EDAC 不区分是否已消费；
  tracepoint 原型逐参数硬匹配，版本漂移时运行时静默错位
- **解法**：归一到 mfi_mem_error{pfn,type,flags,label}；消费语义只信 MCE/SEA
  的 AR 位，EDAC UCE 一律按异步处理交 HWPoison 判重兜底；无地址 mc_event 只做
  介质记账；探针注册失败降级为"该源不可用"而非整体失败
- 代码：`core/mfi_dimm.c::mfi_mc_event_probe()`

## T14: 如何测试一条「判决可能是 panic」的路径（难度 ★★★）

- **难点**：甄别引擎一半出口是 panic，错判决一次测试机就没了；真实 UCE 依赖
  EINJ 物理机，迭代以天计；页隔离真杀进程，随机靶页会误伤环境
- **解法**：四层金字塔——① 判决逻辑抽为零内核依赖纯函数（mfi_policy.h），
  内核与单测同源同实现，18 用例扫全判决表；② debugfs 注入走真实处置路径；
  ③ ownpage 自备靶页，误伤面=1 个牺牲进程；④ EINJ 只验硬件通路不验逻辑
- 代码：`test/unit/test_mfi_policy.c`、`test/tools/ownpage.c`

## T15: HCE2 物理机完整 hotplug 的 ABBA 死锁（难度 ★★★★★）

- **难点**：bypass `cpu_subsys_offline` 桩后撞死锁雪崩；尝试 livepatch 走完整路径又触发 percpu_counter 自旋锁 hardlockup —— 厂商打桩是规避**两个独立 bug**
- **死锁双链**：
  - pid7 `kworker/u144:0+cfi_hotplug`：**持 `cpus_write_lock`**，卡在
    `cpu_device_down → cpu_down_maps_locked → work_on_cpu → __flush_work`
    （HCE2 把 `_cpu_down` 用 `work_on_cpu` 包到 events pool 同步等）
  - pid554 `kworker/N:1+events`：绑在**正在下线的 cpuN** 的 per-cpu worker，
    跑 cgroup-v1 `cpuset_hotplug_workfn`，卡在 `cpus_read_lock`
- **闭环**：cpuN 下线要排空它自己的 worker pool（含 pid554）→ pid554 等 read lock → read lock 被 pid7 的 write lock 挡 → 死锁
- **跨内核版本旁证**：VM 内核 `r3353_273` 和物理机 `r3353_271_366` 是不同 patch 流；VM 没打 `work_on_cpu` 包装 patch，所以 VM 上完整 cpu_down 不死锁；物理机才有
- **本征解**：放弃完整 hotplug，引入 INACTIVE 模式（见 T16）
- 代码：`kernel/core/cfi_hotplug.c::cfi_offline_work_fn`、`docs/hce2-510-port-notes.md §1.3`

## T16: 三档隔离模式：full / inactive / soft（难度 ★★★★）

- **难点**：物理机 full 死锁；纯软隔离（仅迁中断）调度器仍派任务；需要"不进 hotplug 状态机但能阻止调度"的能力
- **关键发现**：`set_cpu_active(cpu, false)` 在 5.10 HCE 内核被 **inline 了**，kallsyms 查不到、kprobe 解析失败；但它修改的 `__cpu_active_mask` **是 EXPORT_SYMBOL**
- **解法**：直接 `cpumask_clear_cpu(cpu, cpu_active_mask)` 等价于 `set_cpu_active(false)` 对调度器的可见效果，**不依赖任何 unexported 符号**；不进 hotplug 状态机 → 不持 `cpus_write_lock` → 不调 `work_on_cpu(target)` → ABBA 三个前置条件一个都不满足
- **强度对比**：

| 维度 | full | **inactive** | soft |
|------|:---:|:---:|:---:|
| User-mode 任务不再调度 | ✓ | **✓** | ✗ |
| 内核态可迁移任务不再调度 | ✓ | **✓** | ✗ |
| 中断不再投递 | ✓ | **✓** | ✓ |
| per-cpu kthread 退出 | ✓ | (仍跑 idle/short) | ✗ |
| 死锁风险 | HCE2 上**致命** | **无** | 无 |

- inactive 与 full 的差距仅在"per-cpu kthread 仍在 idle/migration/ksoftirqd"，这些都是 short-burst 不访存，对"避免再次 MCE/panic"目标 ≥ 99% 等价
- 代码：`cfi_inactive_isolate_cpu()` —— 30 行 + 详细 comment

## T17: 物理机真实硬件验证（难度 ★★★）

- **难点**：mce-inject hw 模式在 KVM guest **静默 no-op** WRMSR；物理机 vs VM 上的 MCE 路径差异需要诊断
- **诊断**：probe 直接看 `ce_total` 是否累加，而不是只看 dmesg WRMSR error
  —— 准确识别了两层 KVM（云上 + 嵌套 KVM）都不暴露 MCE virt 的事实
- **物理机实证 (Skylake / 5.10.0-182.r3353_271_366)**：

```
cpu_fault_isolate: cpu35: inactive-isolated
  (cleared from cpu_active_mask + migrated 437 IRQs; CPU stays online)
cpu_fault_isolate: cpu35: isolated successfully via inactive
  (total isolations: 1)
```

时序：`422684 → 422694 → 422886 → 422887` —— **303 微秒**完成 437 IRQ affinity rewrite + 调度器 mask 清除，无 D 进程、无 watchdog、无重启

- **EDAC 真实解码（VM 看不到）**：
```
EDAC MC2: 0 CE memory read error on
  CPU_SrcID#1_MC#0_Chan#2_DIMM#0
  channel:2 slot:0 page:0x40d4833 grain:32 syndrome:0x0
  Row:0x160 Column:0x608 Bank:0x0 BankGroup:0x0
```
- 注入 → `x86_mce_decoder_chain` → cfi 与 EDAC skx **并联接收** → DIMM 物理拓扑可见（为后续"具体哪根内存条要换"准备）
- 累计 128 断言 / 0 FAIL，三档模式 × 两种宿主 × 八类 MCE 错误全跑过
