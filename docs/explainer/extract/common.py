"""Runs scav and reads its dump, SVG and trace into the explainer's schema."""

import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CHARTS = Path("test_data/charts")  # relative to REPO_ROOT, scav's cwd, so dumps name documents that way

TIER0 = ["through_box", "box_overlap", "flush", "through_region", "retrace",
         "label_over_box", "label_over_route"]
TERMS = ["bends", "corridor", "crossings", "excess_len", "adjacency", "label",
         "label_near", "aspect", "area", "crowding", "length", "transit_bends",
         "whitespace"]


@dataclass
class Scav:
    """One binary, the scratch directory its SVGs go to, and a cap on each run."""
    binary: Path
    scratch: Path
    label: str
    timeout: float = 240

    def run(self, argv: list) -> str:
        p = subprocess.run([str(self.binary), *(str(a) for a in argv)], capture_output=True,
                           encoding="utf-8", cwd=REPO_ROOT, timeout=self.timeout, check=False)
        if p.returncode != 0:
            raise RuntimeError(f"scav {' '.join(map(str, argv))}: rc {p.returncode}: {p.stderr[:400]}")
        return p.stdout

    def dump_json(self, name: str, flags=()) -> dict:
        return json.loads(self.run(["dump", "--json", "--layout", *flags, chart_path(name)]))

    def render(self, name: str, flags=()) -> str:
        out = self.scratch / f"{name}.svg"
        self.run(["render", "-o", out, *flags, chart_path(name)])
        return out.read_text(encoding="utf-8")

    def trace(self, name: str, flags=()) -> tuple[list, dict]:
        """The shipped drawing's events, and the dump they are read against."""
        out = self.run(["dump", "--json", "--layout", *flags, "--trace", chart_path(name)])
        end = out.index("\n]\n") + 3
        return json.loads(out[:end]), json.loads(out[end:])

    def search_rounds(self, name: str, flags=()) -> tuple[list, dict]:
        """Level 1 rounds from the search's trace, and the dump. A round is one run of
        consecutive candidate events, which the reduction loop emits back to back."""
        out = self.run(["dump", "--json", "--layout", *flags, "--trace", "--trace-search",
                        chart_path(name)])
        end = out.index("\n]\n") + 3
        rounds, cur, last_i = [], None, None
        for line in out[:end].splitlines():
            if '"candidate_' not in line:
                continue
            e = json.loads(line.strip().rstrip(","))
            if cur is None or e["i"] != last_i + 1:
                cur = {"first_event": e["i"], "cands": []}
                rounds.append(cur)
            last_i = e["i"]
            if e["kind"] == "candidate_scored":
                e.pop("kind")
                if e.get("row") == 0xFFFFFFFF:
                    e.pop("row")
                cur["cands"].append(e)
            else:
                cur["cands"][-1]["share"] = e["share"]
        return rounds, json.loads(out[end:])


def chart_path(name: str) -> Path:
    return CHARTS / f"{name}.scav"


ATTR = re.compile(r'([a-zA-Z0-9-]+)="([^"]*)"')
ELEM = re.compile(r'<(rect|circle|polygon|polyline|line|text)\s([^>]*?)(/>|>([^<]*)</text>)')
NUM = {"x", "y", "width", "height", "rx", "cx", "cy", "r", "x1", "y1", "x2", "y2",
       "font-size", "textLength", "stroke-width"}


def pts(s: str) -> list:
    return [[int(v) for v in p.split(",")] for p in s.split()]


def parse_svg(svg: str) -> dict:
    """Every drawn primitive, in paint order, with the element it draws."""
    head = re.search(r'<svg[^>]*width="(\d+)" height="(\d+)" viewBox="(-?\d+) (-?\d+) (\d+) (\d+)"', svg)
    style = re.search(r"<style>(.*?)</style>", svg, re.S)
    prims = []
    for z, m in enumerate(ELEM.finditer(svg)):
        el, attrs, _, txt = m.groups()
        a = dict(ATTR.findall(attrs))
        cls = a.pop("class", "")
        cm = re.match(r"scav-(state|trans|sub) scav-id-(\d+)", cls)
        p = {"el": el, "z": z}
        if cm:
            p["of"], p["id"] = cm.group(1), int(cm.group(2))
        for k, v in a.items():
            if k == "points":
                p["points"] = pts(v)
            elif k in NUM:
                p[k] = int(v) if re.fullmatch(r"-?\d+", v) else float(v)
            else:
                p[k] = v
        if el == "text":
            p["text"] = txt
        prims.append(p)
    return {"size": [int(head.group(1)), int(head.group(2))],
            "viewbox": [int(head.group(i)) for i in range(3, 7)],
            "style": style.group(1).strip() if style else "",
            "prims": prims}


