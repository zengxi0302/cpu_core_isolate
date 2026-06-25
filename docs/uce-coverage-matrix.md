# UCE 故障覆盖矩阵 —— CFI 模块加载收益对照

本文回答四个问题：

1. CPU 域 UCE 有几种故障模式，各自的 MCi_STATUS（MSR）取值是什么；
2. 内存域 UCE 有几种故障模式，各自的 MCi_STATUS 取值是什么；
3. 这些 UCE 在 upstream / 较老的 vendor 内核（4.18、5.10）下的**默认行为**——是 `mce_panic` 整机宕机，还是内核本就能放过去不宕；
4. CFI 模块加载后**能防住哪些、防不住哪些**，收益边界在哪。

> 数据来源：本仓库 `kernel/arch/x86/cfi_x86_cache.c`（分类逻辑）、`kernel/core/cfi_panic_suppress.c`（panic 抑制机制）、`test/inject_cases/env.sh`（MCi_STATUS 预设常量），以及已在 **HCE2 + Huawei 2288H V5 物理机实测**的结论（见 `docs/physical-host-panic-vs-isolation.md`）。

---

## 0. 先决：一个 UCE 到底会不会宕机，由什么决定

一个 UCE 上报为 `#MC` 后，内核走
`do_machine_check → mce_severity → (no_way_out?) → mce_panic`。
**"是不是 UCE"并不直接决定宕机**，决定宕机的是下面三件事：

| 因素 | MCi_STATUS 位 | 影响 |
|---|---|---|
| **PCC**（Processor Context Corrupt，处理器上下文损坏） | bit 57 | =1 → 内核无法续跑（`no_way_out`），**几乎必然 panic**，且**不受 `tolerant` 门控** |
| **触发上下文** | 由 RIPV/EIPV + 错误地址在用户态还是内核态决定 | 用户态消费 → 可 kill 进程恢复；内核态消费且无 extable 修复点 → panic |
| **广播同步是否成功** | 多核 broadcast `#MC` 的 monarch/reign 流程 | 有核没按时进同步 → `"Timeout: Not all CPUs entered broadcast exception handler"` / `"Some CPUs didn't answer in synchronization"` → panic。**这条受 `tolerant<3` 门控** |

### MCi_STATUS 位定义（Intel SDM Vol.3 Ch.15 / `cfi_x86.h`）

```
bit 63  VAL    错误有效
bit 62  OVER   溢出
bit 61  UC     未纠正错误（=0 即 CE）
bit 60  EN     错误使能
bit 59  MISCV  MCi_MISC 有效
bit 58  ADDRV  MCi_ADDR 有效
bit 57  PCC    处理器上下文损坏  ← 决定"无条件 panic"
bit 56  S      Signaling（UCR 能力位）
bit 55  AR     Action Required   ← S=1&AR=1 => SRAR；S=1&AR=0 => SRAO
bits[15:0] MCACOD 错误码（区分 cache/TLB/bus/mem 与层级）
```

### CFI 的抑制机制（`cfi_panic_suppress.c`）

CFI 加载时做两件事：

1. **把 `mce_tolerant` 抬到 3**：优先写 `/sys/devices/system/machinecheck/machinecheck{N}/tolerant`；失败则用 kprobe 直接定位并改写内核里的 `mce_tolerant` 变量（兼容 HCE3 的 `tolerant` 符号名）。`tolerant=3` 把**所有 `tolerant<3` 门控的 `mce_panic` 调用点**变成"记录 + 继续"，事件随后落到 `x86_mce_decoder_chain` → CFI notifier，由其做隔离 / 记账 / netlink。
2. **注册 `panic_notifier`（优先级 `INT_MAX`）**：仅作兜底。若 panic 仍逃过门控（即上面的"无条件 panic"边界），它只打印 `PANIC on cpuN: <reason>` 和 `CFI was unable to prevent this panic.`，然后放行，不阻止宕机。

> 结论先行：**CFI 能门控的是"`tolerant<3` 那一类 `mce_panic`"**（典型即广播同步超时），**门控不了 PCC=1 与内核态无修复点 AR**——后者真实硬件下也宕机。

---

## 1. CPU 域 UCE 故障模式（cache / TLB / bus / 内部）

CFI 通过 `cfi_x86_classify_errcode()` 把这些归到 CPU 域。下表 MCi_STATUS 给的是 **UCR（PCC=0，可恢复）** 取值；若该位置带 **UCF（PCC=1）**，再 `| 0x0200000000000000`（置 PCC 位）即可，此时落入"无条件 panic"边界。

