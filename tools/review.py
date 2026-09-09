#!/usr/bin/env python3
"""The shipped drawing of every chart, with each finding marked on it.

A count says how many; it does not say whether the call was right. Every class
this page marks has a threshold somebody chose -- one text height for
detachment, the capped arc for a corner, out-of-machine transitions exempt from
containment -- so the numbers are only as good as those choices, and only a
reader can check them (11.9.3, 11.12).

The marks are injected into the SVG in its own grid units rather than overlaid
in CSS, so a mark cannot drift from the thing it is about.

  tools/review.py                     the corpus, the shipped pick
  tools/review.py --portfolio-row 5   one row of 11.10's table instead
  tools/review.py --gauntlet
  tools/review.py --out FILE
"""

import argparse
import html
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import audit  # noqa: E402

# One colour per class, and the classes a reader is being asked about first.
CLASSES = {
    "label detached from its own polyline": ("#c2185b", "detached"),
    "label outside its enclosing state": ("#6a1b9a", "outside parent"),
    "label over a state box": ("#b71c1c", "over a box"),
    "label over another route": ("#e65100", "over a route"),
    "label nearer another route than its own": ("#f9a825", "nearer another"),
    "attachment on a drawn corner": ("#00695c", "on a corner"),
    "texts overprint each other": ("#37474f", "overprinted"),
}

VIEWBOX = re.compile(r'viewBox="(-?[\d.]+) (-?[\d.]+) ([\d.]+) ([\d.]+)"')


def annotate(svg, marks):
    """The findings as SVG, in the document's own units, before `</svg>`."""
    box = VIEWBOX.search(svg)
    if not box:
        return svg, 0
    # Stroke and type scaled to the drawing, so a 2k chart's marks are legible
    # at the same zoom as a small one's.
    span = max(float(box.group(3)), float(box.group(4)))
    stroke = max(int(span / 400), 2)
    size = max(int(span / 45), 8)
    out = []
    drawn = 0
    for kind, (x, y, w, h), detail in marks:
        colour, short = CLASSES.get(kind, ("#1565c0", kind))
        pad = stroke * 2
        out.append(
            f'<rect x="{x - pad}" y="{y - pad}" width="{w + 2 * pad}" '
            f'height="{h + 2 * pad}" fill="none" stroke="{colour}" '
            f'stroke-width="{stroke}" stroke-dasharray="{stroke * 3},{stroke * 2}"/>')
        out.append(
            f'<text x="{x}" y="{y - pad - (stroke * 2)}" font-size="{size}" '
            f'fill="{colour}" font-family="monospace">'
            f'{html.escape(short)} &#183; {html.escape(detail)}</text>')
        drawn += 1
    return svg.replace("</svg>", "".join(out) + "</svg>"), drawn


HEAD = """<!doctype html><meta charset="utf-8"><title>scav: findings on the page</title>
<style>
 body{font:13px/1.55 -apple-system,system-ui,sans-serif;margin:0;padding:26px;
      background:#f6f6f4;color:#1b1b1a}
 h1{font-size:19px;margin:0 0 4px} .sub{color:#666;margin:0 0 8px;max-width:60em}
 .legend{margin:0 0 24px;padding:12px 14px;background:#fff;border:1px solid #e2e2de;
         border-radius:8px;display:flex;flex-wrap:wrap;gap:6px 20px}
 .legend span{white-space:nowrap;font-family:ui-monospace,monospace;font-size:12px}
 .sw{display:inline-block;width:11px;height:11px;border:2px dashed;margin-right:6px;
     vertical-align:-1px}
 .chart{background:#fff;border:1px solid #e2e2de;border-radius:8px;padding:18px;
        margin-bottom:22px}
 .chart>h2{font-size:15px;margin:0 0 2px;font-family:ui-monospace,monospace}
 .tally{color:#666;margin:0 0 12px;font-family:ui-monospace,monospace;font-size:12px}
 .clean{color:#1c6b3c;font-style:italic}
 svg{width:100%;height:auto;border:1px solid #ecece8;border-radius:4px}
</style>
<h1>Every finding, marked on the drawing it is about</h1>
<p class="sub">One panel per chart, the drawing that ships. Each dashed box is a
finding, labelled with its class and its measurement. <b>The thresholds are
chosen, not given</b> &mdash; detachment is one text height, a corner is the
capped arc, and an out-of-machine transition is exempt from containment
entirely. If a mark is not a violation to you, the threshold is wrong and every
number resting on it is wrong with it.</p>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None)
    ap.add_argument("--gauntlet", action="store_true")
    ap.add_argument("--portfolio-row", dest="row", type=int, default=None)
    ap.add_argument("--scav", default=None)
    args = ap.parse_args()

    scav_bin = audit.find_scav(args.scav)
    if scav_bin is None or not scav_bin.exists():
        print("no scav binary; build first", file=sys.stderr)
        return 1
    root = audit.GAUNTLET if args.gauntlet else audit.CORPUS
    where = audit.REPO_ROOT / "out/review"
    where.mkdir(parents=True, exist_ok=True)
    page = Path(args.out) if args.out else (where / "findings.html")

    body = []
    total = {}
    for chart in sorted(root.glob("*.scav")):
        svg_path = where / (chart.name + ".svg")
        cmd = [str(scav_bin), "render", "-o", str(svg_path)]
        if args.row is not None:
            cmd += ["--portfolio-row", str(args.row)]
        done = subprocess.run(cmd + [str(chart)], capture_output=True, text=True,
                              check=False)
        if done.returncode != 0:
            print(f"{chart.name}: {done.stderr.strip()}", file=sys.stderr)
            continue
        every, rect, doc = audit.geometry(chart, scav_bin, args.row)
        found, _, marks = audit.audit(svg_path.read_text(encoding="utf-8"), every,
                                      rect, doc, False)
        for k, v in found.items():
            total[k] = total.get(k, 0) + v
        marked, _ = annotate(svg_path.read_text(encoding="utf-8"), marks)
        tally = ", ".join(f"{CLASSES[k][1]} {found[k]}" for k in CLASSES
                          if found.get(k))
        note = (f'<p class="tally">{html.escape(tally)}</p>' if tally
                else '<p class="tally clean">nothing this audit knows how to see.</p>')
        body.append(f'<div class="chart"><h2>{html.escape(chart.name)}</h2>'
                    f'{note}{marked}</div>')

    keys = ''.join(f'<span><i class="sw" style="border-color:{c}"></i>{n}</span>'
                   for c, n in CLASSES.values())
    head = HEAD + f'<div class="legend">{keys}</div>'
    summary = ", ".join(f"{CLASSES[k][1]} {total[k]}" for k in CLASSES if total.get(k))
    page.write_text(head + f'<p class="sub"><b>Corpus total:</b> '
                    f'{html.escape(summary)}</p>' + "\n".join(body),
                    encoding="utf-8")
    print(f"wrote {page}")
    print(f"  {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
