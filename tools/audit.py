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
    r'<text x="(-?\d+)" y="(-?\d+)" font-size="(\d+)"[^>]*textLength="(\d+)"[^>]*'
    r'class="scav-(trans|state|sub) scav-id-(\d+)"[^>]*>')

# A mark and the glyph it belongs to carry the same id, which is what lets the
# two be checked against each other without knowing the state's kind.
CIRCLE = re.compile(
    r'<circle cx="(-?\d+)" cy="(-?\d+)" r="(\d+)"[^>]*class="scav-state scav-id-(\d+)"')


def points(text):
    return [tuple(int(v) for v in p.split(",")) for p in text.split()]


def on_border(pt, box):
    x, y, w, h = box
    return ((pt[0] in (x, x + w) and y <= pt[1] <= y + h) or
            (pt[1] in (y, y + h) and x <= pt[0] <= x + w))


def inside(pt, box):
    x, y, w, h = box
    return x < pt[0] < x + w and y < pt[1] < y + h


def overlaps(a, b):
    """Two rects sharing area. Touching is not overlapping: adjacent boxes are
    what packing produces, and adjacent glyphs read fine."""
    return (a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and
            a[1] < b[1] + b[3] and b[1] < a[1] + a[3])


def span(a, b):
    """A segment's bounding box: zero thickness when the segment is axis-aligned,
    so `overlaps` against it is the strict interior test."""
    return (min(a[0], b[0]), min(a[1], b[1]), abs(a[0] - b[0]), abs(a[1] - b[1]))


def flush(a, b, box):
    """The segment runs along one of the box's own edges."""
    x, y, w, h = box
    if a[1] == b[1]:
        return a[1] in (y, y + h) and min(a[0], b[0]) < x + w and max(a[0], b[0]) > x
    if a[0] == b[0]:
        return a[0] in (x, x + w) and min(a[1], b[1]) < y + h and max(a[1], b[1]) > y
    return False


def gap(a, b):
    """The Chebyshev gap between two rects: zero on an axis they overlap or touch
    on, the larger of the two separations otherwise. Layout's own predicate."""
    dx = max(b[0] - (a[0] + a[2]), a[0] - (b[0] + b[2]), 0)
    dy = max(b[1] - (a[1] + a[3]), a[1] - (b[1] + b[3]), 0)
    return max(dx, dy)


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


def trunks(a, b):
    """Which segments of two routes lie in a run they end -- or begin -- as one:
    the common suffix and prefix, plus each merge leg where the two legs reaching
    that run lie along it. 11.5 keeps these together and 11.6 charges neither."""
    n = min(len(a), len(b))
    tail = 0
    while tail < n and a[len(a) - 1 - tail] == b[len(b) - 1 - tail]:
        tail += 1
    head = 0
    while head < n and a[head] == b[head]:
        head += 1
    at = set(range(len(a) - tail, len(a) - 1)) | set(range(max(0, head - 1)))
    bt = set(range(len(b) - tail, len(b) - 1)) | set(range(max(0, head - 1)))
    i, j = len(a) - tail - 1, len(b) - tail - 1
    if tail and i >= 0 and j >= 0 and overlap(a[i], a[i + 1], b[j], b[j + 1]):
        at.add(i)
        bt.add(j)
    k = head - 1
    if head and k + 1 < len(a) and k + 1 < len(b) and \
            overlap(a[k], a[k + 1], b[k], b[k + 1]):
        at.add(k)
        bt.add(k)
    return at, bt


def enclosing(doc, state):
    """`state` and every state whose box contains it. A parent is a submachine
    id, and that submachine's owner is the state one level up."""
    seen = []
    at = state
    while at is not None and at not in seen:
        seen.append(at)
        parent = doc["states"][at]["parent"]
        at = None if parent is None else doc["submachines"][parent]["owner"]
    return set(seen)


def geometry(chart, scav_bin):
    out = subprocess.run([str(scav_bin), "dump", "--layout", "--json", str(chart)],
                         capture_output=True, text=True, check=True).stdout
    doc = json.loads(out)
    live = [i for i, st in enumerate(doc["states"]) if st["live"]]
    rects = doc["geometry"]["state"]
    boxes = [tuple(rects[i]) for i in live if rects[i][2] and rects[i][3]]
    return boxes, tuple(doc["geometry"]["chart"]), doc


