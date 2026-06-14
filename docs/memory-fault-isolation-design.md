# 内存故障自隔离（MFI）— 技术设计文档

**版本**: 0.4.0
**日期**: 2026-06
**作者**: openEuler Community
**状态**: Phase 1/2 内核实现完成（v0.2–v0.4），软件路径自测通过，实机验证中
**关联**: 本文档是 CFI（CPU 核心故障自隔离）项目的扩展，复用其框架，详见 `design-document.md`

---

## 1. 背景与问题定义

### 1.1 现状

内存（DRAM/HBM）与 CPU 同为易损坏器件，其 UCE 同样通过 MCE（x86）/
SEA & SError（arm64）上报，是宿主机宕机的另一大来源。

当前系统的处理能力边界：

| 场景 | 现有行为 | 结果 |
|------|---------|------|
| 用户态进程消费毒页 | `memory_failure()` 杀进程 | 单进程/单 VM 损失，可接受 |
| `copy_from_user` 等 uaccess 路径 | extable fixup → 杀进程 | 单进程/单 VM 损失，可接受 |
| **内核态消费毒页（无 fixup）** | `mce_panic()` / `die()` | **整机宕机，所有 VM 丢失** |
| **patrol scrub 发现 UCE（未消费）** | 行为依赖配置，缺乏统计与上报 | 隔离机会被浪费 |
| **CE 高频页（UCE 前兆）** | 仅 EDAC 计数，无主动动作 | 坐等恶化为 UCE |

### 1.2 内核态 UCE 的可恢复性分类（核心结论）

"内核态 UCE 是否有隔离机会"不取决于"内核态"这个笼统上下文，而取决于
**故障页归属** × **消费方式** 两个维度：

| # | 场景 | 典型来源 | 可恢复性 | 机会窗口 |
|---|------|---------|---------|---------|
| A | 异步发现，未消费（SRAO / patrol scrub / deferred error） | 巡检、prefetch、写回 | **完全可恢复** | 页隔离，零进程损失或仅杀属主 |
| B | 内核态同步消费**用户页/虚机页**，有 MC-safe fixup | uaccess、`copy_mc_to_kernel` | 可恢复 | 杀属主进程（已有能力） |
| C | 内核态同步消费**用户页/虚机页**，无 fixup | KSM 比对、页迁移、vhost、coredump、swap | 当前不可恢复 | **扩 fixup 覆盖（需内核补丁）** |
| D | 虚机 vCPU 直接消费虚机页 | guest 业务访问 | 可恢复 | KVM 注入 vMCE/SEA，guest 内部收敛 |
| E | 内核自身数据被同步消费（slab、页表、内核栈/text） | — | **不可恢复** | 仅能降损：快速 kdump、事前预隔离 |

**诚实的边界**：场景 E 没有隔离机会——SRAR 无 fixup 时指令无法跳过重试
（RIPV 可能不置位），压住 panic 继续运行等于静默数据损坏，比宕机更糟。
本项目对 E 的策略是**主动快速 panic + kdump**，并通过事前预隔离（场景 A
的延伸）压低 E 的发生概率。

### 1.3 设计目标

1. **预隔离**：CE 高频页在恶化为 UCE 前主动 soft offline（零损失）
2. **吃掉异步 UCE**：SRAO/deferred error 走页隔离而非 panic
3. **落点甄别**：panic 抑制后，按页归属差异化处置——能救的救，
   不能救的快速 kdump，绝不静默
4. **VM 级收敛**：虚机页故障 → 仅影响对应 VM，并 netlink 上报触发热迁移
5. **介质级记账**：按 DIMM/rank 维度统计，达到阈值建议整机疏散

### 1.4 约束条件

与 CFI 一致：

- Phase 1/2 以内核模块（ko）形式交付，不改内核源码
- 兼容 openEuler 2403 LTS / HCE3 / Linux 6.6
- 支持 x86_64（Intel/AMD）和 aarch64（鲲鹏 920/930）
- 与 rasdaemon、EDAC、mcelog、CONFIG_RAS_CEC 共存
- **ko 形态无法为既有内核代码添加异常表项**——场景 C 的覆盖面扩展
  （Phase 3）必须以内核补丁交付，走 openEuler 社区路线

---

## 2. 总体路线（三阶段）

