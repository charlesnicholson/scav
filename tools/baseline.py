#!/usr/bin/env python3
"""The side-by-side harness: one chart through scav, `dot`, and elkjs.

The likeliest way this project fails is producing layouts that score well on
Cost and that readers find worse than the PlantUML output they already have.
Nothing in a cost vector detects that, so the answer is to put the three
renderings next to each other and look.

This ships at P5b so that the P6 and P7 gates have something to score against.
It is a *tool*, not a test: `dot` and `node` are not build prerequisites and it
reports what it could not run rather than failing.

  tools/baseline.py                      every corpus chart, default build dir
  tools/baseline.py --chart vac.scav     one of them
  tools/baseline.py --out DIR            somewhere other than out/baseline
"""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS = REPO_ROOT / "test_data/charts"


def find_scav() -> Path:
    """The first `scav` under out/, newest build tree first."""
    candidates = sorted(
        (REPO_ROOT / "out").glob("*/bin/scav"),
        key=lambda p: p.stat().st_mtime, reverse=True)
    if not candidates:
        raise SystemExit("no scav binary under out/; run ./build.sh first")
    return candidates[0]


def run(cmd: list[str], stdin: bytes | None = None) -> tuple[int, bytes, bytes]:
    p = subprocess.run(cmd, input=stdin, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, check=False)
    return p.returncode, p.stdout, p.stderr


def chart_model(scav: Path, chart: Path) -> dict:
    code, out, err = run([str(scav), "dump", "--json", str(chart)])
    if code != 0:
        raise SystemExit(f"scav dump failed for {chart.name}: {err.decode()}")
    return json.loads(out)


def to_dot(model: dict) -> str:
    """The model as a DOT digraph, with submachines as clusters.

    `dot` has no notion of a state machine, so this is the fairest mapping
    available: a composite state becomes a subgraph cluster, which is the
    construct its own documentation points at for nesting.
    """
    states = model["states"]
    subs = model["submachines"]
    lines = ["digraph chart {", "  compound=true;", "  node [shape=box];"]

    def emit_submachine(index: int, depth: int) -> None:
        pad = "  " * (depth + 1)
        sub = subs[index]
        lines.append(f'{pad}subgraph cluster_sub{index} {{')
        lines.append(f'{pad}  label="{sub.get("name", "")}";')
        for state in sub["children"]:
            row = states[state]
            own = row.get("submachines", [])
            if own:
                lines.append(f'{pad}  subgraph cluster_state{state} {{')
                lines.append(f'{pad}    label="{row.get("name", "")}";')
                # A cluster cannot be an edge endpoint, so every composite also
                # carries an invisible anchor node for edges to land on.
                lines.append(f'{pad}    s{state} [style=invis, width=0.01];')
                for child in own:
                    emit_submachine(child, depth + 2)
                lines.append(f'{pad}  }}')
            else:
                lines.append(f'{pad}  s{state} [label="{row.get("name", "")}"];')
        lines.append(f'{pad}}}')

    emit_submachine(model["chart"]["root_submachine"], 0)
    for trans in model["transitions"]:
        label = trans.get("label", "")
        attrs = f' [label="{label}"]' if label else ""
        lines.append(f'  s{trans["src"]} -> s{trans["dst"]}{attrs};')
    lines.append("}")
    return "\n".join(lines) + "\n"


def to_elk(model: dict) -> dict:
    """The model as an ELK graph. Compound nodes, since that is ELK's own
    spelling for nesting, and the layered algorithm, since that is what scav's
    P6 will be."""
    states = model["states"]
    subs = model["submachines"]

    def node_for(state: int) -> dict:
        row = states[state]
        node: dict = {"id": f"s{state}", "width": 120, "height": 40,
                      "labels": [{"text": row.get("name", "")}]}
        children: list[dict] = []
        for sub in row.get("submachines", []):
            for child in subs[sub]["children"]:
                children.append(node_for(child))
        if children:
            node["children"] = children
            node["layoutOptions"] = {"elk.padding": "[top=30,left=12,bottom=12,right=12]"}
        return node

    root = {
        "id": "root",
        "layoutOptions": {
            "elk.algorithm": "layered",
            "elk.hierarchyHandling": "INCLUDE_CHILDREN",
        },
        "children": [node_for(s) for s in subs[model["chart"]["root_submachine"]]["children"]],
        "edges": [{"id": f"e{i}", "sources": [f's{t["src"]}'],
                   "targets": [f's{t["dst"]}']}
                  for i, t in enumerate(model["transitions"])],
    }
    return root


