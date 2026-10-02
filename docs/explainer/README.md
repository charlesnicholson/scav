# How scav lays out a statechart

`scav-explained.html` is an interactive walk through scav's layout: the model,
decomposition, ordering, sizing, routing, labels, the cost, the search, what makes
it fast, and determinism, with a glossary. Every real drawing on the page is scav's
own output for a corpus chart; the algorithm figures run small, correct toy
implementations in the page.

Open `scav-explained.html` in any browser. It is one self-contained file with
no network access, and it is committed, so reading it needs no build.

[PRD.md](../../PRD.md) is the source of truth. Where the page and the PRD
disagree, the page is wrong. The content spec the sections were first written from
was a research note derived from the PRD and the code, and is not kept.

## Rebuilding

The page is assembled from `shell.html`, the fragments in `sections/` and the data
in `data/`. Every data file except `perf.json` is regenerated from a scav binary:

```sh
./build.sh
$(./bin/envy product python3) docs/explainer/extract/build_data.py
$(./bin/envy product python3) docs/explainer/assemble.py
```

`build_data.py` runs the newest `out/*/bin/scav` unless `--scav BIN` names one,
and `--only charts,faces` rebuilds a subset. The committed data came from
`out/macos-clang-libcxx-release/bin/scav`, which rebuilds all of it in about 30 s;
an unoptimised or sanitized build is much slower. The script runs scav from the
repository root with a timeout on each run (`--timeout`, 240 s), keeps its SVGs in a temporary
directory, and records the binary and the checkout's `HEAD` in `charts.json`'s
`binary` field. `assemble.py --only 05,06` builds a page with just those sections,
which is quicker to iterate on; `--out FILE` writes it somewhere else. Commit the
rebuilt page with the data and sections it was built from.

The data is the layout at one commit. When ordering, sizing, routing or the search
change, rebuild it and reread the prose against it: the sections quote costs, pins
and round counts from it. `extract/search.py` narrates brew's kick steps with
trace round numbers read off brew's search trace at the recorded binary, and marks
each step `verified` when laying out the step's pins again reproduces the cost the
trace scored. A `false` there means the narration needs reading again.

| file | builder | holds |
|---|---|---|
| `charts.json` | `extract/charts.py` | estop, led, brew, dock, tcp, axis, toolchanger, bottler: source, model, real-text geometry, and no-text geometry for estop and brew |
| `rows.json` | `extract/rows.py` | brew and tcp, Level 2 rows 0–15, each unsearched and searched; the shipped row and the row Level 2 alone picks |
| `search.json` | `extract/search.py` | brew's search path from its trace (row start, Level 1 moves, kick, re-search, settle), each step laid out again; tcp's Level 1 path; round summaries |
| `stages.json` | `extract/stages.py` | brew and toolchanger stage by stage, from the shipped drawing's trace |
| `faces.json` | `extract/faces.py` | estop with no text, all four face pins at both ends of t3 and t1 |
| `perf.json` | none | wall, CPU and instructions per commit and chart |

`perf.json` is a measured snapshot and is not regenerated. Its numbers are
`/usr/bin/time -l` around `scav dump --layout [--no-text]` for each of the 11
corpus charts, on one 12-core machine, for five builds from `c118892` to `77fd1cf`,
with the best of each run's passes kept. Beside them are summaries of a
hierarchical profile at `aaac379` and of a per-candidate log of bottler's search at
`43de925`. Its `notes`, `builds` and `files` fields record each build, run and
script. The binaries, logs and notes it cites were local to the measuring machine.

## Checking the interactions

`check_interactions.mjs` drives the built page in headless Chrome with real mouse and
keyboard input over the DevTools protocol, using only Node (22 or later) and an installed
Chrome:

```sh
node docs/explainer/check_interactions.mjs --jobs 6
```