| 阶段 | 交付形态 | 内容 | 风险 |
|------|---------|------|------|
| **Phase 1** | ko | CE 统计 + 页级预隔离 + SRAO/deferred 主动隔离 + DIMM/rank 记账 + netlink 上报 | 低（纯收益，不改变 panic 语义） |
| **Phase 2** | ko | 内核态 UCE 落点甄别 + 差异化处置（杀属主 vs 快速 kdump） | 中（依赖 panic 抑制，须严格限定适用范围） |
| **Phase 3** | 内核补丁 | 扩大 machine-check-safe 覆盖：arm64 copy_mc 系列、KSM、页迁移、vhost 路径 | 社区节奏（openEuler 已有 arm64 MC-safe 系列补丁可对齐） |

本文档详细设计 Phase 1，给出 Phase 2 的设计要点与边界，Phase 3 仅列方向。

---

## 3. 系统架构

MFI 作为 `cpu_fault_isolate.ko` 内的新故障域（domain）实现，与 CPU 域并列，
共用 panic 抑制层、netlink、daemon 协议与 sysfs 框架：

```
┌─────────────────────────────────────────────────────────────────────┐
│                        cpu_fault_isolate.ko                         │
├──────────────────────────────┬──────────────────────────────────────┤
│   CPU Fault Domain (CFI)     │   Memory Fault Domain (MFI)  ← 新增  │
│                              │                                      │
│  • Cache UCE → CPU 隔离      │  • Page Accounting（PFN 哈希表）     │
│  • TLB/Bus/Internal → 隔离   │  • Page Pre-Isolation（soft offline）│
│  • 三档隔离 (full/inactive/  │  • UCE Triage（落点甄别, Phase 2）   │
│    soft)                     │  • DIMM/Rank Accounting              │
├──────────────────────────────┴──────────────────────────────────────┤
│              共享基础设施（复用，少量扩展）                          │
│                                                                      │
│  • Panic Suppression（tolerant=3 / kprobe / FMA hooks）             │
│  • Generic Netlink "CFI"（新增 domain 属性 + MEM_* 命令）           │
│  • Sysfs / Debugfs（新增 /sys/kernel/cfi/mem/ 子树）                │
│  • Daemon 协议（cfid 新增内存事件处理）                              │
├──────────────────────────────────────────────────────────────────────┤
│                       Arch Backends（扩展）                          │
│                                                                      │
│  • x86: MCE decode chain → 内存错误解码（MCACOD 分类、SRAO/SRAR）   │
│  • arm64: GHES/APEI → CPER Memory Error Section 解码                │
│  • 公共: trace_mc_event（EDAC CE）、memory_failure_event tracepoint │
└──────────────────────────────────────────────────────────────────────┘
```

### 3.1 事件源（Phase 1）

| 事件源 | 架构 | 获取方式 | 提供信息 |
|--------|------|---------|---------|
| MCE decode chain | x86 | 已注册的 notifier（CFI 复用） | MCACOD、PFN（MCi_ADDR）、严重级 |
| `fma_mce_do_chain` | x86/HCE3 | kprobe 探测（CFI 复用） | 早期 MCE 事件 |
| GHES/APEI tracepoint | arm64 | `trace_arm_event` / CPER 解码 | 物理地址、错误类型、deferred 标志 |
| `trace_mc_event` | 公共 | EDAC tracepoint 挂接 | CE 计数、DIMM/rank/channel 定位 |
| `memory_failure_event` | 公共 | RAS tracepoint 挂接 | 页隔离结果回执（成功/失败/已忽略） |

### 3.2 关键依赖符号

| 符号 | 导出状态 | 获取方式 |
|------|---------|---------|
| `memory_failure_queue()` | EXPORT_SYMBOL_GPL | 直接调用（GHES 同款路径） |
| `soft_offline_page()` | 未导出 | `cfi_lookup_name()` kprobe 技巧（CFI 已有同款方案） |
| `pfn_to_online_page()` | EXPORT_SYMBOL_GPL | 直接调用 |

`soft_offline_page` 的备选方案：通过
`/sys/devices/system/memory/soft_offline_page` 内核内写入。优先 kprobe
直调，与 CFI 处理 `mce_tolerant` 的既有模式保持一致。

---

## 4. Phase 1 详细设计

### 4.1 页级 CE 记账与预隔离

