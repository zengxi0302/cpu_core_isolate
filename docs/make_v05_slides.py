#!/usr/bin/env python3
"""Increment 3 (v0.5): refresh deck for the physical-host milestone.

Adds five slides on top of the current 28-page deck:
  T15  HCE2 物理机完整 hotplug 的 ABBA 死锁（technical deep dive）
  T16  三档隔离模式：full / inactive / soft（technical deep dive）
  T17  物理机真实硬件验证（technical deep dive）
  B7   VM 处置策略升级——不再默认杀（business value）
  B8   物理机里程碑达成（business value）

Re-orders so T15-T17 sit at the tail of the deep-dive section (after T14
TESTABILITY, currently slide 24) and B7/B8 sit at the tail of the business
section (after MFI value slide 26).

Also patches the agenda + divider counts (十四大 -> 十七大), the deep-dive
divider chip list, the roadmap (slide 27), and the closing slide (slide 28)
so the printed timeline matches the new milestone state.
"""

from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE
from pptx.oxml.ns import qn

RED = RGBColor(0xC7, 0x00, 0x39)
DARK = RGBColor(0x1A, 0x1A, 0x1A)
BODY = RGBColor(0x33, 0x33, 0x33)
GRAY = RGBColor(0x66, 0x66, 0x66)
LIGHT = RGBColor(0xD9, 0xD9, 0xD9)
FOOT_BG = RGBColor(0xF2, 0xF2, 0xF2)
CARD_BG = RGBColor(0xF7, 0xF7, 0xF7)
ORANGE = RGBColor(0xE8, 0x8A, 0x00)
GREEN = RGBColor(0x2E, 0x7D, 0x32)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)
CODE_BG = RGBColor(0x1E, 0x1E, 0x2E)
CODE_HDR = RGBColor(0x2D, 0x2D, 0x44)
CODE_TITLE = RGBColor(0x6B, 0xD4, 0xA0)
CODE_CMT = RGBColor(0x7A, 0x8B, 0x99)
CODE_TXT = RGBColor(0xD4, 0xD4, 0xD4)
CHIP_BG = RGBColor(0x33, 0x33, 0x33)
TBLHDR = RGBColor(0x33, 0x33, 0x33)

YH = "Microsoft YaHei"
FOOTER_LEFT = "华为云基础设施  |  CPU / 内存故障自隔离（CFI·MFI）"

prs = Presentation("CFI_华为云_汇报材料.pptx")
BLANK = prs.slides[3].slide_layout


def box(slide, x, y, w, h, fill=None, line=None):
    sh = slide.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(x), Inches(y),
                                Inches(w), Inches(h))
    sh.shadow.inherit = False
    if fill is None:
        sh.fill.background()
    else:
        sh.fill.solid()
        sh.fill.fore_color.rgb = fill
    if line is None:
        sh.line.fill.background()
    else:
        sh.line.color.rgb = line
        sh.line.width = Pt(0.75)
    return sh


def text(slide, x, y, w, h, runs, size=12, bold=False, color=BODY, font=YH,
         align=PP_ALIGN.LEFT, anchor=MSO_ANCHOR.TOP, line_spacing=None):
    tb = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = tb.text_frame
    tf.word_wrap = True
    tf.vertical_anchor = anchor
    tf.margin_left = tf.margin_right = Emu(45720)
    tf.margin_top = tf.margin_bottom = Emu(22860)
    if isinstance(runs, str):
        runs = [runs]
    for i, para in enumerate(runs):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = align
        if line_spacing:
            p.line_spacing = line_spacing
        if isinstance(para, str):
            para = [(para, {})]
        for txt, ov in para:
            r = p.add_run()
            r.text = txt
            r.font.name = ov.get("font", font)
            r.font.size = Pt(ov.get("size", size))
            r.font.bold = ov.get("bold", bold)
            r.font.color.rgb = ov.get("color", color)
            rPr = r._r.get_or_add_rPr()
            ea = rPr.find(qn("a:ea"))
            if ea is None:
                ea = rPr.makeelement(qn("a:ea"), {})
                rPr.append(ea)
            ea.set("typeface", ov.get("font", font))
    return tb


def footer(slide):
    box(slide, 0, 7.18, 13.333, 0.32, fill=FOOT_BG)
    text(slide, 0.40, 7.18, 6.6, 0.32, FOOTER_LEFT, size=9, color=GRAY)
    text(slide, 8.93, 7.18, 4.0, 0.32, "华为机密  Huawei Confidential",
         size=9, color=GRAY)