ELK_DRIVER = """
// Lays out one ELK graph and writes an SVG good enough to look at. elkjs owns
// the layout; the rendering here is deliberately plain so the comparison is
// about placement and routing.
const fs = require('fs');
const ELK = require('elkjs');
const elk = new ELK();
const graph = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));

function draw(node, dx, dy, out) {
  for (const child of node.children || []) {
    const x = dx + (child.x || 0), y = dy + (child.y || 0);
    out.push(`<rect x="${x}" y="${y}" width="${child.width}" height="${child.height}"`
      + ` fill="none" stroke="black"/>`);
    const text = (child.labels && child.labels[0] && child.labels[0].text) || '';
    if (text) { out.push(`<text x="${x + 4}" y="${y + 14}" font-size="12">${text}</text>`); }
    draw(child, x, y, out);
  }
  for (const edge of node.edges || []) {
    for (const section of edge.sections || []) {
      const pts = [section.startPoint, ...(section.bendPoints || []), section.endPoint]
        .map(p => `${dx + p.x},${dy + p.y}`).join(' ');
      out.push(`<polyline points="${pts}" fill="none" stroke="black"/>`);
    }
  }
}

elk.layout(graph).then(laid => {
  const out = [];
  draw(laid, 0, 0, out);
  const w = Math.ceil(laid.width || 100), h = Math.ceil(laid.height || 100);
  process.stdout.write(`<svg xmlns="http://www.w3.org/2000/svg" width="${w}"`
    + ` height="${h}" viewBox="0 0 ${w} ${h}">${out.join('\\n')}</svg>\\n`);
}).catch(e => { process.stderr.write(String(e)); process.exit(1); });
"""


def render_scav(scav: Path, chart: Path, out: Path) -> str | None:
    code, _, err = run([str(scav), "render", "-o", str(out), str(chart)])
    return None if code == 0 else err.decode().strip()


def render_dot(model: dict, out: Path) -> str | None:
    if shutil.which("dot") is None:
        return "`dot` is not installed"
    code, svg, err = run(["dot", "-Tsvg"], to_dot(model).encode())
    if code != 0:
        return err.decode().strip()
    out.write_bytes(svg)
    return None


def render_elk(model: dict, out: Path, work: Path) -> str | None:
    if shutil.which("node") is None:
        return "`node` is not installed"
    driver = work / "elk_driver.js"
    driver.write_text(ELK_DRIVER, encoding="utf-8")
    graph = work / f"{out.stem}.elk.json"
    graph.write_text(json.dumps(to_elk(model)), encoding="utf-8")
    code, svg, err = run(["node", str(driver), str(graph)])
    if code != 0:
        text = err.decode()
        # elkjs is not vendored, so absent is the expected case rather than a
        # failure. Say what to install instead of echoing a node stack frame.
        if "Cannot find module" in text:
            return "elkjs is not installed (`npm install elkjs`)"
        return text.strip().splitlines()[0] if text.strip() else "elkjs failed"
    out.write_bytes(svg)
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--chart", action="append", default=[])
    ap.add_argument("--out", type=Path, default=REPO_ROOT / "out/baseline")
    args = ap.parse_args()

    scav = find_scav()
    charts = ([CORPUS / c for c in args.chart] if args.chart
              else sorted(CORPUS.glob("*.scav")))
    args.out.mkdir(parents=True, exist_ok=True)

    rows: list[str] = []
    skipped: dict[str, str] = {}
    for chart in charts:
        model = chart_model(scav, chart)
        cells = []
        drawn: list[str] = []
        for engine, render in (
                ("scav", lambda o, c=chart: render_scav(scav, c, o)),
                ("dot", lambda o, m=model: render_dot(m, o)),
                ("elkjs", lambda o, m=model: render_elk(m, o, args.out))):
            target = args.out / f"{chart.stem}.{engine}.svg"
            why = render(target)
            if why is None:
                drawn.append(engine)
                cells.append(f'<td><div class="name">{engine}</div>'
                             f'<img src="{target.name}"></td>')
            else:
                skipped.setdefault(engine, why)
                cells.append(f'<td><div class="name">{engine}</div>'
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
    for engine, why in sorted(skipped.items()):
        # Say what was not compared rather than leaving a gap that reads as
        # agreement.
        print(f"  {engine} skipped: {why}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
