"""search.json: brew's search path step by step, tcp's Level 1 path, and their rounds.

The steps follow the search trace's own order; each incumbent is laid out again from its
pins, and `verified` says the re-derived cost equals the trace's. The kick steps and trace
round numbers below are read from brew's trace at the recorded binary.
"""

import collections

import common as X

METHOD = {
    "order": "(a) the trace: `scav dump --json --layout --portfolio-row R --trace --trace-search` -- every Level 1 round's candidates are emitted back to back (candidate_scored + candidate_terms) after the round is scored, so a round is a run of consecutive candidate events; the last `taken` in a round is the move the round takes (`taken` marks each new running best in enumeration order).",
    "geometry": "(b) every incumbent is re-derived with `--portfolio-row R --no-search` plus that incumbent's pins; `verified` says the re-derived t0/t2 equal the trace's score for that move.",
    "kicks": "A kick (iterated local search) is not a candidate_scored event: it re-decides one frame by dropping that frame's rank/cut/face/side/fold pins, adds a reversal, a turn to run down (--orient) or a refold, and searches that frame again. The kick steps below are named from src/layout/layout.cpp kick_rounds and checked by re-derivation.",
    "share_order": X.TERMS}
KICK_ROUND_1 = "every kick of the first kick round, converged; kicks scored on frames other than the one kicked are scoped to that frame. Frames 0 and 2 improve on 6473, so their winners are searched together: 8240->5628, dearer than orient 0 alone, which is taken; orient 2 rested on top of it stays at 5628 and is dropped. Start/converged costs: reverse t1 19538->6684, reverse t9 9736->6961, orient 0 14148->5595, orient 1 6643 (no move improves), orient 2 6401 (no move improves), refold 0 ->5701, refold 1 ->6688, refold 2 ->6473. Kick-start costs re-derived with --no-search."
KICKS_TRIED = [(4, 8, "reverse t1 leg 0 (frame 0)"), (9, 12, "reverse t9 leg 0 (frame 0)"),
               (13, 15, "orient frame 0 down"), (16, 16, "orient frame 1 down"),
               (17, 17, "orient frame 2 down"), (18, 20, "refold frame 0"),
               (21, 22, "refold frame 1"), (23, 24, "refold frame 2"),
               (25, 27, "together: orient frame 0 + orient frame 2"),
               (28, 28, "rest: orient frame 2 on top of orient frame 0")]


PER_EM = {"corridor": 1, "excess_len": 1, "label_near": 1, "aspect": 1, "crowding": 1, "length": 1,
          "area": 2, "whitespace": 2}  # cost.cpp weighted_terms: lengths in ems, areas in ems squared, by ceiling


