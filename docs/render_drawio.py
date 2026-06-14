#!/usr/bin/env python3
"""Render a single-page drawio (mxGraph) XML to a static SVG.

Supports the subset of mxGraph used in docs/cfi-architecture.drawio:
  - rectangles (rounded via arcSize)
  - ellipses (shape style or 'ellipse' token)
  - standalone text (shape=text)
  - edges (source/target referenced cells, with optional exit/entry fractions)
  - dashed stroke, fontStyle bits, fontColor, align/verticalAlign

Usage: python3 render_drawio.py input.drawio output.svg
"""
import sys
import html
import xml.etree.ElementTree as ET


def parse_style(s):
    out = {}
    if not s:
        return out
    for part in s.split(";"):
        part = part.strip()
        if not part:
            continue
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip()] = v.strip()
        else:
            out[part] = "1"
    return out


def load_cells(drawio_path):
    tree = ET.parse(drawio_path)
    model = tree.getroot().find(".//mxGraphModel")
    page_w = int(model.get("pageWidth", "1920"))
    page_h = int(model.get("pageHeight", "1080"))

    cells = {}
    for c in model.findall(".//mxCell"):
        geom = c.find("mxGeometry")
        g = None
        if geom is not None:
            g = {
                "x": float(geom.get("x", "0")),
                "y": float(geom.get("y", "0")),
                "width": float(geom.get("width", "0")),
                "height": float(geom.get("height", "0")),
            }
        cells[c.get("id")] = {
            "value": html.unescape(c.get("value", "") or ""),
            "style": parse_style(c.get("style", "")),
            "vertex": c.get("vertex") == "1",
            "edge": c.get("edge") == "1",
            "source": c.get("source"),
            "target": c.get("target"),
            "geom": g,
        }
    return page_w, page_h, cells


def conn_point(g, frac_x, frac_y):
    return g["x"] + frac_x * g["width"], g["y"] + frac_y * g["height"]


def default_sides(src_g, tgt_g):
    """Pick (exitX, exitY) and (entryX, entryY) based on relative position."""
    scx, scy = src_g["x"] + src_g["width"] / 2, src_g["y"] + src_g["height"] / 2
    tcx, tcy = tgt_g["x"] + tgt_g["width"] / 2, tgt_g["y"] + tgt_g["height"] / 2
    dx, dy = tcx - scx, tcy - scy
    if abs(dx) >= abs(dy):
        if dx > 0:
            return (1.0, 0.5), (0.0, 0.5)
        return (0.0, 0.5), (1.0, 0.5)
    else:
        if dy > 0:
            return (0.5, 1.0), (0.5, 0.0)
        return (0.5, 0.0), (0.5, 1.0)


def render_vertex(cell):
    g = cell["geom"]
    s = cell["style"]
    x, y, w, h = g["x"], g["y"], g["width"], g["height"]
    fill = s.get("fillColor", "none")
    stroke = s.get("strokeColor", "#000000")
    sw = s.get("strokeWidth", "1")
    dashed = s.get("dashed") == "1"
    dash_attr = ' stroke-dasharray="6 4"' if dashed else ""

    is_text_shape = s.get("shape") == "text" or s.get("text") == "1"
    is_ellipse = (s.get("shape") == "ellipse") or "ellipse" in s

    out = []
    if is_text_shape:
        pass  # no shape, just text below
    elif is_ellipse:
        cx, cy = x + w / 2, y + h / 2
        rx, ry = w / 2, h / 2
        out.append(
            f'<ellipse cx="{cx:.1f}" cy="{cy:.1f}" rx="{rx:.1f}" ry="{ry:.1f}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{dash_attr} />'
        )
    else:
        arc = float(s.get("arcSize", "0"))
        rx = max(arc, 0)
        out.append(
            f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
            f'rx="{rx}" ry="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{dash_attr} />'
        )

    value = cell["value"]
    if not value:
        return "\n".join(out)
    lines = value.split("\n")
    fs = float(s.get("fontSize", "12"))
    fc = s.get("fontColor", "#1a1a1a")
    bits = int(s.get("fontStyle", "0"))
    fw = "700" if (bits & 1) else "400"
    fst = "italic" if (bits & 2) else "normal"
    align = s.get("align", "center")
    valign = s.get("verticalAlign", "middle")

    if align == "left":
        tx = x + 8
        anchor = "start"
    elif align == "right":
        tx = x + w - 8
        anchor = "end"
    else:
        tx = x + w / 2
        anchor = "middle"

    line_h = fs * 1.25
    block_h = line_h * len(lines)
    if valign == "top":
        first_y = y + fs * 1.05 + 6
    elif valign == "bottom":
        first_y = y + h - block_h + line_h * 0.85 - 6
    else:
        first_y = y + h / 2 - block_h / 2 + line_h * 0.85

    for i, ln in enumerate(lines):
        ty = first_y + i * line_h
        txt = ln.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        out.append(
            f'<text x="{tx:.1f}" y="{ty:.1f}" font-family="Helvetica,Arial,\'PingFang SC\',\'Microsoft YaHei\',sans-serif" '
            f'font-size="{fs}" font-weight="{fw}" font-style="{fst}" fill="{fc}" text-anchor="{anchor}">{txt}</text>'
        )
    return "\n".join(out)


