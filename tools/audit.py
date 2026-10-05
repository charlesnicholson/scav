#!/usr/bin/env python3
"""Counts reader-visible defects in rendered SVGs against the layout dump, per class.
Exits 1 when the scav binary or every rendered SVG is missing, else 0.

  tools/audit.py                    every corpus chart, out/baseline
  tools/audit.py --in DIR
  tools/audit.py --chart vac.scav   one of them, with each finding listed
  tools/audit.py --gauntlet         the element suite instead of the corpus
  tools/audit.py --portfolio-row 4  one portfolio row instead of the pick
  tools/audit.py --json             the counts as JSON, per chart and totalled
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS = REPO_ROOT / "test_data/charts"
# Element-suite charts; tools/baseline.py --gauntlet renders them to out/baseline.
# Counts use measured text, unlike gauntlet_tests.cpp; never compare the two.
GAUNTLET = CORPUS / "gauntlet"

# State boxes come from the layout dump; these patterns read the SVG's other marks.
POLYLINE = re.compile(r'<polyline points="([^"]+)"[^>]*class="scav-trans scav-id-(\d+)"')
ARROWHEAD = re.compile(r'<polygon points="([^"]+)"[^>]*class="scav-trans scav-id-(\d+)"')
DIVIDER = re.compile(
    r'<line x1="(-?\d+)" y1="(-?\d+)" x2="(-?\d+)" y2="(-?\d+)"[^>]*class="scav-sub')
# A region divider line with its stroke width.
DIVIDER_INK = re.compile(
    r'<line x1="(-?\d+)" y1="(-?\d+)" x2="(-?\d+)" y2="(-?\d+)"[^>]*'
    r'stroke-width="(\d+)"[^>]*class="scav-sub')
TEXT = re.compile(
    r'<text x="(-?\d+)" y="(-?\d+)" font-size="(\d+)"[^>]*textLength="(\d+)"[^>]*'
    r'class="scav-(trans|state|sub) scav-id-(\d+)"[^>]*>')

# A state's circle glyph; it shares its `scav-id` with the state's mark text.
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
    """True when two (x, y, w, h) rects share positive area."""
    return (a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and
            a[1] < b[1] + b[3] and b[1] < a[1] + a[3])


def ink_span(a, b, width):
    """A segment's bounding box grown by half the stroke width on every side."""
    half = width // 2
    return (min(a[0], b[0]) - half, min(a[1], b[1]) - half,
            abs(a[0] - b[0]) + (2 * half), abs(a[1] - b[1]) + (2 * half))


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


def runs_along(a, b, box, near):
    """The segment runs parallel to one of the box's edges within `near` of it, on either
    side, over a positive length of that edge."""
    x, y, w, h = box
    if a[1] == b[1] and a[0] != b[0]:
        return (min(abs(a[1] - y), abs(a[1] - (y + h))) <= near and
                min(a[0], b[0]) < x + w and max(a[0], b[0]) > x)
    if a[0] == b[0] and a[1] != b[1]:
        return (min(abs(a[0] - x), abs(a[0] - (x + w))) <= near and
                min(a[1], b[1]) < y + h and max(a[1], b[1]) > y)
    return False


def gap(a, b):
    """The Chebyshev gap between two rects: zero on an axis they overlap or touch
    on, the larger of the two separations otherwise. Layout's own predicate."""
    dx = max(b[0] - (a[0] + a[2]), a[0] - (b[0] + b[2]), 0)
    dy = max(b[1] - (a[1] + a[3]), a[1] - (b[1] + b[3]), 0)
    return max(dx, dy)


def overlap(a, b, c, d):
    """Shared length of two collinear axis-aligned segments, or 0."""
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
    """Segment indices of `a` and of `b` in the routes' common point prefix and suffix,
    plus the leg next to each when the two routes' legs there overlap.
    """
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
    """The set of `state`'s ancestors, via each parent submachine's owner."""
    seen = []
    parent = doc["states"][state]["parent"]
    at = None if parent is None else doc["submachines"][parent]["owner"]
    while at is not None and at not in seen:
        seen.append(at)
        parent = doc["states"][at]["parent"]
        at = None if parent is None else doc["submachines"][parent]["owner"]
    return set(seen)


def corner_arc(doc, state):
    """Corner-arc radius of `state`, as the dump's `state_corner` gives it."""
    return doc["geometry"]["state_corner"][state]


# Per face (0 left, 1 right, 2 top, 3 bottom): the band lining it and its `ruled` bit.
BAND = {0: ("state_lead", 2), 1: ("state_trail", 3), 2: ("state_before", 0), 3: ("state_after", 1)}


