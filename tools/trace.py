#!/usr/bin/env python3
"""Why a route came out the way it did, read off the decision trace (11.16).

`scav dump --layout --trace` prints every decision the run made. This reads the
JSON back and answers the question the trace exists for: for one transition,
which choices in which phase produced the polyline that shipped.

  tools/trace.py estop.scav               every transition, one line each
  tools/trace.py estop.scav --trans 3     the decision chain for one of them
  tools/trace.py estop.scav --kinks       only the ones that bend
  tools/trace.py estop.scav --raw         the events, unsummarized
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS = REPO_ROOT / "test_data/charts"


def find_scav(explicit=None):
    """The binary to trace with, the host release build preferred."""
    if explicit:
        return Path(explicit)
    at = REPO_ROOT / "out/macos-clang-libcxx-release/bin/scav"
    if at.exists():
        return at
    found = sorted(REPO_ROOT.glob("out/*/bin/scav"))
    return found[0] if found else None


def run(scav, chart, row):
    """The trace and the model beside it: the trace names ids, and the names
    they stand for are the dump's."""
    argv = [str(scav), "dump", "--json", "--layout", "--trace", str(chart)]
    if row is not None:
        argv[4:4] = ["--portfolio-row", str(row)]
    out = subprocess.run(argv, capture_output=True, text=True, check=True).stdout
    # Two documents on one stream, the trace first: the array ends at the first
    # line that is a bare bracket, which no line of the object dump is.
    end = out.index("\n]\n") + 3
    return json.loads(out[:end]), json.loads(out[end:])


# No filtering: `--trace` searches first and traces only the drawing that won,
# so every event in the stream belongs to what shipped (11.16).
def chain_for(events, trans, model):
    """Every event that bears on one transition, in the order it happened."""
    segs = set()
    for e in events:
        if e["kind"] == "net_planned" and e["trans"] == trans:
            segs.add(e["seg"])
    out, nets = [], set()
    for e in events:
        k = e["kind"]
        if k in ("edge_reversed", "route_degraded", "edge_chained") and e["seg"] in segs:
            out.append(e)
        elif k == "node_placed" and e.get("bend_of_seg") in segs:
            out.append(e)
        elif k == "net_planned" and e["trans"] == trans:
            out.append(e)
            nets.add(len(nets))
        elif k == "net_waypoint" and out and out[-1]["kind"] in ("net_planned",
                                                                 "net_waypoint"):
            out.append(e)
    return out


def describe(e, model):
    k = e["kind"]
    if k == "edge_reversed":
        return f"  cycle-breaking reversed segment {e['seg']}"
    if k == "edge_chained":
        return (f"  chained through a bend at rank {e['rank']} "
                f"({e['index']} of {e['count']})")
    if k == "node_placed":
        return f"  that bend was placed at {tuple(e['at'])}"
    if k == "net_planned":
        return (f"  planned {tuple(e['src'])} -> {tuple(e['dst'])}, "
                f"{e['waypoints']} waypoint(s)")
    if k == "net_waypoint":
        return f"    must pass through {tuple(e['at'])}"
    if k == "route_degraded":
        return "  FELL BACK to a straight line"
    return f"  {k}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("chart")
    ap.add_argument("--trans", type=int, default=None)
    ap.add_argument("--kinks", action="store_true")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--portfolio-row", dest="row", type=int, default=None)
    ap.add_argument("--scav", default=None)
    args = ap.parse_args()

    scav = find_scav(args.scav)
    if scav is None:
        print("no scav binary; build one first", file=sys.stderr)
        return 2
    chart = Path(args.chart)
    if not chart.exists():
        chart = CORPUS / args.chart

    events, model = run(scav, chart, args.row)
    if args.raw:
        for e in events:
            print(json.dumps(e))
        return 0

    shipped = events
    states = model["states"]
    routes = model["geometry"]["route"]

    def name(i):
        n = states[i]["name"] if i < len(states) else ""
        return n or f"#{i}"

    if args.trans is not None:
        picked = [args.trans]
    else:
        picked = range(len(model["transitions"]))

    for t in picked:
        tr = model["transitions"][t]
        r = routes[t] if t < len(routes) else []
        kinks = max(len(r) - 2, 0)
        if args.kinks and kinks == 0:
            continue
        label = tr["label"] or "-"
        print(f"t{t} {name(tr['src'])} -> {name(tr['dst'])} [{label}]  "
              f"{kinks} kink(s), {len(r)} point(s)")
        if args.trans is None:
            continue
        for e in chain_for(shipped, t, model):
            print(describe(e, model))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
