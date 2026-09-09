#!/usr/bin/env python3
"""The two things a count cannot say, drawn instead.

`tools/review.py` says how many labels are detached and how many sit on a
corner. It cannot say whether the line drawn between "fine" and "wrong" is in
the right place, and it cannot say what the placer's choices even were. Both
are pictures, so this draws them (11.9, 11.9.3).

  strips      every position 11.9's strip matching considered for one label,
              the chosen one solid and the rest faint, the strips numbered
  gaps        real labels grouped by how far they sit from their own polyline,
              in text heights, so the threshold can be picked by looking

  tools/explain.py                    both sections, the corpus
  tools/explain.py --out FILE
"""

import argparse
import html
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import audit  # noqa: E402

STRIPS = 5
SVG_OPEN = re.compile(r'<svg([^>]*)viewBox="(-?[\d.]+) (-?[\d.]+) ([\d.]+) ([\d.]+)"([^>]*)>')
TEXT = re.compile(r'<text x="(-?\d+)" y="(-?\d+)" font-size="(\d+)"[^>]*textLength="(\d+)"[^>]*'
                  r'class="scav-(trans|state|sub) scav-id-(\d+)"[^>]*>([^<]*)<')


def crop(svg, box, extra=""):
    """The same document through a window, so a detail is legible without
    redrawing anything: only the viewBox and the declared size change."""
    x, y, w, h = box
    scale = 460.0 / max(w, 1)
    head = SVG_OPEN.sub(
        lambda m: f'<svg{m.group(1)}viewBox="{x} {y} {w} {h}"{m.group(6)}>', svg, count=1)
    head = re.sub(r'width="\d+" height="\d+"',
                  f'width="{int(w * scale)}" height="{int(h * scale)}"', head, count=1)
    return head.replace("</svg>", extra + "</svg>")


def legs_of(svg):
    out = {}
    for m in audit.POLYLINE.finditer(svg):
        pts = audit.points(m.group(1))
        out[m.group(2)] = list(zip(pts, pts[1:]))
    return out


def labels_of(svg):
    for m in TEXT.finditer(svg):
        x, y, size, length, which, ident, txt = m.groups()
        if which != "trans":
            continue
        x, y, size, length = int(x), int(y), int(size), int(length)
        yield ident, txt, (x, y - size, length, size), size


def candidates(leg, box_w, box_h):
    """Every position the placer enumerates for one leg, exactly as label.cpp
    walks it: two sides, `STRIPS` strips a box height apart, and slots one box
    height along the leg plus its far end and its centre."""
    (ax, ay), (bx, by) = leg
    flat = ay == by
    if flat == (ax == bx):
        return []
    step = max(box_h, 1)
    lo, hi = (min(ax, bx), max(ax, bx)) if flat else (min(ay, by), max(ay, by))
    out = []
    for side in range(2):
        for strip in range(STRIPS):
            off = strip * step
            slot = lo
            while slot <= hi + (2 * step):
                mid = slot
                if slot > hi + step:
                    mid = lo + (hi - lo) // 2
                elif slot > hi:
                    mid = hi
                if flat:
                    cx = mid - box_w // 2
                    cy = (ay - box_h) - off if side == 0 else ay + off
                else:
                    cx = (ax - box_w) - off if side == 0 else ax + off
                    cy = mid - box_h // 2
                out.append((cx, cy, box_w, box_h, side, strip))
                slot += step
    return out


