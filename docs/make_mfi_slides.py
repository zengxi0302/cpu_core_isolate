#!/usr/bin/env python3
"""Extend CFI_华为云_汇报材料.pptx with the memory fault domain (MFI):
two business slides after the CPU solution section, three deep-dive
slides (T9-T11), one value slide, plus edits to the title/agenda/
divider/roadmap/closing slides. Style replicates the existing deck."""

import copy
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
BLANK = prs.slides[3].slide_layout  # Blank layout used by content slides


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
    """runs: str, or list of paragraphs; paragraph: str or list of
    (txt, dict) run tuples."""
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
        color = CODE_CMT if ln.lstrip().startswith(("/*", "//", "*")) else CODE_TXT
        paras.append([(ln if ln else " ", {"color": color})])
    text(slide, 7.08, 1.84, 5.31, 4.46, paras, size=10.5, font="Consolas",
         line_spacing=1.0)


def metric_card(slide, x, big, label, sub):
    box(slide, x, 1.35, 2.85, 1.85, fill=CARD_BG)
    box(slide, x, 1.35, 2.85, 0.08, fill=RED)
    text(slide, x, 1.50, 2.85, 0.85, big, size=36, bold=True, color=RED,
         align=PP_ALIGN.CENTER)
    text(slide, x, 2.40, 2.85, 0.40, label, size=14, bold=True, color=DARK,
         align=PP_ALIGN.CENTER)
    text(slide, x, 2.80, 2.85, 0.35, sub, size=11, color=GRAY,
         align=PP_ALIGN.CENTER)


def new_slide():
    return prs.slides.add_slide(BLANK)


# ============ B1: 为什么做内存故障隔离（价值/问题） ============
s = new_slide()
biz_header(s, "新战场：内存故障——导致宕机的另一半", "04")
text(s, 0.75, 1.18, 11.8, 0.5,
     "内存（DRAM）与 CPU 同为易损器件，UCE 同样经 MCE 上报。用户态毒页消费已能收敛到单进程/单 VM，"
     "但内核态消费仍是整机宕机。", size=14, color=BODY)

text(s, 0.75, 1.80, 5.8, 0.4, "现有能力边界", size=16, bold=True, color=RED)
rows = [
    ("故障场景", "现有行为", "结果", True),
    ("用户态进程消费毒页", "memory_failure 杀进程", "✓ 单 VM 损失", False),
    ("copy_from_user 等路径", "extable fixup 杀进程", "✓ 单 VM 损失", False),
    ("内核态消费毒页(无fixup)", "mce_panic()", "✗ 整机宕机", False),
    ("巡检发现 UCE(未消费)", "依赖发行版配置", "△ 机会被浪费", False),
    ("CE 高频页(UCE 前兆)", "仅 EDAC 计数", "△ 坐等恶化", False),
]
y = 2.25
for c1, c2, c3, hdr in rows:
    if hdr:
        box(s, 0.75, y, 5.95, 0.42, fill=TBLHDR)
    elif rows.index((c1, c2, c3, hdr)) % 2 == 0:
        box(s, 0.75, y, 5.95, 0.42, fill=FOOT_BG)
    col = WHITE if hdr else BODY
    text(s, 0.80, y, 2.55, 0.42, c1, size=10.5, bold=hdr, color=col,
         anchor=MSO_ANCHOR.MIDDLE)
    text(s, 3.35, y, 1.95, 0.42, c2, size=10.5, bold=hdr, color=col,
         anchor=MSO_ANCHOR.MIDDLE)
    text(s, 5.30, y, 1.45, 0.42, c3, size=10.5, bold=hdr,
         color=col if hdr else (RED if "✗" in c3 else
                                (GREEN if "✓" in c3 else ORANGE)),
         anchor=MSO_ANCHOR.MIDDLE)
    y += 0.42

text(s, 7.00, 1.80, 5.6, 0.4, "内核态 UCE 可恢复性：取决于落点 × 消费方式",
     size=16, bold=True, color=RED)