def pin_list(rests_on: str) -> list:
    """The rests-on string as [[flag, arg], ...]."""
    toks = rests_on.split()
    out, i = [], 0
    while i < len(toks):
        if i + 1 < len(toks) and not toks[i + 1].startswith("--"):
            out.append([toks[i], toks[i + 1]])
            i += 2
        else:
            out.append([toks[i]])
            i += 1
    return out


def state_paths(doc: dict) -> list:
    states, subs = doc["states"], doc["submachines"]
    memo = {}

    def name_of(i):
        return states[i]["name"] or "$" + states[i]["kind"]

    def path(i):
        if i in memo:
            return memo[i]
        sub = subs[states[i]["parent"]]
        if sub["owner"] is None:
            p = name_of(i)
        else:
            p = path(sub["owner"]) + (":" + sub["name"] if sub["name"] else "") + "/" + name_of(i)
        memo[i] = p
        return p

    return [path(i) for i in range(len(states))]


def model_summary(doc: dict) -> dict:
    paths = state_paths(doc)
    return {
        "name": doc["chart"]["name"], "label": doc["chart"]["label"],
        "documents": [d["path"] for d in doc["documents"]],
        "includes": doc.get("includes", []),
        "attrs": doc.get("attrs", []),
        "states": [{"id": i, "name": s["name"], "label": s["label"], "kind": s["kind"],
                    "path": paths[i], "parent_sub": s["parent"], "submachines": s["submachines"],
                    "inst": s["inst"], "live": s["live"]} for i, s in enumerate(doc["states"])],
        "submachines": [{"id": i, "name": m["name"], "label": m["label"], "owner": m["owner"],
                         "region_index": m["ordinal"], "children": m["children"], "inst": m["inst"],
                         "live": m["live"]} for i, m in enumerate(doc["submachines"])],
        "transitions": [{"id": i, "src": t["src"], "dst": t["dst"], "src_path": paths[t["src"]],
                         "dst_path": paths[t["dst"]], "kind": t["kind"], "label": t["label"],
                         "inst": t["inst"], "live": t["live"]}
                        for i, t in enumerate(doc["transitions"])],
    }


def cost_of(g: dict) -> dict:
    c = g["cost"]
    return {"t0": c["t0_violations"], "t1": None, "t2": c["t2"],
            "tier0": {k: c[k] for k in TIER0},
            "terms": {k: c[k] for k in TERMS},
            "shares_bp": dict(zip(TERMS, c["shares_bp"]))}


def downs_from_pins(rests_on: str) -> set:
    return {int(p[1]) for p in pin_list(rests_on) if p[0] == "--orient"}