def on_exit(pt, doc, state):
    """`pt` lies on a drawn edge of the face `state`'s loop room exits by, as the dump's
    `state_loop_place` and `state_ruled` give it: the border where no band lines that face,
    a ruled band's inner edge where one does."""
    g = doc["geometry"]
    face = g["state_loop_place"][state][0]
    bx, by, bw, bh = g["state"][state]
    key, bit = BAND[face]
    band = g[key][state]
    vertical = face < 2
    if band[2 if vertical else 3] > 0:
        if not g["state_ruled"][state] >> bit & 1:
            return False
        edge = [band[0] + band[2], band[0], band[1] + band[3], band[1]][face]
    else:
        edge = [bx, bx + bw, by, by + bh][face]
    if vertical:
        return pt[0] == edge and by <= pt[1] <= by + bh
    return pt[1] == edge and bx <= pt[0] <= bx + bw


def inner_loop(doc, trans):
    """The state an internal or local self-transition is drawn inside, else None."""
    edge = doc["transitions"][int(trans)]
    if edge["src"] != edge["dst"] or edge["kind"] not in ("internal", "local"):
        return None
    return edge["src"]


def geometry(chart, scav_bin, row=None):
    """Live non-empty state boxes, chart rect and layout dump of one candidate.
    `row` pins a portfolio row and must match the row the SVG was rendered at.
    """
    cmd = [str(scav_bin), "dump", "--layout", "--json"]
    if row is not None:
        cmd += ["--portfolio-row", str(row)]
    out = subprocess.run(cmd + [str(chart)],
                         capture_output=True, text=True, check=True).stdout
    doc = json.loads(out)
    live = [i for i, st in enumerate(doc["states"]) if st["live"]]
    rects = doc["geometry"]["state"]
    boxes = [tuple(rects[i]) for i in live if rects[i][2] and rects[i][3]]
    return boxes, tuple(doc["geometry"]["chart"]), doc