box(s, 7.00, 2.25, 5.55, 4.30, fill=CARD_BG)
items = [
    ("A  异步发现（巡检/scrub，未消费）", "完全可恢复 → 页隔离，零损失", GREEN),
    ("B  内核态消费用户页，有 MC-safe fixup", "可恢复 → 杀属主（已有能力）", GREEN),
    ("C  内核态消费用户页，无 fixup", "需内核补丁扩 fixup（Phase 3）", ORANGE),
    ("D  虚机 vCPU 直接消费", "KVM 注 vMCE，收敛到 Guest 内部", GREEN),
    ("E  内核自身数据（slab/页表/text）", "不可恢复 → 受控 kdump，绝不静默", RED),
]
y = 2.42
for head, sub, c in items:
    text(s, 7.20, y, 5.25, 0.34, head, size=11.5, bold=True, color=DARK)
    text(s, 7.45, y + 0.32, 5.0, 0.32, sub, size=10.5, color=c)
    y += 0.74
text(s, 7.20, y + 0.02, 5.2, 0.50,
     "宿主机内存大头是虚机页 → 多数内核态 UCE 落点可救",
     size=11.5, bold=True, color=RED)
footer(s)
B1 = s

# ============ B2: MFI 方案总览 ============
s = new_slide()
biz_header(s, "方案：内存故障域（MFI）三层防线，复用 CFI 底座", "04")
text(s, 0.75, 1.18, 11.8, 0.5,
     "同一个 cpu_fault_isolate.ko 内新增内存域：共用 panic 抑制层 / netlink / sysfs / daemon 协议，"
     "不改内核源码。", size=14, color=BODY)

cards = [
    ("① 事前：CE 预隔离", RED,
     ["CE 高频页是 UCE 前兆（行/列劣化）",
      "页级滑动窗口计数，达阈值即",
      "soft offline：先迁移页内容再下线",
      "→ 业务零感知、零损失"]),
    ("② 事中：异步 UCE 兜底", ORANGE,
     ["patrol scrub / SRAO：数据未被消费",
      "最干净的窗口：memory_failure",
      "页隔离替代 panic，干净页直接丢弃",
      "→ 不宕机，至多影响单进程/单 VM"]),
    ("③ 事后：内核态落点甄别", GREEN,
     ["panic 抑制后由甄别引擎恢复安全语义",
      "用户/虚机页 → 杀属主 + 隔离页",
      "内核页 → 主动受控 panic + kdump",
      "→ 能救的救，不能救的绝不静默"]),
]
x = 0.75
for title, color, lines in cards:
    box(s, x, 1.85, 3.85, 3.30, fill=CARD_BG)
    box(s, x, 1.85, 3.85, 0.08, fill=color)
    text(s, x + 0.15, 2.05, 3.55, 0.45, title, size=15, bold=True, color=color)
    text(s, x + 0.15, 2.55, 3.55, 2.45,
         [[(ln, {})] for ln in lines], size=11.5, color=BODY,
         line_spacing=1.25)
    x += 3.98

box(s, 0.75, 5.45, 11.80, 0.95, fill=FOOT_BG)
box(s, 0.75, 5.45, 0.12, 0.95, fill=RED)
text(s, 1.10, 5.52, 11.2, 0.85,
     "诚实的边界：内核自身数据被同步消费（场景 E）没有隔离机会——硬扛等于静默数据损坏，比宕机更糟。"
     "MFI 的策略是受控 kdump 保现场 + 用①压低 E 的发生概率。",
     size=13.5, bold=True, color=RED, anchor=MSO_ANCHOR.MIDDLE)
text(s, 0.75, 6.55, 11.8, 0.45,
     "DIMM 介质级记账：页隔离治标，介质劣化治本 —— 越限只上报 MIGRATE_ADVISED，整机疏散决策权留给上层调度。",
     size=11.5, color=GRAY)
footer(s)
B2 = s

# ============ T9: 落点甄别 ============
s = new_slide()
dive_header(s, "TRIAGE", "攻坚点九：panic 抑制之后，内核态 UCE 没有安全默认值", "T9", 5)
dive_card(s, 1.40, 1.70, RED, "难点",
          ["CFI 把 tolerant 提到 3 压住了 mce_panic()，副作用：",
           "内核态消费毒页后会带着损坏数据继续执行 ——",
           "静默数据损坏，比宕机更糟。必须有人把「安全语义」",
           "补回来：要么可救，要么立刻受控地死。"])
