#!/usr/bin/env python3
"""Fit 11.6's nine Tier 2 weights to the defects a reader counts (11.12, P9d).

The objective is a *ranking*, so this fits one: over every pair of candidates
for one chart that the audit orders, does `t2` order them the same way? A
regression on defect counts would fit the corpus's absolute numbers, which are
eleven charts' worth of accident; the pairs are what a search actually consumes.

Reports the pairs no weight vector can reach -- two candidates with identical
term vectors and different defect counts -- because that is the ceiling, and
leave-one-chart-out agreement, because nine free parameters over eleven charts
overfits if nobody looks.
"""

import argparse
import json
import pathlib
import random
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))
import audit  # noqa: E402
import candidates  # noqa: E402

TERMS = ("bends", "corridor", "crossings", "excess_len", "adjacency",
         "label", "label_near", "aspect", "area")
# Parallel to TERMS: the em power each is divided by before weighting (11.6).
EM_POWER = (0, 1, 0, 1, 0, 0, 1, 1, 2)
WEIGHT_MAX = 1024


def ceil_div(a, b):
    return -((-a) // b)


def normalised(cost, em):
    """The nine term values `weighted_terms` multiplies a weight into."""
    out = []
    for name, power in zip(TERMS, EM_POWER):
        v = cost[name]
        out.append(v if power == 0 else ceil_div(v, em ** power))
    return tuple(out)


def collect(scav_bin, charts, out_dir, em):
    """Per chart, the candidates that rendered: their terms and their defects."""
    out_dir.mkdir(parents=True, exist_ok=True)
    by_chart = {}
    for chart in charts:
        rows = []
        for row in range(candidates.ROWS):
            svg = out_dir / f"{chart.name}.r{row}.svg"
            if candidates.render(scav_bin, chart, svg, row) is not None:
                continue
            found, cost, _ = candidates.counts(scav_bin, chart, svg, row)
            rows.append({"row": row,
                         "terms": normalised(cost, em),
                         "area": cost["area"],
                         "defects": sum(found.get(k, 0) for k in candidates.DEFECTS)})
        if rows:
            by_chart[chart.name] = rows
    return by_chart


def pairs_of(by_chart):
    """Every within-chart pair a reader orders, as (better, worse) terms.

    **Fewer defects, and among equals the smaller drawing.** Defects alone leave
    the four size terms unconstrained -- nothing in the audit counts area -- and
    a fit on them drives `w_area`, `w_aspect`, `w_corridor` and `w_excess_len`
    to zero, which is an objective that grows a chart without limit. Area is
    11.12's exit criterion, so it is half of what a reader is asked here.
    """
    out = {}
    for name, rows in by_chart.items():
        here = []
        for a in rows:
            for b in rows:
                better = (a["defects"] < b["defects"]) or (
                    (a["defects"] == b["defects"]) and (a["area"] < b["area"]))
                if better:
                    here.append((a["terms"], b["terms"]))
        out[name] = here
    return out


def score(weights, pairs):
    """Pairs ordered right. A tie is not an ordering, so it does not count."""
    good = 0
    for better, worse in pairs:
        lo = sum(w * t for w, t in zip(weights, better))
        hi = sum(w * t for w, t in zip(weights, worse))
        good += 1 if lo < hi else 0
    return good


def picked(weights, by_chart):
    """The defects of the row `argmin(t2, row)` takes, summed over the charts.

    **This is the deployed metric and the one to fit.** Pairwise agreement is a
    proxy for it and a loose one: a vector can order most pairs right and still
    put the wrong row first, which is the only pair `layout_run` consults.
    """
    total = 0
    for rows in by_chart.values():
        best = min(rows, key=lambda r: (sum(w * t for w, t in zip(weights, r["terms"])),
                                        r["row"]))
        total += best["defects"]
    return total


def unreachable(pairs):
    """Pairs no weights can order: the terms are equal and the defects are not."""
    return sum(1 for better, worse in pairs if better == worse)


LADDER = (0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256,
          384, 512, 768, WEIGHT_MAX)


def fit_picked(by_chart, start, seed, steps):
    """Coordinate descent on `picked`, ties broken toward the shipped weight."""
    rnd = random.Random(seed)
    best, best_score = list(start), picked(start, by_chart)
    for attempt in range(steps):
        here = list(start) if attempt == 0 else [rnd.choice(LADDER) for _ in TERMS]
        here_score = picked(here, by_chart)
        improved = True
        while improved:
            improved = False
            for i in range(len(TERMS)):
                keep, keep_score = here[i], here_score
                for v in LADDER:
                    here[i] = v
                    got = picked(here, by_chart)
                    if (got < keep_score) or ((got == keep_score) and (v == start[i])):
                        keep, keep_score = v, got
                here[i] = keep
                improved = improved or (keep_score < here_score)
                here_score = keep_score
        if here_score < best_score:
            best, best_score = list(here), here_score
    return best, best_score


def fit(pairs, start, seed, steps):
    """Coordinate descent from `start`, then from random vectors, best kept.

    Integer weights on a log-ish ladder rather than a dense sweep: 11.6's
    shipped values sit powers of two apart and the profile bounds them to
    [0, 1024], so the ladder is the space rather than a sample of it. Ties
    break toward the shipped weight, so a term the pairs say nothing about
    keeps the value a human chose instead of drifting.
    """
    rnd = random.Random(seed)
    best, best_score = list(start), score(start, pairs)
    for attempt in range(steps):
        here = list(start) if attempt == 0 else [rnd.choice(LADDER) for _ in TERMS]
        here_score = score(here, pairs)
        improved = True
        while improved:
            improved = False
            for i in range(len(TERMS)):
                keep, keep_score = here[i], here_score
                for v in LADDER:
                    here[i] = v
                    got = score(here, pairs)
                    if (got > keep_score) or ((got == keep_score) and (v == start[i])):
                        keep, keep_score = v, got
                here[i] = keep
                improved = improved or (keep_score > here_score)
                here_score = keep_score
        if here_score > best_score:
            best, best_score = list(here), here_score
    return best, best_score


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(REPO_ROOT / "out/fit"))
    ap.add_argument("--scav", default=None)
    ap.add_argument("--gauntlet", action="store_true",
                    help="fit the element suite beside the corpus")
    ap.add_argument("--em", type=int, default=192, help="readable's font_size_grid")
    ap.add_argument("--steps", type=int, default=24)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--spread", type=int, default=12,
                    help="seeds to refit from, for the sensitivity report")
    ap.add_argument("--slack", type=int, default=2,
                    help="pairs a vector may give up and still count as an optimum")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    scav_bin = audit.find_scav(args.scav)
    if scav_bin is None or not scav_bin.exists():
        print("no scav binary; build first", file=sys.stderr)
        return 1

    charts = sorted(audit.CORPUS.glob("*.scav"))
    if args.gauntlet:
        charts += sorted(audit.GAUNTLET.glob("*.scav"))
    by_chart = collect(scav_bin, charts, pathlib.Path(args.out), args.em)
    per_chart = pairs_of(by_chart)
    every = [p for ps in per_chart.values() for p in ps]

    shipped = [64, 48, 32, 4, 16, 24, 48, 2, 1]
    floor = unreachable(every)
    print(f"{len(every)} ordered pairs over {len(per_chart)} charts, "
          f"{floor} of them unreachable by any weights")
    print(f"shipped  {score(shipped, every):4d} / {len(every) - floor}")

    best, best_score = fit(every, shipped, args.seed, args.steps)
    print(f"fitted   {best_score:4d} / {len(every) - floor}   {best}")

    # The deployed metric: what the search actually picks.
    reach = sum(min(r["defects"] for r in rows) for rows in by_chart.values())
    print()
    print(f"defects at the pick, over {len(by_chart)} charts, floor {reach}")
    print(f"  shipped weights        {picked(shipped, by_chart)}")
    print(f"  weights fitted to pairs {picked(best, by_chart)}")
    aimed, aimed_score = fit_picked(by_chart, shipped, args.seed, args.steps)
    print(f"  weights fitted to this  {aimed_score}   {aimed}")
    print(f"  pairs those order       {score(aimed, every)} / {len(every) - floor}")
    print()
    print(f"{'term':<12}{'shipped':>9}{'aimed':>7}{'keeps':>14}  cost of shipped")
    for i, name in enumerate(TERMS):
        held = list(aimed)
        keeps = []
        for v in LADDER:
            held[i] = v
            if picked(held, by_chart) <= aimed_score:
                keeps.append(v)
        held[i] = shipped[i]
        gave = picked(held, by_chart) - aimed_score
        span = f"{keeps[0]}..{keeps[-1]}" if keeps else "-"
        note = ("free" if len(keeps) == len(LADDER) else
                f"+{gave}" if gave > 0 else "none")
        print(f"{name:<12}{shipped[i]:>9}{aimed[i]:>7}{span:>14}  {note}")

    # Held out one chart at a time, fitted on the rest and scored on it alone.
    held = 0
    held_of = 0
    for name in per_chart:
        rest = [p for other, ps in per_chart.items() if other != name for p in ps]
        mine = per_chart[name]
        if not mine or not rest:
            continue
        w, _ = fit(rest, shipped, args.seed, max(args.steps // 3, 1))
        held += score(w, mine)
        held_of += len(mine) - unreachable(mine)
    print(f"held out {held:4d} / {held_of}")

    # Nine weights over a few hundred 0/1 constraints, and a single optimum
    # says nothing about how much of it the data actually chose. Each term is
    # swept across the ladder with the other eight held at the fit, and the
    # range that stays within `slack` is what the pairs have an opinion about.
    # A term whose range spans the ladder is one they do not constrain; a term
    # the fit zeroes but that costs nothing to restore is collinear with
    # another, not unwanted.
    print()
    print(f"{'term':<12}{'shipped':>9}{'fitted':>8}{'keeps':>14}  cost of shipped")
    for i, name in enumerate(TERMS):
        held = list(best)
        keeps = []
        for v in LADDER:
            held[i] = v
            if score(held, pairs=every) >= best_score - args.slack:
                keeps.append(v)
        held[i] = shipped[i]
        gave = best_score - score(held, every)
        span = f"{keeps[0]}..{keeps[-1]}" if keeps else "-"
        note = ("free" if len(keeps) == len(LADDER) else
                f"-{gave}" if gave > 0 else "none")
        print(f"{name:<12}{shipped[i]:>9}{best[i]:>8}{span:>14}  {note}")

    print()
    print(f"{'chart':<18}{'pairs':>7}{'shipped':>9}{'fitted':>8}{'ceiling':>9}")
    for name, ps in sorted(per_chart.items()):
        if not ps:
            continue
        print(f"{name:<18}{len(ps):>7}{score(shipped, ps):>9}"
              f"{score(best, ps):>8}{len(ps) - unreachable(ps):>9}")

    if args.json:
        json.dump({"shipped": shipped, "fitted": best,
                   "pairs": len(every), "unreachable": floor,
                   "shipped_score": score(shipped, every), "fitted_score": best_score},
                  sys.stdout, indent=2)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
