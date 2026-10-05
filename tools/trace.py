#!/usr/bin/env python3
"""Runs `scav dump --layout --trace` on a chart and prints the decisions behind each
transition's route.

  tools/trace.py estop.scav               every transition, one line each
  tools/trace.py estop.scav --trans 3     the decision chain for one of them
  tools/trace.py estop.scav --kinks       only the ones that bend
  tools/trace.py estop.scav --raw         the events, unsummarized
  tools/trace.py bottler.scav --search    each row's searches, kicks and takes

Any other flag is a layout flag passed to `scav dump`, such as `--no-text` or a pin.
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


def run(scav, chart, row, layout=(), outline=False):
    """Runs `scav dump --json --layout --trace`; returns (trace, model dump)."""
    argv = [str(scav), "dump", "--json", "--layout", "--trace", *layout, str(chart)]
    if outline:
        argv[5:5] = ["--trace-outline"]
    if row is not None:
        argv[4:4] = ["--portfolio-row", str(row)]
    out = subprocess.run(argv, capture_output=True, text=True, check=True).stdout
    # The trace array comes first and ends at the first line that is a bare `]`.
    end = out.index("\n]\n") + 3
    return json.loads(out[:end]), json.loads(out[end:])


# `--trace` traces only the winning drawing; every event belongs to it.
def chain_for(events, trans, model):
    """Every event that bears on one transition, in the order it happened."""
    segs = set()
    for e in events:
        if e["kind"] == "net_planned" and e["trans"] == trans:
            segs.add(e["seg"])
    out, nets = [], set()
    for e in events:
        k = e["kind"]
        if (k in ("edge_reversed", "route_degraded", "route_walled", "label_centred",
                  "route_reseated", "route_crossed", "edge_chained", "gap_charged")
                and e["seg"] in segs):
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
    if k == "route_walled":
        return "  crossed the walls that enclose an end"
    if k == "route_reseated":
        return "  routed only without clearance bumpers"
    if k == "route_crossed":
        return "  found no way clear of its own earlier leg and crossed it"
    if k == "label_centred":
        return "  label found no seat beside its route; centred on it"
    if k == "gap_charged" and e["cause"] == "held":
        return (f"  segment {e['seg']}'s label ({e['width']}) is held by frame "
                f"{e['frame']}'s rank boundary {e['boundary']}, charging nothing")
    if k == "gap_charged":
        return (f"  segment {e['seg']} asked frame {e['frame']}'s rank boundary "
                f"{e['boundary']} for {e['width']} ({e['cause']})")
    return f"  {k}"


def print_outline(events):
    """Each row's first search, repeats, kick rounds and takes, then each refold."""
    kicked = None
    for e in events:
        k = e["kind"]
        if k == "row_searched":
            kicked = None
            print(f"row {e['row']:2} {e['pass']:6} t2 {e['t2']} t0 {e['t0']}")
        elif k == "row_repeated":
            print(f"row {e['row']:2} repeats row {e['of']}, not kicked")
        elif k == "kick_scored":
            if kicked != e["row"]:
                kicked = e["row"]
                print(f"row {e['row']:2} kicks")
            what = e["move"]
            if what == "reverse":
                what += f" t{e['trans']}:{e['leg']}"
            print(f"    {what:16} frame {e['frame']:3}  framed {e['framed']:7} "
                  f"t0 {e['framed_t0']}  -> {e['t2']:7} t0 {e['t0']}  {e['verdict']}")
        elif k == "kick_taken":
            kicked = None
            print(f"  took {e['how']:8} t2 {e['t2']} t0 {e['t0']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("chart")
    ap.add_argument("--trans", type=int, default=None)
    ap.add_argument("--kinks", action="store_true")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--portfolio-row", dest="row", type=int, default=None)
    ap.add_argument("--scav", default=None)
    ap.add_argument("--search", action="store_true")
    args, layout = ap.parse_known_args()

    scav = find_scav(args.scav)
    if scav is None:
        print("no scav binary; build one first", file=sys.stderr)
        return 2
    chart = Path(args.chart)
    if not chart.exists():
        chart = CORPUS / args.chart

    events, model = run(scav, chart, args.row, layout, args.search)
    if args.search and not args.raw:
        print_outline(events)
        return 0
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
