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
KICK_ROUND_1 = "every kick of the first kick round, converged; kicks scored on frames other than the one kicked are scoped to that frame. Start/converged costs: reverse t1 18261->3649, reverse t9 7222->4108, reverse t10 12903->4039, orient 1 4361 (no move improves), orient 2 4171 (no move improves), refold 0 16985->3996, refold 1 ->4433, refold 2 ->4225. Kick-start costs for orient 1/2 re-derived with --no-search."
KICKS_TRIED = [(4, 7, "reverse t1 leg 0 (frame 0)"), (8, 11, "reverse t9 leg 0 (frame 0)"),
               (12, 14, "reverse t10 leg 0 (frame 0)"), (15, 15, "orient frame 1 down"),
               (16, 16, "orient frame 2 down"), (17, 20, "refold frame 0"),
               (21, 22, "refold frame 1"), (23, 24, "refold frame 2"),
               (25, 30, "together: reverse t1 + orient frame 2")]


def pin_of(m: dict, ids: dict) -> list | None:
    k = m["move"]
    if k == "rank":
        s = m["state"]
        sid = int(s[1:]) if s.startswith("#") else ids[s]
        return ["--rank", f"{sid}:{m['rank']}"]
    e = 0 if m.get("end") == "src" else 1
    if k == "face":
        return ["--face", f"{m['trans']}:{m['leg']}:{e}:{m['face']}"]
    if k == "side":
        return ["--side", f"{m['trans']}:{m['leg']}:{e}:{m['side']}"]
    if k == "cut":
        return ["--cut", f"{m['trans']}:{m['leg']}"]
    if k == "reverse":
        return ["--reverse", f"{m['trans']}:{m['leg']}"]
    return None


def describe(m: dict, model: dict) -> str:
    st, tr = model["states"], model["transitions"]
    paths = X.state_paths(model)
    if m["move"] == "rank":
        s = m["state"]
        sid = int(s[1:]) if s.startswith("#") else [i for i, x in enumerate(st) if x["name"] == s][0]
        return f"pin {paths[sid]} to rank {m['rank']}"
    if m["move"] == "fold":
        return f"refold frame {m.get('frame')} at layer {m.get('layer')}"
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
    ids = {s["name"]: i for i, s in enumerate(model["states"]) if s["name"]}
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

    step("row 0 as Level 2 lays it out, no moves", [], kind="start", expect=(4, 16985),
         note="the unsearched row; t0 4 = four tier-0 violations")
    pins = []
    for ri in (0, 1, 2):
        m = taken_of(rounds[ri])
        pins = pins + pin_of(m, ids)
        step(f"Level 1 round {ri + 1}: best of {len(rounds[ri]['cands'])} moves", pins, ri, m,
             expect=(m["t0"], m["t2"]))
    conv = {"trace_round": 3, "round": round_summary(rounds[3], model),
            "note": "round 4 scores 31 moves and none is strictly better: Level 1 has converged at t2 4225"}
    kick_pins = ["--reverse", "1:0"]
    step("kick: reverse t1 (Off -> SelfCheck) and re-decide frame 0", kick_pins, 4, None, kind="kick",
         expect=(2, 18261), note="frame 0's rank and face pins are dropped (warm start keeps only reversals and orientations); the reversal alone breaks tier 0 (t0 2), so the frame is searched again")
    pins = list(kick_pins)
    for ri in (4, 5, 6):
        m = taken_of(rounds[ri])
        pins = pins + pin_of(m, ids)
        step(f"kick search round {ri - 3} in frame 0: best of {len(rounds[ri]['cands'])} moves", pins, ri, m,
             kind="kick_search", expect=(m["t0"], m["t2"]))
    step("kick: turn frame 2 (grinder) to run down, on top", pins + ["--orient", "2"], None, None, kind="kick",
         expect=(0, 3595),
         note="frame 2's best kick (alone from the Level 1 optimum it reaches 4171); the two frames' winners searched together reach only 5530 (trace rounds 25-30), so the cheaper single kick is taken and the other frame's kick is added on top")
    settle = {"trace_round": 54, "round": round_summary(rounds[54], model),
              "note": "after a second kick round finds nothing below 3595, one unscoped settling pass scores 30 moves; none is better, so 3595 ships"}
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

    top = sorted([x for x in rounds[0]["cands"] if x["verdict"] != "not_viable"],
                 key=lambda x: (x["t0"], x["t2"]))[:6]
    drawn = []
    for m in top:
        p = pin_of(m, ids)
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
    ids = {s["name"]: i for i, s in enumerate(model["states"]) if s["name"]}
    row = ["--portfolio-row", "8", "--no-search"]
    g, _ = X.geometry(scav, "tcp", row)
    steps = [{"step": 0, "kind": "start", "label": "row 8 as Level 2 lays it out", "pins_added": [],
              "geometry": g, "cost": g["cost"], "verified": (g["cost"]["t0"], g["cost"]["t2"]) == (4, 18120)}]
    pins = []
    ri = 0
    while any(x["verdict"] == "taken" for x in rounds[ri]["cands"]):
        m = taken_of(rounds[ri])
        pins = pins + pin_of(m, ids)
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