def render_edge(cell, cells):
    src = cells.get(cell["source"])
    tgt = cells.get(cell["target"])
    if not (src and tgt and src["geom"] and tgt["geom"]):
        return ""
    s = cell["style"]
    stroke = s.get("strokeColor", "#1a1a1a")
    sw = s.get("strokeWidth", "2")
    dashed = s.get("dashed") == "1"
    dash_attr = ' stroke-dasharray="6 4"' if dashed else ""

    ex = s.get("exitX")
    ey = s.get("exitY")
    nx = s.get("entryX")
    ny = s.get("entryY")
    if ex is not None and ey is not None:
        sp = conn_point(src["geom"], float(ex), float(ey))
    else:
        if nx is not None and ny is not None:
            (e1, e2), _ = default_sides(src["geom"], tgt["geom"])
            sp = conn_point(src["geom"], e1, e2)
        else:
            (e1, e2), _ = default_sides(src["geom"], tgt["geom"])
            sp = conn_point(src["geom"], e1, e2)

    if nx is not None and ny is not None:
        tp = conn_point(tgt["geom"], float(nx), float(ny))
    else:
        _, (n1, n2) = default_sides(src["geom"], tgt["geom"])
        tp = conn_point(tgt["geom"], n1, n2)

    marker = "url(#arrow-end)"
    out = [
        f'<line x1="{sp[0]:.1f}" y1="{sp[1]:.1f}" x2="{tp[0]:.1f}" y2="{tp[1]:.1f}" '
        f'stroke="{stroke}" stroke-width="{sw}"{dash_attr} marker-end="{marker}" />'
    ]

    label = cell["value"]
    if label:
        mx, my = (sp[0] + tp[0]) / 2, (sp[1] + tp[1]) / 2
        fs = float(s.get("fontSize", "13"))
        txt = label.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        # background pill
        char_w = fs * 0.6
        pad = 4
        w = max(len(label) * char_w, 24) + pad * 2
        h = fs + pad * 2
        out.append(
            f'<rect x="{mx - w/2:.1f}" y="{my - h/2:.1f}" width="{w:.1f}" height="{h:.1f}" '
            f'rx="4" ry="4" fill="#ffffff" opacity="0.92" stroke="{stroke}" stroke-width="0.8" />'
        )
        out.append(
            f'<text x="{mx:.1f}" y="{my + fs/3:.1f}" font-family="Helvetica,Arial,\'PingFang SC\',\'Microsoft YaHei\',sans-serif" '
            f'font-size="{fs}" fill="#1a1a1a" text-anchor="middle">{txt}</text>'
        )

    return "\n".join(out)


def render(drawio_path, svg_path):
    page_w, page_h, cells = load_cells(drawio_path)

    parts = []
    parts.append('<?xml version="1.0" encoding="UTF-8"?>')
    parts.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="0 0 {page_w} {page_h}" width="{page_w}" height="{page_h}">'
    )
    parts.append(
        '<defs>'
        '<marker id="arrow-end" viewBox="0 0 10 10" refX="9" refY="5" '
        'markerWidth="8" markerHeight="8" orient="auto-start-reverse">'
        '<path d="M0,0 L10,5 L0,10 Z" fill="context-stroke"/>'
        '</marker>'
        '</defs>'
    )
    parts.append(f'<rect width="{page_w}" height="{page_h}" fill="#ffffff"/>')

    for cid, c in cells.items():
        if c["vertex"] and c["geom"]:
            parts.append(render_vertex(c))
    for cid, c in cells.items():
        if c["edge"]:
            parts.append(render_edge(c, cells))

    parts.append("</svg>")
    with open(svg_path, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: render_drawio.py input.drawio output.svg", file=sys.stderr)
        sys.exit(1)
    render(sys.argv[1], sys.argv[2])
    print(f"wrote {sys.argv[2]}")
