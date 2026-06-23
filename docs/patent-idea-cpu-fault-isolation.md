# 专利创意（Patent Idea）— 云宿主机 CPU/内存硬件故障自隔离

**主题**：面向公有云宿主机的、以可加载内核模块（不改内核源码）形态交付的
CPU 核心 / 内存器件硬件故障运行时自隔离方法，将"单器件故障 → 整机宕机"
收敛为"单核 / 单页 / 单 VM 损失"。

**对应实现**：`cpu_fault_isolate.ko`（CFI = CPU 故障域 + MFI = 内存故障域），
设计见 `docs/design-document.md`、`docs/memory-fault-isolation-design.md`，
物理机实证见 `docs/physical-host-panic-vs-isolation.md`。

**日期**：2026-06　**状态**：创意稿（供专利挖掘评审）

---

## 0. 现有技术检索结论（Prior-Art Search）

| 现有专利 / 技术 | 核心内容 | 与本方案的关键差异（GAP） |
|---|---|---|
| **US 11829265 / US 11385974**（VM 宿主机不可纠正内存错误恢复，Google 系） | 宿主机内核内置的 MCE handler 标记"内核访问 guest 内存"产生的 UCE，通知 VMM 注入模拟 MCE 给受影响 VM 或迁移 VM，替代 panic | ① 仅覆盖**内存**错误，不处理 **cache / CPU 核心**故障；② handler 是**内核源码的一部分**，需改/编内核，非外挂 ko；③ 不做**物理核隔离**；④ 无"按 cache 拓扑分级决策"、无"无下电隔离"、无"落点甄别白名单" |
| **US 12222830 / 11847036 / 11531607**（失效从核 CPU 故障隔离与恢复，AMP 场景） | 裸金属非对称多处理（AMP）下，NMI→自定义 panic handler→**关停 CPU 核**并把上下文切回主实例恢复 | ① 面向**裸机 AMP** 而非**云虚拟化宿主机**；② 依赖真实**核关停 + 上下文切换**，非"保持在线但调度不可见"；③ 不区分 cache 层级；④ 不含 VM 优雅疏散协议 |
| **CN 106203082A**（基于虚拟化硬件特性隔离内核模块） | 用虚拟化硬件特性做内核模块的安全隔离 | 解决的是**安全/可信隔离**问题，与硬件故障 RAS 无关 |
| Linux 内核既有设施（`mce_tolerant`、`CONFIG_RAS_CEC`、`memory_failure()`、`soft_offline_page()`、CPU hotplug、`isolcpus`） | 内核自带的纠错/隔离原语 | 均为**单点原语**，无"运行时拦 panic→接管→分级隔离→VM 协同疏散"的端到端编排；CEC 仅 x86、策略固定、无上报；hotplug 在 vendor 内核存在死锁面 |

**检索结论**：业界已有"内存 UCE 给 VM 注错/迁移"和"裸机 AMP 关核恢复"两类思路，
但**没有**检索到"以不改内核源码的可加载模块，运行时抑制 panic 并按 cache 物理
拓扑分级隔离 CPU 核、配合无下电隔离模式绕开 hotplug 死锁、再以白名单落点甄别
守住静默损坏边界"这一组合方案。本方案的多数创新点具备新颖性。

---

## 1. 四维分析总表（Problem / GAP / Innovation / Effect）

> 下表把项目拆成 7 个可独立或组合保护的创新点（IP-1 ~ IP-7）。
> IP-1/IP-2/IP-5 为核心独立权利要求候选，其余为从属/组合点。

### IP-1　运行时 panic 抑制 + 故障接管"接力"（外挂 ko，不改内核源码）

