#!/usr/bin/env python3
"""Runs the corpus unperturbed and under jitter seeds, for the full and the culled search,
and reports per chart and scale whether one search is worse than the other.

  tools/jitter.py --runs DIR                  run what DIR lacks, then report
  tools/jitter.py --runs DIR --seeds 3        seeds 1..3 beside the unperturbed run
  tools/jitter.py --runs DIR --report         report only
  tools/jitter.py --runs DIR kiln.scav        one chart

Each run is `scav dump --layout --json --search-stats` with `--search` and `--jitter-seed`,
kept in DIR as <chart>.<scale>.<search>.<seed>.json. A search is worse on a chart and scale
when every one of its runs is more than 2% above every run of the other.
"""

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS = REPO_ROOT / "test_data/charts"
SCALES = ("text", "none")
SEARCHES = ("full", "culled")
BAR = 1.02  # worse: every run above every run of the other by this factor


def find_scav() -> Path:
    """The newest `scav` under out/, whichever preset built it."""
    built = sorted(REPO_ROOT.glob("out/*/bin/scav"), key=lambda p: p.stat().st_mtime,
                   reverse=True)
    if not built:
        raise SystemExit("no scav binary under out/; build one first")
    return built[0]


def record_of(out: str) -> dict:
    """The fields a report reads from one `--search-stats` dump."""
    head, body = out.split("\n", 1)
    stats = json.loads(head)
    geo = json.loads(body)["geometry"]
    moves = stats["search"]["moves"].values()
    cost = geo["cost"]
    return {
        "t0": cost["t0_violations"],
        "t2": cost["t2"],
        "bends": cost["tier2"]["bends"],
        "crossings": cost["tier2"]["crossings"],
        "offered": sum(m["offered"] for m in moves),
        "scored": sum(m["offered"] - m["deduped"] for m in moves),
        "skipped": sum(m.get("skipped", 0) for m in moves),
        "searches": stats["search"]["searches"],
        "cpu_ms": stats["cpu_ms"],
        "rests_on": geo["rests_on"],
    }


def run_one(scav: Path, chart: Path, scale: str, search: str, seed: int) -> dict:
    argv = [str(scav), "dump", "--layout", "--json", "--search-stats"]
    if scale == "none":
        argv.append("--no-text")
    if search == "culled":
        argv += ["--search", "culled"]
    if seed:
        argv += ["--jitter-seed", str(seed)]
    out = subprocess.run([*argv, str(chart)], capture_output=True, text=True, check=True)
    return record_of(out.stdout)


def path_of(runs: Path, chart: str, scale: str, search: str, seed: int) -> Path:
    return runs / f"{chart}.{scale}.{search}.{seed}.json"


def load(runs: Path, chart: str, scale: str, search: str, seeds: int) -> dict[int, dict]:
    """The runs DIR holds, by seed."""
    got = {}
    for seed in range(seeds + 1):
        p = path_of(runs, chart, scale, search, seed)
        if p.exists():
            got[seed] = json.loads(p.read_text())
    return got


def worse(a: dict[int, dict], b: dict[int, dict]) -> bool:
    """True when every run of `a` is more than 2% above every run of `b`."""
    return bool(a and b) and (min(r["t2"] for r in a.values()) >
                              BAR * max(r["t2"] for r in b.values()))


def span(rs: dict[int, dict]) -> str:
    lo = min(r["t2"] for r in rs.values())
    hi = max(r["t2"] for r in rs.values())
    return f"{lo}" if lo == hi else f"{lo}-{hi}"


def verdict_of(full: dict[int, dict], culled: dict[int, dict]) -> str:
    if worse(culled, full):
        return "culled worse"
    if worse(full, culled):
        return "full worse"
    return "within noise"


