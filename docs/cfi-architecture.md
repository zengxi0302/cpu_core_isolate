# CFI 架构总览图

技术评审用的"原理架构"一图流，对应 `docs/design-document.md` §2 总体架构和 §3 状态机/隔离动作章节。
PPT (`docs/CFI_华为云_汇报材料.pptx`) 也会复用这张图替换之前的 ASCII 块图。

## 文件

| 文件 | 用途 |
| --- | --- |
| `cfi-architecture.drawio` | 源文件（mxGraph XML）。在 <https://app.diagrams.net/> 打开即可编辑。 |
| `cfi-architecture.svg` | 1920×1080 矢量图。可直接拖进 PPT（PowerPoint 2016+）或贴 README。 |
| `cfi-architecture.png` | 1920×1080 位图。给老版本 PPT / 复制粘贴用。 |
| `render_drawio.py` | 把 `.drawio` 渲成 SVG 的 Python 脚本（无外部依赖）。 |

## 修改并重新生成的流程

1. 在 <https://app.diagrams.net/> 打开 `cfi-architecture.drawio` 编辑。
2. 「文件 → 保存」覆盖回本地（或导出 XML 替换文件）。
3. 重新生成 SVG：

   ```bash
   python3 docs/render_drawio.py docs/cfi-architecture.drawio docs/cfi-architecture.svg
   ```

4. 把 SVG 渲成 PNG（用 playwright 自带 chromium，已在 d2 安装时下载）：

   ```bash
   CHROMIUM="/Users/bin/Library/Caches/ms-playwright/chromium-1134/chrome-mac/Chromium.app/Contents/MacOS/Chromium"
   python3 - <<'PY'
   svg = open('docs/cfi-architecture.svg').read().replace('<?xml version="1.0" encoding="UTF-8"?>\n','')
   open('/tmp/cfi_wrap.html','w').write(
     '<!DOCTYPE html><html><head><meta charset="utf-8">'
     '<style>html,body{margin:0;padding:0;background:#fff}svg{display:block;width:1920px;height:1080px}</style>'
     f'</head><body>{svg}</body></html>'
   )
   PY
   "$CHROMIUM" --headless --disable-gpu --no-sandbox --hide-scrollbars \
     --force-device-scale-factor=1 --window-size=1936,1096 \
     --screenshot="$PWD/docs/cfi-architecture.png" file:///tmp/cfi_wrap.html
   python3 -c "from PIL import Image; Image.open('docs/cfi-architecture.png').crop((0,0,1920,1080)).save('docs/cfi-architecture.png')"
   ```

## 图层说明（讲解锚点）

| 区域 | 颜色 | 内容 | 评审讲解要点 |
| --- | --- | --- | --- |
| 左列 故障事件源 | 橙 | MCE Decoder Chain / EDAC skx / hwpoison + memory_failure | 所有事件统一进 CFI 通知链处理 |
| 中列 CFI 核心 | 紫 | 事件分发 → Per-CPU 阈值累计 → 五态状态机 → 动作派发 → Sysfs · Panic 抑制 | per-CPU 状态机；ISOLATING 用粗红框，强调"决策起点"；Panic 抑制**仅作用于 MCE 路径**，不改 lockup panic 默认值 |
| 右列 三档隔离 | 绿 | FULL / **INACTIVE** (默认，HCE2 物理机) / SOFT | INACTIVE 是项目自有改造：`cpu_active_mask` 清位 + IRQ 迁移，绕开 HCE2 vendor patch 的 hotplug 死锁 |
| 下排 用户态协同 | 蓝/红 | Netlink → cfi-daemon → cgroup/sysfs → VM 分级响应 | 不再默认 kill VM，按 vCPU 亲和性分档（libvirt `vcpupin`） |

## Excalidraw 旧版本（停用）

之前生成的 `cfi-architecture.excalidraw` 在 excalidraw.com 上 label 字段不被识别，
仅显示空框。新流程不再依赖 excalidraw。如需删除请 `git rm`。