DRAM 失效有明显前兆：同一行/列的 CE 反复出现，最终恶化为 UCE。
内核自带的 CEC（`CONFIG_RAS_CEC`）做了类似工作，但仅 x86、策略固定、
无上报。MFI 的页记账是其策略化、跨架构、可上报的增强版：

```
trace_mc_event (CE, 携带 PFN)
        │
        v
┌──────────────────────────────┐
│  PFN 哈希表（固定容量 1024） │
│  ┌────────────────────────┐  │
│  │ pfn | ce_count | first │  │     ce_count >= page_ce_threshold
│  │     | last_ts  | state │  │ ──────────────────────────────────┐
│  └────────────────────────┘  │                                   │
│  LRU 淘汰 + 窗口过期清理     │                                   v
└──────────────────────────────┘                    ┌──────────────────────┐
                                                    │ 预隔离工作队列        │
                                                    │ soft_offline_page()  │
                                                    │ （迁移页内容后下线， │
                                                    │   不杀任何进程）     │
                                                    └──────────────────────┘
```

要点：

- **soft offline 是无损操作**：内核先迁移页内容（用户页 migrate、
  pagecache 丢弃重读），成功后将物理页标记 HWPoison 永不再分配。
  业务无感知。
- 迁移失败（如页被长期 pin 住，典型如设备直通 VM 的内存）→ 记
  `PRE_ISO_FAILED`，netlink 上报，由 daemon 决策（如建议迁移该 VM）。
- 与 CEC 共存：检测到 `RAS_CEC` 启用时，MFI 默认只记账上报、不重复
  soft offline（`mem_pre_isolate=0`），避免双重动作。

### 4.2 异步 UCE（SRAO / deferred）主动隔离

数据未被消费，是最干净的隔离窗口：

| 架构 | 识别特征 | 动作 |
|------|---------|------|
| x86 | SRAO：MCACOD 0x0C0–0x0CF（patrol scrub）、0x17A（L3 显式写回），且 `MCi_STATUS.AR=0` | `memory_failure_queue(pfn, 0)` |
| arm64 | CPER Memory Error Section + deferred error 标志（RAS 扩展 ESB 路径） | `memory_failure_queue(pfn, 0)` |

`memory_failure()` 的内置逻辑负责分类处置：干净 pagecache 直接丢弃、
脏页/匿名页杀映射进程、空闲页直接下线。MFI 在 `memory_failure_event`
tracepoint 上接收回执并记账、上报。

注意：upstream GHES 对 arm64 已有 `memory_failure_queue` 调用；x86 的
SRAO 在 `tolerant>=1` 时 uc_decode 也会走到。**MFI 在这里的角色是兜底 +
记账 + 上报**——确认每个异步 UCE 都被页隔离消化，而非依赖发行版配置；
检测到内核已处理（通过 tracepoint 回执去重）则只记账不重复触发。

### 4.3 DIMM/Rank 介质级记账

单页隔离治标，介质劣化治本要靠换硬件/疏散：

- 从 `trace_mc_event` 提取 EDAC 定位信息（mc/csrow/channel 或
  dimm label），维护 per-DIMM 计数器（CE/UCE 分开，滑动窗口同 CFI）。
- DIMM 级 UCE 达 `dimm_uce_threshold` 或 CE 达 `dimm_ce_threshold` →
  netlink 发送 `MEM_MIGRATE_ADVISED` 事件，建议 daemon 触发整机 VM
  疏散 + 报修。
- **MFI 不做整机级自动动作**（不主动触发疏散/下电），介质级决策权
  完全交给 daemon/上层调度——这与 CFI 的 CPU offline 不同，因为内存
  不存在"摘除一根 DIMM 继续跑"的在线操作。

### 4.4 页状态机

```
                    CE >= page_ce_threshold
    ┌─────────┐    （窗口内）        ┌──────────┐
    │ WATCHED │ ───────────────────> │ PRE_ISO  │
    └─────────┘                      └──────────┘
         │                             │       │
         │ UCE (SRAO/deferred)         │成功    │迁移失败
         v                             v       v
    ┌──────────┐  memory_failure  ┌─────────┐ ┌──────────────┐
    │ POISONED │ ───────────────> │ OFFLINED│ │ PRE_ISO_FAIL │
    └──────────┘   成功            └─────────┘ └──────────────┘
         │
         │ memory_failure 失败
         v
    ┌──────────┐
    │  FAILED  │ ──> netlink 告警（页仍带毒在线，建议迁移属主 VM）
    └──────────┘
```

