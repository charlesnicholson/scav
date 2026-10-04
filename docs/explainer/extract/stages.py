"""stages.json: brew and toolchanger stage by stage, from the shipped drawing's trace."""

import collections

import common as X

NOTES = [
    "Pipeline per candidate (src/layout/layout.cpp search_candidate): decompose (once per chart) -> order every frame (phase 1: cycle breaking, ranks, chaining) -> size (pack frames bottom-up into rects) -> facing pass (ports turned toward where their routes go; if that flips any edge, order and size again) -> route every frame's nets (seats, waypoints, nudged lanes) -> place labels. Everything here is the shipped drawing re-derived under `--trace` (shipped scope: one thread, no search).",
    "decomposition: frames are submachines (subs). A transition whose ends lie in different frames is split into one segment per frame it passes through; `ports` are where its route crosses a composite border (side 0 left, 1 right, 2 top, 3 bottom; depth = nesting depth of the border crossed) and `boundary_nodes` are the node each inner segment starts or ends at on its frame's edge (node_placed with bend_of_seg).",
    "ordering: ranks are rank_assigned (longest path) overridden by rank_pinned (the --rank pins), then, where a frame has pins, each initial re-seated one rank before the state it touches and empty ranks squeezed, as order.cpp does after the pins. In-rank order is NOT emitted by the trace: it is recovered here from the sizing pass's node_placed cross coordinate (y in a frame running across, x in a frame turned down), which is the order sizing kept. `reversed` lists cycle-breaking flips and --reverse pins. `chained` (edge_chained, bend nodes for multi-rank segments) is empty when no segment spans more than one rank. gap_charged is the width a segment's label (or its lanes) asked of the rank boundary after rank `boundary`.",
    "sizing: node_placed `at` is (rank-axis start, cross-axis centre) in chart coordinates -- (x, centre y) across, (centre x, y) down. Each pass ends at its run of node_placed; a second pass is the re-size after the facing pass flipped something (toolchanger has two, brew one). Final rects are the sized rects: routing and labels do not move boxes.",
    "routing: net_planned src/dst are the node centres a net runs between before seating; `seats` are 11.5's passes moving each end (attach to the border, reface, align, spread, separate, nudge, loop); In the attach pass a seat's `from` is the point the end aims at (the code sets `was = toward`), not a previous seat. lanes are corridor lanes from nudging. `routes` are the final polylines (stopping one arrowhead length, 96, short of the target border).",
    "labels: `box` is the placed label box (the dump's `placed`); `text` is the SVG text drawn in it. An internal self-transition's route is a loop inside its state, in the state's loop room, and its box sits beside the loop's far leg. `leader` is the dump's `label leader` value."]
SIZING_KINDS = {"fold_pinned", "fold_cut", "boundary_carried", "piece_packed", "pseudostate_seated",
                "column_centred", "port_attached", "node_placed", "spacing_inflated", "port_turned"}


def on_border(pt: list, r: list) -> bool:
    x, y, w, h = r
    return ((pt[0] in (x, x + w) and y <= pt[1] <= y + h) or
            (pt[1] in (y, y + h) and x <= pt[0] <= x + w))


def ancestors(doc: dict, s: int) -> list:
    out = []
    sub = doc["submachines"][doc["states"][s]["parent"]]
    while sub["owner"] is not None:
        out.append(sub["owner"])
        sub = doc["submachines"][doc["states"][sub["owner"]]["parent"]]
    return out