| 维度 | 内容 |
|---|---|
| **问题 (Problem)** | 单个 CPU/内存器件出现致命硬件错误（fatal MCE）时，Linux 内核在 `do_machine_check()` 早期就走 `mce_panic()` 整机宕机，牵连同机 100+ 核上**全部 VM**；故障事件根本来不及进入可处置的 decode chain。云厂商无法逐版本改内核源码上线。 |
| **现有技术 / GAP** | 现有 VM-UCE 恢复方案（US 11829265）把恢复逻辑**编进内核 MCE handler**，需要定制内核、随内核版本维护，且只覆盖内存。`mce_tolerant` 虽是内核既有旋钮，但无人将其用作"运行时由外挂模块拉到 3，把所有 fatal 分支从'panic'改写为'记录+继续'，再由模块注册的 notifier 接力分类处置"的完整机制；该符号在部分 vendor 内核（如 HCE3）未导出甚至无 sysfs。 |
| **创新点 (Innovation)** | ① **加载期 panic 抑制**：模块 init 阶段用 **kprobe 定位未导出符号** `mce_tolerant` 并直写为 3，使 `do_machine_check()` 中所有受 `tolerant<3` 门控的 panic 分支（sync 超时 / MCIP 未置 / fatal）退化为"记录+返回"，事件回流到 `x86_mce_decoder_chain`；② **接管接力**：模块注册的 MCE notifier 收到回流事件→分类→派发隔离动作，形成"拦 panic → 接管 → 隔离"闭环；③ **双层兜底**：再注册 `panic_notifier`，对 tolerant 门控拦不住的路径做最后日志/取证；④ **vendor 适配**：运行时探测 HCE3 的 `fma_mce_do_chain` / `mce_panic_chain` 钩子，有则挂、无则跳，保持对 upstream 的兼容；卸载时**完整回滚**所有原始值。 |
| **技术效果 / 业务价值** | 物理机实证（Huawei 2288H V5 / HCE2）：**同一份 `.mce` 注入**，无模块→`mce_panic`→kdump→整机重启；加载模块→不 panic、事件被接管隔离、整机存活。以**纯外挂 ko**交付，跨内核版本零源码改动、可灰度可热插拔可回滚，显著降低云厂商上线与维护成本。把"整机宕机"的爆炸半径从全机 VM 收敛到单核绑定的 1 个 VM。 |

### IP-2　INACTIVE 无下电隔离模式（绕开 CPU hotplug 死锁的核隔离）

| 维度 | 内容 |
|---|---|
| **问题 (Problem)** | 把故障核摘出调度域，最直接是 `cpu_down()`/`remove_cpu()` 热插拔下线；但在大量 vendor 内核上，hotplug 回调链（如 `percpu_counter_cpu_dead`）存在**硬死锁**，下线 28s 后另一个核 hard-LOCKUP，反而把"隔离"变成"二次宕机"。而 `isolcpus` 是**启动期静态**配置，无法对运行中突发故障的核动态生效。 |
| **现有技术 / GAP** | 既有核隔离/恢复方案（US 12222830 等）依赖真实**关核**或上下文切换；内核 IRQ 迁移专利（US 10089265）也只服务于"即将 unplug 的 CPU"。**没有**"不触发 hotplug、保持物理在线、仅让调度器与中断子系统视其为已下线"的运行时隔离原语。 |
| **创新点 (Innovation)** | 提出**三档自适应隔离引擎**，核心是新增的 **INACTIVE（无下电隔离）模式**：对故障核 `set_cpu_active(cpu,false)` 从 `cpu_active_mask` 清位（调度器不再派活），并遍历该核中断亲和性把**全部可迁移 IRQ 迁出**到健康核（实证一次迁移 462 个），**完全不调 `cpu_down`**，从而**绕开整条 hotplug 死锁面**；CPU 保持 `online=1` 但对业务"形同下线"。三档为：`full`（真下线，需 livepatch 修 vendor bug）/ `inactive`（默认，无下电）/ `soft`（最轻），**按内核能力自动选择**。另含"跨核隔离调度"：故障核可能无法跑自己的 workqueue，故 `queue_work_on()` 到健康核执行隔离动作；并设 `protect_cpu0` / `cpumask_any_but()` 拒绝自隔离与下到最后一个核。 |
| **技术效果 / 业务价值** | 实证：inactive 隔离后 `taskset -c 3 yes` 返回 *Invalid argument*、`top` 该核 `us=sy=0、idle≈99.7%`——业务**永远拿不到该核**，且**不触发 hotplug 死锁**。无需 livepatch 即可在 vendor 内核安全落地，是"在不可改源码、hotplug 不可靠的真实生产现场也能用"的关键工程突破。降损同时避免了隔离动作本身引入的二次故障。 |