与 CFI 的 CPU 状态机不同点：页是海量对象，状态只在哈希表内维护，
OFFLINED 后从表中移除（内核 HWPoison 标记是持久事实来源）。

### 4.5 模块组成（实际落地）

| 文件 | 职责 |
|------|------|
| `core/mfi_core.c` | 页哈希表（256 桶/1024 上限/LRU）、状态机、域 init/exit、CEC 检测 |
| `core/mfi_page.c` | soft offline（kprobe 解析符号）/ memory_failure_queue / memory_failure_event 回执 |
| `core/mfi_dimm.c` | DIMM/rank 记账 + `ras:mc_event` 探针（兼作 arm64 地址来源） |
| `core/mfi_triage.c` | Phase-2 落点甄别（页归属分类 + 判决执行） |
| `core/mfi_policy.h` | 纯决策函数（甄别判决/预隔离门控/窗口），与主机单测共享 |
| `core/mfi_sysfs.c` | /sys/kernel/cfi/mem/ 子树 |
| `arch/x86/mfi_x86.c` | MCACOD 内存错误分类（SRAO/SRAR/deferred 判定）、甄别入口 |
| `core/cfi_netlink.c`（扩展） | MEM_* 命令与 domain 属性 |
| `core/cfi_debugfs.c`（扩展） | 内存错误软件注入（含 ripv/pcc 旋钮） |
| `test/tools/cfimon.c` | 零依赖 genetlink 监视器 / MEM_GET_STATUS 客户端（cfid 参考实现） |
| `test/tools/ownpage.c` | 注入靶页辅助（mmap+mlock+pagemap） |
| `test/unit/test_mfi_policy.c` | 策略单测（18 用例，已全绿） |

实现偏差说明：

- **未新增 `arch/arm64/mfi_arm64.c`**：arm64 的内存错误经
  `ghes_edac → ras:mc_event`（带地址与 DIMM label）和
  `memory_failure_event` 回执两个公共探针即可覆盖，GHES 自身已对毒页
  调用 `memory_failure_queue`，Phase 1 无需独立后端。
- **跨域修正**：CPU 域 x86 分类器原本会把内存 MCACOD 落入
  GENERIC_CORE 兜底并计入 CPU 计数（坏 DIMM 可拖垮健康核），已在
  `cfi_x86.h` 提取共享的 `cfi_x86_is_memory_errcode()` 并让 CPU 域
  显式忽略内存错误。

---

## 5. Phase 2 设计要点：内核态 UCE 落点甄别

### 5.1 前提与边界（必须严格遵守）

CFI 的 panic 抑制（tolerant=3）已经使内存 UCE 的 fatal MCE 也流入
decode chain。**这对场景 E 是危险的**——必须由 MFI 的甄别逻辑补上
"不能救就主动死"的环节，否则 panic 抑制反而引入静默损坏风险。

适用范围（白名单制，不在名单内一律快速 panic）：

1. AO 类错误（未消费）→ Phase 1 已覆盖
2. 有 fixup 的 SRAR → 内核自行恢复，MFI 只记账
3. **无 fixup 的 SRAR，且毒页是用户页/虚机页，且 RIPV=1**
   （x86：`MCG_STATUS.RIPV` 置位，返回地址有效）→ 甄别处置

判决的纯逻辑实现在 `mfi_policy.h::mfi_triage_decide()`，由
`test/unit/test_mfi_policy.c` 按本节判决表全覆盖验证。补充一个实现
细化：buddy 空闲页（`MFI_PG_FREE`）也允许 RECOVER——没有活映射消费
它，直接隔离即可。

### 5.2 甄别流程

```
MCE/SEA（内核态，无 fixup，panic 已被抑制）
        │
        v
   PFN 有效？ ──否──> 主动 panic（信息不足，不赌）
        │是
        v
   pfn_to_online_page() → 页归属判定
        │
   ┌────┴─────────────────────────┐
   │                              │
   v                              v
 用户页/虚机页                  内核页
 (PageLRU / PageAnon /         (PageSlab / 页表 /
  hugetlb / guest memslot)      reserved / 内核 text)
   │                              │
   v                              v
 RIPV=1？ ──否──> 主动 panic    主动 panic + 触发 kdump
   │是                          （快速保现场，绝不静默）
   v
 memory_failure(pfn) 杀属主
   + 若属主为 qemu →
     netlink MEM_VM_KILLED 事件
     （daemon 通知调度器重建 VM）
```