| # | CPU UCE 模式 | MCi_STATUS (UCR) | 关键位 / errcode | 内核默认 (4.18 / 5.10 upstream) | CFI 加载后 | 收益 | 复现 case |
|---|---|---|---|---|---|---|---|
| 1 | **L1D cache UCE** | `0xb00000000000000d` | UC，errcode=0x0D（simple LL=1） | 广播 `#MC` 同步失败 → **mce_panic 宕机**；同步成功多为 kill 当前任务 | tolerant=3 门掉同步超时 panic + **隔离该 core** | 🟢 救机 + 单核隔离 | `15_cache_uce_l1.sh` |
| 2 | **L1I cache UCE** | `0xb000000000000801` | UC，compound TT=Instr LL=L1 | 同上 | 同上，分类成 L1I，**隔离该 core** | 🟢 救机 + 单核隔离 | `16_cache_uce_l1i.sh` |
| 3 | **L2 cache UCE** | `0xb00000000000000e` | UC，errcode=0x0E（simple LL=2） | 同上（**物理机实测：无 CFI → mce_panic + kdump**） | tolerant=3 门控 + **隔离该 core**（实测整机存活） | 🟢 **救机（已实测）** | `04_cache_uce_l2.sh` / `13_hw_cache_uce.sh` |
| 4 | **L3 / LLC UCE** | `0xb00000000000000f` | UC，errcode=0x0F（LL=3） | 同上 | tolerant=3 门控 panic 存活，但**默认不隔离 core**（`isolate_on_l3_uce=0`，L3 socket 共享），仅记账 + netlink | 🟢 救机 / 🟡 隔离受限 | `05_cache_uce_l3.sh` |
| 5 | **TLB UCE** | `0xb000000000000816` | UC，compound bit4=TLB | 同上 | tolerant=3 门控 + 归类 TLB，per-core 可隔离 | 🟢 救机 + 隔离 | `07_tlb_uce.sh` |
| 6 | **Bus / 互联 UCE** | `0xb000000000000e0f` | UC，compound class=0xE | 同上 | tolerant=3 门控 + 归类 BUS；部分总线错误非 per-cpu，可能只记账 | 🟢 救机 / 🟡 归属可能模糊 | `08_bus_uce.sh` |
| 7 | **内部 / 通用核错误** | errcode 0x0400–0x0FFF | UC | 同上 | tolerant=3 门控 + `CFI_ERR_INTERNAL` 记账 / 隔离 | 🟢 救机 | （无独立 case） |
| — | **以上任意 + PCC=1（UCF）** | `…\| 0x0200…` | UC + **PCC** | **无条件 panic**（处理器上下文损坏，no_way_out，不受 tolerant 门控） | panic_notifier 仅事后记日志 | 🔴 **救不了**（硬件语义如此） | — |

> **关键说明**：表中第 1–7 行，无 CFI 时在物理机上**真正把整机打宕的是"广播同步超时"路径**——也就是 `Timeout: Not all CPUs entered broadcast exception handler` / `Some CPUs didn't answer in synchronization`。这条受 `tolerant<3` 门控，所以 CFI 能稳定防住。而 PCC=1（UCF）那条是真·不可恢复，`tolerant` 门控不了，谁都救不了。

---

## 2. 内存域 UCE 故障模式

CFI 通过 `cfi_x86_is_memory_errcode()` 识别内存错误并交给 MFI 子系统。内存 UCE 主要 4 种严重度模式（外加 PCC=1 边界）：

| # | 内存 UCE 模式 | MCi_STATUS | 关键位 | 内核默认 (4.18 / 5.10 upstream) | CFI 加载后 | 收益 | 复现 case |
|---|---|---|---|---|---|---|---|
| 1 | **SRAO**（异步可恢复，patrol scrub 发现，未消费） | `0xbc00000000000094` | UC，**S=1，AR=0**，PCC=0 | **不宕机**：MCE_AO → `memory_failure_queue` 异步下线页 | 不宕机 + `uce_async` 记账 + 硬下线页 + netlink | 🟡 内核本就不宕；CFI 加隔离 / 上报 | `02_mem_srao.sh` / `11_hw_mem_srao.sh` / `21_einj_mem_uce_nonfatal.sh` |
| 2 | **SRAR / 用户态消费**（同步，正在使用的数据） | `0xbd80000000000094` | UC，**S=1，AR=1**，PCC=0，**用户态** | **不宕机**：MCE_AR → `memory_failure(MF_ACTION_REQUIRED)` → SIGBUS 杀进程 | 不宕机 + 同步下线 + owner SIGBUS + 记账 | 🟡 内核本就不宕；CFI 加记账 / 上报 | `03_mem_srar.sh` / `12_hw_mem_srar.sh` / `22_einj_mem_uce_fatal.sh` |
| 3 | **SRAR / 内核态消费**（无 extable 修复点） | `0xbd80000000000094`（内核态命中） | UC，S=1，AR=1，**内核态** | **宕机**：`mce_panic("Fatal machine check")`，**不受 tolerant 门控** | panic_notifier 仅事后记日志 | 🔴 **救不了**（无进程可杀，内核无恢复路径） | `12 --lmce`（实测仍 panic） |
| 4 | **UCNA**（uncorrected no-action，发现未消费） | `0xb800000000000094` | UC，**S=0，AR=0**，PCC=0 | **不宕机**：deferred，`memory_failure_queue` 下线页 | 不宕机 + 记账 + netlink | 🟡 内核本就不宕；CFI 加隔离 / 上报 | （UCNA 变体，参 `02`） |
| — | **内存 UCE + PCC=1** | `…\| 0x0200…` | UC + **PCC** | **无条件 panic** | 救不了 | 🔴 硬件语义如此 | — |