def audit(svg, every, chart, doc, verbose):
    found = {}
    notes = []
    # Finding locations as (kind, rect, detail) in grid units; a finding without `at`
    # is counted with no mark.
    marks = []

    def note(kind, detail, at=None):
        found[kind] = found.get(kind, 0) + 1
        if at is not None:
            marks.append((kind, tuple(at), detail))
        if verbose:
            notes.append(f"    {kind}: {detail}")

    cx, cy, cw, ch = chart
    # A route keeps `border_band`, one pad, from a state border it runs along.
    band = doc["geometry"]["profile"]["border_band"]
    live = [i for i, st in enumerate(doc["states"]) if st["live"]]
    rects = doc["geometry"]["state"]
    bands = tuple(doc["geometry"][k] for k in ("state_before", "state_after", "state_lead",
                                                "state_trail"))
    legs = []
    route = {}
    starts = []
    tips = []

    # A route end on a state border, or an inner loop's end on its exit boundary.
    def attached(pt, trans):
        if any(on_border(pt, box) for box in every):
            return True
        state = inner_loop(doc, trans)
        pts = route.get(trans, [])
        return state is not None and len(pts) > 1 and on_exit(pt, doc, state)

    for m in POLYLINE.finditer(svg):
        pts, trans = points(m.group(1)), m.group(2)
        route[trans] = pts
        found["route segments"] = found.get("route segments", 0) + len(pts) - 1
        found["route starts"] = found.get("route starts", 0) + 1
        starts.append((pts[0], trans))
        if not attached(pts[0], trans):
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
            near = [box for box in every if runs_along(a, b, box, band - 1)]
            if near:
                note("segment nearer than pad to a border it runs along",
                     f"t{trans} {a}-{b} box {near[0]}, pad {band}")

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
        if not attached(tip, trans):
            note("arrowhead not on any border", f"t{trans} tip {tip}")

    # Counts route ends on state borders and flags those within a corner arc.
    for pt, trans in starts + tips:
        for i in live:
            bx, by, bw, bh = rects[i]
            if not (bw and bh) or not on_border(pt, (bx, by, bw, bh)):
                continue
            found["state attachments"] = found.get("state attachments", 0) + 1
            r = corner_arc(doc, i)
            if not r:
                break
            vertical = pt[0] in (bx, bx + bw)
            along, lo, length = ((pt[1], by, bh) if vertical else (pt[0], bx, bw))
            into = max(lo + r - along, along - (lo + length - r))
            if into > 0:
                note("attachment on a drawn corner",
                     f"{into} into r={r}",
                     (pt[0] - r, pt[1] - r, 2 * r, 2 * r))
            break

    # Flags an arrowhead tip at the same point as another transition's route start.
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

    inked = []
    boundaries = [ink_span((int(a), int(b)), (int(c), int(d)), int(w))
                  for a, b, c, d, w in DIVIDER_INK.findall(svg)]

    # Flags parallel legs of two routes that overlap lengthwise and lie closer than the
    # smallest font size, excluding collinear legs.
    heights = [int(m.group(3)) for m in TEXT.finditer(svg)]
    line = min(heights) if heights else 0
    for i, (a, b, t1, _) in enumerate(legs):
        for c, d, t2, _ in legs[i + 1:]:
            if t1 == t2 or not line:
                continue
            flat = (a[1] == b[1]) and (c[1] == d[1])
            up = (a[0] == b[0]) and (c[0] == d[0])
            if not (flat or up):
                continue
            if flat:
                apart = abs(a[1] - c[1])
                along = min(max(a[0], b[0]), max(c[0], d[0])) - \
                    max(min(a[0], b[0]), min(c[0], d[0]))
            else:
                apart = abs(a[0] - c[0])
                along = min(max(a[1], b[1]), max(c[1], d[1])) - \
                    max(min(a[1], b[1]), min(c[1], d[1]))
            if (0 < apart < line) and (along > 0):
                note("lanes closer than one line of text",
                     f"t{t1}/t{t2} {apart} apart over {along}, one line is {line}",
                     ink_span(a, b, apart))

    # Flags a placed label box farther than `label_leader` from every leg of its route.
    leader = doc["geometry"].get("label_leader")
    for i, rect in enumerate(doc["geometry"].get("placed") or []):
        subjects = doc["geometry"].get("placed_subject") or []
        if leader is None or i >= len(subjects):
            continue
        t = subjects[i]
        pts = doc["geometry"]["route"][t] if t < len(doc["geometry"]["route"]) else []
        pairs = list(zip(pts, pts[1:]))
        if not pairs:
            continue
        found["placed label boxes"] = found.get("placed label boxes", 0) + 1
        near = min(gap(tuple(rect), span(tuple(a), tuple(b))) for a, b in pairs)
        if near > leader:
            note("label not anchored to its own polyline",
                 f"t{t} box is {near} from its nearest own leg, leader {leader}",
                 tuple(rect))

    for m in TEXT.finditer(svg):
        x, y, size, length, which, ident = m.groups()
        x, y, size, length = int(x), int(y), int(size), int(length)
        em = (x, y - size, length, size)  # one font size above the baseline `y`
        inked.append((em, which, ident))
        if which != "trans":
            continue
        found["transition labels"] = found.get("transition labels", 0) + 1
        if not (cx <= x and x + length <= cx + cw and cy <= y <= cy + ch):
            note("drawn outside the chart rect", f"t{ident} label at ({x},{y})")

        def struck(rect):
            bx, by, bw, bh = rect
            return bw and bh and x < bx + bw and x + length > bx and by < y < by + bh

        # The label must clear text bands of states in `under` and boxes of all others.
        edge = doc["transitions"][int(ident)]
        # States enclosing both ends.
        src, dst = edge["src"], edge["dst"]
        under = enclosing(doc, src) & enclosing(doc, dst)
        # A composite end holding the other end encloses the label as well.
        under |= {s for s, t in ((src, dst), (dst, src)) if s in enclosing(doc, t)}
        # An internal or local self-transition is drawn inside its state.
        if src == dst and edge["kind"] in ("internal", "local"):
            under.add(src)
        for i in live:
            if i in under:
                hit = any(struck(band[i]) for band in bands)
            else:
                hit = struck(rects[i])
            if hit:
                note("label over a state box", f"t{ident} over state {i}",
                     (x, y - size, length, size))
                break
        for a, b, other, _ in legs:
            if other != ident and overlaps(em, span(a, b)):
                note("label over another route", f"t{ident} over t{other}",
                     (x, y - size, length, size))
                break

        # Flags a label whose em box overlaps a leg of its own route.
        for a, b, other, _ in legs:
            if other == ident and overlaps(em, span(a, b)):
                note("label sliced by its own route", f"t{ident} on its own leg",
                     (x, y - size, length, size))
                break

        # Flags a label whose em box overlaps a region divider's stroke.
        for boundary in boundaries:
            if overlaps(em, boundary):
                note("label sliced by a region divider", f"t{ident} across a divider",
                     (x, y - size, length, size))
                break

        # Flags a label em box nearer than `label_leader` to another route's leg.
        theirs = [gap(em, span(a, b)) for a, b, other, _ in legs if other != ident]
        if theirs and leader is not None and min(theirs) < leader:
            note("a foreign line inside a label's leader",
                 f"{min(theirs)} away, leader {leader}", (x, y - size, length, size))

        # Flags a label outside any state box enclosing both its transition's ends.
        both = enclosing(doc, edge["src"]) & enclosing(doc, edge["dst"])
        for i in both:
            bx, by, bw, bh = rects[i]
            if not (bw and bh):
                continue
            if not (bx <= x and x + length <= bx + bw and by <= y - size
                    and y <= by + bh):
                note("label outside its enclosing state",
                     f"outside state {i}", (x, y - size, length, size))
                break

    # Flags each pair of overlapping text em boxes.
    found["texts"] = found.get("texts", 0) + len(inked)
    for i, (a_box, a_which, a_id) in enumerate(inked):
        for b_box, b_which, b_id in inked[i + 1:]:
            if overlaps(a_box, b_box):
                note("texts overprint each other",
                     f"{a_which} {a_id} over {b_which} {b_id}", a_box)

    # Flags state text with an em-box corner outside the state's largest circle.
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

    return found, notes, marks


