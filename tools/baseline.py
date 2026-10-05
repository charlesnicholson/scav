#!/usr/bin/env python3
"""Renders each chart with scav, PlantUML and elkjs into one side-by-side HTML table.
SCAV_BASELINE=1 provisions PlantUML and elkjs via envy; each skip is reported.

  SCAV_BASELINE=1 tools/baseline.py      every corpus chart, newest scav under out/
  tools/baseline.py --chart vac.scav     one of them, scav only
  tools/baseline.py --gauntlet           the element suite instead of the corpus
  tools/baseline.py --puml ~/charts      render real .puml where names match
  tools/baseline.py --out DIR            somewhere other than out/baseline
"""

import argparse
import html
import json
import math
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS = REPO_ROOT / "test_data/charts"
# Element-suite charts, one layout element each, also tested by gauntlet_tests.cpp.
# Those tests use no space requests; these renders use measured text.
GAUNTLET = CORPUS / "gauntlet"

# Layout grid units per point; ELK leaf sizes are given in points.
GRID_PER_PT = 16


def find_scav() -> Path:
    """The newest `scav` under out/, whichever preset built it."""
    candidates = sorted(
        (c for name in ("scav", "scav.exe")
         for c in (REPO_ROOT / "out").glob(f"*/bin/{name}")),
        key=lambda p: p.stat().st_mtime, reverse=True)
    if not candidates:
        raise SystemExit("no scav binary under out/; run ./build.sh first")
    return candidates[0]


def run(cmd: list[str], stdin: bytes | None = None) -> tuple[int, bytes, bytes]:
    p = subprocess.run(cmd, input=stdin, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, check=False)
    return p.returncode, p.stdout, p.stderr