### 5.3 已知风险与缓解

| 风险 | 缓解 |
|------|------|
| RIPV=1 但损坏已扩散（如 DMA 写入） | 仅对单 bank、单地址、无 PCC 标志的 MCE 启用甄别；多 bank 同报 → panic |
| 页归属判定本身访问出错页元数据 | 只读 struct page（位于 vmemmap，与数据页不同介质区域）；判定全程不触碰毒页本身 |
| 与 upstream `kill_me_maybe` 路径重复动作 | 通过 hwpoison 标记判重，已标记则跳过 |
| arm64 无 RIPV 等价语义 | Phase 2 首版仅 x86；arm64 视 openEuler MC-safe 系列普及度评估 |

---

## 6. Phase 3 方向：扩大 MC-safe 覆盖（内核补丁）

ko 无法为既有代码添加异常表项，以下路径的恢复能力需要内核补丁，
按云场景收益排序：

1. **页迁移路径**（`migrate_pages` 的页拷贝）——热迁移期间命中虚机
   毒页直接 panic 是当前最痛的场景之一；改造为 `copy_mc_highpage`
   并在失败时放弃迁移该页 + 标记 hwpoison。
2. **KSM 页比对**（`memcmp_pages`）——MC-safe 化，比对失败按页隔离处理。
3. **vhost/virtio 内核态拷贝**——guest 内存的宿主内核访问路径。
4. **arm64 copy_mc 系列对齐**——openEuler 已合入 arm64
   machine-check-safe 系列（uaccess、copy_page 等），对齐其内核版本
   并评估向 HCE3 移植。

每项独立成补丁系列，走 openEuler 社区评审，与本仓库 ko 解耦交付。

---

## 7. 接口设计

### 7.1 模块参数（新增）

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `mem_enable` | Y | 内存故障域总开关 |
| `mem_pre_isolate` | Y | 是否启用 CE 预隔离（检测到 RAS_CEC 时默认 N） |
| `page_ce_threshold` | 8 | 单页窗口内 CE 达此值 → 预隔离 |
| `mem_window_secs` | 86400 | 页 CE 滑动窗口（秒，DRAM 劣化尺度比 CPU 长） |
| `dimm_ce_threshold` | 1000 | DIMM 级 CE 阈值 → MIGRATE_ADVISED |
| `dimm_uce_threshold` | 3 | DIMM 级 UCE 阈值 → MIGRATE_ADVISED |
| `mem_triage` | N | Phase 2 落点甄别开关（默认关，灰度启用） |

### 7.2 Sysfs

```
/sys/kernel/cfi/mem/
    enable, pre_isolate, page_ce_threshold, window_secs,
    dimm_ce_threshold, dimm_uce_threshold, triage
    stats                     # 全局统计（key value 每行一项）
    dimms                     # 介质级表（label ce= uce= [migrate-advised]）
```

### 7.3 Generic Netlink（family "CFI" 扩展）

新增属性 `CFI_ATTR_DOMAIN`（CPU=0 / MEM=1），新增命令：

| 方向 | 命令 | 用途 |
|------|------|------|
| K→U | MEM_ERROR_EVENT | 内存错误记录（PFN、类型、DIMM 定位） |
| K→U | MEM_PAGE_OFFLINED | 页隔离完成（含预隔离/被动隔离区分） |
| K→U | MEM_PAGE_FAILED | 页隔离失败，毒页仍在线 |
| K→U | MEM_VM_KILLED | 甄别处置杀掉 VM 属主进程（Phase 2） |
| K→U | MEM_MIGRATE_ADVISED | DIMM 级阈值触发，建议整机疏散 |
| U→K | MEM_GET_STATUS | 查询统计 |
| U→K | MEM_SET_POLICY | 动态调阈值 |
| U→K | MEM_OFFLINE_PAGE | 手动隔离指定 PFN |

### 7.4 Debugfs 注入（扩展）

