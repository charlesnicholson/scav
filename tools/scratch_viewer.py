#!/usr/bin/env python3
"""Renders `scav dump --layout --json` as a standalone SVG-in-HTML page, for
eyeballing layouts. Throwaway: superseded by `scav render` once that exists.

    python3 tools/scratch_viewer.py test_data/charts/vac.scav > /tmp/vac.html
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

PALETTE = ["#4C78A8", "#F58518", "#54A24B", "#B279A2", "#E45756", "#72B7B2"]


def newest_scav() -> Path:
    exes = sorted(REPO_ROOT.glob("out/*/bin/scav"), key=lambda p: p.stat().st_mtime)
    if not exes:
        raise SystemExit("no built scav under out/; run ./build.sh first")
    return exes[-1]


def esc(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def state_title(doc: dict, i: int) -> str:
    s = doc["states"][i]
    return s["name"] or f"${s['kind']}"


def render(doc: dict) -> str:
    g = doc["geometry"]
    cx, cy, cw, ch = g["chart"]
    pad = 200  # grid units of margin around the chart box
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="{cx - pad} {cy - pad} {cw + 2 * pad} {ch + 2 * pad}" '
        f'style="width:100%; height:auto; background:#fafafa">',
        "<defs><marker id='arr' viewBox='0 0 8 8' refX='7' refY='4' "
        "markerWidth='7' markerHeight='7' orient='auto-start-reverse'>"
        "<path d='M0,0 L8,4 L0,8 z' fill='context-stroke'/></marker></defs>",
    ]

    # Submachine regions first, dashed, so states draw over them.
    for m, (x, y, w, h) in enumerate(g["sub"]):
        if w == 0 and h == 0 and doc["submachines"][m]["live"] == 0:
            continue
        out.append(
            f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="none" '
            f'stroke="#bbb" stroke-width="6" stroke-dasharray="24 18"/>'
        )

    font = 130  # grid units; text is decoration here, not measurement
    for i, (x, y, w, h) in enumerate(g["state"]):
        if doc["states"][i]["live"] == 0:
            continue
        out.append(
            f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="40" '
            f'fill="#fff" stroke="#333" stroke-width="8">'
            f"<title>{esc(state_title(doc, i))} #{i}</title></rect>"
        )
        out.append(
            f'<text x="{x + 60}" y="{y + font + 40}" font-size="{font}" '
            f'font-family="JetBrains Mono, monospace" fill="#333">'
            f"{esc(state_title(doc, i))}</text>"
        )

    for t, points in enumerate(g["route"]):
        if not points:
            continue
        color = PALETTE[t % len(PALETTE)]
        pts = " ".join(f"{x},{y}" for x, y in points)
        src = state_title(doc, doc["transitions"][t]["src"])
        dst = state_title(doc, doc["transitions"][t]["dst"])
        out.append(
            f'<polyline points="{pts}" fill="none" stroke="{color}" '
            f'stroke-width="10" stroke-opacity="0.75" marker-end="url(#arr)">'
            f"<title>{esc(src)} -&gt; {esc(dst)} #{t}</title></polyline>"
        )
        for x, y, side, depth in g["port"][t]:
            out.append(
                f'<circle cx="{x}" cy="{y}" r="22" fill="{color}">'
                f"<title>port s{side} d{depth}</title></circle>"
            )

    out.append("</svg>")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("chart", type=Path)
    ap.add_argument("--exe", type=Path, default=None, help="scav binary to run")
    args = ap.parse_args()

    exe = args.exe or newest_scav()
    result = subprocess.run(
        [str(exe), "dump", "--layout", "--json", str(args.chart)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=REPO_ROOT,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        return result.returncode
    doc = json.loads(result.stdout)
    g = doc["geometry"]

    name = esc(doc["chart"]["name"])
    print(f"""<!doctype html><meta charset="utf-8">
<title>scav scratch: {name}</title>
<body style="margin:0; font:14px 'JetBrains Mono', monospace">
<div style="padding:8px 12px; background:#222; color:#eee">
  {name} &middot; {esc(str(args.chart))} &middot;
  gen {g["gen"]} &middot; structural {g["structural_hash"]:08x} &middot;
  coordinate {g["coordinate_hash"]:08x} &middot;
  extent {g["chart"][2]}&times;{g["chart"][3]}
</div>
{render(doc)}
</body>""")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