> 注：case 03 在 sw 注入路径下**不进** `do_machine_check`，所以无 CFI 也不 panic（只是没动作）；真正能演示"内存 UCE 宕机 vs 存活"的是真 `#MC` 路径的 case 12（mce-inject）与 case 22（EINJ）。case 03 `--lmce`（带 S+AR、内核态命中）是上表第 3 行那条**CFI 也救不了**的边界，已在物理机实测会 panic。

---

## 3. 收益总结（模块上线后到底多了什么）

把上面两表按"收益类型"压缩——这是对外汇报最该用的一张：

| 故障类别 | 无 CFI（upstream 4.18 / 5.10）默认 | CFI 收益 | 性质 |
|---|---|---|---|
| **cache/TLB/bus UCE（per-core，走广播同步路径）** | 广播同步超时 → **mce_panic + kdump 整机宕** | **门掉 panic + 隔离坏核，整机存活** | 🟢 **救机**（核心卖点，已物理实测） |
| **L3 / LLC UCE** | 同上宕机 | **门掉 panic 存活**；但隔离单核无意义（根因在共享 LLC），默认只记账 | 🟢 救机 / 🟡 隔离受限 |
| **内存 SRAO / 用户态 SRAR / UCNA** | 内核已能不宕（下线页 / 杀进程） | **不是救机**，是多了页下线追踪、DIMM 记账、netlink 上报、CE 预隔离 | 🟡 增强可观测 / 可运维 |
| **内核态 SRAR（无修复点）** | mce_panic（不受 tolerant 门控） | 救不了，只能事后日志 | 🔴 边界，应主动讲 |
| **任意 PCC=1（UCF）** | 无条件 panic | 救不了 | 🔴 边界，硬件语义 |

---

## 4. 给汇报用的三句话结论

- **CFI 真正"救机"的是 CPU 域 per-core UCE（L1 / L2 / TLB / bus）走广播 `#MC` 同步那条路径**——这正是最初遇到的 `Timeout: Not all CPUs entered broadcast exception handler` 宕机；`tolerant=3` 门控 + 坏核隔离后整机存活（HCE2 + 2288H V5 已实测，见 `physical-host-panic-vs-isolation.md`）。
- **内存类 UCE（SRAO / 用户态 SRAR / UCNA）在 4.18 / 5.10 内核本来就不宕机**，CFI 的价值在隔离追踪与上报，不是防宕；汇报时不要算成"救机"。
- **两类边界 CFI 明确救不了**：① PCC=1（处理器上下文损坏）；② 内核态消费、无 extable 修复点的 AR 错误。这两条 `mce_panic` 不受 `tolerant` 门控，真实硬件下也宕机，应主动讲清楚，避免被反问"那真坏 DIMM 命中内核态时 CFI 救得了吗"——诚实答："救不了，那种情况真实硬件下也会宕；CFI 拦的是不必要的 panic（广播同步误判、坏核本可隔离）。"

---

## 5. 注入路径与本表的对应

| 注入方式 | 是否真发 `#MC` | 适用 | 对应 case |
|---|---|---|---|
| `flags=sw`（debugfs） | 否，仅 `mce_log` 解码链 | VM / 物理机通用，永不 panic，只验证分类与策略 | 01–08、15–17 |
| `mce-inject(8)` 用户态 raise | **是**（WRMSR + IPI-NMI 单核 raise） | 物理机演示"宕机 vs 存活"；panic 成因是**广播同步超时工艺** | 10–14 |
| **APEI EINJ**（固件层） | **是**（固件 → CPER → GHES） | 物理机，**仅 Memory / PCIe**（多数平台不暴露 Processor 类型）；panic 成因是**真实严重性驱动** | 20–28 |

> 同一个故障，建议 mce-inject 与 EINJ 两条路都跑：mce-inject 在 VM 上也能跑、覆盖 CPU 域；EINJ 在物理机上 panic 成因更接近真实硬件（`Fatal machine check on current CPU` 而非同步超时工艺），但 HCE2 实测**不支持 Processor 类 EINJ**，CPU/cache 域仍需走 mce-inject。详见 `test/inject_cases/README.md`。