```bash
# 软件路径注入（状态机验证）
echo "domain=mem pfn=0x12345 type=ce" > /sys/kernel/debug/cfi/inject
echo "domain=mem pfn=0x12345 type=uce_srao" > /sys/kernel/debug/cfi/inject

# 真实路径验证用内核自带设施：
#   - madvise(MADV_HWPOISON)（CAP_SYS_ADMIN，软注入消费路径）
#   - /sys/kernel/debug/hwpoison/corrupt-pfn（hwpoison-inject 模块）
#   - ACPI EINJ /sys/kernel/debug/apei/einj/（物理机硬注入）
```

---

## 8. Daemon（cfid）协作扩展

```
  Kernel (MFI)                          Daemon (cfid)
      │                                      │
      │──── MEM_PAGE_FAILED(pfn,pid) ───────>│
      │                                      │ [定位 pid → VM，触发该 VM 热迁移]
      │                                      │
      │──── MEM_MIGRATE_ADVISED(dimm) ──────>│
      │                                      │ [上报调度器：整机疏散 + 报修]
      │                                      │
      │──── MEM_VM_KILLED(pid)  ────────────>│
      │                                      │ [上报调度器：异地重建 VM]
```

与 CPU 域不同，内存事件**不需要 ACK 协议**——页隔离不存在"等 daemon
迁走 VM 再执行"的窗口（毒页处置越快越好），daemon 只做事后响应。

---

## 9. 安全性考量

| 风险 | 缓解措施 |
|------|---------|
| panic 抑制放大场景 E 的静默损坏风险 | Phase 2 甄别白名单制：非白名单一律主动 panic + kdump；`mem_triage` 默认关闭 |
| 预隔离误伤被 pin 的 VM 内存 | soft_offline 失败安全（迁不走就不动），失败转上报 |
| 哈希表内存占用失控 | 固定容量 + LRU 淘汰，最坏 ~64KB |
| 与 rasdaemon/CEC 重复动作 | tracepoint 回执判重 + hwpoison 标记判重 + CEC 检测降级 |
| tracepoint 回调中的开销 | 仅哈希查找 + 计数，重操作全部入工作队列 |
| 模块卸载 | 注销全部 tracepoint/notifier；已 offline 页不回滚（HWPoison 持久） |

---

## 10. 测试验证计划

### 10.1 软件路径（VM 内可测）

- [ ] debugfs 注入 CE → WATCHED → 阈值 → PRE_ISO → OFFLINED
- [ ] debugfs 注入 uce_srao → memory_failure_queue → 回执记账
- [ ] madvise(MADV_HWPOISON) 用户页 → 进程被杀 → MEM_ERROR_EVENT 上报
- [ ] hwpoison-inject 指定 PFN → 页隔离 → sysfs 统计正确
- [ ] CEC 启用时 pre_isolate 自动降级
- [ ] 窗口过期、LRU 淘汰、netlink 全命令收发

### 10.2 物理机硬注入（EINJ）

- [ ] EINJ 注入 CE 到指定地址 → trace_mc_event 捕获 → 计数 + DIMM 定位
- [ ] EINJ 注入 patrol scrub UCE（SRAO）→ 页隔离，不 panic，无进程被杀
- [ ] EINJ 注入 SRAR 到 VM 内存 → guest 收到 vMCE，仅 guest 内部受影响
- [ ] EINJ 注入 SRAR 到 KSM/迁移路径（Phase 2/3 场景）→ 甄别处置或受控 panic
- [ ] DIMM 阈值 → MIGRATE_ADVISED → cfid 端到端

---

## 11. 里程碑

| 版本 | 内容 | 状态 |
|------|------|------|
| v0.1 | 本设计文档评审定稿 | 完成 |
| v0.2 | Phase 1 实现：页记账 + 预隔离 + SRAO 兜底 + netlink/sysfs | **完成**（6.8 头文件零警告编译） |
| v0.3 | DIMM 记账 + cfimon（cfid 参考实现）+ 注入脚本 | **完成**（EINJ 物理机验证待执行） |
| v0.4 | Phase 2 实现：落点甄别（x86 first，灰度开关）+ 策略单测 | **完成**（单测 18/18 通过） |
| v0.5 | Phase 3 补丁系列首批（页迁移 MC-safe）提交 openEuler | 未开始 |

当前验证状态详见 `build-test-guide.md` 第 9 章 MFI 测试矩阵：软件路径
（编译、工具、策略单测）已在开发环境完成；模块加载类用例需要可
insmod 的 VM；EINJ/甄别实机用例需要物理机。
