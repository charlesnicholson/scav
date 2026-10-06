#!/usr/bin/env python3
"""Every corpus chart drawn by two searches side by side, each panel with its cost, counts
and search CPU, and the corpus totals on top.

  tools/shootout.py --runs DIR --out FILE.html
  tools/shootout.py --runs DIR --out FILE.html --shots DIR    also one PNG per chart row
  tools/shootout.py --runs DIR --out FILE.html --scale none   with no space requests
  tools/shootout.py --runs DIR --out FILE.html --searches full,culled --prefix PFX

Reads and completes the unperturbed runs `tools/jitter.py` keeps in DIR, and draws each
from its pins. A chart without every seeded run of both searches reads "unseeded".
"""

import argparse
import base64
import html
import json
import math
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import jitter  # noqa: E402

REPO_ROOT = jitter.REPO_ROOT
CORPUS = jitter.CORPUS
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"  # `--chrome` default
LOOK = {"full": ("Full search", "#2a78d6"), "culled": ("Culled search", "#eb6834")}
# What each search does beyond the one before it, for the page's heading.
WHAT = {
    "culled": ("The culled search kicks only the rows cheapest after their first search "
               "(<code>kick_rows</code>, 4), drops the compaction rows, refolds no repeated "
               "row, and skips moves its don't-look bits mark until what they read changes. "
               "It is <code>search_cull</code> 1, or <code>--search culled</code>."),
}

STYLE = """
:root {
  color-scheme: light;
  --surface-0: #f4f4f2; --surface-1: #fcfcfb; --line: #dededa;
  --text-primary: #0b0b0b; --text-secondary: #52514e; --text-muted: #85847e;
  --worse: #b42318; --better: #067647;
}
* { box-sizing: border-box; }
body {
  margin: 0; padding: 32px 36px 56px; background: var(--surface-0);
  color: var(--text-primary);
  font: 14px/1.5 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
  font-feature-settings: "tnum" 1;
}
h1 { font-size: 28px; letter-spacing: -0.02em; margin: 0 0 6px; }
.sub { color: var(--text-secondary); margin: 0 0 18px; max-width: 96ch; }
.tiles { display: flex; gap: 12px; flex-wrap: wrap; margin: 0 0 28px; }
.tile {
  background: var(--surface-1); border: 1px solid var(--line); border-radius: 10px;
  padding: 12px 16px; min-width: 190px;
}
.tile .k { color: var(--text-secondary); font-size: 12px; }
.tile .v { font-size: 24px; font-weight: 650; letter-spacing: -0.01em; margin-top: 2px; }
.tile .n { color: var(--text-muted); font-size: 12px; }
.card {
  background: var(--surface-1); border: 1px solid var(--line); border-radius: 12px;
  padding: 16px 16px 12px; margin-bottom: 18px; break-inside: avoid;
}
.card h2 { font-size: 17px; margin: 0 0 2px; letter-spacing: -0.01em; }
.card .meta { color: var(--text-secondary); font-size: 12.5px; margin-bottom: 12px; }
.panels { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; }
.panel { border: 1px solid var(--line); border-radius: 8px; overflow: hidden; background: #fff; }
.panel .head {
  display: flex; align-items: baseline; justify-content: space-between; gap: 8px;
  padding: 8px 10px; border-bottom: 1px solid var(--line); background: var(--surface-1);
}
.panel .who { font-weight: 650; display: flex; align-items: center; gap: 7px; }
.swatch { width: 12px; height: 12px; border-radius: 3px; display: inline-block; }
.panel .dim { color: var(--text-secondary); font-size: 12px; }
.stage { height: 680px; padding: 10px; }
.stage img { width: 100%; height: 100%; object-fit: contain; display: block; }
table.stats { width: 100%; border-collapse: collapse; font-size: 12.5px; }
table.stats td { padding: 4px 10px; border-top: 1px solid var(--line); }
table.stats td:first-child { color: var(--text-secondary); }
table.stats td:last-child { text-align: right; font-weight: 600; }
.worse { color: var(--worse); }
.better { color: var(--better); }
.same { color: var(--text-muted); font-weight: 400; }
footer { margin-top: 26px; color: var(--text-secondary); font-size: 13px; max-width: 96ch; }
footer li { margin-bottom: 4px; }
"""


