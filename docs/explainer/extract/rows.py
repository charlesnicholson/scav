"""rows.json: brew's and tcp's Level 2 rows 0-15, each unsearched and searched."""

import time

import common as X

NOTES = [
    "Level 2 is a table of 16 rows; row i is the profile with four bits flipped: bit 0 the box packer (trybox), bit 1 compaction on, bit 2 the DAR source (owner's hole), bit 3 fold always instead of fold-to-scale (src/layout/layout.cpp search_tuple). Row 0 is the caller's own tuple.",
    "unsearched = `--portfolio-row N --no-search`: the row laid out whole from no pins (portfolio_k = 0, so Level 1 does not run). This is the Level 2 table entry as scored before any move.",
    "searched = `--portfolio-row N`: a table of one row, then everything the full run does to a row: Level 1 greedy rounds to convergence, the kick schedules (iterated local search), and the second pass that adds fold moves. The full run does this for every viable row and ships the cheapest (t0 first, then t2; ties to the lower index).",
    "level2_pick_unsearched = `--no-search` with no row: Level 2 alone picks the cheapest unsearched row.",
    "costs compare lexicographically: t0 (tier-0 violation count) first, then t2. scav also keeps a t1_hints field internally that the dump does not print, so t1 is null throughout."]


def tuple_of(i: int) -> dict:
    return {"index": i, "box_packer_flipped": bool(i & 1), "compaction": bool(i & 2),
            "dar": "owner_hole" if i & 4 else "profile", "fold": "always" if i & 8 else "scale"}


def build(scav: X.Scav) -> dict:
    res = {"notes": NOTES, "charts": {}}
    for name in ["brew", "tcp"]:
        t = time.time()
        shipped, _ = X.geometry(scav, name, with_svg=False)
        pick, _ = X.geometry(scav, name, ["--no-search"], with_svg=False)
        rows = []
        for i in range(16):
            un, _ = X.geometry(scav, name, ["--portfolio-row", str(i), "--no-search"])
            se, _ = X.geometry(scav, name, ["--portfolio-row", str(i)])
            rows.append({"row": i, "tuple": tuple_of(i), "unsearched": un, "searched": se})
        best = min(range(16), key=lambda i: (rows[i]["searched"]["cost"]["t0"],
                                             rows[i]["searched"]["cost"]["t2"], i))
        res["charts"][name] = {
            "shipped_pins": shipped["pins"], "shipped_cost": shipped["cost"],
            "shipped_row": int(X.pin_list(shipped["pins"])[0][1]),
            "best_searched_row": best,
            "level2_pick_unsearched": {"pins": pick["pins"], "cost": pick["cost"],
                                       "row": int(X.pin_list(pick["pins"])[0][1])},
            "rows": rows}
        c = res["charts"][name]
        print(f"  {name} {time.time() - t:.1f}s shipped row {c['shipped_row']}, best searched {best}, "
              f"Level 2 pick {c['level2_pick_unsearched']['row']}", flush=True)
    return res