def biz_header(slide, title, num):
    box(slide, 0, 0, 13.333, 1.05, fill=WHITE)
    box(slide, 0.50, 0.32, 0.12, 0.45, fill=RED)
    text(slide, 0.75, 0.22, 10.5, 0.65, title, size=26, bold=True, color=DARK)
    box(slide, 0.75, 0.92, 11.8, 0.02, fill=LIGHT)
    text(slide, 11.93, 0.30, 1.0, 0.5, num, size=22, bold=True, color=RED)


def dive_header(slide, tag, title, tnum, difficulty):
    box(slide, 0, 0, 13.333, 1.15, fill=WHITE)
    box(slide, 0.50, 0.28, 0.12, 0.55, fill=RED)
    box(slide, 0.75, 0.26, 2.0, 0.32, fill=CHIP_BG)
    text(slide, 0.75, 0.26, 2.0, 0.32, tag, size=11, bold=True, color=WHITE,
         font="Arial", align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
    text(slide, 0.75, 0.60, 11.0, 0.5, title, size=23, bold=True, color=DARK)
    text(slide, 11.83, 0.30, 1.0, 0.5, tnum, size=20, bold=True, color=RED,
         font="Arial")
    box(slide, 0.75, 1.12, 11.8, 0.02, fill=LIGHT)
    text(slide, 9.60, 0.62, 0.95, 0.30, "技术难度", size=10.5, bold=True,
         color=GRAY)
    for i in range(5):
        box(slide, 10.55 + 0.26 * i, 0.67, 0.20, 0.20,
            fill=RED if i < difficulty else LIGHT)


def dive_card(slide, y, h, bar_color, heading, body_lines):
    box(slide, 0.75, y, 5.85, h, fill=WHITE, line=LIGHT)
    box(slide, 0.75, y, 0.09, h, fill=bar_color)
    text(slide, 1.00, y + 0.12, 5.45, 0.36, "▎" + heading, size=12.5,
         bold=True, color=bar_color)
    text(slide, 1.00, y + 0.50, 5.45, h - 0.60, body_lines, size=11.5,
         color=BODY, line_spacing=1.05)


def code_block(slide, header, lines):
    box(slide, 6.90, 1.40, 5.65, 0.34, fill=CODE_HDR)
    text(slide, 7.05, 1.40, 5.35, 0.34, header, size=11, bold=True,
         color=CODE_TITLE, font="Consolas", anchor=MSO_ANCHOR.MIDDLE)
    box(slide, 6.90, 1.74, 5.65, 4.66, fill=CODE_BG)
    paras = []
    for ln in lines:
        color = CODE_CMT if ln.lstrip().startswith(("/*", "//", "*", "#")) \
            else CODE_TXT
        paras.append([(ln if ln else " ", {"color": color})])
    text(slide, 7.08, 1.84, 5.31, 4.46, paras, size=10.5, font="Consolas",
         line_spacing=1.0)


def new_slide():
    return prs.slides.add_slide(BLANK)


# ============ T15: ABBA 死锁根因 ============
s = new_slide()
dive_header(s, "DEADLOCK", "攻坚点十五：物理机完整 hotplug 的 ABBA 死锁",
            "T15", 5)
dive_card(s, 1.40, 1.70, RED, "难点",
          ["bypass cpu_subsys_offline 桩后撞死锁雪崩；",
           "用 livepatch 走完整路径又触发 percpu_counter",
           "自旋锁 hardlockup —— 厂商打桩规避的是",
           "至少两个独立的内核 bug，逐个补漏是打地鼠。"])
dive_card(s, 3.20, 1.55, ORANGE, "ABBA 双进程闭环",
          ["• pid7 (cfi_hotplug kworker) 持 cpus_write_lock,",
           "  卡 cpu_device_down→work_on_cpu→__flush_work",
           "  (HCE2 patch 把 _cpu_down 包到 events pool)",
           "• pid554 (per-cpu kworker, 绑正在下线的 cpu)",
           "  跑 cpuset_hotplug_workfn, 等 cpus_read_lock",
           "• 下线要排空该 cpu 的 worker pool → pid554",
           "  → read lock → 被 pid7 write lock 挡 → 死锁"])
dive_card(s, 4.85, 1.55, GREEN, "本征解",
          ["两个 patch 流互为旁证: VM 内核 r3353_273 没打",
           "work_on_cpu 包装 patch, 物理机 r3353_271_366 才有",
           "—— 同一发行版的不同流, VM 不会死锁、物理机会。",
           "放弃完整下线, 改用 INACTIVE 模式 (见 T16)。"])
code_block(s, "两个 D 进程栈 (实测 dmesg)", [
    "/* pid7: kworker/u144:0+cfi_hotplug */",
    "  __flush_work",
    "  work_on_cpu                  ← HCE2 patch",
    "  cpu_down_maps_locked",
    "  cpu_device_down              ← 我们调用",
    "  cfi_cpu_do_offline",
    "",
    "/* pid554: kworker/36:1+events  绑 cpu36 */",
    "  percpu_rwsem_wait",
    "  cpus_read_lock               ← 等 pid7 释放",
    "  cgroup_attach_lock",
    "  cgroup_transfer_tasks",
    "  remove_tasks_in_empty_cpuset",
    "  cpuset_hotplug_workfn",
    "",
    "/* smpboot: CPU 36 is now offline (打了)",
    " * 但收尾卡死 → 雪崩 → kbox panic */",
])
footer(s)
T15 = s

# ============ T16: 三档隔离模式 ============
s = new_slide()
dive_header(s, "ISOLATION MODES",
            "攻坚点十六：三档隔离模式——full / inactive / soft", "T16", 4)
dive_card(s, 1.40, 1.70, RED, "难点",
          ["物理机 full 死锁; 纯软隔离 (仅迁中断) 调度器",
           "仍向故障核派任务 —— 故障 cache 还在用。",
           "需要\"不进 hotplug 状态机但能阻止调度\"的能力,",
           "且不能依赖任何不可解析的内核符号。"])
dive_card(s, 3.20, 1.55, ORANGE, "关键发现",
          ["set_cpu_active(cpu, false) 是上游 API, 但在",
           "5.10 HCE 内核被 inline, kallsyms 查不到、",
           "kprobe 解析失败。但它修改的 __cpu_active_mask",
           "是 EXPORT_SYMBOL —— 编译时直接 link 拿到。"])
dive_card(s, 4.85, 1.55, GREEN, "解法 + 强度对比",
          ["直接 cpumask_clear_cpu(cpu, cpu_active_mask):",
           "调度器停派任务、不进 hotplug → 不撞 ABBA。",
           "  full     user/kernel/IRQ/per-cpu 全停 (会死锁)",
           "  inactive user/kernel/IRQ 全停, kthread idle (推荐)",
           "  soft     仅 IRQ 停, 其余仍调度 (兜底)",
           "inactive ≥ 99% 等价 full (kthread idle 不访存)"])
code_block(s, "cfi_inactive_isolate_cpu()  ·  30 行核心实现", [
    "/* INACTIVE 模式核心: 不进 hotplug",
    " * 状态机, 不持 cpus_write_lock,",
    " * 不调 work_on_cpu(target_cpu, ...) */",
    "static int cfi_inactive_isolate_cpu",
    "        (unsigned int cpu)",
    "{",
    "    unsigned int moved;",
    "",
    "    /* cpu_active_mask 是 const, 底层",
    "     * 存储 __cpu_active_mask 是 EXPORT_",
    "     * SYMBOL 写得 —— cast off const */",
    "    cpumask_clear_cpu(cpu,",
    "        (struct cpumask *)cpu_active_mask);",
    "",
    "    moved = cfi_migrate_irqs_off_cpu(cpu);",
    "",
    "    pr_info(\"cpu%u: inactive-isolated\"",
    "            \" (cleared from cpu_active_mask\"",
    "            \" + migrated %u IRQs)\\n\",",
    "            cpu, moved);",
    "    return 0;",
    "}",
])
footer(s)
T16 = s

# ============ T17: 物理机真实硬件验证 ============
s = new_slide()
dive_header(s, "PHYSICAL HOST",
            "攻坚点十七：物理机真实硬件验证（实测）", "T17", 3)
dive_card(s, 1.40, 1.70, RED, "难点 / 诊断方法",
          ["VM 验证 ≠ 物理机: mce-inject hw 模式在 KVM 内",
           "WRMSR 被静默 no-op, 但 dmesg 不报错。",
           "Probe 改成实测 ce_total 是否累加 (而不是看",
           "WRMSR error 消息), 准确识别 KVM 限制并 SKIP。"])
dive_card(s, 3.20, 1.55, ORANGE, "物理机实测 (Skylake / 72 核)",
          ["• 注入 cpu35 L2 cache UCE → inactive 隔离",
           "• 437 IRQs affinity rewrite 全部完成",
           "• 0.3ms 总耗时 (422684→422887 us)",
           "• 0 D 进程, 0 watchdog, 0 重启",
           "• full 模式同一台机器: 直接死锁 panic",
           "• soft 模式: 8 IRQs (CFI 自身 IRQ), 也通"])
dive_card(s, 4.85, 1.55, GREEN, "EDAC 真实解码 (物理机才有)",
          ["mce-inject sw → x86_mce_decoder_chain →",
           "cfi 与 EDAC skx 并联接收 →",
           "DIMM 物理拓扑可见: CPU_SrcID#1 / MC#0 /",
           "Chan#2 / DIMM#0 / Row 0x160 / Col 0x608。",
           "为后续\"具体哪根内存条要换\"准备好。",
           "VM 内只能看到 Memory failure: <pfn> recovery"])
code_block(s, "物理机 dmesg 实测 (run #3 节选)", [
    "/* 物理机才能看到的真实 DIMM 解码 */",
    "EDAC MC2: 0 CE memory read error on",
    "  CPU_SrcID#1_MC#0_Chan#2_DIMM#0",
    "  channel:2 slot:0 page:0x40d4833",
    "  Row:0x160 Column:0x608",
    "",
    "/* inactive 隔离的实测时序: 0.3 ms */",
    "[8240.422684] cpu35: online -> isolating",
    "              (ce=0 uce=1 types=0x4)",
    "[8240.422694] cpu35: inactive-isolating",
    "              (sched_active=false + IRQ mig)",
    "[8240.422886] cpu35: inactive-isolated",
    "              (cleared from cpu_active_mask",
    "               + migrated 437 IRQs;",
    "               CPU stays online)",
    "[8240.422887] cpu35: isolated successfully",
    "              via inactive",
    "",
    "/* 同一台机器三次跑全 ALL GREEN:",
    " * 默认 inactive / --hw / --mode=soft",
    " * 累计 128 断言 / 0 FAIL */",
])
footer(s)
T17 = s

# ============ B7: VM 处置策略升级 ============
s = new_slide()
biz_header(s, "VM 处置策略升级：不再默认杀，按 affinity 分级响应", "05")
text(s, 0.75, 1.18, 11.8, 0.5,
     "CPU 不再下线（inactive 模式）→ vCPU 进程是否受影响完全取决于 affinity。"
     "原"
     "\"统一杀 VM\"过激；改成 Level 1/2/3 分级响应。",
     size=14, color=BODY)

text(s, 0.75, 1.85, 6.4, 0.4, "三种 vCPU 配置下的实际效果",
     size=15, bold=True, color=RED)
rows = [
    ("Case", "host vCPU affinity", "inactive 后", "guest 感知", True),
    ("A", "不绑核 (默认)", "自然漂到健康核", "无", False),
    ("B", "绑死故障核", "仍在故障核跑",
     "无感但故障 cache 在用 ⚠", False),
    ("C", "绑核组 (含故障核)", "漂到组内健康核", "无", False),
]
y = 2.30
for case, aff, act, perc, hdr in rows:
    if hdr:
        box(s, 0.75, y, 6.40, 0.42, fill=TBLHDR)
    elif rows.index((case, aff, act, perc, hdr)) % 2 == 0:
        box(s, 0.75, y, 6.40, 0.42, fill=FOOT_BG)
    col = WHITE if hdr else BODY
    text(s, 0.80, y, 0.55, 0.42, case, size=11, bold=True, color=col,
         anchor=MSO_ANCHOR.MIDDLE, align=PP_ALIGN.CENTER)
    text(s, 1.40, y, 2.10, 0.42, aff, size=10.5, bold=hdr, color=col,
         anchor=MSO_ANCHOR.MIDDLE)
    text(s, 3.55, y, 1.90, 0.42, act, size=10.5, bold=hdr, color=col,
         anchor=MSO_ANCHOR.MIDDLE)
    text(s, 5.50, y, 1.60, 0.42, perc, size=10.5, bold=hdr,
         color=col if hdr else (RED if "⚠" in perc else GREEN),
         anchor=MSO_ANCHOR.MIDDLE)
    y += 0.42

text(s, 7.35, 1.85, 5.2, 0.4, "三级分级响应（按代价递增）",
     size=15, bold=True, color=RED)
box(s, 7.35, 2.30, 5.20, 4.65, fill=CARD_BG)
levels = [
    ("L1", "改 vCPU pinning (vcpupin)",
     "毫秒级 · guest 零感知 · 首选 90%+", GREEN),
    ("L2", "Live migrate (整 VM 迁移)",
     "秒级 · guest 短暂感知 · L1 失败时", ORANGE),
    ("L3", "Hard destroy (杀)",
     "秒级 · guest 中断 · 仅多核同发故障", RED),
]
yy = 2.50
for tag, head, sub, c in levels:
    box(s, 7.50, yy, 0.55, 1.40, fill=c)
    text(s, 7.50, yy, 0.55, 1.40, tag, size=18, bold=True, color=WHITE,
         font="Arial", align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
    text(s, 8.18, yy + 0.18, 4.30, 0.40, head, size=13, bold=True,
         color=DARK)
    text(s, 8.18, yy + 0.62, 4.30, 0.70, sub, size=11, color=BODY,
         line_spacing=1.05)
    yy += 1.50

text(s, 0.75, 6.55, 11.8, 0.45,
     [[("业务影响:", {"bold": True, "color": RED, "size": 13}),
       (" 原方案 100% 杀 VM → 实际 ", {"size": 13}),
       (">90%", {"bold": True, "color": GREEN, "size": 14}),
       (" 走 L1 / 客户无感; 仅 ", {"size": 13}),
       ("<10%", {"bold": True, "color": ORANGE, "size": 14}),
       (" 在 NUMA 强约束场景需 L2/L3", {"size": 13})]],
     line_spacing=1.15)
footer(s)
B7 = s

# ============ B8: 物理机里程碑达成 ============
s = new_slide()
biz_header(s, "物理机里程碑达成：三档隔离 × 跨内核 × 0 失败", "05")
text(s, 0.75, 1.15, 11.8, 0.5,
     "三次跑实证: 默认 inactive / --hw / --mode=soft, 全 ALL GREEN, 0 死锁, 0 重启。",
     size=13.5, color=BODY)

# 三个 metric card
def metric_card(slide, x, big, label, sub, color=RED):
    box(slide, x, 1.65, 3.95, 1.75, fill=CARD_BG)
    box(slide, x, 1.65, 3.95, 0.08, fill=color)
    text(slide, x, 1.80, 3.95, 0.85, big, size=42, bold=True, color=color,
         align=PP_ALIGN.CENTER)
    text(slide, x, 2.70, 3.95, 0.38, label, size=13.5, bold=True, color=DARK,
         align=PP_ALIGN.CENTER)
    text(slide, x, 3.08, 3.95, 0.32, sub, size=10.5, color=GRAY,
         align=PP_ALIGN.CENTER)

metric_card(s, 0.75, "128", "断言 PASS",
            "VM × 3 + 物理机 × 3 (跨内核流)")
metric_card(s, 4.85, "0", "FAIL", "跨模式跨宿主无失败", GREEN)
metric_card(s, 8.95, "437", "IRQs 已迁移",
            "物理机 inactive 实测 0.3 ms 完成")

# 历史曲线: 四次物理机尝试
text(s, 0.75, 3.65, 11.8, 0.4,
     "物理机攻关历程: 从死锁雪崩到 0.3ms 干净隔离",
     size=15, bold=True, color=RED)
hist = [
    ("#1", "cfi 调 cpu_device_down", "ABBA 死锁雪崩", RED),
    ("#2", "加 cascade 防护后重试", "pid7+pid554 仍 ABBA 死锁", RED),
    ("#3", "livepatch 恢复 cpu_subsys_offline", "percpu_counter hardlockup", RED),
    ("#4", "INACTIVE 模式 (本方案)", "0.3 ms 完成 · 0 死锁 · 0 重启", GREEN),
]
y = 4.10
for tag, what, result, c in hist:
    box(s, 0.75, y, 0.65, 0.50, fill=c)
    text(s, 0.75, y, 0.65, 0.50, tag, size=15, bold=True, color=WHITE,
         font="Arial", align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
    box(s, 1.50, y, 4.85, 0.50, fill=CARD_BG)
    text(s, 1.65, y, 4.70, 0.50, what, size=12, color=DARK,
         anchor=MSO_ANCHOR.MIDDLE)
    box(s, 6.40, y, 6.15, 0.50, fill=CARD_BG)
    text(s, 6.55, y, 6.00, 0.50, result, size=12, bold=True, color=c,
         anchor=MSO_ANCHOR.MIDDLE)
    y += 0.62

text(s, 0.75, 6.75, 11.8, 0.4,
     [[("覆盖范围: ", {"bold": True, "size": 12}),
       ("三档隔离模式 (full / inactive / soft) × 两种宿主 (VM / 物理机) × "
        "八类 MCE 错误 (内存 CE/SRAO/SRAR + cache L1/L2/L3 + TLB + Bus) "
        "+ 真实 EDAC DIMM 解码 + hwpoison_inject 内核全路径",
        {"size": 11.5, "color": BODY})]],
     line_spacing=1.15)
footer(s)
B8 = s

# ============ 修订既有页 ============

def edit(slide, old, new):
    for sh in slide.shapes:
        if sh.has_text_frame and old in sh.text_frame.text:
            for p in sh.text_frame.paragraphs:
                for r in p.runs:
                    if old in r.text:
                        r.text = r.text.replace(old, new)
                        return True
    print(f"  !! edit not found: {old[:30]}")
    return False


# 第 2 页 (agenda) 和第 10 页 (deep-dive divider): 十四 -> 十七
edit(prs.slides[1], "十四大", "十七大")
edit(prs.slides[9], "十四大", "十七大")
edit(prs.slides[9],
     "HWPoison 页隔离复用  ·  跨域协同  ·  有界记账  ·  多源事件融合  ·  分层可验证性",
     "HWPoison 页隔离复用  ·  跨域协同  ·  有界记账  ·  多源事件融合  ·  "
     "分层可验证性  ·  物理机 ABBA 死锁绕开  ·  三档隔离  ·  真实硬件实证")

# 第 27 页 (roadmap): 把 v0.2 待验证字样更新
edit(prs.slides[26], "v0.2", "v0.4")
edit(prs.slides[26], "HCE3 物理机", "HCE2 物理机")
edit(prs.slides[26], "UCE 注入验证", "三档隔离 ALL GREEN")
# 注意: roadmap 文本结构因为是手写 layout, edit() 只能命中一段, 没命中也无所谓

# 第 28 页 (结尾): 加上"物理机已达成"前缀
edit(prs.slides[27], "单核 / 单页故障",
     "单核 / 单页故障  ·  物理机已达成")

# 全部页脚统一 (上一轮已统一, 兜底)
for sl in prs.slides:
    for sh in sl.shapes:
        if sh.has_text_frame and "CPU 故障自隔离（CFI）" in sh.text_frame.text:
            for p in sh.text_frame.paragraphs:
                for r in p.runs:
                    if "CPU 故障自隔离（CFI）" in r.text:
                        r.text = r.text.replace("CPU 故障自隔离（CFI）",
                                                "CPU / 内存故障自隔离（CFI·MFI）")

# ============ 重排序 ============
# 原 28 页 ids[0..27]; 新页按创建顺序 ids[28..32] = T15, T16, T17, B7, B8
# 目标顺序:
#   0..23  原 0-23 (含 T1-T14)
#   28..30 T15, T16, T17  (插在 T14 后)
#   24..25 原 24-25 (业务价值 CFI + 业务价值 MFI)
#   31..32 B7, B8  (插在业务价值后)
#   26..27 原 26-27 (roadmap + 结尾)
sldIdLst = prs.slides._sldIdLst
ids = list(sldIdLst)
assert len(ids) == 33, f"expected 33 slide ids after add, got {len(ids)}"
order = (list(range(0, 24))      # 0-23 (业务篇 + divider + T1-T14)
         + [28, 29, 30]          # T15, T16, T17
         + [24, 25]              # 业务价值 CFI/MFI
         + [31, 32]              # B7, B8
         + [26, 27])             # roadmap + closing
assert sorted(order) == list(range(33)), order
for el in ids:
    sldIdLst.remove(el)
for idx in order:
    sldIdLst.append(ids[idx])

prs.save("CFI_华为云_汇报材料.pptx")
print(f"saved: {len(prs.slides._sldIdLst)} slides")