def audit(svg, every, chart, doc, verbose):
    found = {}
    notes = []

    def note(kind, detail):
        found[kind] = found.get(kind, 0) + 1
        if verbose:
            notes.append(f"    {kind}: {detail}")

    cx, cy, cw, ch = chart
    legs = []
    route = {}
    starts = []
    tips = []
    for m in POLYLINE.finditer(svg):
        pts, trans = points(m.group(1)), m.group(2)
        route[trans] = pts
        found["route segments"] = found.get("route segments", 0) + len(pts) - 1
        # The tail is the arrowhead's business; this is the other end, which
        # has no glyph of its own to give it away.
        found["route starts"] = found.get("route starts", 0) + 1
        starts.append((pts[0], trans))
        if not any(on_border(pts[0], box) for box in every):
            note("route start not on any border", f"t{trans} at {pts[0]}")
        for pt in pts:
            if not (cx <= pt[0] <= cx + cw and cy <= pt[1] <= cy + ch):
                note("drawn outside the chart rect", f"t{trans} point {pt}")
        for k, (a, b) in enumerate(zip(pts, pts[1:])):
            legs.append((a, b, trans, k))
            if a[0] != b[0] and a[1] != b[1]:
                note("segment not axis-aligned", f"t{trans} {a}-{b}")
            for box in every:
                if flush(a, b, box):
                    note("segment flush along a box", f"t{trans} {a}-{b} box {box}")

    merged = {}
    for i, (a, b, t1, ka) in enumerate(legs):
        for c, d, t2, kb in legs[i + 1:]:
            if t1 == t2:
                continue
            shared = overlap(a, b, c, d)
            if not shared:
                continue
            if (t1, t2) not in merged:
                merged[(t1, t2)] = trunks(route[t1], route[t2])
            at, bt = merged[(t1, t2)]
            if ka in at and kb in bt:
                found["merged trunks"] = found.get("merged trunks", 0) + 1
                found["merged trunk units"] = (found.get("merged trunk units", 0) +
                                               shared)
                continue
            found["overlapped units"] = found.get("overlapped units", 0) + shared
            note("routes share a run", f"t{t1}/t{t2} {a}-{b} over {shared}")

    for m in ARROWHEAD.finditer(svg):
        tip, trans = points(m.group(1))[0], m.group(2)
        found["arrowheads"] = found.get("arrowheads", 0) + 1
        tips.append((tip, trans))
        if not any(on_border(tip, box) for box in every):
            note("arrowhead not on any border", f"t{trans} tip {tip}")

    # A head and a departure on one point of one box: the head is inked over the
    # other route's own first leg, so it reads as belonging to the line it sits
    # on. Two arrivals sharing a point are a fan-in and keep their one head
    # (11.5's bundles); this is the mixed case, which no trunk explains. A fork
    # bar is where it shows, every branch off one face having been handed the
    # box's centre before 11.5's attachment projected them apart.
    for tip, head in tips:
        for start, leaving in starts:
            if head != leaving and tip == start:
                note("an arrowhead over another route's end",
                     f"t{head} head on t{leaving} at {tip}")

    for m in DIVIDER.finditer(svg):
        x1, y1, x2, y2 = (int(v) for v in m.groups())
        found["region dividers"] = found.get("region dividers", 0) + 1
        if x1 != x2 and y1 != y2:
            note("divider not axis-aligned", f"({x1},{y1})-({x2},{y2})")

    # A transition label overlapping a state box is the label placement 11.9
    # calls strip matching and P7d owns; counted so its arrival is measurable.
    # `y` is a baseline and `textLength` the advance sum, so one font size up from
    # the baseline is the em box the builder reserved -- the same rectangle it
    # measured with, which is what makes these comparable to the geometry.
    live = [i for i, st in enumerate(doc["states"]) if st["live"]]
    rects = doc["geometry"]["state"]
    bands = (doc["geometry"]["state_before"], doc["geometry"]["state_after"])
    inked = []
    for m in TEXT.finditer(svg):
        x, y, size, length, which, ident = m.groups()
        x, y, size, length = int(x), int(y), int(size), int(length)
        em = (x, y - size, length, size)
        inked.append((em, which, ident))
        if which != "trans":
            continue
        found["transition labels"] = found.get("transition labels", 0) + 1
        if not (cx <= x and x + length <= cx + cw and cy <= y <= cy + ch):
            note("drawn outside the chart rect", f"t{ident} label at ({x},{y})")

        def struck(rect):
            bx, by, bw, bh = rect
            return bw and bh and x < bx + bw and x + length > bx and by < y < by + bh

        # The composite a transition runs inside encloses its own label, so only
        # the text bands that composite reserved are out of bounds for it (11.6).
        # A transition with no route is the exception: nothing placed its label,
        # and the builder draws it in the band its source reserved for exactly it.
        edge = doc["transitions"][int(ident)]
        under = enclosing(doc, edge["src"]) | enclosing(doc, edge["dst"])
        own_band = edge["src"] if not doc["geometry"]["route"][int(ident)] else None
        for i in live:
            hit = (any(struck(band[i]) for j, band in enumerate(bands)
                       if not (j == 1 and i == own_band)) if i in under
                   else struck(rects[i]))
            if hit:
                note("label over a state box", f"t{ident} at ({x},{y})+{length}")
                break
        for a, b, other, _ in legs:
            if other != ident and overlaps(em, span(a, b)):
                note("label over another route", f"t{ident} over t{other} {a}-{b}")
                break

        # A reader ties a label to the nearest line, so a box that is not nearer
        # its own route than every other by a line of its own text reads as
        # somebody else's -- 11.6's `label_near`, counted where it is inked. The
        # em box is shorter than the line box layout reserved, so this is the
        # conservative count of the two.
        mine = [gap(em, span(a, b)) for a, b, other, _ in legs if other == ident]
        theirs = [gap(em, span(a, b)) for a, b, other, _ in legs if other != ident]
        if mine and theirs and min(mine) + size > min(theirs):
            note("label nearer another route than its own",
                 f"t{ident} own {min(mine)} other {min(theirs)}")

    # Two strings inked into the same place read as one unreadable string, which
    # is a different defect from a label over a box: nudging moves the routes a
    # label follows, but nothing moves two labels apart. 11.9's strip matching.
    found["texts"] = found.get("texts", 0) + len(inked)
    for i, (a_box, a_which, a_id) in enumerate(inked):
        for b_box, b_which, b_id in inked[i + 1:]:
            if overlaps(a_box, b_box):
                note("texts overprint each other",
                     f"{a_which} {a_id} over {b_which} {b_id} at {a_box[:2]}")

    # A mark drawn past the glyph that holds it. The circle and its text share an
    # id, and the em box's worst corner against the radius is the whole check --
    # `kind_min_*` sizes the glyph and knows nothing about what goes in it.
    glyph = {}
    for m in CIRCLE.finditer(svg):
        gx, gy, r, ident = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
        if r > glyph.get(ident, (0, 0, -1))[2]:
            glyph[ident] = (gx, gy, r)
    for box, which, ident in inked:
        if which != "state" or ident not in glyph:
            continue
        gx, gy, r = glyph[ident]
        found["marks in a glyph"] = found.get("marks in a glyph", 0) + 1
        bx, by, bw, bh = box
        worst = max((px - gx) ** 2 + (py - gy) ** 2
                    for px in (bx, bx + bw) for py in (by, by + bh))
        if worst > r * r:
            note("mark outside its glyph", f"state {ident} in r={r} at {box}")

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
        every, chart, doc = geometry(CORPUS / name, scav_bin)
        found, notes = audit(svg.read_text(encoding="utf-8"), every, chart, doc,
                             verbose)
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
             "an arrowhead over another route's end": "arrowheads",
             "divider not axis-aligned": "region dividers",
             "drawn outside the chart rect": "route segments",
             "routes share a run": "route segments",
             "label over a state box": "transition labels",
             "label over another route": "transition labels",
             "label nearer another route than its own": "transition labels",
             "texts overprint each other": "texts",
             "mark outside its glyph": "marks in a glyph"}
    for key in ("route segments", "route starts", "arrowheads", "region dividers",
                "transition labels", "texts", "marks in a glyph"):
        print(f"{key:<40} {total.get(key, 0)}")
    print()
    # Not a ratio, so it sits outside the block below: the shared run's extent is
    # what nudging has to pay back, and the pair count alone hides which chart owes.
    print(f"  {'shared run, grid units':<40} {total.get('overlapped units', 0)}")
    # The run two routes end or begin as one, which is a fan-in rather than a
    # lane and is left alone by both nudging and the corridor term.
    print(f"  {'merged trunks':<40} {total.get('merged trunks', 0)}")
    print(f"  {'merged trunk, grid units':<40} {total.get('merged trunk units', 0)}")
    clean = True
    for key, over in scale.items():
        count = total.get(key, 0)
        if count:
            clean = False
        print(f"  {key:<40} {count} of {total.get(over, 0)}")
    if clean:
        print("\nnothing this audit knows how to see.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