dive_card(s, 3.20, 1.55, ORANGE, "常规做法为何失效",
          ["• 直接放行 → 静默损坏，污染存储/网络不可回溯",
           "• 一律 panic → 白白丢掉可救场景（宿主内存大头",
           "  是虚机页，多数落点可收敛到单 VM）",
           "• 在 NMI 里做复杂判定 → 死锁/重入风险"])
dive_card(s, 4.85, 1.55, GREEN, "我们的解法",
          ["白名单判决：PCC=0 且 RIPV=1 且毒页为用户/虚机页",
           "（LRU/anon/hugetlb）或空闲页才救，其余主动 panic；",
           "判定只读 vmemmap 页元数据，绝不触碰毒页本身；",
           "mem_triage 默认关闭，灰度启用；判决表有 18 项主机单测。"])
code_block(s, "core/mfi_policy.h  ·  甄别判决表（纯函数，可单测）", [
    "/* 误判最坏后果: RECOVER 变成一次",
    " * 失败的恢复(上报), 而非数据损坏 */",
    "mfi_triage_decide(class, ripv, pcc)",
    "{",
    "    if (pcc)   return PANIC;  /* 上下文已损 */",
    "    if (!ripv) return PANIC;  /* 无法续点 */",
    "",
    "    switch (class) {",
    "    case PG_USER:  /* LRU/anon/hugetlb */",
    "        return RECOVER; /* 杀属主+隔离页 */",
    "    case PG_FREE:",
    "        return RECOVER; /* 无人消费,直接隔离 */",
    "    case PG_KERNEL: /* slab/页表/text */",
    "    case PG_INVALID:",
    "    default:",
    "        return PANIC;   /* kdump 保现场 */",
    "    }",
    "}",
])
footer(s)
T9 = s

# ============ T10: 页隔离机制复用 ============
s = new_slide()
dive_header(s, "HWPOISON", "攻坚点十：不改内核，复用 memory_failure 全套页隔离", "T10", 4)
dive_card(s, 1.40, 1.70, RED, "难点",
          ["页隔离的正确实现（迁移/rmap 杀进程/HWPoison 标记）",
           "全在内核 mm 里，但 soft_offline_page 未导出给模块；",
           "memory_failure 结果异步产生；GHES / CEC / madvise",
           "多个主体可能同时处置同一个页 —— 必须去重。"])
dive_card(s, 3.20, 1.55, ORANGE, "常规做法为何失效",
          ["• 把页迁移逻辑抄进 ko → 数千行 mm 代码不可维护",
           "• 轮询页标志等结果 → 竞态 + 开销",
           "• 不去重直接触发 → 与内核自身处理双重动作，",
           "  重复杀进程 / 重复上报"])
dive_card(s, 4.85, 1.55, GREEN, "我们的解法",
          ["① soft_offline_page：kprobe 查符号直调（复用 CFI",
           "   处理 mce_tolerant 的成熟手法）；",
           "② 硬隔离走 memory_failure_queue（GHES 同款导出路径）；",
           "③ 结果回执挂 memory_failure_event tracepoint（运行时",
           "   查找），叠加 HWPoison 标志判重 —— 双重去重。"])
code_block(s, "core/mfi_page.c  ·  三个机制要点", [
    "/* 1. 未导出符号: kprobe 解析后直调 */",
    "kp.symbol_name = \"soft_offline_page\";",
    "register_kprobe(&kp); fn = kp.addr;",
    "",
    "/* 2. 硬隔离: 导出接口, NMI 安全 */",
    "memory_failure_queue(pfn, 0);",
    "",
    "/* 3. 回执: 运行时找 tracepoint */",
    "for_each_kernel_tracepoint(find_cb, ..);",
    "tracepoint_probe_register(tp, probe, ..);",
    "",
    "/* probe 里凭 \"是否在我表中且 POISONED\"",
    " * 归因; PageHWPoison 已置位 → 别人",
    " * 已处理, 只记账不重复触发 */",
])
footer(s)
T10 = s