It finds every listener the page binds, cross-checked against DevTools' own list, and
every button and input. Each element is scrolled into view and hit-tested at points
inside its own fill or stroke, so a mark that a label, overlay or another mark covers
is reported, not tested at a point that misses it. Then it hovers (a tooltip with
sensible text or a highlight, gone on leave), clicks (the drawing or the control
changes; a choice moves its `on` state; a stepper steps, plays and pauses), drags (the
mark follows the pointer through every step, not just the first, and the drawing
updates), drags each slider's thumb and sets every value, toggles each checkbox both
ways, and types into the filter. Drags run before clicks, and whatever a click reveals
(another mode's handles, a stage's overlays) is tested at once; anything an earlier action
removed before its turn is listed as not reached. Any exception fails the interaction.
The whole pass runs again after the theme button, which must redraw every figure once
with the same listeners.

A drag in a figure that redraws while dragging goes through `SX.drag(e, {onMove, onEnd})`,
which listens on the window, so replacing the dragged element does not end the drag; read
pointer positions with `SX.svgPoint(svg, e)` against the svg currently on the page.

It writes `report.md` (counts by section, failures with screenshots, every
interaction) and `results.json` to `--out` (default `$TMPDIR/sx-interactions`) and exits
non-zero on any failure. `--only order,route` checks some sections, `--jobs N` shards
the sections over N browsers, `--no-theme` skips the second pass, `--chrome BIN`
names the browser and `--budget SECS` caps the run.

## Authoring a section

### The fragment

```html
<section class="sx" id="order" data-toc="4 · Ordering">
  <div class="kicker">Phase 1</div>
  <h2>Ordering: which rank, which slot</h2>
  <p class="lede">One or two sentences: what this stage decides and why it matters.</p>
  <p>Prose…</p>
  <div class="callout rule"><span class="t">Rule</span>A rule stated exactly.</div>
  <div data-fig="order-median"></div>
  <p>More prose…</p>
</section>
<script>
SX.register('order', function (sec) {
  var f = SX.figure(sec.querySelector('[data-fig="order-median"]'), { caption: '<b>Median sweep.</b> …' });
  // build controls into f.ctrl, draw into f.stage
});
</script>
```

Fragments are `sections/NN-id.html`, assembled in filename order. Data files are
embedded whole and read with `SX.data('name')`, by file stem.

### Rules

- One `<section class="sx">` per file. Its `id` matches the `SX.register` id, and `data-toc` is the TOC label.
- Prose is static HTML in the fragment. Figures are created only inside `[data-fig]` placeholders, from the init function. When the theme toggles, every placeholder is emptied and the init runs again, so the init must be idempotent: build everything from scratch and keep no module-level state that assumes a first run.
- No external resources at all: no CDN, fonts, images or fetches. Everything is inline SVG, built with `SX.svg`.
- Colours come only from CSS tokens, read with `SX.css('--series-2')` or set as `var(--…)` in style attributes:
  - categorical: `--series-1..8` (fixed order: blue, orange, aqua, yellow, magenta, green, violet, red);
  - status: `--good`, `--warning`, `--serious`, `--critical`;
  - chart roles: `--state-fill`, `--state-stroke`, `--route`, `--ink`, `--hl` (orange highlight), `--hl-2` (blue), `--hl-3` (aqua);
  - ink: `--text-primary`, `--text-secondary`, `--text-muted`, `--grid`, `--axis`, `--surface-1`, `--surface-2`.

  Never hard-code a hex. Text is never drawn in a series colour; a coloured swatch or mark beside it carries the identity.
- Diagrams follow the data-viz rules where they are charts: one y-axis, 2px lines, a ≥8px marker where a point matters, a hover tooltip on every mark (`SX.tipOn(node, html)`), and a legend when there are ≥2 series.
- Interactivity is the point. Every figure should let the reader *do* something: step an algorithm, drag a node, toggle a rule, slide a weight, hover for the number. Animation steps come from the algorithm itself, computed in JS on a toy input or read from real data, never faked.
- Toy algorithms in JS must be correct implementations of what the text says: median heuristic, longest path, Brandes–Köpf, A*, and so on. Keep them small and readable.
- Prose is precise and plain: every claim matches PRD.md and the code. Use scav's own terms (frame, submachine, rank, segment, port, face, pin, row, kick, Tier 0/2) and define each at first use; the glossary has them all.