def strip_figure(svg, leg, chosen, box_w, box_h, span):
    """The candidate set as SVG: faint outlines, the leg heavy, the chosen one
    solid, and a numeral per strip so `strip 0` is visibly on the line."""
    (ax, ay), (bx, by) = leg
    stroke = max(int(span / 700), 2)
    size = max(int(span / 40), 9)
    out = [f'<line x1="{ax}" y1="{ay}" x2="{bx}" y2="{by}" stroke="#c2185b" '
           f'stroke-width="{stroke * 3}" opacity="0.35"/>']
    seen = set()
    for cx, cy, w, h, side, strip in candidates(leg, box_w, box_h):
        out.append(f'<rect x="{cx}" y="{cy}" width="{w}" height="{h}" fill="none" '
                   f'stroke="#1565c0" stroke-width="{stroke}" opacity="0.30"/>')
        if (side, strip) not in seen:
            seen.add((side, strip))
            out.append(f'<text x="{cx - (size // 2)}" y="{cy + h}" font-size="{size}" '
                       f'fill="#1565c0" text-anchor="end" font-family="monospace" '
                       f'opacity="0.85">{strip}</text>')
    cx, cy, w, h = chosen
    out.append(f'<rect x="{cx}" y="{cy}" width="{w}" height="{h}" fill="none" '
               f'stroke="#b71c1c" stroke-width="{stroke * 3}"/>')
    return "".join(out)