def geometry(scav: Scav, name: str, flags=(), scale="text", with_svg=True) -> tuple[dict, dict]:
    """One laid-out chart in the explainer schema, and the dump it came from."""
    flags = list(flags) + (["--no-text"] if scale == "notext" and "--no-text" not in flags else [])
    doc = scav.dump_json(name, flags)
    g = doc["geometry"]
    paths = state_paths(doc)
    down = downs_from_pins(g["rests_on"])
    rec = {"chart": name, "scale": scale, "flags": flags, "pins": g["rests_on"],
           "pin_list": pin_list(g["rests_on"]),
           "box": g["chart"], "hash": {"structural": g["structural_hash"],
                                      "coordinate": g["coordinate_hash"]},
           "cost": cost_of(g), "label_leader": g["label_leader"]}
    placed = dict(zip(g["placed_subject"], g["placed"]))
    prims = parse_svg(scav.render(name, flags)) if with_svg else None
    by = {}
    if prims:
        rec["svg"] = {"size": prims["size"], "viewbox": prims["viewbox"], "style": prims["style"]}
        for p in prims["prims"]:
            by.setdefault((p.get("of"), p.get("id")), []).append(p)
        rec["svg"]["unowned"] = by.get((None, None), [])
    rec["states"] = []
    for i, s in enumerate(doc["states"]):
        draw = by.get(("state", i), [])
        rec["states"].append({
            "id": i, "name": s["name"], "label": s["label"], "path": paths[i], "kind": s["kind"],
            "parent_sub": s["parent"], "submachines": s["submachines"], "live": s["live"],
            "rect": g["state"][i], "before": g["state_before"][i], "after": g["state_after"][i],
            "text": [p["text"] for p in draw if p["el"] == "text"],
            "draw": draw})
    rec["subs"] = []
    for i, m in enumerate(doc["submachines"]):
        rec["subs"].append({
            "id": i, "name": m["name"], "label": m["label"], "owner": m["owner"],
            "region_index": m["ordinal"], "children": m["children"], "live": m["live"],
            "rect": g["sub"][i], "down": i in down, "draw": by.get(("sub", i), [])})
    rec["trans"] = []
    for i, t in enumerate(doc["transitions"]):
        rec["trans"].append({
            "id": i, "src": t["src"], "dst": t["dst"], "src_path": paths[t["src"]],
            "dst_path": paths[t["dst"]], "label": t["label"], "kind": t["kind"],
            "live": t["live"], "points": g["route"][i] if i < len(g["route"]) else [],
            "ports": [{"at": [p[0], p[1]], "side": p[2], "depth": p[3]}
                      for p in (g["port"][i] if i < len(g["port"]) else [])],
            "label_box": placed.get(i),
            "draw": by.get(("trans", i), [])})
    return rec, doc


def ranks_from_trace(events: list, doc: dict, down=()) -> list:
    """Per-frame ranks and in-rank order, from the shipped drawing's own trace."""
    def sid_of(frame, name):
        for c in doc["submachines"][frame]["children"]:  # names are unique within one frame only
            if (doc["states"][c]["name"] or f"#{c}") == name:
                return c
        return None

    frames = {}
    for e in events:
        k = e["kind"]
        if k in ("rank_assigned", "rank_pinned"):
            f = frames.setdefault(e["frame"], {"rank_of": {}, "pinned": {}, "at": {}})
            sid = sid_of(e["frame"], e["state"])
            if k == "rank_assigned":
                f["rank_of"].setdefault(sid, e["rank"])
                f.setdefault("assigned", {})[sid] = e["rank"]
            else:
                f["pinned"][sid] = e["rank"]
        if k == "node_placed" and "state" in e:
            f = frames.setdefault(e["frame"], {"rank_of": {}, "pinned": {}, "at": {}})
            f["at"][sid_of(e["frame"], e["state"])] = e["at"]
    out = []
    for fr in sorted(frames):
        f = frames[fr]
        final = dict(f.get("assigned", {}))
        final.update(f["pinned"])
        if f["pinned"]:
            # Re-seats each initial one rank before the nearest state it touches, then squeezes empty ranks.
            for sid in list(final):
                if doc["states"][sid]["kind"] != "initial":
                    continue
                ends = ([final[t["dst"]] for t in doc["transitions"]
                         if t["src"] == sid and t["dst"] != sid and t["dst"] in final] +
                        [final[t["src"]] for t in doc["transitions"]
                         if t["dst"] == sid and t["src"] != sid and t["src"] in final])
                if not ends:
                    continue
                near = min(ends)
                if near == 0:
                    for u in final:
                        if u != sid:
                            final[u] += 1
                    near = 1
                final[sid] = near - 1
            used = sorted(set(final.values()))
            final = {k: used.index(v) for k, v in final.items()}
        by_rank = {}
        for sid, r in final.items():
            by_rank.setdefault(r, []).append(sid)
        rk = []
        for r in sorted(by_rank):
            ids = by_rank[r]
            ax = 0 if fr in down else 1
            ids.sort(key=lambda s: (f["at"].get(s, [0, 0])[ax], f["at"].get(s, [0, 0])[1 - ax]))
            rk.append({"rank": r, "states": ids,
                       "paths": [doc["states"][s]["name"] or f"#{s}" for s in ids]})
        out.append({"frame": fr, "ranks": rk,
                    "assigned": {str(k): v for k, v in f.get("assigned", {}).items()},
                    "pinned": {str(k): v for k, v in f["pinned"].items()},
                    "node_at": {str(k): v for k, v in f["at"].items()}})
    return out