# ============ T11: 跨域协同 ============
s = new_slide()
dive_header(s, "CROSS-DOMAIN", "攻坚点十一：两个故障域共享一条 MCE 通路", "T11", 3)
dive_card(s, 1.40, 1.70, RED, "难点",
          ["CPU 域与内存域监听同一条 MCE decode chain。",
           "实测发现：内存控制器错误码落入 CPU 域分类器的",
           "GENERIC_CORE 兜底分支 —— 一根劣化 DIMM 的 CE 风暴",
           "会把汇报它的健康 CPU 核推进 DEGRADED 甚至隔离。"])
dive_card(s, 3.20, 1.55, ORANGE, "常规做法为何失效",
          ["• 两个域各写一份错误码分类 → 标准漂移，",
           "  同一条 MCE 可能被两边同时计数",
           "• 与内核 CEC（CE 收集器）并存时重复 soft offline，",
           "  同一页被处置两次"])
dive_card(s, 4.85, 1.55, GREEN, "我们的解法",
          ["① MCACOD 内存签名判定提为共享内联函数，",
           "   CPU 域显式忽略内存错误（修复跨域污染）；",
           "② 探测到 CEC 启用 → 预隔离自动降级为只记账；",
           "③ DIMM 介质记账与页隔离解耦：越限仅上报",
           "   MIGRATE_ADVISED，不做整机自动动作。"])
code_block(s, "arch/x86/cfi_x86.h  ·  共享签名判定", [
    "/* 单一事实来源, 两个域共用 */",
    "static inline bool",
    "cfi_x86_is_memory_errcode(u16 mcacod)",
    "{",
    "    mcacod &= MCACOD; /* 去 filter 位 */",
    "    /* 内存控制器: 0000_0001_MMMM_CCCC",
    "     * (含 patrol scrub 0xC0-0xCF) */",
    "    if ((mcacod & 0xff80) == 0x0080)",
    "        return true;",
    "    /* 缓存 bank 报告的毒数据消费 */",
    "    return mcacod == MCACOD_DATA  ||",
    "           mcacod == MCACOD_INSTR ||",
    "           mcacod == MCACOD_L3WB;",
    "}",
    "",
    "/* CPU 域分类器第一行: ",
    " *   if (is_memory_errcode(ec)) return 0; */",
])
footer(s)
T11 = s

# ============ V: MFI 业务价值 ============
s = new_slide()
biz_header(s, "业务价值：内存故障从「看天」到「可治理」", "05")
metric_card(s, 0.75, "0 损失", "CE 预隔离", "soft offline，业务无感")
metric_card(s, 3.73, "不宕机", "异步 UCE 兜底", "页隔离替代 panic")
metric_card(s, 6.71, "单 VM", "内核态 UCE 甄别", "虚机页落点收敛到 1 个 VM")
metric_card(s, 9.69, "0 静默", "不可救场景", "受控 kdump，现场可回溯")

box(s, 0.75, 3.50, 11.80, 3.05, fill=CARD_BG)
text(s, 1.00, 3.62, 11.0, 0.45, "故障场景处置对比（同一台宿主机，60 台 ECS）",
     size=14, bold=True, color=DARK)
vrows = [
    ("故障场景", "无 MFI（现状）", "启用 MFI", True),
    ("CE 高频页（UCE 前兆）", "坐等恶化为 UCE", "提前迁移页内容，零损失", False),
    ("patrol scrub 发现 UCE", "依赖配置，可能整机宕机", "页隔离，不宕机", False),
    ("内核态消费 UCE（虚机页）", "整机宕机，60 台全丢", "杀 1 台 VM，59 台无感", False),
    ("内核自身数据消费 UCE", "整机宕机（或静默损坏）", "受控 kdump，保留现场", False),
    ("DIMM 介质持续劣化", "反复报错直至故障", "越限上报，建议疏散+报修", False),
]
y = 4.12
for c1, c2, c3, hdr in vrows:
    if hdr:
        box(s, 1.00, y, 11.30, 0.40, fill=TBLHDR)
    elif vrows.index((c1, c2, c3, hdr)) % 2 == 0:
        box(s, 1.00, y, 11.30, 0.40, fill=FOOT_BG)
    col = WHITE if hdr else BODY
    text(s, 1.05, y, 3.85, 0.40, c1, size=11.5, bold=hdr, color=col,
         anchor=MSO_ANCHOR.MIDDLE)
    text(s, 4.95, y, 3.30, 0.40, c2, size=11.5, bold=hdr, color=col,
         anchor=MSO_ANCHOR.MIDDLE)
    text(s, 8.30, y, 3.95, 0.40, c3, size=11.5, bold=hdr,
         color=WHITE if hdr else RED, anchor=MSO_ANCHOR.MIDDLE)
    y += 0.40