def find_scav(explicit=None):
    """The binary to read geometry with, the host release build preferred."""
    if explicit:
        return Path(explicit)
    at = REPO_ROOT / "out/macos-clang-libcxx-release/bin/scav"
    if at.exists():
        return at
    found = sorted(REPO_ROOT.glob("out/*/bin/scav"))
    return found[0] if found else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="where", default=str(REPO_ROOT / "out/baseline"))
    ap.add_argument("--chart", default=None)
    ap.add_argument("--gauntlet", action="store_true",
                    help="the element suite under test_data/charts/gauntlet")
    ap.add_argument("--scav", default=None)
    ap.add_argument("--portfolio-row", dest="row", type=int, default=None,
                    help="audit one row of 11.10's table rather than the pick")
    ap.add_argument("--json", action="store_true",
                    help="the counts as JSON, per chart and totalled")
    args = ap.parse_args()
    scav_bin = find_scav(args.scav)
    if scav_bin is None or not scav_bin.exists():
        print("no scav binary; build first", file=sys.stderr)
        return 1

    where = Path(args.where)
    root = GAUNTLET if args.gauntlet else CORPUS
    names = ([args.chart] if args.chart else
             sorted(p.name for p in root.glob("*.scav")))
    verbose = args.chart is not None

    total = {}
    per_chart = {}
    missing = []
    for name in names:
        svg = where / (name + ".svg")
        if not svg.exists():
            missing.append(name)
            continue
        every, chart, doc = geometry(root / name, scav_bin, args.row)
        found, notes, _ = audit(svg.read_text(encoding="utf-8"), every, chart,
                                doc, verbose)
        per_chart[name] = found
        for key, count in found.items():
            total[key] = total.get(key, 0) + count
        if verbose and notes:
            print(f"{name}:")
            print("\n".join(notes))

    if args.json:
        json.dump({"row": args.row, "charts": per_chart, "total": total},
                  sys.stdout, indent=2, sort_keys=True)
        print()
        return 1 if len(missing) == len(names) else 0

    if missing:
        print(f"not rendered ({len(missing)}): run tools/baseline.py first",
              file=sys.stderr)
        if len(missing) == len(names):
            return 1

    # Counts first, then the findings, so the ratio is visible.
    scale = {"segment not axis-aligned": "route segments",
             "lanes closer than one line of text": "route segments",
             "label sliced by its own route": "transition labels",
             "label sliced by a region divider": "transition labels",
             "attachment on a drawn corner": "state attachments",
             "label not anchored to its own polyline": "placed label boxes",
             "label outside its enclosing state": "transition labels",
             "segment flush along a box": "route segments",
             "segment nearer than pad to a border it runs along": "route segments",
             "route start not on any border": "route starts",
             "arrowhead not on any border": "arrowheads",
             "an arrowhead over another route's end": "arrowheads",
             "divider not axis-aligned": "region dividers",
             "drawn outside the chart rect": "route segments",
             "routes share a run": "route segments",
             "label over a state box": "transition labels",
             "label over another route": "transition labels",
             "a foreign line inside a label's leader": "transition labels",
             "texts overprint each other": "texts",
             "mark outside its glyph": "marks in a glyph"}
    for key in ("route segments", "route starts", "arrowheads", "state attachments",
                "region dividers", "transition labels", "placed label boxes", "texts",
                "marks in a glyph"):
        print(f"{key:<40} {total.get(key, 0)}")
    print()
    print(f"  {'shared run, grid units':<40} {total.get('overlapped units', 0)}")
    # Shared runs inside two routes' common point prefix or suffix (see `trunks`).
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
