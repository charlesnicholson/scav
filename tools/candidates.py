#!/usr/bin/env python3
"""Every candidate the portfolio can produce, rendered and audited.

Level 2 writes the geometry of whichever row `Cost` ranks first, so the
objective decides which drawings exist -- and it is the objective under
calibration (11.10, 11.12). This renders every row of the table for every
chart, audits each with the reader-visible counts tools/audit.py reports, and
answers two questions the weights are not needed for:

  the bound   per chart and per defect class, the fewest any row achieves
  the regret  what the pick costs against that bound, class by class

A bound the pick already meets says the search has no headroom left on that
class and the defect belongs to a section rather than to a weight. A regret
says the weights are choosing badly over candidates they already have.

The bound is a floor and not a drawing: it takes each class's minimum
independently, so no single row need achieve all of them at once. The
best-single-row column beside it is the reachable one.

  tools/candidates.py                 the corpus, every row
  tools/candidates.py --rows 0,1,4
  tools/candidates.py --gauntlet
  tools/candidates.py --json
  tools/candidates.py --poster        a page pairing the pick with the best row
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import audit  # noqa: E402
import poster  # noqa: E402

REPO_ROOT = audit.REPO_ROOT
ROWS = 8


def render(scav_bin, chart, out, row):
    """One candidate's SVG. `row` of None is the pick, which is the search."""
    cmd = [str(scav_bin), "render", "-o", str(out)]
    if row is not None:
        cmd += ["--portfolio-row", str(row)]
    done = subprocess.run(cmd + [str(chart)], capture_output=True, text=True,
                          check=False)
    return None if done.returncode == 0 else (done.stderr.strip() or "render failed")