footer(s)
V = s

# ============ 修订既有页 ============

def set_text(shape, old, new):
    for p in shape.text_frame.paragraphs:
        for r in p.runs:
            if old in r.text:
                r.text = r.text.replace(old, new)
                return True
    return False


def edit(slide, old, new):
    for sh in slide.shapes:
        if sh.has_text_frame and old in sh.text_frame.text:
            if set_text(sh, old, new):
                return True
    print(f"  !! not found: {old[:30]}")
    return False

slides = prs.slides
# 标题页
edit(slides[0], "CPU 核心故障自隔离（CFI）", "CPU / 内存故障自隔离（CFI · MFI）")
edit(slides[0], "让单核硬件故障不再拖垮整机", "让单核 / 单页硬件故障不再拖垮整机")
edit(slides[0], "版本 v0.2", "版本 v0.4")
# 目录页
edit(slides[1], "CPU 故障自隔离三层防御", "CPU / 内存双故障域防御")
edit(slides[1], "关键技术与八大攻坚点", "关键技术与十一大攻坚点")
# 深潜分隔页
edit(slides[7], "技术深潜：八大攻坚点", "技术深潜：十一大攻坚点")
edit(slides[7], "状态机与阈值引擎",
     "状态机与阈值引擎  ·  内存 UCE 落点甄别  ·  HWPoison 页隔离复用  ·  跨域协同")
# 规划页：左侧进展追加两行
prog = slides[17]
text(prog, 0.85, 4.35, 0.40, 0.40, "✓", size=15, bold=True, color=GREEN)
text(prog, 1.25, 4.35, 5.60, 0.45, "内存故障域 MFI：预隔离/UCE 甄别开发完成",
     size=12.5, color=BODY)
text(prog, 0.85, 4.87, 0.40, 0.40, "○", size=15, bold=True, color=ORANGE)
text(prog, 1.25, 4.87, 5.60, 0.45, "MFI EINJ 物理机注入验证",
     size=12.5, color=BODY)
edit(prog, "守护进程 + VM 热迁移联动", "守护进程（含内存事件）+ VM 热迁移联动")
edit(prog, "对接上层调度（弹性伸缩 / 编排）",
     "MC-safe 内核补丁入 openEuler + 上层调度对接")
# 结尾页
edit(slides[18], "单核故障  ·  自动隔离  ·  守护 ECS SLA",
     "单核 / 单页故障  ·  自动隔离  ·  守护 ECS SLA")
# 全部页脚统一
for sl in slides:
    for sh in sl.shapes:
        if sh.has_text_frame and "CPU 故障自隔离（CFI）" in sh.text_frame.text:
            set_text(sh, "CPU 故障自隔离（CFI）", "CPU / 内存故障自隔离（CFI·MFI）")

# ============ 重排序 ============
sldIdLst = prs.slides._sldIdLst
ids = list(sldIdLst)
# 原 19 页为 ids[0..18]，新页按创建顺序为 ids[19..24]
order = (list(range(0, 7))        # 0-6 原业务篇
         + [19, 20]               # B1 B2
         + list(range(7, 16))     # divider + T1-T8
         + [21, 22, 23]           # T9 T10 T11
         + [16, 24]               # CPU 价值 + MFI 价值
         + [17, 18])              # 规划 + 结尾
assert sorted(order) == list(range(25)), order
for el in ids:
    sldIdLst.remove(el)
for idx in order:
    sldIdLst.append(ids[idx])

prs.save("CFI_华为云_汇报材料.pptx")
print(f"saved: {len(prs.slides.__iter__.__self__._sldIdLst)} slide ids")