def data_uri(svg: str) -> str:
    return "data:image/svg+xml;base64," + base64.b64encode(svg.encode("utf-8")).decode()


def render(scav: Path, chart: Path, rec: dict, out: Path) -> str:
    """The SVG of `rec`'s drawing, laid out again from its pins."""
    if not out.exists():
        argv = [str(scav), "render", "-o", str(out), *rec["rests_on"].split(),
                "--no-search", str(chart)]
        subprocess.run(argv, check=True, capture_output=True)
    return out.read_text(encoding="utf-8")


def counts(scav: Path, chart: Path) -> tuple[int, int]:
    """Live states and transitions."""
    doc = json.loads(subprocess.run([str(scav), "dump", "--json", str(chart)],
                                    capture_output=True, text=True, check=True).stdout)
    return (sum(1 for s in doc["states"] if s.get("live", 1)),
            sum(1 for t in doc["transitions"] if t.get("live", 1)))


def delta(culled: float, full: float, lower_is_better: bool = True) -> str:
    """The culled value against the full one, as a signed percentage, coloured."""
    if full == culled:
        return '<span class="same">same</span>'
    if full == 0:
        return f'<span class="worse">+{culled}</span>'
    pct = (culled / full - 1) * 100
    worse = (pct > 0) == lower_is_better
    return f'<span class="{"worse" if worse else "better"}">{pct:+.1f}%</span>'


def stats_rows(rec: dict, other: dict | None) -> str:
    rows = [
        ("t2", f"{rec['t2']:,}", "t2"),
        ("Tier 0", f"{rec['t0']}", "t0"),
        ("bends", f"{rec['bends']}", "bends"),
        ("crossings", f"{rec['crossings']}", "crossings"),
        ("candidates evaluated", f"{rec['scored']:,}", "scored"),
        ("search CPU", f"{rec['cpu_ms'] / 1e3:.2f} s", "cpu_ms"),
    ]
    out = []
    for label, value, key in rows:
        note = f" &nbsp;{delta(rec[key], other[key])}" if other is not None else ""
        out.append(f"<tr><td>{label}</td><td>{value}{note}</td></tr>")
    return "".join(out)


def card(name: str, made: tuple[int, int], recs: dict, svgs: dict, verdicts: dict,
         names: tuple[str, str]) -> str:
    base, cand = names
    panels = []
    for key in names:
        label, colour = LOOK[key]
        rec = recs[key]
        other = recs[base] if key == cand else None
        row = rec["rests_on"].split("--portfolio-row ")[1].split()[0]
        panels.append(
            f'<div class="panel"><div class="head"><span class="who">'
            f'<span class="swatch" style="background:{colour}"></span>{label}</span>'
            f'<span class="dim">row {row}</span></div>'
            f'<div class="stage"><img alt="{name} by the {key} search" '
            f'src="{data_uri(svgs[key])}"></div>'
            f'<table class="stats">{stats_rows(rec, other)}</table></div>')
    factor = recs[base]["scored"] / max(recs[cand]["scored"], 1)
    cpu = recs[base]["cpu_ms"] / max(recs[cand]["cpu_ms"], 1)
    seeds = ", ".join(f"{scale}: {verdicts.get((name, scale), 'unseeded')}"
                      for scale in ("text", "none"))
    return (f'<section class="card" id="{html.escape(Path(name).stem)}">'
            f'<h2>{html.escape(name)}</h2>'
            f'<div class="meta">{made[0]} states &middot; {made[1]} transitions &middot; '
            f'{factor:.2f}&times; fewer evaluations, {cpu:.2f}&times; less CPU &middot; '
            f'seed bar ({seeds})</div>'
            f'<div class="panels">{"".join(panels)}</div></section>')