def counts(scav_bin, chart, svg, row, verbose=False):
    """One candidate's audit counts, its cost vector, and its fingerprint.

    All three come off the one dump the audit already needs, so the objective
    that ranked a candidate travels beside the defects a reader sees in it.
    """
    every, rect, doc = audit.geometry(chart, scav_bin, row)
    found, _, _ = audit.audit(svg.read_text(encoding="utf-8"), every, rect, doc,
                              verbose)
    return found, doc["geometry"]["cost"], (tuple(every), rect)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(REPO_ROOT / "out/candidates"))
    ap.add_argument("--rows", default=None,
                    help="a comma-separated subset; every row otherwise")
    ap.add_argument("--gauntlet", action="store_true")
    ap.add_argument("--chart", default=None)
    ap.add_argument("--scav", default=None)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--poster", default=None, nargs="?", const="",
                    help="write a page pairing each chart's pick with the row a "
                         "reader would take; default out/candidates/poster.html")
    args = ap.parse_args()

    scav_bin = audit.find_scav(args.scav)
    if scav_bin is None or not scav_bin.exists():
        print("no scav binary; build first", file=sys.stderr)
        return 1
    # A binary predating the flag refuses every row and leaves a table of
    # zeroes that reads like a clean audit, so the flag is probed rather than
    # assumed. `find_scav` prefers the release build, which is the one a
    # testable-only rebuild leaves behind.
    probe = subprocess.run([str(scav_bin), "render", "--portfolio-row", "0", "-o",
                            "/dev/null", str(audit.CORPUS / "estop.scav")],
                           capture_output=True, text=True, check=False)
    if probe.returncode != 0:
        print(f"{scav_bin} does not take --portfolio-row; rebuild it",
              file=sys.stderr)
        return 1

    root = audit.GAUNTLET if args.gauntlet else audit.CORPUS
    names = ([args.chart] if args.chart
             else sorted(p.name for p in root.glob("*.scav")))
    rows = ([int(v) for v in args.rows.split(",")] if args.rows
            else list(range(ROWS)))
    where = Path(args.out)

    # `pick` is the unpinned run, so it is keyed apart from the rows even where
    # its geometry is one of theirs.
    per = {}       # (name, row|"pick") -> counts
    cost = {}      # (name, row|"pick") -> the cost vector that ranked it
    geom = {}      # (name, row|"pick") -> the fingerprint, to name the pick's row
    failed = []
    for name in names:
        for row in rows + ["pick"]:
            at = row if row != "pick" else None
            out = where / (f"row{row}" if row != "pick" else "pick")
            out.mkdir(parents=True, exist_ok=True)
            svg = out / (name + ".svg")
            why = render(scav_bin, root / name, svg, at)
            if why is not None:
                failed.append((name, row, why))
                continue
            # The live state boxes and the canvas together, because two rows
            # can come out at one extent while placing different states.
            per[(name, row)], cost[(name, row)], geom[(name, row)] = counts(
                scav_bin, root / name, svg, at)

    for name, row, why in failed:
        print(f"{name} row {row}: {why}", file=sys.stderr)

    classes = DEFECTS
    # Per chart, the fewest any row achieves; and the row that minimises the
    # whole vector's sum, which is a drawing rather than a floor.
    bound = {k: 0 for k in classes}
    pick = {k: 0 for k in classes}
    best_row = {}
    picked_row = {}
    for name in names:
        have = [r for r in rows if (name, r) in per]
        if not have:
            continue
        for k in classes:
            bound[k] += min(per[(name, r)].get(k, 0) for r in have)
            pick[k] += per.get((name, "pick"), {}).get(k, 0)
        best_row[name] = min(
            have, key=lambda r: (sum(per[(name, r)].get(k, 0) for k in classes), r))
        same = [r for r in have if geom.get((name, r)) == geom.get((name, "pick"))]
        picked_row[name] = same[0] if same else None

    totals = {r: {k: sum(per[(n, r)].get(k, 0) for n in names if (n, r) in per)
                  for k in classes}
              for r in rows}

    if args.poster is not None:
        page = Path(args.poster) if args.poster else (where / "poster.html")
        write_poster(where, page, names, per, cost, geom, classes, picked_row, rows)
        print(f"wrote {page}")

    if args.json:
        json.dump({"rows": rows, "per_chart": {f"{n}|{r}": v for (n, r), v in per.items()},
                   "row_totals": {str(r): v for r, v in totals.items()},
                   "cost": {f"{n}|{r}": v for (n, r), v in cost.items()},
                   "bound": bound, "pick": pick,
                   "regret": {k: pick[k] - bound[k] for k in classes},
                   "picked_row": picked_row, "best_row": best_row},
                  sys.stdout, indent=2, sort_keys=True)
        print()
        return 0

    head = "".join(f"{('r' + str(r)):>7}" for r in rows)
    print(f"{'defect class':<42}{head}{'bound':>8}{'pick':>7}{'regret':>8}")
    for k in classes:
        cells = "".join(f"{totals[r].get(k, 0):>7}" for r in rows)
        print(f"{k:<42}{cells}{bound[k]:>8}{pick[k]:>7}"
              f"{pick[k] - bound[k]:>8}")
    print()
    # The reachable column: one row per chart, chosen by its own defect sum, so
    # it is a set of drawings rather than a floor no candidate meets.
    reachable = sum(sum(per[(n, best_row[n])].get(k, 0) for k in classes)
                    for n in names if n in best_row)
    shipped = sum(pick[k] for k in classes)
    print(f"{'defects, all classes':<42}{'bound':>8}{'reachable':>11}{'pick':>7}")
    print(f"{'':<42}{sum(bound.values()):>8}{reachable:>11}{shipped:>7}")
    print()
    # What the weights are being asked: of the rows this chart ran, does the one
    # `t2` ranks first also read best? Where it does not, the exchange rate
    # between two terms is wrong and no amount of search reaches past it (11.6).
    print(f"{'chart':<18}{'row':>4}{'t2':>13}{'defects':>9}{'':>3}"
          f"{'t2 rank':>8}{'defect rank':>12}")
    for name in names:
        rows_here = sorted(r for r in rows if (name, r) in per)
        if not rows_here:
            continue
        by_t2 = sorted(rows_here, key=lambda r: (cost[(name, r)]["t2"], r))
        by_def = sorted(rows_here,
                        key=lambda r: (sum(per[(name, r)].get(k, 0) for k in classes), r))
        for r in rows_here:
            defects = sum(per[(name, r)].get(k, 0) for k in classes)
            mark = ""
            if r == by_t2[0] and r != by_def[0]:
                mark = "<- t2 picks this"
            elif r == by_def[0] and r != by_t2[0]:
                mark = "<- a reader picks this"
            print(f"{(name if r == rows_here[0] else ''):<18}{r:>4}"
                  f"{cost[(name, r)]['t2']:>13}{defects:>9}{'':>3}"
                  f"{by_t2.index(r) + 1:>8}{by_def.index(r) + 1:>12}  {mark}")
    print()
    print(f"{'chart':<20}{'pick is row':>12}{'fewest defects':>16}{'that row':>10}"
          f"{'the pick':>10}")
    for name in names:
        at = picked_row.get(name)
        best = best_row.get(name)
        mine = sum(per[(name, "pick")].get(k, 0) for k in classes) \
            if (name, "pick") in per else "-"
        theirs = sum(per[(name, best)].get(k, 0) for k in classes) \
            if best is not None else "-"
        print(f"{name:<20}{('?' if at is None else at):>12}"
              f"{('-' if best is None else best):>16}{theirs:>10}{mine:>10}")
    return 0