HEAD = """<!doctype html><meta charset="utf-8"><title>scav: strips and gaps</title>
<style>
 body{font:13px/1.55 -apple-system,system-ui,sans-serif;margin:0;padding:26px;
      background:#f6f6f4;color:#1b1b1a;max-width:1500px}
 h1{font-size:20px;margin:0 0 4px} h2{font-size:16px;margin:28px 0 6px}
 p{max-width:62em;color:#333} .q{color:#a8321e;font-weight:600}
 .card{background:#fff;border:1px solid #e2e2de;border-radius:8px;padding:14px;
       margin:14px 0}
 .cap{font-family:ui-monospace,monospace;font-size:12px;color:#555;margin:0 0 8px}
 .row{display:flex;flex-wrap:wrap;gap:14px}
 .cell{background:#fff;border:1px solid #e2e2de;border-radius:8px;padding:10px}
 .cell .t{font-family:ui-monospace,monospace;font-size:12px;margin:0 0 6px}
 .band{font-weight:700;font-size:14px;margin:20px 0 2px;font-family:ui-monospace,monospace}
 svg{display:block;border:1px solid #ecece8;border-radius:4px;background:#fff}
</style>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None)
    ap.add_argument("--scav", default=None)
    args = ap.parse_args()
    scav_bin = audit.find_scav(args.scav)
    if scav_bin is None or not scav_bin.exists():
        print("no scav binary; build first", file=sys.stderr)
        return 1
    where = audit.REPO_ROOT / "out/review"
    where.mkdir(parents=True, exist_ok=True)
    page = Path(args.out) if args.out else (where / "strips-and-gaps.html")

    charts = {}
    for chart in sorted(audit.CORPUS.glob("*.scav")):
        svg_p = where / (chart.name + ".svg")
        if not svg_p.exists():
            subprocess.run([str(scav_bin), "render", "-o", str(svg_p), str(chart)],
                           check=False, capture_output=True)
        if svg_p.exists():
            charts[chart.name] = svg_p.read_text(encoding="utf-8")

    body = [HEAD, "<h1>Strips, and how far is too far</h1>"]

    # ---- section one: what a strip is ----
    body.append("<h2>1. What a strip is</h2><p>&sect;11.9 places a label by "
                "enumerating positions along each leg of its own route: "
                "<b>two sides</b>, <b>five strips</b> one box height apart, and a "
                "slot every box height along the leg plus its far end and its "
                "centre. Faint blue is every position considered; the numeral is "
                "the strip index; heavy red is the one chosen. <b>Strip 0 puts the "
                "box edge on the line</b> &mdash; so the strip index <i>is</i> the "
                "gap, in box heights.</p>"
                "<p class=\"q\">What this decides: a hard \"within N text heights\" "
                "rule admits strips 0..N and forbids the rest. At N=1 that is 2 of "
                "5 per side.</p>")
    shown = 0
    for name, svg in charts.items():
        legs = legs_of(svg)
        span = max(float(SVG_OPEN.search(svg).group(4)), float(SVG_OPEN.search(svg).group(5)))
        for ident, txt, em, size in labels_of(svg):
            mine = legs.get(ident, [])
            if not mine or shown >= 3:
                continue
            # The leg the box sits nearest, which is the one it rides.
            leg = min(mine, key=lambda ab: audit.gap(em, audit.span(*ab)))
            g = audit.gap(em, audit.span(*leg))
            if shown == 0 and g == 0:
                pass          # a slice, worth showing first
            elif shown == 1 and g == 0:
                continue
            pad = max(em[2], em[3]) * 3
            box = (min(em[0], leg[0][0], leg[1][0]) - pad,
                   min(em[1], leg[0][1], leg[1][1]) - pad,
                   abs(max(em[0] + em[2], leg[0][0], leg[1][0]) -
                       min(em[0], leg[0][0], leg[1][0])) + (2 * pad),
                   abs(max(em[1] + em[3], leg[0][1], leg[1][1]) -
                       min(em[1], leg[0][1], leg[1][1])) + (2 * pad))
            fig = strip_figure(svg, leg, (em[0], em[1], em[2], em[3]), em[2], em[3], span)
            body.append(f'<div class="card"><p class="cap">{html.escape(name)} '
                        f'&middot; {html.escape(txt)!r} &middot; gap to its own leg '
                        f'<b>{g}</b> units = {g / max(size, 1):.1f} text heights'
                        f'{" &mdash; sliced" if g == 0 else ""}</p>'
                        f'{crop(svg, box, fig)}</div>')
            shown += 1

    # ---- section two: the gap gallery ----
    body.append("<h2>2. How far is too far</h2><p>Every corpus label, grouped by "
                "its gap to its own polyline in text heights. Each window shows the "
                "label and the leg it belongs to and nothing else. "
                "<span class=\"q\">Pick the band where these stop being "
                "acceptable.</span></p>")
    bands = [("0 &mdash; the line runs through the label", 0, 0),
             ("0 to 1 text height", 0, 1), ("1 to 2", 1, 2),
             ("2 to 3", 2, 3), ("3 to 5", 3, 5), ("more than 5", 5, 10 ** 9)]
    buckets = {i: [] for i in range(len(bands))}
    for name, svg in charts.items():
        legs = legs_of(svg)
        for ident, txt, em, size in labels_of(svg):
            mine = legs.get(ident, [])
            if not mine:
                continue
            g = min(audit.gap(em, audit.span(*ab)) for ab in mine)
            h = g / max(size, 1)
            for i, (_, lo, hi) in enumerate(bands):
                if (i == 0 and g == 0) or (i > 0 and lo < h <= hi):
                    buckets[i].append((name, txt, em, size, g, h, legs[ident]))
                    break
    for i, (title, _, _) in enumerate(bands):
        got = buckets[i]
        body.append(f'<p class="band">{title} &mdash; {len(got)} labels</p>')
        if not got:
            body.append("<p>none</p>")
            continue
        body.append('<div class="row">')
        for name, txt, em, size, g, h, mine in got[:4]:
            leg = min(mine, key=lambda ab: audit.gap(em, audit.span(*ab)))
            pad = max(em[2], em[3]) * 2
            xs = [em[0], em[0] + em[2], leg[0][0], leg[1][0]]
            ys = [em[1], em[1] + em[3], leg[0][1], leg[1][1]]
            box = (min(xs) - pad, min(ys) - pad,
                   (max(xs) - min(xs)) + (2 * pad), (max(ys) - min(ys)) + (2 * pad))
            svgc = charts[name]
            body.append(f'<div class="cell"><p class="t">{html.escape(txt)!r} '
                        f'&middot; {h:.1f} heights &middot; {html.escape(name)}</p>'
                        f'{crop(svgc, box)}</div>')
        body.append("</div>")

    page.write_text("\n".join(body), encoding="utf-8")
    print(f"wrote {page}")
    for i, (title, _, _) in enumerate(bands):
        print(f"  {re.sub('&[a-z]+;', '-', title):<44}{len(buckets[i]):>4}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