def tiles(runs: Path, charts: list[str], seeds: int, verdicts: dict,
          names: tuple[str, str]) -> str:
    """Corpus totals at each scale and at both, unperturbed, and the seed bar's count."""
    base, cand = names
    out = []
    both = {s: {"scored": 0, "cpu_ms": 0} for s in names}
    for scale, title in (("text", "real text"), ("none", "no text")):
        tot = {s: {"scored": 0, "cpu_ms": 0, "t2": 0, "t0": 0} for s in names}
        logs = []
        for chart in charts:
            a = jitter.load(runs, chart, scale, base, 0).get(0)
            b = jitter.load(runs, chart, scale, cand, 0).get(0)
            if a is None or b is None:
                continue
            for key, rec in ((base, a), (cand, b)):
                for k in tot[key]:
                    tot[key][k] += rec[k]
            logs.append(math.log(b["t2"] / a["t2"]))
        f, c = tot[base], tot[cand]
        if not c["scored"]:
            continue
        for key in both:
            both[key]["scored"] += tot[key]["scored"]
            both[key]["cpu_ms"] += tot[key]["cpu_ms"]
        gm = (math.exp(sum(logs) / len(logs)) - 1) * 100 if logs else 0.0
        out.append(
            f'<div class="tile"><div class="k">candidates evaluated, {title}</div>'
            f'<div class="v">{f["scored"] / c["scored"]:.2f}&times; fewer</div>'
            f'<div class="n">{f["scored"]:,} {base} &rarr; {c["scored"]:,} {cand}</div></div>'
            f'<div class="tile"><div class="k">search CPU, {title}</div>'
            f'<div class="v">{f["cpu_ms"] / c["cpu_ms"]:.2f}&times; less</div>'
            f'<div class="n">{f["cpu_ms"] / 1e3:.1f} s &rarr; {c["cpu_ms"] / 1e3:.1f} s</div>'
            f'</div>'
            f'<div class="tile"><div class="k">t2, {title}</div>'
            f'<div class="v">{(c["t2"] / f["t2"] - 1) * 100:+.2f}%</div>'
            f'<div class="n">sum {f["t2"]:,} &rarr; {c["t2"]:,}; geometric mean {gm:+.2f}%; '
            f'Tier 0 {f["t0"]} &rarr; {c["t0"]}</div></div>')
    f, c = both[base], both[cand]
    if c["scored"] and c["cpu_ms"]:
        out.append(
            f'<div class="tile"><div class="k">both scales, reduction</div>'
            f'<div class="v">{f["scored"] / c["scored"]:.2f}&times; fewer</div>'
            f'<div class="n">evaluations {f["scored"]:,} &rarr; {c["scored"]:,}; '
            f'CPU {f["cpu_ms"] / c["cpu_ms"]:.2f}&times; less, '
            f'{f["cpu_ms"] / 1e3:.0f} s &rarr; {c["cpu_ms"] / 1e3:.0f} s</div></div>')
    worse = sorted(f"{Path(c).stem} {s}" for (c, s), v in verdicts.items()
                   if v == f"{cand} worse")
    better = sorted(f"{Path(c).stem} {s}" for (c, s), v in verdicts.items()
                    if v == f"{base} worse")
    out.append(
        f'<div class="tile"><div class="k">seed bar, {seeds} seeds, both scales</div>'
        f'<div class="v">{len(worse)} worse, {len(better)} better</div>'
        f'<div class="n">{cand} worse: {", ".join(worse) or "none"}; '
        f'better: {", ".join(better) or "none"}</div></div>')
    return "".join(out)


def page(head: str, body: str, names: tuple[str, str]) -> str:
    return (f'<!doctype html><html lang="en"><meta charset="utf-8">'
            f'<title>scav: {names[0]} search vs {names[1]} search</title><style>{STYLE}</style>'
            f'<body>{head}{body}</body></html>')


