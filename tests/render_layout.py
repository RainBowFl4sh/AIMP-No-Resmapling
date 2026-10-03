#!/usr/bin/env python3
"""Draws the settings page layout written by MockHost (layout.txt) as one PNG per tab, so the grid can be
checked without AIMP: boxes at the real positions (96 DPI), the text roughly in AIMP's font size.
Needs ImageMagick ("convert"). Usage: tests/render_layout.py <layout.txt> <output folder>"""
import os
import subprocess
import sys
import textwrap

KIND_STYLE = {
    "Label": ("none", "#1a1a1a"), "Check": ("#ffffff", "#1a1a1a"), "Combo": ("#ffffff", "#1a1a1a"),
    "Edit": ("#ffffff", "#1a1a1a"), "Spin": ("#ffffff", "#1a1a1a"), "Button": ("#e1e1e1", "#1a1a1a"),
    "Memo": ("#ffffff", "#555555"), "Image": ("#7a4fc0", "#ffffff"),
}


def esc(t):
    return t.replace("\\", "\\\\").replace("'", "\\'").replace('"', '\\"')


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    tabs = {}
    for line in open(src, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 10:
            continue
        tab, name, kind, x, y, w, h, bold, hidden = parts[:9]
        text = "\t".join(parts[9:])
        tabs.setdefault((int(tab), name), []).append((kind, int(x), int(y), int(w), int(h), bold == "1", hidden == "1", text))
    scale = 2
    for (idx, name), items in sorted(tabs.items()):
        cmd = ["convert", "-size", f"{465 * scale}x{420 * scale}", "xc:#f0f0f0", "-font", "DejaVu-Sans"]
        for kind, x, y, w, h, bold, hidden, text in items:
            fill, ink = KIND_STYLE.get(kind, ("#ffdddd", "#000"))
            stroke = "#cc0000" if kind == "Label" else "#8a8a8a"
            if hidden:
                stroke, fill = "#bbbbbb", "none"
            cmd += ["-fill", fill, "-stroke", stroke, "-strokewidth", "1",
                    "-draw", f"rectangle {x * scale},{y * scale} {(x + w) * scale - 1},{(y + h) * scale - 1}"]
            if kind == "Check":
                cmd += ["-fill", "#ffffff", "-stroke", "#444", "-draw",
                        f"rectangle {(x + 2) * scale},{(y + 4) * scale} {(x + 14) * scale},{(y + 16) * scale}"]
            label = text if kind != "Combo" else "▾"
            if not label:
                continue
            tx = x + (20 if kind == "Check" else 4 if kind in ("Button", "Combo") else 0)
            chars = max(1, int((w - (tx - x)) / (6.6 if bold else 6.0)))
            lines = textwrap.wrap(label, chars) if h > 20 else [label[:chars]]
            for n, l in enumerate(lines[: max(1, h // 16)]):
                cmd += ["-stroke", "none", "-fill", ink, "-pointsize", str(9 * scale * 1.33 * 0.85),
                        "-weight", "bold" if bold else "normal",
                        "-draw", f"text {tx * scale},{(y + 13 + n * 16) * scale} '{esc(l)}'"]
        path = os.path.join(out, f"{idx}-{name.replace(' / ', '-').replace(' ', '')}.png")
        subprocess.run(cmd + [path], check=True)
        print("written", path)


if __name__ == "__main__":
    main()