def report(runs: Path, charts: list[str], seeds: int) -> dict:
    """Prints the table and returns the verdicts by (chart, scale)."""
    print(f"{'chart':12} {'scale':5} {'full t2':>13} {'culled t2':>13} {'verdict':>14} "
          f"{'full eval':>10} {'culled eval':>11} {'x':>5} {'full cpu':>9} "
          f"{'culled cpu':>10}")
    verdicts = {}
    ratios = {seed: [] for seed in range(seeds + 1)}
    totals = {s: {"scored": 0, "cpu_ms": 0} for s in SEARCHES}
    for chart in charts:
        for scale in SCALES:
            full = load(runs, chart, scale, "full", seeds)
            culled = load(runs, chart, scale, "culled", seeds)
            if (0 not in full) or (0 not in culled):
                continue
            verdict = verdict_of(full, culled)
            verdicts[(chart, scale)] = verdict
            for seed in full.keys() & culled.keys():
                a, b = full[seed]["t2"], culled[seed]["t2"]
                ratios[seed].append(b / a if a else 1.0)
            f0, c0 = full[0], culled[0]
            for s, r in (("full", f0), ("culled", c0)):
                totals[s]["scored"] += r["scored"]
                totals[s]["cpu_ms"] += r["cpu_ms"]
            x = f0["scored"] / c0["scored"] if c0["scored"] else 0.0
            print(f"{Path(chart).stem:12} {scale:5} {span(full):>13} {span(culled):>13} "
                  f"{verdict:>14} {f0['scored']:10} {c0['scored']:11} {x:5.2f} "
                  f"{f0['cpu_ms'] / 1e3:9.2f} {c0['cpu_ms'] / 1e3:10.2f}")
    print()
    for seed, rs in ratios.items():
        if rs:
            gm = math.exp(sum(math.log(r) for r in rs) / len(rs))
            print(f"seed {seed}: culled over full, geometric-mean t2 ratio {gm:.4f} "
                  f"over {len(rs)} chart-scales")
    f, c = totals["full"], totals["culled"]
    if c["scored"] and c["cpu_ms"]:
        print(f"unperturbed totals: full {f['scored']} evaluations, {f['cpu_ms'] / 1e3:.1f} s; "
              f"culled {c['scored']}, {c['cpu_ms'] / 1e3:.1f} s; "
              f"{f['scored'] / c['scored']:.2f}x fewer evaluations, "
              f"{f['cpu_ms'] / c['cpu_ms']:.2f}x less CPU")
    bad = [k for k, v in verdicts.items() if v != "within noise"]
    print(f"outside the bar: {', '.join(f'{c} {s} ({verdicts[(c, s)]})' for c, s in bad) or 'none'}")
    return verdicts


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("chart", nargs="*")
    ap.add_argument("--runs", type=Path, required=True)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--scales", default=",".join(SCALES))
    ap.add_argument("--searches", default=",".join(SEARCHES))
    ap.add_argument("--report", action="store_true", help="report the runs DIR holds")
    ap.add_argument("--scav", type=Path, default=None)
    args = ap.parse_args()

    charts = [Path(c).name for c in args.chart] or sorted(
        p.name for p in CORPUS.glob("*.scav"))
    args.runs.mkdir(parents=True, exist_ok=True)
    if not args.report:
        scav = args.scav or find_scav()
        # Smallest first, so a capped run finishes the most charts.
        order = sorted(charts, key=lambda c: (CORPUS / c).stat().st_size)
        for seed in range(args.seeds + 1):
            for chart in order:
                for scale in args.scales.split(","):
                    for search in args.searches.split(","):
                        p = path_of(args.runs, chart, scale, search, seed)
                        if p.exists():
                            continue
                        rec = run_one(scav, CORPUS / chart, scale, search, seed)
                        p.write_text(json.dumps(rec))
                        print(f"{chart} {scale} {search} seed {seed}: t2 {rec['t2']} "
                              f"scored {rec['scored']} cpu {rec['cpu_ms']} ms", flush=True)
    report(args.runs, charts, args.seeds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
