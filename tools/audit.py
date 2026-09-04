#!/usr/bin/env python3
"""What is wrong with the picture, counted rather than looked at.

Reads the rendered SVG back and checks the properties a reader notices, so a
regression is a number that moved. A tool, not a test: the defect classes are
the ones actually reported, and several are known-open work with an owner in
the PRD, so it reports rather than fails.

  tools/audit.py                    every corpus chart, out/baseline
  tools/audit.py --in DIR
  tools/audit.py --chart vac.scav   one of them, with each finding listed
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS = REPO_ROOT / "test_data/charts"

# State boxes come from the geometry columns, not the drawing: a choice is a
# polygon and a final state two circles, so reading rects back exempts them.
POLYLINE = re.compile(r'<polyline points="([^"]+)"[^>]*class="scav-trans scav-id-(\d+)"')
ARROWHEAD = re.compile(r'<polygon points="([^"]+)"[^>]*class="scav-trans scav-id-(\d+)"')
DIVIDER = re.compile(
    r'<line x1="(-?\d+)" y1="(-?\d+)" x2="(-?\d+)" y2="(-?\d+)"[^>]*class="scav-sub')
TEXT = re.compile(
    r'<text x="(-?\d+)" y="(-?\d+)"[^>]*textLength="(\d+)"[^>]*'
    r'class="scav-(trans|state) scav-id-(\d+)"[^>]*>')


def points(text):
    return [tuple(int(v) for v in p.split(",")) for p in text.split()]


def on_border(pt, box):
    x, y, w, h = box
    return ((pt[0] in (x, x + w) and y <= pt[1] <= y + h) or
            (pt[1] in (y, y + h) and x <= pt[0] <= x + w))


def inside(pt, box):
    x, y, w, h = box
    return x < pt[0] < x + w and y < pt[1] < y + h


def flush(a, b, box):
    """The segment runs along one of the box's own edges."""
    x, y, w, h = box
    if a[1] == b[1]:
        return a[1] in (y, y + h) and min(a[0], b[0]) < x + w and max(a[0], b[0]) > x
    if a[0] == b[0]:
        return a[0] in (x, x + w) and min(a[1], b[1]) < y + h and max(a[1], b[1]) > y
    return False


def overlap(a, b, c, d):
    """Two collinear segments' shared length, or 0. Routes sharing a run read as
    one polyline that fans out at the ends; 11.5's nudging separates them."""
    if a[1] == b[1] == c[1] == d[1]:
        i, j = sorted((a[0], b[0]))
        k, l = sorted((c[0], d[0]))
    elif a[0] == b[0] == c[0] == d[0]:
        i, j = sorted((a[1], b[1]))
        k, l = sorted((c[1], d[1]))
    else:
        return 0
    return max(0, min(j, l) - max(i, k))


def geometry(chart, scav_bin):
    out = subprocess.run([str(scav_bin), "dump", "--layout", "--json", str(chart)],
                         capture_output=True, text=True, check=True).stdout
    doc = json.loads(out)
    live = [i for i, st in enumerate(doc["states"]) if st["live"]]
    rects = doc["geometry"]["state"]
    boxes = [tuple(rects[i]) for i in live if rects[i][2] and rects[i][3]]
    return boxes, tuple(doc["geometry"]["chart"])