PAGE_HEAD = """<!doctype html><meta charset="utf-8"><title>scav candidates</title>
<style>
 body{font:13px/1.5 -apple-system,system-ui,sans-serif;margin:0;padding:28px;
      background:#f6f6f4;color:#1b1b1a}
 h1{font-size:19px;margin:0 0 4px} .sub{color:#666;margin:0 0 26px}
 .chart{background:#fff;border:1px solid #e2e2de;border-radius:8px;padding:18px;
        margin-bottom:22px}
 .chart>h2{font-size:15px;margin:0 0 12px;font-family:ui-monospace,monospace}
 .pair{display:grid;grid-template-columns:1fr 1fr;gap:18px}
 .panel{min-width:0}
 .cap{display:flex;gap:10px;align-items:baseline;margin-bottom:6px}
 .who{font-weight:600} .num{color:#666;font-variant-numeric:tabular-nums}
 .ships{color:#a8321e} .reads{color:#1c6b3c}
 img{width:100%;height:auto;background:#fff;border:1px solid #ecece8;border-radius:4px}
 .same{color:#888;font-style:italic}
</style>
<h1>Every candidate the portfolio can produce, two at a time</h1>
<p class="sub">Left: the row <code>Cost</code> ranks first, which is what ships.
Right: the row with the fewest reader-visible defects <code>tools/audit.py</code>
can see. Where they are the same row the objective is already choosing well and
only one panel is drawn. <b>t2</b> is the weighted sum; <b>defects</b> is the
audit's count over every class it knows.</p>
"""


def write_poster(where, out, names, per, cost, geom, classes, picked_row, rows):
    """The page the fit is judged on: the pick beside the candidate it lost to.

    A cost vector cannot see what this shows, which is the whole reason 11.12
    keeps a page in the loop rather than a golden.
    """
    def defects(name, row):
        return sum(per[(name, row)].get(k, 0) for k in classes)

    body = []
    for name in names:
        here = sorted(r for r in rows if (name, r) in per)
        if not here:
            continue
        by_t2 = min(here, key=lambda r: (cost[(name, r)]["t2"], r))
        by_def = min(here, key=lambda r: (defects(name, r), r))
        panels = []
        for who, row, cls in (("ships \u2014 lowest t2", by_t2, "ships"),
                              ("reads best \u2014 fewest defects", by_def, "reads")):
            svg = where / f"row{row}" / (name + ".svg")
            if not svg.exists():
                continue
            text = svg.read_text(encoding="utf-8")
            size = poster.extent(text)
            dims = f"{int(size[0])}\u00d7{int(size[1])}pt" if size else "?"
            panels.append(
                f'<div class="panel"><div class="cap">'
                f'<span class="who {cls}">{who}</span>'
                f'<span class="num">row {row} &middot; t2 {cost[(name, row)]["t2"]:,}'
                f' &middot; {defects(name, row)} defects &middot; {dims}</span>'
                f'</div><img src="{poster.data_uri(text)}"></div>')
            if by_t2 == by_def:
                break
        note = ('<p class="same">The objective already takes the row a reader '
                'would.</p>' if by_t2 == by_def else "")
        body.append(f'<div class="chart"><h2>{name}</h2>{note}'
                    f'<div class="pair">{"".join(panels)}</div></div>')
    out.write_text(PAGE_HEAD + "\n".join(body), encoding="utf-8")
    return out


# The audit's defect classes, which are the keys it prints a ratio for. The
# denominators it counts beside them are not defects and have no floor.
DEFECTS = [
    "segment not axis-aligned", "segment flush along a box",
    "route start not on any border", "arrowhead not on any border",
    "an arrowhead over another route's end", "divider not axis-aligned",
    "drawn outside the chart rect", "routes share a run",
    "label over a state box", "label over another route",
    "label nearer another route than its own", "texts overprint each other",
    "mark outside its glyph",
    # The classes P9d's review added, each one an absolute the objective was
    # blind to rather than a preference it priced badly.
    "attachment on a drawn corner", "label detached from its own polyline",
    "label sliced by its own route", "label sliced by a region divider",
    "label outside its enclosing state",
]

if __name__ == "__main__":
    raise SystemExit(main())
