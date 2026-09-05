#!/usr/bin/env python3
"""One page of every corpus chart drawn three ways, for blind review.

`baseline.py` renders the SVGs and reports what it could not run; this turns
that directory into a single self-contained page a reviewer can open, print, or
send. The SVGs are inlined as data URIs so the file carries no dependencies,
and every panel states the extent it came out at, because "which of these is
better" is partly "which of these fits on anything".

  tools/poster.py                      out/baseline -> out/baseline/poster.html
  tools/poster.py --in DIR --out F
"""

import argparse
import base64
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS = REPO_ROOT / "test_data/charts"

# scav, then the two incumbents. Categorical slots 1-3 of the validated
# palette, which clears the all-pairs floors in light mode; aqua sits under 3:1
# on this surface, so every mark that uses it carries a visible label.
ENGINES = (
    ("scav", "scav", "#2a78d6"),
    ("plantuml", "PlantUML", "#eb6834"),
    ("elkjs", "elkjs", "#1baf7a"),
)

# A cell that rendered but lost something. `baseline.py` prints these; they are
# repeated here so the page cannot credit an engine with a chart it mangled.
DEGRADED = {
    ("bottler.scav", "plantuml"): "reference into a non-first region: "
                                  "Weighing -> Dispensing anchored to its composite",
    ("tcp.scav", "plantuml"): "reference into a non-first region: "
                              "FinSent -> Established anchored to its composite",
}

SIZE = re.compile(r'width="(\d+(?:\.\d+)?)(?:px)?"\s+height="(\d+(?:\.\d+)?)(?:px)?"')


def extent(svg: str):
    """Width and height in user units, from the first size the document states."""
    found = SIZE.search(svg[:2000])
    if found is None:
        return None
    return (round(float(found.group(1))), round(float(found.group(2))))