def envy_product(name: str) -> Path | None:
    """Path of one baseline tool from `envy product`, provisioning it if missing.
    None when SCAV_BASELINE is unset or envy fails.
    """
    if not os.environ.get("SCAV_BASELINE"):
        return None
    launcher = REPO_ROOT / ("bin/envy.bat" if os.name == "nt" else "bin/envy")
    p = subprocess.run([str(launcher), "product", name],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if p.returncode != 0:
        return None
    # envy prints only the path on stdout; its log goes to stderr.
    path = p.stdout.decode().strip()
    return Path(path) if path else None


def chart_model(scav: Path, chart: Path) -> dict:
    """The `scav dump --json --layout` output: the model plus measured geometry."""
    code, out, err = run([str(scav), "dump", "--json", "--layout", str(chart)])
    if code != 0:
        raise SystemExit(f"scav dump failed for {chart.name}: {err.decode()}")
    return json.loads(out)


def lca_submachine(model: dict) -> "callable":
    """Maps a transition to its innermost common enclosing submachine's ordinal."""
    states, subs = model["states"], model["submachines"]

    def chain(state: int) -> list[int]:
        """Root submachine down to the one this state sits in."""
        up = []
        sub = states[state]["parent"]
        while sub is not None:
            up.append(sub)
            owner = subs[sub]["owner"]
            sub = None if owner is None else states[owner]["parent"]
        return up[::-1]

    def of(trans: dict) -> int:
        src, dst = chain(trans["src"]), chain(trans["dst"])
        common = [a for a, b in zip(src, dst) if a == b]
        return common[-1] if common else src[0]

    return of


# --- PlantUML ---------------------------------------------------------------

# scav names a pseudostate kind; PlantUML draws the same set as stereotypes.
# `junction` has no distinct glyph there and shares `choice`'s diamond.
STEREOTYPE = {
    "initial": "<<start>>",
    "final": "<<end>>",
    "choice": "<<choice>>",
    "junction": "<<choice>>",
    "fork": "<<fork>>",
    "join": "<<join>>",
    "history": "<<history>>",
    "deephistory": "<<deepHistory>>",
}


def puml_text(s: str) -> str:
    return s.replace('"', "'").replace("\n", "\\n")


def to_puml(model: dict) -> tuple[str, list[str]]:
    """The model as PlantUML text, and a note per endpoint inside a non-first region,
    which is re-anchored at that region's composite.
    """
    states, subs = model["states"], model["submachines"]
    innermost = lca_submachine(model)

    def chain(state: int) -> list[int]:
        up = []
        sub = states[state]["parent"]
        while sub is not None:
            up.append(sub)
            owner = subs[sub]["owner"]
            sub = None if owner is None else states[owner]["parent"]
        return up[::-1]

    def is_first_region(sub: int) -> bool:
        owner = subs[sub]["owner"]
        if owner is None:
            return True
        live = [m for m in states[owner]["submachines"] if subs[m]["live"]]
        return bool(live) and live[0] == sub

    degraded: list[str] = []

    def nameable(state: int, lca: int) -> int:
        """The deepest state PlantUML can reference from inside `lca`'s block."""
        path = chain(state)
        for sub in path[path.index(lca) + 1:]:
            if not is_first_region(sub):
                owner = subs[sub]["owner"]
                degraded.append(
                    f'{states[state]["name"] or "pseudostate"} -> '
                    f'{states[owner]["name"]} (unreachable region)')
                return owner
        return state

    by_sub: dict[int, list[str]] = {}
    described: dict[int, list[str]] = {}
    for trans in model["transitions"]:
        if not trans["live"]:
            continue
        # An internal or local transition's label becomes a description line of its source.
        if trans["kind"] in ("internal", "local"):
            if trans["label"]:
                described.setdefault(trans["src"], []).append(trans["label"])
            continue
        lca = innermost(trans)
        src, dst = nameable(trans["src"], lca), nameable(trans["dst"], lca)
        arrow = f"s{src} --> s{dst}"
        if trans["label"]:
            arrow += f' : {puml_text(trans["label"])}'
        by_sub.setdefault(lca, []).append(arrow)

    out = ["@startuml", "hide empty description"]

    def emit_state(index: int, depth: int) -> None:
        pad = "  " * depth
        row = states[index]
        stereo = STEREOTYPE.get(row["kind"], "")
        # An unnamed state is declared by its bare alias `s<index>`.
        head = (f"{pad}state s{index}" if not row["name"]
                else f'{pad}state "{puml_text(row["name"])}" as s{index}')
        if stereo:
            head += f" {stereo}"

        regions = [m for m in row.get("submachines", []) if subs[m]["live"]]
        if regions:
            out.append(head + " {")
            for i, m in enumerate(regions):
                if i:
                    out.append(f"{pad}  --")
                for child in subs[m]["children"]:
                    if states[child]["live"]:
                        emit_state(child, depth + 1)
                out.extend(f"{pad}  {a}" for a in by_sub.pop(m, []))
            out.append(pad + "}")
        else:
            out.append(head)

        if row["label"]:
            out.append(f'{pad}s{index} : {puml_text(row["label"])}')
        for text in described.get(index, []):
            out.append(f"{pad}s{index} : {puml_text(text)}")

    root = model["chart"]["root_submachine"]
    for child in subs[root]["children"]:
        if states[child]["live"]:
            emit_state(child, 0)

    # The root's own, plus anything whose region never rendered.
    out.extend(by_sub.pop(root, []))
    for leftover in list(by_sub):
        out.extend(by_sub.pop(leftover))

    out.append("@enduml")
    return "\n".join(out) + "\n", degraded


def puml_reason(svg: Path) -> str | None:
    """PlantUML's error message: the last text run of the SVG it writes, or None."""
    if not svg.is_file():
        return None
    runs = re.findall(r"<text[^>]*>(.*?)</text>", svg.read_text(encoding="utf-8"), re.S)
    return html.unescape(runs[-1]).strip() if runs else None


def render_puml(plantuml: Path | None, source: Path, out: Path) -> str | None:
    if plantuml is None:
        return "not provisioned; re-run with SCAV_BASELINE=1"
    # `-Playout=smetana` selects PlantUML's bundled layout engine over a host graphviz.
    code, _, err = run([str(plantuml), "-tsvg", "-Playout=smetana",
                        "-o", str(out.parent.resolve()), str(source)])
    produced = out.parent / f"{source.stem}.svg"
    if code != 0:
        reason = puml_reason(produced)
        produced.unlink(missing_ok=True)
        if reason:
            return reason
        text = err.decode().strip()
        return text.splitlines()[0] if text else "plantuml failed"
    if produced != out:
        if not produced.is_file():
            return "plantuml wrote no svg"
        produced.replace(out)
    return None


# --- elkjs ------------------------------------------------------------------


def to_elk(model: dict) -> dict:
    """The model as an ELK graph: nested compound nodes, leaf sizes in points from
    scav's geometry, and a state's regions merged into one child list.
    """
    states, subs = model["states"], model["submachines"]
    measured = model["geometry"]["state"]
    innermost = lca_submachine(model)
    root = model["chart"]["root_submachine"]

    # Keyed by the state that owns the enclosing region, or None for the root.
    by_owner: dict[int | None, list[dict]] = {}
    for i, trans in enumerate(model["transitions"]):
        # Every live transition, of any kind, becomes an edge.
        if not trans["live"]:
            continue
        sub = innermost(trans)
        edge = {"id": f"e{i}",
                "sources": [f's{trans["src"]}'],
                "targets": [f's{trans["dst"]}']}
        if trans["label"]:
            edge["labels"] = [{"text": trans["label"]}]
        by_owner.setdefault(None if sub == root else subs[sub]["owner"], []).append(edge)

    def node_for(index: int) -> dict:
        row = states[index]
        node: dict = {"id": f"s{index}",
                      "labels": [{"text": row["name"]}]}
        children = [node_for(c)
                    for m in row.get("submachines", []) if subs[m]["live"]
                    for c in subs[m]["children"] if states[c]["live"]]
        if children:
            node["children"] = children
            node["layoutOptions"] = {
                "elk.padding": "[top=30,left=12,bottom=12,right=12]",
            }
        else:
            _, _, w, h = measured[index]
            node["width"] = math.ceil(w / GRID_PER_PT)
            node["height"] = math.ceil(h / GRID_PER_PT)
        edges = by_owner.pop(index, [])
        if edges:
            node["edges"] = edges
        return node

    children = [node_for(c) for c in subs[root]["children"] if states[c]["live"]]

    return {
        "id": "root",
        "layoutOptions": {
            "elk.algorithm": "layered",
            # INCLUDE_CHILDREN lays out hierarchy-crossing edges; ORTHOGONAL routes them
            # through hierarchical ports. Neither is ELK's default.
            "elk.hierarchyHandling": "INCLUDE_CHILDREN",
            "elk.edgeRouting": "ORTHOGONAL",
        },
        "children": children,
        # The root's own, plus any container that never rendered.
        "edges": by_owner.pop(None, []) + [e for v in by_owner.values() for e in v],
    }


ELK_DRIVER = """
// Lays out one ELK graph with elkjs and writes a plain SVG of it.
const fs = require('fs');
const ELK = require(process.argv[2]);
const elk = new ELK();
const graph = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));

function esc(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// Extent over every drawn point, including edges routed outside the root graph.
const box = { x0: Infinity, y0: Infinity, x1: -Infinity, y1: -Infinity };
function seen(x, y) {
  box.x0 = Math.min(box.x0, x); box.y0 = Math.min(box.y0, y);
  box.x1 = Math.max(box.x1, x); box.y1 = Math.max(box.y1, y);
}

// Edges ELK returned with no sections; any makes the driver exit 2.
let unrouted = 0;

function draw(node, dx, dy, out) {
  for (const child of node.children || []) {
    const x = dx + (child.x || 0), y = dy + (child.y || 0);
    out.push(`<rect x="${x}" y="${y}" width="${child.width}" height="${child.height}"`
      + ` fill="none" stroke="black"/>`);
    seen(x, y); seen(x + child.width, y + child.height);
    const text = (child.labels && child.labels[0] && child.labels[0].text) || '';
    if (text) {
      out.push(`<text x="${x + 4}" y="${y + 14}" font-size="12">${esc(text)}</text>`);
    }
    draw(child, x, y, out);
  }
  // Edges are drawn in the frame of the container they were declared on.
  for (const edge of node.edges || []) {
    if (!edge.sections || !edge.sections.length) { unrouted++; }
    for (const section of edge.sections || []) {
      const pts = [section.startPoint, ...(section.bendPoints || []), section.endPoint];
      pts.forEach(p => seen(dx + p.x, dy + p.y));
      out.push(`<polyline points="${pts.map(p => `${dx + p.x},${dy + p.y}`).join(' ')}"`
        + ` fill="none" stroke="black" marker-end="url(#arrow)"/>`);
    }
    for (const label of edge.labels || []) {
      if (label.x === undefined) { continue; }
      out.push(`<text x="${dx + label.x}" y="${dy + label.y + 9}" font-size="10"`
        + ` fill="#444">${esc(label.text)}</text>`);
    }
  }
}

elk.layout(graph).then(laid => {
  const out = [];
  draw(laid, 0, 0, out);
  const pad = 8;
  const x = Math.floor(box.x0) - pad, y = Math.floor(box.y0) - pad;
  const w = Math.ceil(box.x1 - box.x0) + 2 * pad;
  const h = Math.ceil(box.y1 - box.y0) + 2 * pad;
  // The arrowhead marker each polyline ends with.
  const defs = '<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5"'
    + ' markerWidth="6" markerHeight="6" orient="auto-start-reverse">'
    + '<path d="M 0 0 L 10 5 L 0 10 z" fill="black"/></marker></defs>';
  if (unrouted) { process.stderr.write(`unrouted ${unrouted}`); process.exit(2); }
  process.stdout.write(`<svg xmlns="http://www.w3.org/2000/svg" width="${w}"`
    + ` height="${h}" viewBox="${x} ${y} ${w} ${h}">${defs}${out.join('\\n')}</svg>\\n`);
}).catch(e => { process.stderr.write(String(e)); process.exit(1); });
"""


def render_elk(node: Path | None, bundle: Path | None,
               model: dict, out: Path, work: Path) -> str | None:
    if node is None or bundle is None:
        return "not provisioned; re-run with SCAV_BASELINE=1"
    driver = work / "elk_driver.js"
    driver.write_text(ELK_DRIVER, encoding="utf-8")
    graph = work / f"{out.stem}.elk.json"
    graph.write_text(json.dumps(to_elk(model)), encoding="utf-8")
    code, svg, err = run([str(node), str(driver), str(bundle), str(graph)])
    if code != 0:
        text = err.decode().strip()
        if text.startswith("unrouted "):
            return f"elkjs left {text.split()[1]} edge(s) unrouted"
        return text.splitlines()[0] if text else "elkjs failed"
    out.write_bytes(svg)
    return None


# --- scav -------------------------------------------------------------------


def render_scav(scav: Path, chart: Path, out: Path) -> str | None:
    code, _, err = run([str(scav), "render", "-o", str(out), str(chart)])
    return None if code == 0 else err.decode().strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--chart", action="append", default=[])
    ap.add_argument("--gauntlet", action="store_true",
                    help="the element suite under test_data/charts/gauntlet")
    ap.add_argument("--out", type=Path, default=REPO_ROOT / "out/baseline")
    ap.add_argument("--puml", type=Path,
                    help="directory of real .puml sources; used where the stem matches")
    ap.add_argument("--scav", type=Path,
                    help="the binary to render with; found under out/ otherwise")
    args = ap.parse_args()

    scav = args.scav or find_scav()
    if not scav.is_file():
        raise SystemExit(f"no scav binary at {scav}")
    root = GAUNTLET if args.gauntlet else CORPUS
    charts = ([root / c for c in args.chart] if args.chart
              else sorted(root.glob("*.scav")))
    args.out.mkdir(parents=True, exist_ok=True)

    plantuml = envy_product("plantuml")
    node = envy_product("node")
    elk_bundle = envy_product("elk_bundle")

    rows: list[str] = []
    skipped: list[tuple[str, str, str]] = []
    notes: list[tuple[str, str, str]] = []
    for chart in charts:
        model = chart_model(scav, chart)

        # An authored .puml with the chart's stem replaces the translation.
        original = (args.puml / f"{chart.stem}.puml") if args.puml else None
        if original is not None and original.is_file():
            source, origin = original, "authored"
        else:
            source = args.out / f"{chart.stem}.puml"
            text, degraded = to_puml(model)
            source.write_text(text, encoding="utf-8")
            origin = "translated"
            for what in degraded:
                notes.append((chart.name, "plantuml", f"anchored {what}"))

        cells = []
        drawn: list[str] = []
        for engine, label, render in (
                ("scav", "scav", lambda o, c=chart: render_scav(scav, c, o)),
                ("plantuml", f"plantuml ({origin})",
                 lambda o, s=source: render_puml(plantuml, s, o)),
                ("elkjs", "elkjs", lambda o, m=model: render_elk(
                    node, elk_bundle, m, o, args.out))):
            target = args.out / f"{chart.stem}.{engine}.svg"
            why = render(target)
            if why is None:
                drawn.append(engine)
                cells.append(f'<td><div class="name">{label}</div>'
                             f'<img src="{target.name}"></td>')
            else:
                skipped.append((chart.name, engine, why))
                cells.append(f'<td><div class="name">{label}</div>'
                             f'<p class="skip">{why}</p></td>')
        rows.append(f"<tr><th>{chart.name}</th>{''.join(cells)}</tr>")
        print(f"{chart.name}: {' '.join(drawn) if drawn else '(nothing rendered)'}")

    index = args.out / "index.html"
    index.write_text(
        "<!doctype html><meta charset=utf-8><title>scav baseline</title>"
        "<style>body{font:13px system-ui;margin:2rem}"
        "table{border-collapse:collapse}td,th{border:1px solid #ccc;padding:8px;"
        "vertical-align:top}img{max-width:520px;max-height:700px}"
        ".name{font-weight:600;margin-bottom:4px}.skip{color:#888}</style>"
        f"<h1>Blind review corpus</h1><table>{''.join(rows)}</table>",
        encoding="utf-8")

    print(f"\nwrote {index}")
    for name, engine, why in skipped:
        print(f"  {name} {engine}: {why}")
    # Lists each endpoint the PlantUML translation re-anchored.
    for name, engine, what in notes:
        print(f"  {name} {engine}: {what}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