### IP-3　按 cache 物理拓扑分级的隔离决策

| 维度 | 内容 |
|---|---|
| **问题 (Problem)** | "cache UCE 一律隔离该核"是错的：L3/LLC 是 **socket 共享**（Intel CHA 分布式 / AMD CCX 共享），隔离单核**不解决根因**，反而误伤一个健康核；而 L1/L2 是 per-core，隔离才对。一刀切策略要么漏（L3 没救）、要么误伤（白扔核）。 |
| **现有技术 / GAP** | 现有 RAS 隔离方案不区分 cache 层级与共享域；内核 decode 仅解析 MCACOD，不据此做"是否隔离物理核"的拓扑级分流决策。 |
| **创新点 (Innovation)** | 在 MCE/RAS 解码后按 **cache 层级 × 共享域**分流：L1（含 L1I/L1D）/L2/TLB/Bus/Internal（per-core）→ 隔离上报该核；**L3/LLC（socket 共享）→ 仅记账 + netlink 上报，CPU 留在线交 daemon 决策**（地址级 hwpoison 或 socket-level drain），由模块参数 `isolate_on_l3_uce`（默认 0）控制。x86/arm64 各有分类后端，且修正了"内存 MCACOD 误落入 CPU 计数拖垮健康核"的跨域 bug。 |
| **技术效果 / 业务价值** | 隔离决策与**物理故障域对齐**：能隔离的精准隔离、不能靠隔离解决的（共享 LLC）不做无效且有害的核摘除，把误隔离率降到最低，最大化健康算力留存。 |

### IP-4　内核-用户态 daemon 协同的"延迟隔离"协议（ACK + 超时兜底）

| 维度 | 内容 |
|---|---|
| **问题 (Problem)** | 内核一旦判定隔离就立刻下线核，会**硬杀**绑定其上的 VM，损失从"可热迁移"恶化为"直接掉"。但又不能无限等用户态，否则故障核长期带病运行。 |
| **现有技术 / GAP** | 现有方案要么内核直接动作（无优雅疏散窗口），要么纯用户态轮询（拿不到内核早期事件、有竞态）。缺乏"内核决策、给用户态有界时间窗疏散、超时强制兜底"的协同协议。 |
| **创新点 (Innovation)** | 定义 Generic Netlink（family "CFI"）双向协议与**延迟隔离状态机**：内核进入 `ISOLATING` 即 `STATE_CHANGE` 通知 daemon→daemon 热迁移/疏散该核 VM→回 `ACK_ISOLATE`→内核取消定时器执行下线；若 `defer_timeout_ms`（默认 30s）内无 ACK 则**强制下线兜底**。内存域则相反——毒页处置越快越好，**不设 ACK**，daemon 只事后响应。配套 per-CPU sysfs、debugfs 软注入、`user_pinned` 抑制隔离等接口。 |
| **技术效果 / 业务价值** | 在"尽快隔离"与"优雅疏散"间取得有界折衷：常态下 VM 被**热迁走零损失**，异常（daemon 卡死）下仍有超时兜底保证故障核必被隔离。打通内核 RAS 与上层云调度（Nova/K8s）的标准化通道。 |

### IP-5　内核态 UCE"落点甄别"白名单（守住静默损坏边界）

