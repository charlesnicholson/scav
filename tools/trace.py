#!/usr/bin/env python3
"""Runs `scav dump --layout --trace` on a chart and prints the decisions behind each
transition's route.

  tools/trace.py estop.scav               every transition, one line each
  tools/trace.py estop.scav --trans 3     the decision chain for one of them
  tools/trace.py estop.scav --kinks       only the ones that bend
  tools/trace.py estop.scav --raw         the events, unsummarized
  tools/trace.py bottler.scav --outline   each row's searches, kicks and takes
  tools/trace.py --stats [chart...]       moves culled, skipped, scored, deduped, CPU; corpus default

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


class Traced:
    """`scav dump --json --layout --trace` read from its pipe as it runs: `events()` yields
    each event as its line arrives, then `model()` parses the model dump after the trace."""

    def __init__(self, scav, chart, row, layout=(), outline=False):
        argv = [str(scav), "dump", "--json", "--layout", "--trace", *layout, str(chart)]
        if outline:
            argv[5:5] = ["--trace-outline"]
        if row is not None:
            argv[4:4] = ["--portfolio-row", str(row)]
        self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE, text=True)

    def events(self):
        # One event per line, comma-terminated but the last, between `[` and `]` lines.
        if self.proc.stdout.readline() != "[\n":
            return
        for line in self.proc.stdout:
            if line == "]\n":
                return
            yield json.loads(line.rstrip("\n").rstrip(","))

    def model(self):
        """The model dump; raises if scav failed."""
        text = self.proc.stdout.read()
        if self.proc.wait() != 0:
            raise subprocess.CalledProcessError(self.proc.returncode, self.proc.args)
        return json.loads(text)


# The kinds `chain_for` reads.
CHAIN_KINDS = frozenset({"edge_reversed", "route_degraded", "route_walled", "label_centred",
                         "route_reseated", "route_crossed", "edge_chained", "gap_charged",
                         "node_placed", "net_planned", "net_waypoint"})


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
            if what == "grow":
                what += f" {e['state']} {e['seats'][0]}x{e['seats'][1]}"
            print(f"    {what:16} frame {e['frame']:3}  framed {e['framed']:7} "
                  f"t0 {e['framed_t0']}  -> {e['t2']:7} t0 {e['t0']}  {e['verdict']}")
        elif k == "kick_taken":
            kicked = None
            print(f"  took {e['how']:8} t2 {e['t2']} t0 {e['t0']}")


MOVES = ("rank", "cut", "reverse", "face", "side", "fold", "orient", "loop", "grow")
VALUED = ("--profile", "--rank", "--cut", "--reverse", "--end", "--orient", "--fold",
          "--loop", "--grow", "--search", "--jitter-seed", "-j")  # scav layout flags that take a value


def split_layout(argv):
    """Moves each `VALUED` flag and its value out of `argv`; returns (rest, layout)."""
    rest, layout = [], []
    it = iter(argv)
    for a in it:
        if a in VALUED:
            layout += [a, next(it, "")]
        else:
            rest.append(a)
    return rest, layout


def search_stats(scav, chart, layout):
    """Runs `scav dump --layout --search-stats`; returns its counts object."""
    argv = [str(scav), "dump", "--layout", "--search-stats", *layout, str(chart)]
    out = subprocess.run(argv, capture_output=True, text=True, check=True).stdout
    return json.loads(out[:out.index("\n")])


def print_stats(scav, charts, layout):
    """Prints `--search-stats` per chart and scale, then summed per move kind."""
    scales = [[]] if "--no-text" in layout else [[], ["--no-text"]]
    kinds = {m: [0, 0, 0, 0, 0, 0, 0] for m in MOVES}
    print(f"{'chart':14} {'scale':6} {'culled':>8} {'skipped':>8} "
          f"{'offered':>9} {'deduped':>9} {'drawn':>9} {'pruned':>9} "
          f"{'scored':>9} {'stopped':>9} {'taken':>6} {'searches':>8} {'memo MB':>8} "
          f"{'cpu s':>8}")
    for chart in charts:
        for scale in scales:
            st = search_stats(scav, chart, [*layout, *scale])
            s = st["search"]
            culled = sum(v["culled"] for v in s["moves"].values())
            skipped = sum(v["skipped"] for v in s["moves"].values())
            offered = sum(v["offered"] for v in s["moves"].values())
            deduped = sum(v["deduped"] for v in s["moves"].values())
            pruned = sum(v["pruned"] for v in s["moves"].values())
            stopped = sum(v["stopped"] for v in s["moves"].values())
            taken = sum(v["taken"] for v in s["moves"].values())
            for m, v in s["moves"].items():
                kinds[m][0] += v["culled"]
                kinds[m][1] += v["offered"]
                kinds[m][2] += v["deduped"]
                kinds[m][3] += v["taken"]
                kinds[m][4] += v["skipped"]
                kinds[m][5] += v["pruned"]
                kinds[m][6] += v["stopped"]
            print(f"{Path(chart).stem:14} {'notext' if scale else 'text':6} {culled:8} "
                  f"{skipped:8} {offered:9} {deduped:9} "
                  f"{s['drawn']:9} {pruned:9} "
                  f"{offered - deduped - pruned:9} {stopped:9} {taken:6} "
                  f"{s['searches']:8} {s['memo_bytes'] / 1e6:8.1f} {st['cpu_ms'] / 1e3:8.2f}")
    print()
    print(f"{'move':14} {'culled':>8} {'skipped':>8} "
          f"{'offered':>9} {'deduped':>9} {'pruned':>9} {'scored':>9} {'stopped':>9} "
          f"{'taken':>6}")
    for m in MOVES:
        c, o, d, t, k, p, x = kinds[m]
        if c or o or t:
            print(f"{m:14} {c:8} {k:8} {o:9} {d:9} {p:9} {o - d - p:9} {x:9} {t:6}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("chart", nargs="*")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--trans", type=int, default=None)
    ap.add_argument("--kinks", action="store_true")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--portfolio-row", dest="row", type=int, default=None)
    ap.add_argument("--scav", default=None)
    ap.add_argument("--outline", action="store_true")
    rest, layout = split_layout(sys.argv[1:])
    args, flags = ap.parse_known_args(rest)
    layout += flags

    scav = find_scav(args.scav)
    if scav is None:
        print("no scav binary; build one first", file=sys.stderr)
        return 2
    charts = [Path(c) if Path(c).exists() else CORPUS / c for c in args.chart]
    if args.stats:
        row = [] if args.row is None else ["--portfolio-row", str(args.row)]
        print_stats(scav, charts or sorted(CORPUS.glob("*.scav")), [*row, *layout])
        return 0
    if len(charts) != 1:
        ap.error("one chart, or --stats")
    chart = charts[0]

    traced = Traced(scav, chart, args.row, layout, args.outline)
    if args.outline and not args.raw:
        print_outline(traced.events())
        traced.model()
        return 0
    if args.raw:
        for e in traced.events():
            print(json.dumps(e))
        traced.model()
        return 0

    shipped = [e for e in traced.events()  # the chain's events, read only for `--trans`
               if args.trans is not None and e["kind"] in CHAIN_KINDS]
    model = traced.model()
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