def stages(scav: X.Scav, name: str) -> dict:
    geo, doc = X.geometry(scav, name)
    ev, _ = scav.trace(name)
    paths = X.state_paths(doc)
    down = X.downs_from_pins(geo["pins"])

    seg_info = {}
    for e in ev:
        if e["kind"] == "net_planned" and e["seg"] not in seg_info:
            seg_info[e["seg"]] = {"seg": e["seg"], "trans": e["trans"], "frame": e["frame"]}
    by_trans = collections.defaultdict(list)
    for s in sorted(seg_info):
        by_trans[seg_info[s]["trans"]].append(s)
    for t, segs in by_trans.items():
        for k, s in enumerate(segs):
            seg_info[s]["leg"] = k
    split = []
    for t, segs in sorted(by_trans.items()):
        tr = geo["trans"][t]
        ports = []
        anc = set(ancestors(doc, tr["src"])) | set(ancestors(doc, tr["dst"]))
        for p in tr["ports"]:
            owner = [a for a in anc if on_border(p["at"], doc["geometry"]["state"][a])]
            ports.append(dict(p, border_of=[paths[a] for a in owner]))
        if len(segs) > 1 or ports:
            split.append({"trans": t, "src_path": tr["src_path"], "dst_path": tr["dst_path"],
                          "segments": segs, "frames": [seg_info[s]["frame"] for s in segs], "ports": ports})
    last_boundary = {}  # the last sizing pass's node per (segment, frame)
    for e in ev:
        if e["kind"] == "node_placed" and "bend_of_seg" in e:
            last_boundary[(e["bend_of_seg"], e["frame"])] = {"seg": e["bend_of_seg"], "frame": e["frame"],
                                                             "at": e["at"]}
    frames = [{"id": s["id"], "owner": s["owner"],
               "owner_path": paths[s["owner"]] if s["owner"] is not None else None,
               "name": s["name"], "region_index": s["region_index"], "rect": s["rect"], "down": s["down"],
               "states": list(s["children"])} for s in geo["subs"]]
    decomposition = {"frames": frames, "segments": [seg_info[s] for s in sorted(seg_info)],
                     "split_transitions": split, "boundary_nodes": list(last_boundary.values()),
                     "unrouted_transitions": [t["id"] for t in geo["trans"] if not t["points"]]}

    order = []
    for f in X.ranks_from_trace(ev, doc, down):
        fr = f["frame"]
        order.append({"frame": fr, "down": fr in down,
                      "reversed": [{"seg": e["seg"], "trans": seg_info.get(e["seg"], {}).get("trans")}
                                   for e in ev if e["kind"] == "edge_reversed" and e["frame"] == fr],
                      "ranks": f["ranks"], "assigned": f["assigned"], "pinned": f["pinned"],
                      "chained": [e for e in ev if e["kind"] == "edge_chained" and e.get("frame") == fr],
                      "gap_charged": [{"boundary": e["boundary"], "seg": e["seg"],
                                       "trans": seg_info.get(e["seg"], {}).get("trans"),
                                       "width": e["width"], "cause": e["cause"]}
                                      for e in ev if e["kind"] == "gap_charged" and e["frame"] == fr]})

    passes, cur, last = [], [], None  # a pass ends at its run of node_placed
    for e in ev:
        if e["kind"] in SIZING_KINDS:
            if last == "node_placed" and e["kind"] != "node_placed":
                passes.append(cur)
                cur = []
            cur.append(e)
            last = e["kind"]
    if cur:
        passes.append(cur)
    sizing = {"passes": passes,
              "rects": {str(s["id"]): {"path": s["path"], "rect": s["rect"], "before": s["before"],
                                       "after": s["after"], "lead": s["lead"], "trail": s["trail"]}
                        for s in geo["states"]},
              "subs": {str(s["id"]): s["rect"] for s in geo["subs"]},
              "box": geo["box"]}

    nets = []
    frame_nets = collections.defaultdict(list)
    cur_net = None
    for e in ev:
        k = e["kind"]
        if k == "net_planned":
            cur_net = {"frame": e["frame"], "seg": e["seg"], "trans": e["trans"], "src": e["src"],
                       "dst": e["dst"], "waypoints": [], "seats": [], "lanes": []}
            frame_nets[e["frame"]].append(cur_net)
            nets.append(cur_net)
        elif k == "net_waypoint" and cur_net is not None:
            cur_net["waypoints"].append(e["at"])
        elif k == "seat_moved":
            n = frame_nets[e["frame"]][e["net"]]
            n["seats"].append({"pass": e["pass"], "end": e["end"], "from": e["from"], "to": e["to"]})
        elif k == "lane_assigned":
            n = frame_nets[e["frame"]][e["net"]]
            n["lanes"].append({"lane": e["lane"], "at": e["at"]})
    routing = {"nets": nets, "routes": {str(t["id"]): t["points"] for t in geo["trans"]},
               "degraded": [e for e in ev if e["kind"] == "route_degraded"]}

    labels = {"leader": geo["label_leader"],
              "placed": [{"trans": t["id"], "label": t["label"], "box": t["label_box"],
                          "text": [p for p in t["draw"] if p["el"] == "text"]}
                         for t in geo["trans"] if t["label_box"]],
              "inline": [{"trans": t["id"], "label": t["label"], "kind": t["kind"],
                          "text": [p for p in t["draw"] if p["el"] == "text"]}
                         for t in geo["trans"] if not t["label_box"] and t["label"]]}
    return {"chart": name, "pins": geo["pins"], "cost": geo["cost"], "geometry": geo, "trace": ev,
            "decomposition": decomposition, "ordering": order, "sizing": sizing, "routing": routing,
            "labels": labels}


def build(scav: X.Scav) -> dict:
    res = {"notes": NOTES}
    for name in ["brew", "toolchanger"]:
        s = res[name] = stages(scav, name)
        print(f"  {name}: {len(s['decomposition']['frames'])} frames, "
              f"{len(s['decomposition']['split_transitions'])} split transitions, "
              f"{len(s['sizing']['passes'])} sizing passes, {len(s['routing']['nets'])} nets", flush=True)
    return res