| 维度 | 内容 |
|---|---|
| **问题 (Problem)** | panic 抑制（IP-1）是把双刃剑：对"内核自身数据被同步消费且无 fixup"（slab/页表/内核栈）的致命错误，**压住 panic 继续跑 = 静默数据损坏**，比宕机更糟。必须区分"能救"与"不能救但绝不能装没事"。 |
| **现有技术 / GAP** | US 11829265 只判"是否内核访问 guest 内存"这一种情形；无系统化的"页归属 × 消费方式"二维判决，也未处理"panic 抑制反而放大静默损坏"这一被引入的新风险。 |
| **创新点 (Innovation)** | 提出**白名单制落点甄别**：按 **故障页归属（用户页/虚机页 vs 内核页/页表/text）× 消费方式（异步未消费 / 有 fixup SRAR / 无 fixup SRAR）× 可恢复信号（x86 `RIPV=1`、单 bank、无 PCC）** 做判决——①异步未消费(SRAO/deferred)→页隔离零损失；②有 fixup→内核自恢复，仅记账；③无 fixup 但毒页是用户/虚机页且 RIPV=1→`memory_failure()` 杀属主（qemu 则 netlink `MEM_VM_KILLED` 触发异地重建）；**不在白名单内一律主动快速 panic + kdump，绝不静默**。判决为纯函数，单测全覆盖；甄别全程只读 `struct page`（vmemmap，与毒数据页异介质区），不触碰毒页本身。 |
| **技术效果 / 业务价值** | 把 panic 抑制从"赌"变成"可证明的分级处置"：能救的（绝大多数用户/虚机页场景）救成"仅 1 个 VM 损失"，不能救的（内核数据）**主动**走 kdump 保现场。既扩大了可恢复覆盖面，又**杜绝了静默数据损坏**这一致命风险——这是把 panic 抑制做成生产可用方案的"安全阀"。 |

### IP-6　页级 CE 预隔离 + DIMM/rank 介质级记账（内存故障域）

| 维度 | 内容 |
|---|---|
| **问题 (Problem)** | DRAM 失效有前兆（同行/列 CE 反复出现后恶化为 UCE），坐等恶化为 UCE 才动作就晚了；且单页隔离治标，**介质劣化**（整条 DIMM 坏）治本要靠疏散/换件，但缺乏介质维度统计与触发。 |
| **现有技术 / GAP** | 内核 `CONFIG_RAS_CEC` 仅 x86、策略固定、无上报；`soft_offline_page()` 是单点原语，无策略化/跨架构/可上报的编排，也无 DIMM 级聚合与疏散建议。 |
| **创新点 (Innovation)** | ①**页级预隔离**：以固定容量+LRU 的 PFN 哈希表做窗口化 CE 记账，达 `page_ce_threshold` 即 `soft_offline_page()`（迁移页内容后下线，业务**零损失**）；迁移失败（如直通 VM pin 住的页）转 netlink 上报由 daemon 迁 VM；检测到 RAS_CEC 启用则自动降级只记账避免双动作。②**介质级记账**：从 EDAC `trace_mc_event` 提取 mc/csrow/channel/DIMM label，维护 per-DIMM CE/UCE 滑动窗口计数，达阈值发 `MEM_MIGRATE_ADVISED` 建议整机疏散+报修（内存不存在"在线拔一条 DIMM 继续跑"，故介质级决策权全交 daemon，模块不自动疏散）。③统一双域框架：CPU 域与内存域**共用** panic 抑制层 / netlink / daemon 协议 / sysfs，跨 x86 与 arm64（GHES/APEI/CPER）。 |
| **技术效果 / 业务价值** | 把内存故障从"被动等 UCE 宕机"前移到"CE 前兆期零损失预隔离"，并以介质维度驱动换件/疏散闭环。一套框架统一覆盖 CPU 与内存两类最主要的宿主机宕机源，复用度高、跨架构。 |

### IP-7　跨架构统一 RAS 自隔离框架（组合保护）