def weighted(terms: dict, prof: dict) -> int:
    """The Tier 2 sum under the dump's profile weights and em."""
    return sum(prof["w_" + k] * -(-terms[k] // prof["em"] ** PER_EM.get(k, 0)) for k in X.TERMS)


def with_bound(scav: X.Scav, row: list, base: list, m: dict) -> dict:
    """`m` laid out again on `base`, with the lower bound a labelled round scores it by
    (layout.cpp scored_of unlabelled): no label terms, and aspect unpriced unless every label fits."""
    g, doc = X.geometry(scav, "brew", row + base + pin_of(m), with_svg=False)
    c, chart, prof = doc["geometry"]["cost"], doc["geometry"]["chart"], doc["geometry"]["profile"]
    if weighted(c, prof) != c["t2"]:
        raise RuntimeError(f"brew {pin_of(m)}: weights give {weighted(c, prof)}, scav {c['t2']}")
    b = dict(c, label=0, label_near=0)
    if not all(r[2] <= chart[2] and r[3] <= chart[3] for r in doc["geometry"]["placed"]):
        b["aspect"] = 0
    xs = [v for r in doc["geometry"]["state"] for v in (r[0], r[0] + r[2])]
    xs += [q[0] for leg in doc["geometry"]["route"] for q in leg]
    ys = [v for r in doc["geometry"]["state"] for v in (r[1], r[1] + r[3])]
    ys += [q[1] for leg in doc["geometry"]["route"] for q in leg]
    inside = all(r[0] >= min(xs) and r[1] >= min(ys) and r[0] + r[2] <= max(xs) and r[1] + r[3] <= max(ys)
                 for r in doc["geometry"]["placed"])
    return dict(m, bound=[c["t0_violations"] - c["tier0"]["label_over_box"] - c["tier0"]["label_over_route"],
                         weighted(b, prof)],
                inside=inside, rederived=(g["cost"]["t0"], g["cost"]["t2"]) == (m["t0"], m["t2"]))


def pin_of(m: dict) -> list | None:
    k = m["move"]
    if k == "rank":
        return ["--rank", f"{m['state_id']}:{m['rank']}"]
    e = 0 if m.get("end") == "src" else 1
    if k in ("face", "side"):
        return ["--end", f"{m['trans']}:{m['leg']}:{e}:{m[k]}"]
    if k == "cut":
        return ["--cut", f"{m['trans']}:{m['leg']}"]
    if k == "reverse":
        return ["--reverse", f"{m['trans']}:{m['leg']}"]
    if k == "loop":
        return ["--loop", f"{m['state_id']}:{m['face']}:{m['end']}"]
    return None


def describe(m: dict, model: dict) -> str:
    tr = model["transitions"]
    paths = X.state_paths(model)
    if m["move"] == "rank":
        return f"pin {paths[m['state_id']]} to rank {m['rank']}"
    if m["move"] == "fold":
        return f"refold frame {m.get('frame')} at layer {m.get('layer')}"
    if m["move"] == "loop":
        return (f"put {paths[m['state_id']]}'s loop room on face {m['face']} end {m['end']}"
                " (face 0 left,1 right,2 top,3 bottom; end 0 leading,1 trailing)")
    t = tr[m["trans"]]
    what = f"t{m['trans']} {paths[t['src']]} -> {paths[t['dst']]}"
    if m["move"] == "face":
        return f"turn {m['end']} end of {what} leg {m['leg']} to face {m['face']} (0 left,1 right,2 top,3 bottom)"
    if m["move"] == "side":
        return f"move {m['end']} end of {what} leg {m['leg']} to side {m['side']}"
    return f"{m['move']} {what} leg {m['leg']}"


def round_summary(r: dict, model: dict, full=False) -> dict:
    c = r["cands"]
    viable = [x for x in c if x["verdict"] != "not_viable"]
    taken = [x for x in c if x["verdict"] == "taken"]
    out = {"first_event": r["first_event"], "n": len(c),
           "by_verdict": dict(collections.Counter(x["verdict"] for x in c)),
           "by_move": dict(collections.Counter(x["move"] for x in c)),
           "taken": ({k: v for k, v in taken[-1].items() if k != "share"} if taken else None),
           "best": min(((x["t0"], x["t2"]) for x in viable), default=None)}
    if full:
        out["candidates"] = [dict(x, what=describe(x, model)) for x in c]
    return out


def taken_of(r: dict) -> dict:
    return [x for x in r["cands"] if x["verdict"] == "taken"][-1]


def brew(scav: X.Scav) -> dict:
    rounds, model = scav.search_rounds("brew", ["--portfolio-row", "0"])
    row = ["--portfolio-row", "0", "--no-search"]
    steps = []

    def step(label, pins, rnd=None, move=None, kind="level1", expect=None, note=None):
        g, _ = X.geometry(scav, "brew", row + pins)
        s = {"step": len(steps), "kind": kind, "label": label, "pins_added": pins, "geometry": g,
             "cost": g["cost"]}
        if rnd is not None:
            s["trace_round"] = rnd
            s["round"] = round_summary(rounds[rnd], model)
        if move is not None:
            s["move"] = move
            s["what"] = describe(move, model)
        if expect is not None:
            s["verified"] = (g["cost"]["t0"], g["cost"]["t2"]) == tuple(expect)
            s["trace_cost"] = list(expect)
        if note:
            s["note"] = note
        steps.append(s)

    step("row 0 as Level 2 lays it out, no moves", [], kind="start", expect=(4, 16866),
         note="the unsearched row; t0 4 = four tier-0 violations")
    pins = []
    for ri in (0, 1, 2):
        m = taken_of(rounds[ri])
        pins = pins + pin_of(m)
        step(f"Level 1 round {ri + 1}: best of {len(rounds[ri]['cands'])} moves", pins, ri, m,
             expect=(m["t0"], m["t2"]))
    conv = {"trace_round": 3, "round": round_summary(rounds[3], model),
            "note": f"round 4 scores {len(rounds[3]['cands'])} moves and none is strictly better: Level 1 has converged at t2 6473"}
    kick_pins = ["--orient", "0"]
    step("kick: turn frame 0 (the root) to run down and re-decide it", kick_pins, 13, None, kind="kick",
         expect=(0, 14148), note="frame 0's rank and face pins are dropped (warm start keeps only reversals and orientations), so the turned frame is searched again from 14148")
    pins = list(kick_pins)
    for ri in (13, 14):
        m = taken_of(rounds[ri])
        pins = pins + pin_of(m)
        step(f"kick search round {ri - 12} in frame 0: best of {len(rounds[ri]['cands'])} moves", pins, ri, m,
             kind="kick_search", expect=(m["t0"], m["t2"]))
    settle = {"trace_round": 43, "round": round_summary(rounds[43], model),
              "note": f"after a second kick round finds nothing below 5595, one unscoped settling pass scores {len(rounds[43]['cands'])} moves; none is better, so 5595 ships"}
    kicks_tried = [{"kick": what, "trace_rounds": [lo, hi],
                    "rounds": [round_summary(rounds[i], model) for i in range(lo, hi + 1)]}
                   for lo, hi, what in KICKS_TRIED]
    shipped, _ = X.geometry(scav, "brew")
    res = {"row": 0, "shipped_pins": shipped["pins"], "shipped_cost": shipped["cost"],
           "order_source": "trace (a), each step re-derived (b)",
           "steps": steps, "level1_converged": conv, "settle": settle,
           "kick_round_1": {"note": KICK_ROUND_1, "kicks": kicks_tried},
           "all_rounds": [round_summary(r, model) for r in rounds],
           "round_1_candidates": round_summary(rounds[0], model, full=True),
           "round_2_candidates": round_summary(rounds[1], model, full=True)}
    for key, base in (("round_1_candidates", []), ("round_2_candidates", steps[1]["pins_added"])):
        res[key]["candidates"] = [with_bound(scav, row, base, m) for m in res[key]["candidates"]]

    top = sorted([x for x in rounds[0]["cands"] if x["verdict"] != "not_viable"],
                 key=lambda x: (x["t0"], x["t2"]))[:6]
    drawn = []
    for m in top:
        p = pin_of(m)
        if p is None:
            continue
        g, _ = X.geometry(scav, "brew", row + p)
        drawn.append({"move": m, "what": describe(m, model), "pins_added": p, "geometry": g,
                      "verified": (g["cost"]["t0"], g["cost"]["t2"]) == (m["t0"], m["t2"])})
    res["round_1_top"] = drawn
    print("  brew steps", [(s["cost"]["t0"], s["cost"]["t2"], s.get("verified")) for s in steps], flush=True)
    return res


def tcp(scav: X.Scav) -> dict:
    rounds, model = scav.search_rounds("tcp", ["--portfolio-row", "8"])
    row = ["--portfolio-row", "8", "--no-search"]
    g, _ = X.geometry(scav, "tcp", row)
    steps = [{"step": 0, "kind": "start", "label": "row 8 as Level 2 lays it out", "pins_added": [],
              "geometry": g, "cost": g["cost"], "verified": (g["cost"]["t0"], g["cost"]["t2"]) == (4, 19418)}]
    pins = []
    ri = 0
    while any(x["verdict"] == "taken" for x in rounds[ri]["cands"]):
        m = taken_of(rounds[ri])
        pins = pins + pin_of(m)
        g, _ = X.geometry(scav, "tcp", row + pins)
        steps.append({"step": len(steps), "kind": "level1",
                      "label": f"Level 1 round {ri + 1}: best of {len(rounds[ri]['cands'])} moves",
                      "pins_added": pins, "move": m, "what": describe(m, model), "trace_round": ri,
                      "round": round_summary(rounds[ri], model), "geometry": g, "cost": g["cost"],
                      "verified": (g["cost"]["t0"], g["cost"]["t2"]) == (m["t0"], m["t2"])})
        ri += 1
    shipped, _ = X.geometry(scav, "tcp")
    steps.append({"step": len(steps), "kind": "shipped", "label": "after the kick schedules and the fold pass",
                  "pins_added": shipped["pin_list"], "geometry": shipped, "cost": shipped["cost"],
                  "note": "the kick phase is not decomposed for tcp: its trace holds %d rounds after Level 1 converges; see all_rounds" % (len(rounds) - ri - 1)})
    print("  tcp steps", [(s["cost"]["t0"], s["cost"]["t2"], s.get("verified")) for s in steps], flush=True)
    return {"row": 8, "shipped_pins": shipped["pins"], "shipped_cost": shipped["cost"],
            "order_source": "trace (a) for Level 1, each step re-derived (b); kick phase summarised only",
            "steps": steps, "level1_converged": {"trace_round": ri, "round": round_summary(rounds[ri], model)},
            "all_rounds": [round_summary(r, model) for r in rounds],
            "round_1_candidates": round_summary(rounds[0], model, full=True)}


def build(scav: X.Scav) -> dict:
    return {"method": METHOD, "charts": {"brew": brew(scav), "tcp": tcp(scav)}}