def counts(chart: Path, scav_bin: Path):
    """States and transitions, from the model rather than from the picture."""
    try:
        out = subprocess.run([str(scav_bin), "dump", "--json", str(chart)],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    doc = json.loads(out)
    live = sum(1 for s in doc["states"] if s.get("live", 1))
    return (live, sum(1 for t in doc["transitions"] if t.get("live", 1)))


def aspect(size):
    return size[0] / size[1] if size[1] else 0.0


def data_uri(svg: str) -> str:
    return "data:image/svg+xml;base64," + base64.b64encode(svg.encode("utf-8")).decode()


STYLE = """
:root {
  color-scheme: light;
  --surface-0: #f4f4f2;
  --surface-1: #fcfcfb;
  --line: #dededa;
  --text-primary: #0b0b0b;
  --text-secondary: #52514e;
  --text-muted: #85847e;
  --scav: #2a78d6;
  --plantuml: #eb6834;
  --elkjs: #1baf7a;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  padding: 40px 40px 64px;
  background: var(--surface-0);
  color: var(--text-primary);
  font: 14px/1.5 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
  font-feature-settings: "tnum" 1;
}
h1 { font-size: 30px; letter-spacing: -0.02em; margin: 0 0 6px; }
.sub { color: var(--text-secondary); margin: 0 0 22px; max-width: 62ch; }
.legend { display: flex; gap: 20px; flex-wrap: wrap; margin: 0 0 10px; }
.chip { display: flex; align-items: center; gap: 8px; font-weight: 600; }
.swatch { width: 12px; height: 12px; border-radius: 3px; }
.tiles { display: flex; gap: 12px; flex-wrap: wrap; margin: 20px 0 34px; }
.tile {
  background: var(--surface-1); border: 1px solid var(--line);
  border-radius: 10px; padding: 12px 16px; min-width: 172px;
}
.tile .k { color: var(--text-secondary); font-size: 12px; letter-spacing: 0.02em; }
.tile .v { font-size: 24px; font-weight: 650; letter-spacing: -0.01em; margin-top: 2px; }
.tile .n { color: var(--text-muted); font-size: 12px; }
.card {
  background: var(--surface-1); border: 1px solid var(--line);
  border-radius: 12px; padding: 18px 18px 14px; margin-bottom: 20px;
  break-inside: avoid;
}
.card h2 { font-size: 17px; margin: 0 0 2px; letter-spacing: -0.01em; }
.card .meta { color: var(--text-secondary); font-size: 12.5px; margin-bottom: 14px; }
.panels { display: grid; grid-template-columns: repeat(3, 1fr); gap: 14px; }
.panel { border: 1px solid var(--line); border-radius: 8px; overflow: hidden; background: #fff; }
.panel .head {
  display: flex; align-items: baseline; gap: 8px; justify-content: space-between;
  padding: 8px 10px; border-bottom: 1px solid var(--line); background: var(--surface-1);
}
.panel .who { font-weight: 650; display: flex; align-items: center; gap: 7px; }
.panel .dim { color: var(--text-secondary); font-size: 12px; }
.stage { height: 340px; padding: 10px; }
/* A grid item's max-height resolves against the track, not the box, so a tall
   drawing overflows and the panel clips it. Sizing the image to the stage and
   letting object-fit letterbox is what actually fits all three. */
.stage img { width: 100%; height: 100%; object-fit: contain; display: block; }
.miss { color: var(--text-muted); font-size: 13px; }
.flag {
  margin: 8px 10px 10px; padding: 7px 9px; border-radius: 6px;
  background: #fdf1ea; border: 1px solid #f3cdb6;
  color: #7c3b13; font-size: 12px;
}
.bars { margin-top: 14px; display: grid; gap: 2px; }
.bar { display: grid; grid-template-columns: 78px 1fr auto; align-items: center; gap: 10px; }
.bar .lbl { color: var(--text-secondary); font-size: 12px; text-align: right; }
.bar .track {
  display: block; height: 10px; background: var(--surface-0); border-radius: 5px;
}
.bar .fill { display: block; height: 10px; border-radius: 5px; }
.bar .val { font-size: 12px; color: var(--text-secondary); min-width: 92px; }
.caption { color: var(--text-muted); font-size: 12px; margin-top: 8px; }
footer { margin-top: 30px; color: var(--text-secondary); font-size: 13px; max-width: 80ch; }
footer h3 { font-size: 14px; margin: 0 0 6px; }
footer li { margin-bottom: 5px; }
table.summary { border-collapse: collapse; margin-top: 10px; font-size: 13px; }
table.summary th, table.summary td {
  border: 1px solid var(--line); padding: 6px 12px; text-align: right;
}
table.summary th:first-child, table.summary td:first-child { text-align: left; }
table.summary thead th { background: var(--surface-1); }
@media print {
  body { background: #fff; padding: 12mm; }
  .card { break-inside: avoid; }
}
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", type=Path,
                    default=REPO_ROOT / "out/baseline")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--scav", type=Path, default=None,
                    help="defaults to whichever built binary is in out/")
    args = ap.parse_args()
    out = args.out or (args.src / "poster.html")
    scav_bin = args.scav
    if scav_bin is None:
        built = sorted(REPO_ROOT.glob("out/*/bin/scav"))
        scav_bin = built[0] if built else Path("scav")

    charts = sorted(p for p in CORPUS.glob("*.scav"))
    if not charts:
        print("no corpus charts", file=sys.stderr)
        return 1

    head = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                          cwd=REPO_ROOT, capture_output=True, text=True)
    commit = head.stdout.strip() or "unknown"

    cards = []
    totals = {key: 0 for key, _, _ in ENGINES}
    aspects = {key: [] for key, _, _ in ENGINES}
    wins = {key: 0 for key, _, _ in ENGINES}
    flags = []

    for chart in charts:
        panels, bars, sizes = [], [], {}
        for key, label, color in ENGINES:
            svg_path = args.src / f"{chart.stem}.{key}.svg"
            if not svg_path.exists():
                panels.append(
                    f'<div class="panel"><div class="head">'
                    f'<span class="who"><span class="swatch" style="background:{color}"></span>'
                    f'{label}</span></div>'
                    f'<div class="stage"><span class="miss">not rendered</span></div></div>')
                continue
            svg = svg_path.read_text(encoding="utf-8")
            size = extent(svg)
            sizes[key] = size
            if size:
                totals[key] += size[0] * size[1]
                aspects[key].append(aspect(size))
            dim = f"{size[0]} x {size[1]}" if size else "size unstated"
            note = DEGRADED.get((chart.name, key))
            if note:
                flags.append((chart.name, label, note))
            panels.append(
                f'<div class="panel"><div class="head">'
                f'<span class="who"><span class="swatch" style="background:{color}"></span>'
                f'{label}</span><span class="dim">{dim}</span></div>'
                f'<div class="stage"><img alt="{chart.stem} through {label}" '
                f'src="{data_uri(svg)}"></div>'
                + (f'<div class="flag">degraded: {note}</div>' if note else "")
                + "</div>")

        areas = {k: (v[0] * v[1]) for k, v in sizes.items() if v}
        if areas:
            smallest = min(areas, key=lambda k: areas[k])
            wins[smallest] += 1
            widest = max(areas.values())
            for key, label, color in ENGINES:
                if key not in areas:
                    continue
                pct = areas[key] / widest
                bars.append(
                    f'<div class="bar"><span class="lbl">{label}</span>'
                    f'<span class="track"><span class="fill" '
                    f'style="width:{pct * 100:.1f}%;background:{color}"></span></span>'
                    f'<span class="val">{areas[key] / 1000:,.0f}k units&sup2;</span></div>')

        made = counts(chart, scav_bin)
        meta = (f"{made[0]} states &middot; {made[1]} transitions"
                if made else "&nbsp;")
        cards.append(
            f'<section class="card"><h2>{chart.name}</h2>'
            f'<div class="meta">{meta}</div>'
            f'<div class="panels">{"".join(panels)}</div>'
            f'<div class="bars">{"".join(bars)}</div>'
            f'<div class="caption">Drawing area, relative to the largest of the three. '
            f'Every panel is scaled to its own box, so these bars are where size lives.</div>'
            "</section>")

    tiles = []
    for key, label, color in ENGINES:
        ratio = sorted(aspects[key])
        median = ratio[len(ratio) // 2] if ratio else 0.0
        tiles.append(
            f'<div class="tile"><div class="k">'
            f'<span class="swatch" style="display:inline-block;background:{color}"></span> '
            f'{label}</div>'
            f'<div class="v">{median:.2f} : 1</div>'
            f'<div class="n">median aspect &middot; {totals[key] / 1e6:.1f}M units&sup2; total '
            f'&middot; smallest on {wins[key]} of {len(charts)}</div></div>')

    rows = "".join(
        f"<tr><td>{label}</td><td>{totals[key] / 1e6:.2f}M</td>"
        f"<td>{(sorted(aspects[key])[len(aspects[key]) // 2] if aspects[key] else 0):.2f}</td>"
        f"<td>{wins[key]}</td></tr>"
        for key, label, _ in ENGINES)

    page = f"""<!doctype html><html lang="en"><meta charset="utf-8">
<title>scav layout shootout</title><style>{STYLE}</style>
<body>
<h1>Statechart layout shootout</h1>
<p class="sub">Every chart in the transcribed corpus, drawn by scav and by the two
incumbents it exists to replace. Same model, same eleven files, three engines.
scav is at <code>{commit}</code>: layered ranks per submachine, Brandes &amp;
K&ouml;pf coordinates, and orthogonal routes from an A* over a per-frame
visibility graph.</p>
<div class="legend">{"".join(
    f'<span class="chip"><span class="swatch" style="background:{c}"></span>{l}</span>'
    for _, l, c in ENGINES)}</div>
<div class="tiles">{"".join(tiles)}</div>
{"".join(cards)}
<footer id="notes">
<h3>Reading this</h3>
<ul>
<li><b>Aspect is the headline.</b> elkjs draws ribbons, PlantUML draws columns,
scav sits between them &mdash; which matters because a diagram that is 8:1 fits
nothing.</li>
<li><b>Two PlantUML cells are degraded, not missing.</b> A reference into a
non-first concurrent region is rejected outright, so the harness anchors that
endpoint at its enclosing composite and says so. The arrow you see is not the
arrow the model holds.</li>
<li><b>What is still missing from scav is between the wires, not the wires.</b>
No chart here routes an edge through a box. Two things are unbuilt and both are
visible: nothing separates two edges that reach the same lane, so they draw as
one polyline that fans out at its ends, and nothing places a transition label
away from a state, so most of them land on a name.</li>
</ul>
<table class="summary"><thead><tr><th>engine</th><th>total area</th>
<th>median aspect</th><th>smallest of three</th></tr></thead>
<tbody>{rows}</tbody></table>
</footer>
</body></html>"""

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(page, encoding="utf-8")
    print(f"wrote {out} ({out.stat().st_size / 1024:.0f} KB)")
    for name, label, why in flags:
        print(f"  {name} {label}: {why}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