| 维度 | 内容 |
|---|---|
| **问题 (Problem)** | x86（MCE/MCACOD）与 arm64（GHES/APEI/CPER、鲲鹏）RAS 上报路径迥异；vendor 内核（HCE2/HCE3）有 firmware-first(FMA) 等私有钩子与 hotplug bug，方案易碎片化、难统一上线。 |
| **现有技术 / GAP** | 现有方案多绑定单架构/单内核，缺乏"同一策略层 + 多架构后端 + 运行时探测 vendor 钩子"的可移植框架。 |
| **创新点 (Innovation)** | 分层架构：公共**策略/状态机/netlink/sysfs 层** + 可插拔 **arch 后端**（x86 MCE decode + FMA、arm64 GHES/APEI tracepoint + RAS 分类）+ **运行时符号/钩子探测**（kprobe `cfi_lookup_name()` 取未导出符号、探测 `fma_*` 钩子有则挂）。同一份 ko 在 Intel/AMD/鲲鹏 920/930、upstream 与 vendor 内核上自适应运行。 |
| **技术效果 / 业务价值** | 一套代码覆盖云厂商主流异构机型与内核，与 rasdaemon/EDAC/mcelog/CEC **共存不冲突**，大幅降低多机型 RAS 能力的工程与维护成本。 |

---

## 2. 推荐的权利要求布局（供专利代理参考）

- **独立权利要求 1（方法）**：一种云宿主机硬件故障自隔离方法 = IP-1（运行时 panic 抑制 + 接管接力，ko 不改源码）作为骨架。
- **独立权利要求 2（系统/装置）**：含三档隔离引擎、其中 INACTIVE 无下电隔离（IP-2）为核心特征。
- **从属权利要求**：
  - 按 cache 拓扑分级隔离（IP-3）
  - 延迟隔离 ACK+超时协同协议（IP-4）
  - 内核态 UCE 白名单落点甄别（IP-5，强烈建议单独成案，新颖性高且解决 panic 抑制引入的新风险）
  - 页级 CE 预隔离 + DIMM 介质记账（IP-6）
  - 跨架构运行时符号/钩子探测框架（IP-7）
- **建议优先级**：IP-2（无下电隔离）、IP-5（落点甄别白名单）、IP-1（外挂式 panic 抑制接管）三点新颖性最强、最难规避，建议优先布局。

---

## 3. 最直接的实证证据（用于 EP/技术效果举证）

`docs/physical-host-panic-vs-isolation.md`：在 Huawei 2288H V5 + HCE2 物理机上，
**完全相同的 `.mce` 故障注入**——

1. 未加载模块 → `mce_panic("Fatal machine check")` → kdump → 整机重启（vmcore 栈为证）；
2. 加载模块（`isolation_mode=inactive`）→ `cpu3: inactive-isolated（迁移 462 IRQs，CPU 仍 online）` → 整机存活；
3. `taskset -c 3 yes` 失败 + `top` cpu3 `idle≈99.7%` → 证明故障核对业务真正不可见。

这组对照实验是"技术效果"最直接、可视化的举证材料。

---

## 4. 诚实的覆盖边界（写入说明书"有益效果的限定"避免过度主张）

- 内核态 **AR 严重性、无 fixup** 事件（如 `--lmce` + SRAR 命中内核数据）走 `mce_panic("Fatal local machine check")` 的**无条件路径**，不受 `mce_tolerant` 门控——本方案对此**主动 kdump 兜底**而非声称可救（IP-5 的白名单正是为此设边界）。
- `full` 真下线模式在未打 livepatch 的 vendor 内核会撞 `percpu_counter_cpu_dead` 死锁，故默认用 `inactive`（IP-2 的价值所在）。
- 说明书宜将"有益效果"限定为"消除**本不该发生**的整机 panic、并对不可恢复故障提供受控 kdump 取证"，而非"消除一切 panic"。
