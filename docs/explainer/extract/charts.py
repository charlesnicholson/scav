"""charts.json: every chart the page draws, its source, model, geometry, paint and ranks."""

import time

import common as X

CHARTS = ["estop", "led", "brew", "dock", "tcp", "axis", "toolchanger", "bottler"]
NOTEXT = ("estop", "brew")


def build(scav: X.Scav) -> dict:
    res = {"schema": "explainer geometry v1; integers are scav grid units (1/16 pt)",
           "binary": scav.label,
           "source_of": {"geometry": "scav dump --json --layout [flags]",
                         "draw": "scav render [flags], parsed per element (class scav-state|trans|sub scav-id-N), paint order in z",
                         "ranks": "scav dump --json --layout --trace (shipped scope): rank_assigned/rank_pinned per frame, in-rank order by node_placed cross coordinate"},
           "charts": {}}
    for name in CHARTS:
        t = time.time()
        rec, doc = X.geometry(scav, name)
        ev, _ = scav.trace(name)
        rec["ranks"] = X.ranks_from_trace(ev, doc, X.downs_from_pins(rec["pins"]))
        entry = {"source": (X.REPO_ROOT / X.chart_path(name)).read_text(encoding="utf-8"),
                 "model": X.model_summary(doc), "text": rec}
        if name in NOTEXT:
            entry["notext"], _ = X.geometry(scav, name, scale="notext")
        res["charts"][name] = entry
        print(f"  {name} {time.time() - t:.2f}s t2 {rec['cost']['t2']}: {rec['pins']}", flush=True)
    return res