def audit(svg, every, chart, verbose):
    found = {}
    notes = []

    def note(kind, detail):
        found[kind] = found.get(kind, 0) + 1
        if verbose:
            notes.append(f"    {kind}: {detail}")

    cx, cy, cw, ch = chart
    legs = []
    for m in POLYLINE.finditer(svg):
        pts, trans = points(m.group(1)), m.group(2)
        found["route segments"] = found.get("route segments", 0) + len(pts) - 1
        # The tail is the arrowhead's business; this is the other end, which
        # has no glyph of its own to give it away.
        found["route starts"] = found.get("route starts", 0) + 1
        if not any(on_border(pts[0], box) for box in every):
            note("route start not on any border", f"t{trans} at {pts[0]}")
        for pt in pts:
            if not (cx <= pt[0] <= cx + cw and cy <= pt[1] <= cy + ch):
                note("drawn outside the chart rect", f"t{trans} point {pt}")
        for a, b in zip(pts, pts[1:]):
            legs.append((a, b, trans))
            if a[0] != b[0] and a[1] != b[1]:
                note("segment not axis-aligned", f"t{trans} {a}-{b}")
            for box in every:
                if flush(a, b, box):
                    note("segment flush along a box", f"t{trans} {a}-{b} box {box}")

    for i, (a, b, t1) in enumerate(legs):
        for c, d, t2 in legs[i + 1:]:
            if t1 == t2:
                continue
            shared = overlap(a, b, c, d)
            if shared:
                found["overlapped units"] = found.get("overlapped units", 0) + shared
                note("routes share a run", f"t{t1}/t{t2} {a}-{b} over {shared}")

    for m in ARROWHEAD.finditer(svg):
        tip, trans = points(m.group(1))[0], m.group(2)
        found["arrowheads"] = found.get("arrowheads", 0) + 1
        if not any(on_border(tip, box) for box in every):
            note("arrowhead not on any border", f"t{trans} tip {tip}")

    for m in DIVIDER.finditer(svg):
        x1, y1, x2, y2 = (int(v) for v in m.groups())
        found["region dividers"] = found.get("region dividers", 0) + 1
        if x1 != x2 and y1 != y2:
            note("divider not axis-aligned", f"({x1},{y1})-({x2},{y2})")

    # A transition label overlapping a state box is the label placement 11.9
    # calls strip matching and P7d owns; counted so its arrival is measurable.
    for m in TEXT.finditer(svg):
        x, y, length, which, ident = m.groups()
        if which != "trans":
            continue
        x, y, length = int(x), int(y), int(length)
        found["transition labels"] = found.get("transition labels", 0) + 1
        if not (cx <= x and x + length <= cx + cw and cy <= y <= cy + ch):
            note("drawn outside the chart rect", f"t{ident} label at ({x},{y})")
        for box in every:
            bx, by, bw, bh = box
            if x < bx + bw and x + length > bx and by < y < by + bh:
                note("label over a state box", f"t{ident} at ({x},{y})+{length}")
                break

    return found, notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="where", default=str(REPO_ROOT / "out/baseline"))
    ap.add_argument("--chart", default=None)
    ap.add_argument("--scav", default=None)
    args = ap.parse_args()
    scav_bin = Path(args.scav) if args.scav else (
        REPO_ROOT / "out/macos-clang-libcxx-release/bin/scav")
    if not scav_bin.exists():
        found = sorted(REPO_ROOT.glob("out/*/bin/scav"))
        if not found:
            print("no scav binary; build first", file=sys.stderr)
            return 1
        scav_bin = found[0]

    where = Path(args.where)
    names = ([args.chart] if args.chart else
             sorted(p.name for p in CORPUS.glob("*.scav")))
    verbose = args.chart is not None

    total = {}
    missing = []
    for name in names:
        svg = where / f"{name}.svg" if name.endswith(".svg") else where / f"{name}.svg"
        svg = where / (name + ".svg")
        if not svg.exists():
            missing.append(name)
            continue
        every, chart = geometry(CORPUS / name, scav_bin)
        found, notes = audit(svg.read_text(encoding="utf-8"), every, chart, verbose)
        for key, count in found.items():
            total[key] = total.get(key, 0) + count
        if verbose and notes:
            print(f"{name}:")
            print("\n".join(notes))

    if missing:
        print(f"not rendered ({len(missing)}): run tools/baseline.py first",
              file=sys.stderr)
        if len(missing) == len(names):
            return 1

    # Counts first, then the findings, so the ratio is visible.
    scale = {"segment not axis-aligned": "route segments",
             "segment flush along a box": "route segments",
             "route start not on any border": "route starts",
             "arrowhead not on any border": "arrowheads",
             "divider not axis-aligned": "region dividers",
             "drawn outside the chart rect": "route segments",
             "routes share a run": "route segments",
             "label over a state box": "transition labels"}
    for key in ("route segments", "route starts", "arrowheads", "region dividers",
                "transition labels"):
        print(f"{key:<28} {total.get(key, 0)}")
    print()
    clean = True
    for key, over in scale.items():
        count = total.get(key, 0)
        if count:
            clean = False
        print(f"  {key:<28} {count} of {total.get(over, 0)}")
    if clean:
        print("\nnothing this audit knows how to see.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