def shoot(chrome: Path, profile: Path, html_file: Path, png: Path, height: int) -> None:
    """One headless Chrome screenshot of `html_file`, 1800 wide; ends Chrome's process
    group once the file stops growing."""
    for lock in profile.glob("Singleton*"):
        lock.unlink(missing_ok=True)
    png.unlink(missing_ok=True)
    argv = [str(chrome), "--headless=new", "--disable-gpu", "--hide-scrollbars",
            "--no-first-run", "--no-default-browser-check",
            f"--user-data-dir={profile}", f"--window-size=1800,{height}",
            f"--screenshot={png}", html_file.as_uri()]
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True)
    deadline = time.monotonic() + 90
    size = -1
    try:
        while (time.monotonic() < deadline) and (proc.poll() is None):
            now = png.stat().st_size if png.exists() else -1
            if now > 0 and now == size:
                break
            size = now
            time.sleep(0.5)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
    if not png.exists():
        raise SystemExit(f"no screenshot of {html_file}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--shots", type=Path, default=None)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--scale", choices=jitter.SCALES, default="text")
    ap.add_argument("--scav", type=Path, default=None)
    ap.add_argument("--searches", default=",".join(jitter.SEARCHES),
                    help="the baseline and the search judged against it")
    ap.add_argument("--prefix", default="shootout", help="the PNG file names' prefix")
    ap.add_argument("--chrome", type=Path, default=Path(CHROME))
    args = ap.parse_args()

    names = tuple(args.searches.split(","))
    if len(names) != 2 or any(n not in LOOK for n in names):
        raise SystemExit(f"--searches takes two of {', '.join(LOOK)}")
    base, cand = names
    scav = args.scav or jitter.find_scav()
    charts = sorted(p.name for p in CORPUS.glob("*.scav"))
    for chart in charts:
        for search in names:
            p = jitter.path_of(args.runs, chart, args.scale, search, 0)
            if not p.exists():
                p.write_text(json.dumps(jitter.run_one(scav, CORPUS / chart, args.scale,
                                                       search, 0)))
    verdicts = {}
    for chart in charts:
        for scale in jitter.SCALES:
            a = jitter.load(args.runs, chart, scale, base, args.seeds)
            b = jitter.load(args.runs, chart, scale, cand, args.seeds)
            if len(a) == len(b) == args.seeds + 1:
                verdicts[(chart, scale)] = jitter.verdict_of(a, b, names)

    svg_dir = args.runs / "svg"
    svg_dir.mkdir(parents=True, exist_ok=True)
    cards = {}
    for chart in charts:
        recs = {s: jitter.load(args.runs, chart, args.scale, s, 0)[0] for s in names}
        svgs = {s: render(scav, CORPUS / chart, recs[s],
                          svg_dir / f"{chart}.{args.scale}.{s}.svg")
                for s in names}
        cards[chart] = card(chart, counts(scav, CORPUS / chart), recs, svgs, verdicts,
                            names)

    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO_ROOT,
                            capture_output=True, text=True).stdout.strip() or "unknown"
    scale_words = "under real text" if args.scale == "text" else "with no space requests"
    a_title, b_title = LOOK[base][0], LOOK[cand][0]
    head = (f'<h1>{a_title} vs {b_title.lower()}</h1>'
            f'<p class="sub">Every corpus chart {scale_words}, searched unpinned at '
            f'<code>readable</code>, scav at <code>{commit}</code>. {WHAT.get(cand, "")} '
            f'Deltas under the {cand} panel are against the {base} search. The seed bar '
            f'counts a chart worse only when every {cand} run is more than 2% above every '
            f'{base} run, over the unperturbed run and {args.seeds} jitter seeds.</p>'
            f'<div class="tiles">{tiles(args.runs, charts, args.seeds, verdicts, names)}</div>')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(page(head, "".join(cards.values()), names), encoding="utf-8")
    print(f"wrote {args.out} ({args.out.stat().st_size / 1024:.0f} KB)")

    if args.shots is not None:
        if not args.chrome.exists():
            print(f"no Chrome at {args.chrome}", file=sys.stderr)
            return 1
        args.shots.mkdir(parents=True, exist_ok=True)
        profile = Path(tempfile.mkdtemp(prefix="shootout-chrome-"))  # Chrome's user data
        suffix = "" if args.scale == "text" else f"_{args.scale}"
        rows = args.runs / f"rows{suffix}"
        rows.mkdir(parents=True, exist_ok=True)
        for chart, body in cards.items():
            stem = Path(chart).stem
            one = rows / f"{stem}.html"
            one.write_text(page("", body, names), encoding="utf-8")
            png = args.shots / f"{args.prefix}_{stem}{suffix}.png"
            shoot(args.chrome, profile, one, png, 1040)
            print(f"wrote {png}")
        top = rows / "header.html"
        top.write_text(page(head, "", names), encoding="utf-8")
        png = args.shots / f"{args.prefix}_header{suffix}.png"
        shoot(args.chrome, profile, top, png, 560)
        print(f"wrote {png}")
        shutil.rmtree(profile, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