### Helpers (in `shell.html`)

| helper | does |
|---|---|
| `SX.el(tag, attrs, …children)` / `SX.svg(tag, attrs, …children)` | build HTML/SVG; attrs: `class`, `style` (object), `text`, `html`, `onclick`… |
| `SX.figure(host, {caption})` | → `{fig, ctrl, stage, caption}`; append controls to `ctrl`, drawing to `stage` |
| `SX.stepper({steps, onStep(i), label(i), interval, start})` | → `{el, go(i)}`; prev / play / next / scrub. Append `.el` to `f.ctrl` |
| `SX.slider({label, min, max, step, value, format, onInput})` | → `{el, set, get}` |
| `SX.toggle({label, value, onChange})`, `SX.choice({options:[{value,label}], value, onChange})`, `SX.button(label, onClick)` | controls |
| `SX.tip.show(e, html)`, `SX.tip.hide()`, `SX.tipOn(node, html \| fn)` | tooltip |
| `SX.chart(stage, geom, opts)` | draws a real scav layout exactly as scav's SVG does (from each element's `draw` pieces, theme-aware). Returns handles: `states[id] / trans[id] / subs[id]` = `{nodes, data}`; `mark(kind, id, color?, on?)` colours one element (`kind` = 'state', 'trans' or 'sub'; color defaults to `var(--hl)`), `unmarkAll()`, `dim({states:[ids], trans:[ids]})` fades everything else, `rect(r, attrs)`, `line(pts, attrs)`, `dot(p, r, attrs)` and `text(p, str, attrs)` draw overlays in chart units, `clear()` removes overlays; `sw` is scav's stroke width (16). opts: `{layers:{subs,states,text,routes,labels}, highlight:{states:[], trans:[]}, onState(s,e,'move'\|'leave'\|'click'), onTrans(t,e,…), click, labelBoxes, pad, maxHeight}`; hits report `'click'`, and show a pointer, only with `click: true` |
| `SX.lineChart(stage, {series:[{name, points:[[x,y,note]]}], xLabel, yLabel, markers, yFormat, xFormat, xTicks, onHover})` | line chart with crosshair tooltip |
| `SX.barChart(stage, {bars:[{label, value, note, color}], unit, format})` | horizontal bars with tooltips |
| `SX.data(name)` | parsed `data/name.json` |
| `SX.fmt(n)` | thousands separators |

### The real-layout schema

Units are scav grid units (1/16 pt), integers.

```
geom = {
  chart, scale: "text" | "notext", pins,
  box: [x, y, w, h],
  states: [{id, name, path, kind, parent_sub, rect, before, after, text: [lines]}],
  subs:   [{id, owner, rect, ...}],
  trans:  [{id, src, dst, label, kind, points: [[x, y]...], label_box}],
  cost:   {t0, t1, t2, tier0: {...}, terms: {...}, shares_bp: {...}}
}
```

Geometry also carries `pins`, `pin_list`, `hash`, `ranks` (per frame) and `svg`.
Every element has `draw`, the exact paint pieces, and transitions have `ports`
(`{at, side 0 left/1 right/2 top/3 bottom, depth}`). Internal transitions have no
points. Read a data file with a quick script before using it.

### Look

- Prose column ≤ 760 px; figures may run to the section's 1180 px.
- Captions say what to try: "**Drag** a node…", "**Step** through…".
- One idea per figure. Prefer a small, crisp toy over a cluttered real chart for algorithms; use real charts to show what the rule does on the page.
