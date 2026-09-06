# scav — PRD / ERD

Statechart authoring, layout, rendering. Normative. Terse by intent: read by agents and by humans ramping up.

`[OPEN]` marks unsettled decisions. Everything else is settled; changing it needs new evidence, not a new opinion.

**A living document, and a peer of the source, not its upstream.** The two co-evolve: a design decision lands here first, an implementation finding lands in the code first, and each review reconciles the pair. Where this document describes something the tree does not yet do, it says so with `[OWED]`; where the tree taught the design something, the section records the measurement. A claim here that the tree contradicts is a defect in one of them, and the review decides which.

## 0. Status ledger

Reconciled against the tree on 2026-09-05. Phase names are §17's.

| Built and green | Owed, with an owner section |
|---|---|
| PB bootstrap, P0 language, P1 model spine, P2 loader, P3 printer and CLI, P4 space tables, P5a metrics + `DrawList` + reference builder, P5b SVG + baseline harness, P5c ABI JSON + Python binding, P6 real layout, P7a router boundary, P7b orthogonal router, P7c nudging, P7d label placement, inflation, and the bench, P7e the combinatorial stage, the attachment, and the element suite | **the incumbent comparison**, rescheduled to P9's exit because a single-candidate render measures the missing search (§11.12) · **routing between two concurrent submachines of one state** (§11.8), which no corpus chart exercises and the element suite pins · rip-up-and-reroute for the boxes no strip fits (§11.9) · P8 determinism infrastructure and `selftest` (§6) · P9 search and calibration (§11.10) · P10 viewer · P11 wasm · P12 editor |
| | **ABI: entity rows, builder, validate, columns, attrs, and path resolution from C** (§16) · **hints** (§14) · **top/bottom port sides** (§11.3) · **a bulk attribute path** (§7.3) · **`scav_read_file`/`scav_load_file` in C** (§16.2) · **`--strict-attrs`** (§8) · **ImGui backend** (§3.1) |

The bottom-left cell is empty on purpose: everything built is in the top row, and everything owed names the section that owns it.

**Two scales, and every number in this document is on one of them.** Phase tables in §17 score with no space requests, which is what `golden/layout/corpus_cost.txt` holds; rendered-quality numbers come from real text through `measure_chart`, which is what `scav render`, `dump --layout` and `tools/audit.py` produce. `label` is identically zero on the first and nonzero on the second (§11.6). A number carried across the two checks nothing.

---

## 1. Purpose

Harel statecharts have no adequate tooling. Verified:

- ELK's default `SEPARATE_CHILDREN` **silently drops** edges that skip a containment level — `ElkGraphImporter.importFlatGraph` has an `if` with no `else`. No route, no warning.
- Its only fix, `INCLUDE_CHILDREN`, is "generally a little bit buggy" per its maintainer (eclipse-elk#891, open since 2022) and collapses the subtree into one layout run, losing per-subtree `direction` (elkjs#26 WONTFIX) and `aspectRatio` (elk#773). `topdownLayout` **throws** if combined with it, so top-down packing and correct cross-hierarchy edges are mutually exclusive.
- Mermaid documents the limitation. itemis CREATE deleted its auto-layout. Stately declares layout a non-goal. Across ~20 tools surveyed, **none publishes a statechart layout algorithm.**

Vocabulary (ELK's): a **short hierarchical edge** crosses one boundary; a **long hierarchical edge** crosses more.

**scav's bet:** Phase-0 port splitting (§11.1) keeps submachines independently *orderable* while long hierarchical edges stay correct. Prior art has one or the other.

## 2. Non-goals

- **No runtime.** No event dispatch, no semantics, no notion of a triggering event (§7.1). Holds for live highlighting too (§13): the host computes what is active.
- **No semantic validation.** Reachability, determinism, ambiguity: out. Structural integrity only (§10).
- **No SCXML in core.** Not a source of truth, not required for interop. It ships as a *plugin* (§8.2), the worked example of the extension boundary.
- **No appearance.** scav owns geometry and the `DrawList` vocabulary; what things look like is the application's (§3).
- **No interpreter in core.** Scripting is an application concern (§8.3).
- **No PDF in v1** (§12.1). SVG only.
- **No off-page connector glyphs in v1.** Designed for, not built (§11.7).

## 3. Architecture

**scav is a toolkit, not a framework.** It owns the model, the layout, and the vocabularies that layout and drawing must agree on. It does not own appearance, composition, or the render loop — those belong to the application.

**What scav owns as a contract:**

| | Why it must be scav's |
|---|---|
| data model + columns (§7) | the thing everything reads |
| layout, writing **geometry columns** (§11) | the value proposition |
| the **`DrawList`** type (§12) | builder and backend must agree on the vocabulary |
| **space requests** (§8.1) | the only way content affects layout, and layout is scav's |
| **text metrics** (§11.9) | builder and backend must produce identical numbers |

**What scav ships but does not own** — all optional, all replaceable: interior-subdivision and shape helpers (§8.1.1), a reference `DrawList` builder for standard statechart appearance, an SVG backend, an ImGui backend.

**What the application owns:** how it organizes its builder, what else it draws, and the imperative render function that turns a `DrawList` into ImGui calls, SVG text, a PDF stream, or anything else.

### 3.0 Two tests for adding anything

> **Is this describing the format, or the reference implementation?** If the latter it is that implementation's documentation, not a contract — let callers pass their own values.
>
> **A helper is a function the application calls. A framework is a function that calls the application.**

Rejected by these tests, so do not re-propose: named appearance slots with composition axes; priority ordering of contributions; retained `decay_ms`; reserved depth bands. Each enumerated a vocabulary for a decision the app makes better, and each collapsed to one line of app code.

Pipeline. Layout and everything after it is a deterministic function of its inputs.

```
app: measure content -> space requests    (uses scav text metrics)
        |
scav:   layout  ->  writes geometry columns into the model
        |
app: build DrawList from model columns    (may use scav's reference builder)
        |
app: render DrawList                      (may use scav's SVG or ImGui backend)
```

`layout` is font-blind and appearance-blind: it consumes extents and constraints, never content. It writes **derived** columns only and never touches authored data (§7).

Platforms: native (macOS/Linux/Windows) and **`wasm32-wasi`** for core, layout, draw, and the SVG backend. The ImGui viewer is **native first**, with a documented path to the browser (§16.3) rather than a second wasm target in v1.

### 3.1 Libraries and applications

| Library | Holds | Links |
|---|---|---|
| **`libscavcore`** | model, columns, string pool, builder, validation, `.scav` parse + canonical print, JSON dump, includes, resolution | — |
| **`libscavlayout`** | space requests, phases 0–3, routers, cost, threading shim | `libscavcore` |
| **`libscavdraw`** | the `DrawList` type, text metrics, optional helpers (§8.1.1), the reference builder | `libscavcore` |
| **`libscavsvg`** | reference backend: `DrawList` -> SVG | `libscavdraw` |
| **`libscavimgui`** | reference backend: `DrawList` -> ImGui draw calls | `libscavdraw` + ImGui |

Applications, each supplying (or reusing) a builder and a backend:

| App | Is |
|---|---|
| `scav` | the CLI — see §3.2 |
| `scavview` | ImGui viewer [P10]. Embeds Lua so users can script appearance without rebuilding it (§8.3) |
| *yours* | e.g. an enemy-AI editor: links core+layout+draw, writes a builder that also draws threat radii, writes its own ImGui backend. No scav change required |

`libscavdraw` depends on `libscavcore` but **not** on `libscavlayout`: a builder reads geometry columns, and does not care who wrote them. Enforced in CI, along with the rule that no library links anything above it.

Internally each library is one static archive built twice, shipping and `testable`, and the subsystem boundaries inside it are directories plus a CI layering check (`scav_check_layering`), not separate CMake targets. Per-subsystem `OBJECT` libraries were the plan and were not built: the directory rule and the include-path rule below already make a cross-subsystem include a build failure, and a target per subsystem would have been machinery for a requirement nothing exercises.

### 3.2 The CLI

**Primary user is a build system, not a person.** Nobody types these in a loop; CI does — `render` in a docs pipeline, `fmt --check` and `check` as PR gates, `deps` so a stale diagram is impossible. That is the utility; the rest is that having a binary is nearly free once the library exists.

It is also the end-to-end exercise of the load path over a real filesystem — pending list, real paths in diagnostics, a cycle reported against files rather than buffers — which is one of the three transports P2's exit gate requires (§16.2).

| Verb | For |
|---|---|
| `render` | chart -> SVG. Docs pipelines, PR preview images. The gap that motivated scav: nothing renders `.puml` today |
| `fmt` | canonical print, `--check` to gate. §15's canonicity is a property of *running the printer*, not of the format — no verb, no pre-commit hook, and the format hash and merge story both degrade |
| `check` | structural validation (§10) as a PR gate |
| `deps` | the document network as a `make`/`ninja` depfile. Chart A includes B; without this, editing B either leaves A's diagram stale or forces a full re-render. `gcc -M`, and the need follows from includes existing |
| `dump` | `--json` for non-C++ consumers and `jq`; `--layout` runs layout first and includes the geometry columns; `--hash` prints the structural digest alone, which is how two transports of one network are compared without a golden |
| `selftest` | recompute §6's hashes on this toolchain and diff against the goldens. **[OWED, P8]**: the five verbs above exist; this one lands with the determinism infrastructure it reports on |

**No `gen`.** Synthetic chart generation is test tooling (PB, P0) and lives in the harness; shipping it as a verb would imply a user need nobody has. Same reasoning retired a separate `layout` verb — dumping geometry is `dump --layout`, not a second command.

### 3.3 Repository layout

Mirrors `~/src/envy`: SHA-pinned deps under `cmake/deps/`, unit tests adjacent to sources, Python functional tests, sanitizer suppressions, presets.

```
include/scav/          the cross-library vocabulary: POD spellings, no functions
src/<lib>/include/scav/  that library's public API: `scav_<lib>.h`, plus `scav_<lib>_c.h`
                       where it projects a C surface. A C header is a second language,
                       not a second place to look for the same symbol
src/scav_*.h           determinism primitives that belong to no subsystem: the vendored
                       stable sort and hash §6 mandates in place of the standard library's.
                       Private — every library reaches them by `-Isrc`, none installs them
src/<lib>/*.cpp        library-wide, owned by no subsystem — core's diagnostics, which
                       every layer below produces
src/core/lang/         .scav lexer, parser, canonical printer, JSON dump. Bytes to
                       statements, and nothing here knows what an entity is
src/core/model/        columnar aggregates, ids, string pool, builder, lowering,
                       path resolution, structural digest, validation
src/core/load/         the loader, document-name resolution, the filesystem
                       batteries. A directory of its own because §16.2 is titled
                       "loading and parsing are separate systems" — it composes `lang`
                       and `model` and is neither
src/<lib>/tests/       suite-level test classes (functional_*, fuzz_*, perf_*) + shared fixtures;
                       unit tests stay adjacent to their subject as <foo>_tests.cpp
src/layout/            space requests, phases 0-3, routers, cost; thread shim [OWED, P8].
                       gauntlet_tests.cpp is the element suite over the charts above
src/draw/              DrawList type, text metrics, helpers, reference builder
src/svg/               reference SVG backend
src/imgui/             reference ImGui backend [OWED, P10]
apps/cli/              the scav executable
apps/view/             ImGui viewer + its Lua host [OWED, P10]
plugins/libhsm/        columns, attributes, builder contribution [OWED; importer + codegen: §17]
plugins/scxml/         reference example: importer, exporter, builder contribution [OWED]
assets/font/           the bundled TTF — a layout-hash input, so it is versioned here
abi/scav_abi.json      committed golden ABI description (§16), extracted from the
                       C headers — the description is the ABI, the headers are the API
bindings/python/scav/  `_abi.py` generated from that JSON; `__init__.py` the thin
                       hand-written idiomatic half. The generated half never drifts
test_data/charts/      corpus: synthetic fixtures + hand-transcribed real charts
test_data/charts/gauntlet/  one layout element per chart, for the properties a
                       whole diagram answers about sixty transitions at once (5)
test_data/golden/      drawlist/, svg/, layout/, dump/ — see below
functional_tests/      Python, drives the CLI and the C ABI via ctypes
cmake/*.cmake          the build's own modules: warnings, sanitizers, coverage, layering,
                       test registration. No `deps/`: envy packages every third-party
                       dependency the tree has, so nothing needs a SHA-pinned fetch
tools/                 project-wide tooling only; a script one library uses lives with that library
envy.lua               toolchain + package manifest, and envy's root marker
build.sh  build.bat    one command, clean checkout to green tests
CMakePresets.json      one preset per §6 matrix cell, generated by tools/gen_presets.py
.github/workflows/     the matrix, on bare runners, plus the weekly container image
out/                   gitignored: all build output + the envy cache (§4.2)
```

No `docs/`: this document and the README are the documentation, and a directory for prose nobody has written would be an invitation to write it somewhere other than here.

`apps/` is separate from `src/` because an application is a *consumer* — that keeps the CLI and viewer from quietly becoming privileged layers.

**Unit tests are adjacent** and compile library sources with `-DSCAV_TESTING`, which is how `SCAV_INTERNAL_BEGIN`/`_END` (§5) drop the anonymous namespace. Each library builds twice, shipping and testable; both are matrix rows (§6).

**Public and private are a directory, not a convention.** A library's API is `src/<lib>/include/scav/scav_*.h` and that directory is its only `PUBLIC` include path; everything else under `src/<lib>/` — private headers, sources, tests — needs `-Isrc`, which is `PRIVATE` to the library and its own tests. A consumer therefore *cannot* name a private header, in the build tree or in an installed one, and `func.install_consumer` compiles every public header of every library with nothing on its include path but the install prefix. **One public header per library** — `scav/scav_core.h` is all of `libscavcore` — so a reader never has to work out which of several headers a symbol lives in. Within it, **every function carries the prefix of its section** (`parse_`, `lex_`, `source_text_`, `diag_`, `syntax_`, `string_`), which makes the section list a table of contents and a symbol self-locating. Splitting a library's API across headers is the thing this rule exists to prevent: it trades one decision the author makes once for a decision every caller makes repeatedly.

The `scav_` prefix on a *filename* is for the consumer's include path, where `scav/scav_parser.h` sits among other projects' headers. It is deliberately **not** carried onto C++ identifiers: `namespace scav` already supplies it, and §16 reserves `scav_lower_snake` for ABI-shaped types, so a `std::`-holding type named `scav_parsed_document` would advertise a guarantee it cannot keep.

**Goldens are layered by stage**: `layout/` (structural + coordinate hashes), `drawlist/` (the canonical render IR, §12, the primary surface), `svg/` (thin serializer check). A layout change moves the first two; an SVG-writer change moves only the third. `svg/` alone → serializer bug; all three → review starts at `layout/`.

Third-party, all permissive, none vendored by copy: **doctest** (MIT), **ImGui** (MIT, native viewer only), **stb_image** (MIT/public domain, native viewer only), **lua** + **sol2** (viewer only — see §8.3). Plus the bundled TTF, which is a layout-hash input (§18). **envy provisions both the toolchain and the packages** (§4.2); `cmake/deps/` covers only what envy does not, SHA-pinned.

## 4. Language rules

C++20, flat `extern "C"` API per layer.

| Rule | |
|---|---|
| Data | POD aggregates only. No inheritance, no methods, no virtuals. Strong id types: `struct StateId { uint32_t v; };` |
| Behavior | Free functions, POD in, POD out |
| Errors | Return codes + out-params. **No exceptions** (`-fno-exceptions`) |
| Bytes | `scav_byte` = alias for **`unsigned char`** — see below |
| Integers | `<cstdint>` fixed-width only |
| Templates | **Function templates parameterized on a functor are encouraged** — see below. Generic containers of `T` are not needed (`std::vector` covers it), and type-level computation is discouraged. Concepts: none |
| Containers | `std::vector`, `std::array`. Heap fine |
| Invariant | **No pointer or reference between records.** Records link by ordinal. Algorithms may hold pointers while working |
| Unavailable | RTTI, exceptions, modules, coroutines, `<regex>`, virtual inheritance. Enforced by build flags or by portability — not a style question |
| `std::format` | Permitted, and preferred over stream formatting: default specifiers are locale-independent by design, and float conversion goes through shortest-round-trip rather than libm. Two conditions — format strings must be **literals**, since a `consteval`-checked literal turns a bad one into a compile error while `vformat` on a runtime string throws `format_error` into `-fno-exceptions`; and never `{:L}`, which opts back into the locale |
| Streams | `<sstream>` for building strings is fine. What a **library** must not do is touch the global objects — `cout`, `cerr`, `cin` — because a library reports by returning data (§6's diagnostic triple), and because including `<iostream>` instantiates them with static constructors in every consumer. Apps and the CLI use them freely |
| Discouraged | `<ranges>` and view pipelines · clever `<algorithm>` compositions · stateful classes · work in constructors · operator overloading beyond id comparison · SFINAE, CRTP, type-list computation · `auto` where the type isn't locally obvious. Usable with a comment saying why; the default answer is the plain loop |
| Build | CMake + Ninja, toolchain pinned by envy — see §4.2 |

The bar for a discouraged construct is that a reader can still follow control flow from the source. That's what rules them out by default, and it's also what makes exceptions legitimate when the construct genuinely reads better.

**Preferred idioms:**

- **Snap-together function templates over function-pointer indirection.** The useful template work here is not containers of `T` — it's algorithms parameterized on a functor, monomorphized so the functor **inlines**:
  ```cpp
  template <typename T, typename Less>
  void scav_stable_sort(std::vector<T>& v, Less less);  // less() inlines
  ```
  C's `qsort` shape pays an indirect call per comparison and blocks inlining entirely. Sorting is in the hot path — intra-rank ordering, packing, label matching, canonical output — and comparators here are mandatory total orders (§6), so this is where the cost lands. Same pattern for sweeps taking a predicate and for `argmin(Cost, index)` reductions.

- **`const` by default, initialized by an IIFE when the value needs computing.** `auto` is the right tool here and its use is encouraged, not merely tolerated:
  ```cpp
  auto const rank_count = [&] {
    uint32_t n = 0;
    for (auto const& s : submachine_states) n = std::max(n, s.rank + 1u);
    return n;
  }();
  ```
  Preferring this over a mutable local declared early and assigned later is a real gain in a codebase this size.
- Explicit loops with named indices; early returns; flat control flow; functions short enough to read in one screen.

**`scav_byte` is `unsigned char`, not `uint8_t`.** Only `char`, `unsigned char`, and `std::byte` may alias an object representation; `uint8_t` need not be one of them, so byte inspection through it is UB where it isn't. Not `std::byte` either: no arithmetic operators, and the C ABI needs `unsigned char`.

**Runtime polymorphism has exactly one permitted site:** the internal `Router` abstract base class (§11.5: no data members, one virtual call per frame, never crossing the C ABI, which sees routers by name). The P12 editor may add a second if the command-buffer mechanism wins over arena snapshots (§17). Everywhere else: static selection, separate binaries, or link-time choice.

### 4.1 Data structure discipline

**About the kind of data, not the layer.** One test:

> Does this outlive the call, get serialized, get hashed into output, or get addressed by path?

**Yes to any** → core model: columnar POD aggregates cross-indexed by ordinal (§7). No pointers between records, no hash maps, no nodes.

**No to all** → transient scratch, whatever is convenient: `unordered_map`, priority queues, visibility graphs, sweep structures, union-find. Not an exception grudgingly granted to `layout` — the normal treatment of data built and discarded inside one call.

Two constraints survive on scratch (§6), both structurally enforceable: **never iterate an unordered container where order reaches output**, and **never let a hash value escape**. `HashMap` makes the first a compile error.

What this rules out is not hash maps but a **graph of long-lived heap nodes pointing at each other** — the thing that makes a model unserializable, unhashable, and untestable.

### 4.2 Build system

**CMake + Ninja.** GN is excellent for first-party code in a private ecosystem and hostile to sharing: no `install()`, no export/config packages, no `find_package`. CMake is clunky and is the lingua franca, and scav is a library meant to be consumed — including by a wheel that needs a shared object with proper install rules (§16.1). That, plus presets expressing §6's matrix directly, is the whole argument.

**Toolchain provisioned by [envy](https://github.com/charlesnicholson/envy) — for *our* CI, not for users.** §6's evidence is only as good as the compilers that produced it, and "clang" meaning whatever is on `PATH` makes a matrix row unreproducible; envy pins exact builds by fingerprint. It also packages ninja, python, and clang-tools.

**Everything scav generates lives under `out/`, and nothing outside the source tree.** One gitignored directory, so a clean checkout plus `rm -rf out` is a factory reset and no build step writes to `$HOME`:

```
out/.envy/            envy's package cache — the project default (see below)
out/rel/  out/dbg/    per-configuration build trees, named for the CMake preset
out/test/             functional-test scratch, golden diffs, perf reports
```

**The envy cache defaults into the build root**, declared in the manifest. `envy.lua` sits at the repo root and is envy's root marker, so a relative path is relative to it:

```lua
-- @envy cache-local "out/.envy"
```

envy's own default is a user-wide cache; naming a project tree is what opts out. One directive for every platform, because `cache-local` is a literal project-relative path — no absolute values, no `..`, and no expansion of any kind, which is what leaves the binary and both bootstrap launchers nothing to disagree about. (envy ≤ 0.1.9 had `cache-posix`/`cache-win` holding absolute paths with shell-style expansion; four readers implemented it four ways. Those directives are now hard errors naming their replacement, so this migration could not fail silently.)

**The sandbox is the promise; sharing is the opt-in.** Someone who builds scav once and does not care about envy must be able to `rm -rf out` and have nothing left, so the manifest's choice is checked by a test that runs whether or not envy is required. A maintainer with several worktrees runs `./bin/envy cache --shared` once, which records a zero-byte `.envy-cache-shared` marker beside the manifest; `--local` reverses it. A marker is written only when it diverges from the manifest, so most projects never have one. Markers are gitignored and listed in `.worktreeinclude`, which is what carries the choice into a new worktree.

Precedence, identical in every reader: `ENVY_CACHE_ROOT` (absolute only) > marker > `@envy cache-mode` > `@envy cache-local` > user-wide. `build.sh` prints the resolved root, so a 540 MB per-checkout copy is never silent.

**Sharing one cache is safe, and measured rather than assumed.** Packages are keyed by content fingerprint, so an override is usually a partial hit on arrival — scav's `envy.cmake@r0` is the same build another project already fetched. Three simultaneous `envy sync` runs against one fresh cache all succeed. Parallel agents in separate worktrees can share one cache without a lock of our own.

**Not a CMake preset**, which is the obvious place to look: envy resolves the cache and hands back absolute paths to cmake, ninja and python before CMake is invoked at all.

**Not a build prerequisite, and deliberately so.** §6's claim is about the C++ abstract machine, so any conforming C++20 toolchain is supported and `scav selftest` is how a user confirms theirs agrees. Requiring envy to build would contradict that. Standard CMake: `find_package`, no vendored toolchain assumptions, no compiler-specific flags in the exported targets.

## 5. Testing

**All code exhaustively tested, unit and functional. A phase is not done until its tests are.**

Testability comes from pure functions, not seams. **Mocks and interface seams are rejected** — they cost an indirection and make control flow unreadable without running it. Intrusive access instead:

```cpp
// Brackets the *definition* of a function that would otherwise be file-local.
#ifdef SCAV_TESTING
#  define SCAV_INTERNAL_BEGIN     // external linkage; tests declare the prototype and link
#  define SCAV_INTERNAL_END
#else
#  define SCAV_INTERNAL_BEGIN namespace {
#  define SCAV_INTERNAL_END }
#endif
```

A bracket pair rather than a `static` keyword, because the file-local spelling in this codebase is the anonymous namespace, and a keyword that has to appear on every definition is one a reader forgets. Never in a header: the test declares the prototype itself.

**Unit means unit: no disk, no parse, no prior stage.** A unit test builds its subject's inputs directly in RAM — as literal PODs, or through the in-memory chart builders where a real model is what is actually under test — and asserts on the return value. It does not read a `.scav` file, does not run the stage before the one under test in order to manufacture an input, and does not open a font. The payoff grows with pipeline depth: a defect in rank assignment should fail one test that types in a graph, not a golden hash three stages downstream that reports only that something moved. This constrains code shape as much as test shape, which is why every stage's intermediate type is declared in a header rather than being file-local (§11). Functional tests do the composing, over the corpus, end to end — the two classes answer different questions and neither substitutes for the other.

All layout arithmetic is integer, so inlining cannot change results: the `testable` build must be byte-identical to release, and is a row in the matrix. Divergence means UB.

**The corpus says whether a diagram comes out well; it cannot say which element was wrong.** Every layout defect this project has found was found on a corpus chart and then bisected by hand to one shape — a fork bar, a mark, a pair of states each other's target — because a chart with sixty transitions in it answers about all sixty at once. So there is a third body of input beside the unit tests and the corpus: `test_data/charts/gauntlet/`, one element per chart, small enough that every route in it can be named, and `src/layout/gauntlet_tests.cpp`, which holds each to the properties a reader checks rather than to a hash. A chart earns its place there by isolating a *shape*, and adding one means adding the assertion that would have caught the defect. Where a property does not hold yet the chart is carved out **with a count pinned beside it and the section that owns it named**, because a carve-out with no number on it is an excuse. **The directory and the array that suite iterates are held to the same list**, by a functional test that also runs `fmt --check`, `check` and `render` over every chart in it: a chart nobody asserts anything about and a chart nobody ever renders are the same gap, and both are the kind a hand-maintained list opens.

Required test classes: **unit** (every internal function, doctest) · **functional** (full pipeline over the corpus) · **gauntlet** (one layout element per chart, held to reader-visible properties) · **golden** (canonical serialization, structural hash, coordinate hash, **`DrawList`** — the primary surface, §12 — plus a thin SVG serializer check, ABI JSON) · **property** (round-trip identity, all refs resolve, zero box overlap, zero edge-through-box, surrogate cost ranks like exact cost) · **determinism** (§6) · **sanitizer** (UBSan signed-overflow+shift, ASan, TSan, and MSan on Linux/clang, which is the only place an instrumented `libc++` exists — see PB) · **fuzz** (deserializer and reference resolver — untrusted input) · **binding** (drive the C ABI from Python/ctypes in CI) · **baseline** (§11.12) · **performance** (see below) · **regression** (every fixed bug leaves a test).

**Performance tests assert floors, not times.** Inputs are generated **in RAM** — a disk-backed benchmark measures the filesystem. Each asserts a throughput floor and a peak-memory-to-input ratio, per stage, on a named machine. Their job is catching accidental `O(n²)`, not tracking milliseconds, so they are **not** matrix rows and **not** goldens (§6): timing is not reproducible and must never gate a determinism claim.

Measure branch coverage; an untested file fails the build. No percentage target.

## 6. Determinism contract

**Layout output is a function of the C++ abstract machine, not of any implementation.** That is the claim, stated stronger than a test matrix: *any* conforming C++20 toolchain must produce the same bytes, because the code depends on nothing an implementation is free to choose. **Users bring their own compilers** — the six triples below are *evidence* for the property, not its definition, and a seventh toolchain is expected to agree without anyone having tried it.

Two obligations follow. **No implementation-defined or unspecified behaviour may reach output** — the banned list below is that rule enumerated, and it is closed rather than advisory. And **`scav selftest`** recomputes the structural and coordinate hashes on the corpus and diffs them against the committed goldens, so a user on an untested compiler verifies the claim in one command instead of trusting it. A failure is a scav bug until proven otherwise.

**Canonical matrix**, defined once and referenced everywhere:

```
{ macOS/clang/libc++, Linux/clang/libc++, Linux/clang/libstdc++,
  Linux/gcc/libstdc++, Windows/MSVC/MSVC STL, Windows/clang/MSVC STL }
  x {Debug, Release, testable} x {1,2,3,5,8,13,16} threads
  + wasm32-wasi single-threaded
```

Six realizable platform triples, not a 27-cell cross product — most combinations of that product do not exist. They are chosen to span the axes that historically diverge: three standard libraries, three vendors' codegen, LP64 vs LLP64 vs ILP32, and x86_64 vs arm64 vs wasm32.

Odd and prime thread counts are mandatory — they expose reduction-shape bugs powers of two hide. Tier it: a small blocking subset per PR, full grid nightly and advisory, or a trickle of one-cell failures halts velocity.

**Rules:**

- **Integer only in the metrics helper and `layout`.** The space tables are `int32_t` by construction. Float appears only in a backend's geometry, and never in emitted text (§12.1).
- Shard count is `clamp(bit_ceil(entity_count / 64), 1, 256)` — a pure function of the model, never `hardware_concurrency()`, never tunable. Shards are work items; worker count never affects results, and the null backend runs the same shards inline in index order.
- Randomness is **stateless and position-addressed**: `rnd(seed, phase, item_index, step)` via the splitmix64 finalizer. **Never** a per-shard stream and **never** keyed on shard index — that would couple output to the decomposition.
- Synthesized ids derive from a **global stable key**, never from shard ranges. Per synthesized kind: port = `(compound_state, side, transition, crossing_depth)`; split segment = `(transition, segment_ordinal)`; label dummy = `(transition, rank)`; routing-graph node = rank under a total sort on `(x, y, plane, kind)`.
- Reductions merge in **index order**. Combine operators must be **associative**; commutativity is not required. `+`/`min`/`max`/`xor` qualify outright. List append and `argmin(value, index)` are associative but not commutative — legal, and index order is what makes them well-defined, which is why §11.10 reduces a portfolio that way. **Saturating add is banned**: it is not associative, so a shard split changes the answer.
- Every comparator is a **total order** ending in a stable input-derived key. Ties are forks.
- **Vendored stable merge sort** for any sort whose result reaches output. `std::sort` is permitted only in tests.
- All rectangles are **half-open**: `[x0,x1) x [y0,y1)`.
- Fixed iteration counts. Every retry loop states its cap, its integer increment schedule, its subject order, and its terminal diagnostic.
- Diagnostics are collected per shard, concatenated in shard order, then sorted by `(code, subject_kind, subject_index)`. They are part of the golden artifact. **A diagnostic carries nothing but that triple** — file, line, column, and the offending text are derived by walking to the statement's `src` span (§7), so no layer threads positions through its call stack. Diagnostics that precede the model — normalization, lexing, parsing — have no entity to name, so they carry `subject_kind = None` plus a raw source span directly: no statement exists yet to walk to. **An ordinal of `INVALID` under a real kind names the kind and no row** — a column whose count disagrees with its entity array reports that entity's kind and `INVALID`, since `ElemKind` has no `Column` and cannot grow without an ABI break — and `None` carries `INVALID` and nothing else. So a reader never has to know, per code, what an ordinal means. On a model mutated since load that span is cleared, and a diagnostic degrades to the triple alone; goldens run on unmutated models, so the artifact is unaffected.
- **Text is normalized at parse**: LF-only, BOM stripped, NFC. Ship `.gitattributes` with `*.scav -text`. Without this, `core.autocrlf` on Windows and NFD on macOS change the name and label *bytes* — same commit, different canonical print, and different text metrics, so different space requests and different coordinates. NFC needs a table: it is a **P0** dependency.
- Threading via a shim over pthreads / Win32 / **null (inline)**. Not C11 `<threads.h>`. The null backend makes WASI and the matrix work.

**Banned constructs.** Unlike §4's discouraged list, these are correctness bans with no escape hatch — each is a documented cross-platform divergence, not a readability preference:

| | Why |
|---|---|
| `int`, `long`, `unsigned`, `short`, plain `char` | `long` is 32-bit on MSVC (LLP64); plain `char` signedness differs x86 vs ARM, corrupting id hashing. `const char*` is permitted at the C ABI for NUL-terminated *input* names only, never stored in a record and never hashed |
| `size_t` in value computation | 32-bit on wasm32; unsigned wrap is *defined*, so it silently gives a different correct answer per platform. A `size_t` **length or count parameter** at an API boundary is correct and expected — that is what the type is for. Narrow it with `narrow<T>()` on entry and compute in the fixed-width type |
| `std::hash` | Permitted to differ **between runs of the same binary** |
| **Iterating** `unordered_map`/`_set` where order can reach output | Iteration order varies across all three standard libraries. Key lookup is fine and fast — see below |
| Pointer-keyed containers | Address order; ASLR randomizes run to run |
| `<random>` — engines **and** distributions | Only the statistical requirement is standardized; implementations and versions differ; distributions are stateful |
| Raw `/` `%` on possibly-negative values | Truncates toward zero, so grid bucketing breaks asymmetrically about the origin. Use `floor_div`/`floor_mod`/`ceil_div`, defined once, negatives specified. These are the **only** division primitives |
| `__builtin_clz`/`ctz` | UB at 0; x86 `BSR` and ARM `CLZ` disagree. Use `<bit>` |
| `<cmath>` in the core | libm differs across glibc/musl/Apple/UCRT. Integer helpers only: `isqrt` (floor), `ilog2 = bit_width(x)-1`, ratio compare by cross-multiplication |
| Side effects in function arguments | Argument evaluation order is unspecified |
| `memcmp`/`memcpy`-hashing a struct | Reads padding, whose values are unspecified |
| Locale-aware compare, `setlocale`, and locale-sensitive stream formatting | Environment-dependent: the same `<<` on the same integer differs under a non-classic locale, so a stream whose bytes reach output is `imbue`d with `std::locale::classic()` at construction or is not used for numbers at all. Collation is byte-wise only — Hebrew/Arabic therefore sort in codepoint order, a permanent accepted trade |
| `directory_iterator` order | Unspecified. Sort collected paths byte-wise |
| **Bitfields** | Allocation order, straddling, and padding are all implementation-defined. Use explicit masks on a fixed-width integer |
| **`__int128`, `__builtin_*`, `#pragma pack`, attributes outside `[[...]]`** | Not standard C++, so they defeat the whole claim. §11.2's budget exists to keep degree-4 arithmetic — and therefore `int128` — off the table |
| Shifting by `>= ` the operand width, or left-shifting a negative | UB. Shift counts are asserted in range; `<bit>` covers the rest |
| Narrowing without an explicit range check | Implementation-defined before C++20 and easy to get wrong after. One `narrow<T>()` helper, checked in every build |
| `enum` without an explicit underlying type | The type is otherwise the implementation's choice, which changes struct layout and wire size |

C++20's P0907 fixed two's-complement *representation* but kept signed overflow UB, and optimizers exploit it. Do not use `-fwrapv` — MSVC has no equivalent, so it would introduce a platform semantic difference. Prove no overflow (§11.2), net it with UBSan.

**Flags are belt-and-braces, never semantics.** `-fno-exceptions`, `-fno-rtti`, and their absence must all produce the same bytes: the code never throws and never asks a type its identity, so a compiler lacking those switches is still a supported compiler. Same for optimization level — with no UB, `-O0` and `-O3` cannot disagree, which is why the `testable` build is a matrix row rather than a trusted equivalence.

**`libscavlayout` uses a documented standard-library subset** — `<cstdint>`, `<bit>`, `<limits>`, `<vector>`, `<array>`, `<utility>`, `<type_traits>`, `<cstring>` — and nothing else. Every sort, hash, and container with iteration order that reaches output is scav's own (above). The subset is enforced by an include-check in CI, so "bring your own compiler" does not quietly mean "bring your own conforming `<algorithm>`".

**Scope of this section: anything that can reach layout geometry or rendered output.** A structure that only ferries data inside one call is outside it, and `std::unordered_map` is the right choice there — deterministic *by usage*, because a key lookup has no order and the hash value never escapes as a bucket index. Enforce that structurally: `HashMap<K,V>` exposes `find`/`at`/`insert` and **no `begin()`/`end()`**, so "never iterated" is a compile error rather than a review comment.

**Golden hash.** Split into a **structural hash** (ranks, orders, port assignments, bend sequences) and a **coordinate hash**, so a translation-only change is a reviewable diff instead of a global reflow.

**The inputs digest is a third value beside them, not a seed for them.** It covers profile id, packer choice, router name and version, **and the space-request columns** — a golden is reproducible only against a stated measurement policy, and the corpus goldens use the reference builder's. It lands in `scav.geom.inputs` (§11.7a) so it round-trips the model and a binding can read it, and a golden row is `inputs structural coordinate`. Seeding the two geometry hashes with it was the obvious shape and is wrong: the space tables move whenever a label's width does, so the structural hash would move on every remeasure and the split would report a global reflow for a change that reordered nothing. Three values say *which inputs* and *what moved* separately, which is what a reviewer needs.

**Font identity and version reach that digest through the space tables, not as an argument.** Layout is font-blind by construction — text arrives only as integers the app measured — so a font field on `scav_layout_opts` would be a knob layout never reads and a caller could set wrongly. Two fonts that measure one corpus identically *should* hash identically at this stage, because layout genuinely produced the same geometry; the difference is real at the `DrawList`, where glyph advances and `textLength` live, and that is where font identity is hashed explicitly.

**The hash is xxHash32, ours rather than the standard library's.** `std::hash` is permitted to differ between runs of one binary, let alone between implementations. xxhash is not cryptographic and does not need to be: what a golden wants is speed and even distribution over inputs measured in kilobytes. Lane reads are assembled from bytes rather than cast, so a big-endian host agrees, and rotation goes through `<bit>` because `__builtin_rotl` is not standard C++ and a hand-rolled shift pair is UB at a rotation of zero.

**The model's structural digest arrives before layout does** (P2), because P2's exit gate has to compare one network loaded three ways and there are no coordinates yet to compare. It is a *serialization* first and a hash second: field by field in array order, never a struct's bytes — padding is unspecified — and length-prefixed on every string, so two adjacent names cannot spell one. The bytes are exposed alongside the hash because when two models disagree, diffing them says where and a hash only says that. Excluded: document names, which differ legitimately between a filesystem, a buffer, and a URL; statement ids and source spans, which say where a thing was written rather than what it is; and columns, which are the extension's to hash. P8 seeds the layout structural hash from this.

## 7. Data model

**Not object-oriented. Flat arrays of POD aggregates linked by ordinal.** Column boundaries follow natural groupings, not individual fields.

**The point is end-to-end traceability, and that is the real payoff of columnar storage** — ahead of serialization mechanics, determinism, or cache behaviour. The model is the single place to look. Any function anywhere in the pipeline can walk from a rendered primitive back to the source bytes that produced it, by following columns:

```
Prim.origin  ->  entity  ->  StmtId  ->  Statement.src  ->  the authored bytes
     (§12)                                    |
                 entity  ->  InstId  ->  Include  ->  which instantiation, and its host
```

No context object, no side table, no callback. That is why source documents live in columns rather than being discarded at parse time.

**Terminology: `state` and `submachine`**, never "region". A composite state holds one or more submachines; more than one makes it concurrent. Applies to the ERD, ABI, diagnostics, and format.

```cpp
struct DocId    { uint32_t v; };   // a parsed file
struct InstId   { uint32_t v; };   // an include instantiation = index into `includes`
struct StmtId   { uint32_t v; };   // an authored statement
struct StateId  { uint32_t v; };
struct SubmachineId { uint32_t v; };
struct TransId  { uint32_t v; };
struct StrRef   { uint32_t off, len; };            // into StringPool
struct Span     { uint32_t off, len; };            // into a side array
struct scav_point  { int32_t x, y; };              // grid units (§11.2)
struct scav_extent { int32_t w, h; };
struct scav_rect   { int32_t x, y, w, h; };        // half-open (§6); one spelling in
                                                   // both languages, never aliased
// Ids are global from the start, so endpoints are plain StateIds (§9).

constexpr uint32_t INVALID = 0xFFFF'FFFFu;        // per-id sentinel

enum class StateKind : uint32_t {   // names match the DSL's state_kind (§15), CamelCased
  Normal, Initial, Final, Choice, Junction, Fork, Join, History, DeepHistory
};
// `initial` and `final` are reachable only via `*` in the format (§15), never `kind`.
// Load-bearing for layout and rendering, not passthrough metadata. See §11.14.
enum class TransKind : uint32_t { External, Internal, Local };

struct State {
  StrRef        name;        // empty for pseudostates; see §9
  StrRef        label;       // the positional string (§15); opaque, may be empty
  SubmachineId  parent;
  StateKind     kind;
  Span          submachines; // -> submachine_ids
  Span          attrs;
  StmtId        stmt;        // the statement that declared it (§9); INVALID when code-built
  InstId        inst;        // INVALID in the root document
  uint32_t      live;        // 0 = tombstone; see §7.3
};
struct Submachine {
  StateId       owner;       // INVALID for a document root
  uint32_t      ordinal;
  StrRef        name;
  StrRef        label;
  Span          children;    // -> state_ids, document order
  Span          attrs;
  StmtId        stmt;
  InstId        inst;
  uint32_t      live;
};
struct Transition {
  StateId       src, dst;
  TransKind     kind;
  StrRef        label;       // opaque; see §7.1
  Span          attrs;
  StmtId        stmt;
  InstId        inst;
  uint32_t      live;
};
struct Include  {            // one row per instantiation; its ordinal is the InstId
  StrRef  alias;
  StrRef  path;              // the authored string, verbatim — see below
  DocId   target;            // the file it instantiates
  StateId host;              // the alias state it synthesizes; never INVALID
  StmtId  stmt;
};
struct Attr     { AttrKeyId key; StrRef value; StmtId stmt; };

struct Document {            // one per distinct *file*, parsed once (P0)
  StrRef   path;
  Span     text;             // -> src_bytes
  Span     statements;       // -> stmts, authored source order
};
// Not §8's ElemKind, which enumerates entities: `Include` and `Attr` exist only
// as statements, `Point` and `PathBox` only as entities. Two enums, two jobs.
enum class StmtKind : uint32_t { Chart, Include, State, Submachine, Trans, Attr };

struct Statement {           // one per authored construct, shared by every instantiation
  StmtKind kind;
  DocId    doc;
  Span     src;              // -> src_bytes; valid iff unmutated since load
  Span     comments;         // -> Chart.comments, grouped by owner
  uint32_t blank_before;     // a blank line preceded it: the one whitespace recorded (§15)
};

enum class CommentPos : uint32_t { Leading, Trailing, OwnLine };
struct Trivia   { Span src; CommentPos pos; };     // -> src_bytes; includes the "//"

struct StringPool { std::vector<scav_byte> bytes; };   // StrRef carries off+len

struct Chart {
  std::vector<Document>    documents;
  std::vector<Statement>   stmts;
  std::vector<Trivia>      comments;     // Statement.comments spans into this
  std::vector<scav_byte>   src_bytes;    // normalized source (§6); never canonicalized
  std::vector<State>       states;       // indexed by StateId
  std::vector<Submachine>  submachines;
  std::vector<Transition>  transitions;
  std::vector<Include>     includes;
  std::vector<Attr>        attrs;
  std::vector<StrRef>      attr_key_names; // indexed by AttrKeyId.v; -> attr_keys
  StringPool               attr_keys;    // interned key bytes — see below
  std::vector<Column>      columns;      // §8
  StringPool               column_names; // separate pool — see below
  std::vector<StateId>     state_ids;    // Span targets
  std::vector<SubmachineId> submachine_ids;
  StringPool               strings;      // authored names and labels; append order
  StrRef                   name, label;
  SubmachineId             root_submachine;
  Span                     chart_attrs;
};
// No Project type. Documents are rows in one model, not charts to be merged.
```

**All documents share the same arrays**, each entity tagged with the statement that declared it and the instantiation it belongs to — so no flattening step and no second model shape (§9).

**An attribute carries its own `StmtId`, not its subject's.** `state On { @doc = "..." }` is two authored statements, so an `Attr` that only knew its owner could not be pointed at — a diagnostic about the attribute would name the `state` line, and an editor could not find the text to rewrite. The block spelling `@ns { a, b }` is one statement producing N rows, all naming it.

**`Include.path` is the authored string, not the resolved key.** Three candidates were available and only one works: `documents[target].path` is the name the *caller* handed to `scav_load_add` and may spell the same file differently or not be a path at all; re-lexing `Statement.src` makes the printer depend on the parser; and dropping it entirely means an `include` statement cannot be reprinted at all, which §15's canonical form requires. So it is a `StrRef` into `strings` like every other authored token. The resolved key lives in the loader, which is where fetch policy already lives.

`src_bytes` is a **separate pool from `strings`**, and neither can be derived from the other. A decoded string literal is not a span of any file — `"a\u0041b"` is nine authored bytes and three decoded ones — and neither is a `Document::path`, which the caller supplies. So `strings` is not an index into the source, it is storage. Going the other way, `src_bytes` is verbatim and never rewritten, so it cannot absorb decoded text without invalidating every `Statement.src`. "Verbatim" means post-normalization — §6 normalizes at parse (LF, no BOM, NFC), and `Statement.src` offsets index the **normalized** bytes, so reported columns are stable across platforms.

**Load-established, not serialized, not hashed.** Writing a document *produces* text; the format hash covers canonical output, not the possibly-non-canonical bytes loaded. Provenance is implied by which file an entity is written into.

**`Statement.src` is valid iff the statement is unmutated since load.** Mutation clears it, so source mapping degrades gracefully rather than lying.

**A model is one document network rooted at one document.** Unrelated charts go in separate models; nothing structurally prevents intermingling, and the result is meaningless.

**Ids are append-only with tombstones.** `StateId` is ordinal *and* array index, so compaction invalidates every app-side column keyed by it. Deletion tombstones (`live = 0`), ids never reused, **all columns tombstone in lockstep**. Compaction is explicit and an output change. Named `live` rather than `gen` because it is a liveness flag and nothing more: ids are never reused, so there is no generation to validate, and compaction renumbers wholesale so a counter would not help there either. `scav.geom.gen` (§13) is an unrelated chart-level counter — different concept, and it does not share the word.

**Three column classes, not two.** Conflating the last two is a licensed determinism break, so they are named separately:

| Class | Serialized | Hashed | Container | Written by |
|---|---|---|---|---|
| **authored** | yes, as the attributes it was projected from (§8) | format hash | columnar POD | builder API, editor |
| **derived-persistent** — the geometry columns layout writes | **no** | **layout hash**, by explicit allowlist (§11.7a) | **columnar POD; tombstones in lockstep** | layout only |
| **derived-scratch** — name→id and path→id indices, state→in/out edges, containment depth, LCA table, per-transition crossing counts and flags, each submachine's initial state | no | no | §4.1 convenience; `HashMap` where lookup-only | anyone, rebuilt freely |

Only **derived-scratch** gets §4.1's container latitude. Geometry is hashed and read across frames, so it is columnar POD — a route polyline in a `HashMap`, iterated for the coordinate hash, is the §6 failure this split exists to forbid.

`ColumnDesc` carries a `derived` flag (§8). Nothing writes a derived column back out — not the printer, not any later serializer — and they are **exempt from round-trip-unknown**, or a stale geometry snapshot survives a save and gets trusted instead of recomputed.

**Column names live in their own pool, not `Chart.strings`.** A registered name is not authored text, and `scav.geom.state` names a **derived** column the serializer skips (§8) — so keeping the two apart is what stops saving a laid-out chart from leaking a derived column's name into the authored pool. Registration takes a `char const*` and interns it there; no pointer is stored in a row, so a `ColumnDesc` is hashable and serializable like any other record.

Serialization is mechanical (write each vector). Iteration order is array order is document order.

**Canonical ordering is by name or key *bytes*, never by id or interning order** — those are first-encounter, so two producers building the same model would emit different bytes. `StrRef` and `AttrKeyId` are never comparison or tie-break keys: a comparator dereferences to the bytes, which is exactly what §15's attribute order does.

**The pool's byte layout is not canonical, and does not need to be.** No hash reads it. The format hash covers *printed* text (§15), which carries every name as text and no offset at all; the layout hash reads integer columns and never touches a string (§11); §6's hashed inputs are font, profile, packer, router, and the integer space-request tables. So nothing in any output is a function of where a string sits in the pool, and re-interning into a sorted pool before serializing would buy nothing — the last sentence above already says no ordering may depend on a `StrRef`'s value.

Names are therefore appended as met, and **not deduplicated**: `StrRef` equality is span equality and says nothing about the text, so anything comparing two names compares the views. `AttrKeyId` is the opposite case and *is* genuinely interned, because it is an identity — equal keys must give equal ids, which is the whole point of having an id instead of a string. Interning requires deduplication and `strings` is defined not to deduplicate, so key bytes live in their own `attr_keys` pool, with `AttrKeyId` indexing `attr_key_names`; a namespaced key interns composed (`@ns { a }` interns `ns:a`), so one spelling has one id however it was written. The sorted intern index is derived-scratch like any other lookup.

### 7.1 No events in the core

There is no event entity, no event table, no trigger. A transition carries an opaque `label`. Semantics are that transitions are taken programmatically; scav models no triggering mechanism.

Event lists, guard expressions, executable content, and source spans are **extension data** (§8). This also removes any question of event-vocabulary unification across documents.

### 7.2 Fork, join, and semantic neutrality

Fork/join is the case that most tempts scav into having an opinion, so the boundary is worth stating explicitly.

**What the model holds:** a pseudostate of kind `fork` or `join`, and ordinary transitions. Nothing else. Arity is *derived* by counting incident edges; there is no grouping record, because the pseudostate is the grouping.

**What scav does not decide.** Exit, reset, and sibling-deactivation semantics under partial fork deactivation are dialect-specific, projects answer them incompatibly, and scav answers none of them. **It must nonetheless draw every one of those topologies**, which it does, because all of them are a pseudostate plus transitions.

**The bar is a fixed-size box.** Layout's only fork-specific behaviour is negative: it takes the bar's extent from the profile like any other pseudostate, and `w_adj` excludes fork edges because adjacency above arity 2 is unsatisfiable (§11.8) — no fan-out algorithm, no arity scaling. A fork/join pseudostate is an ordinary small box — wide and thin — from the profile's per-`StateKind` min extent (§11.15). Layout places it and routes N edges out of it; the builder draws a filled rect. That is what PlantUML does, and it is enough: the bar is the **same size for two branches as for five**, with the routes simply fanning out, including sideways.

**Bar orientation is the profile's, and both shipped profiles now stand the bar up.** It was lying down — 60x4pt and 48x3pt — which is right for a top-down engine and wrong here, because ranks run in +x (§11.3) and a bar has to be thin across the axis the flow crosses it on. Transposed at `profile_version` 3. An app that wants it the other way sets `kind_min_*`, or requests a `BoxSpace`; what it cannot do is leave the shipped default disagreeing with the shipped layering axis.

**Validation is structural only** (§10): in/out degree per kind. No check that branches land in distinct submachines, no reachability, no concurrency reasoning — those are dialect rules and belong to a plugin.

### 7.3 Relationships between columns

Columns are never all 1:1, so "index into the other array" is only one of five patterns. Every relationship in the model is one of these, and **no new mechanism is needed for any of them**:

| Kind | Mechanism | Example |
|---|---|---|
| 1:1 | parallel arrays, index directly | `scav.geom.state` ↔ `StateId` |
| N:1 | the target's id in the child row | `Transition.src`, `PathBox.subject`, `State.doc` |
| 1:N, ordered | a `Span` into a shared id array | `Submachine.children` -> `state_ids` |
| **M:N** | **a junction row that is itself an entity** | `Transition` between states; an entity between its `Statement` and its `Include` (§9) |
| the inverse of any of the above | derived-scratch, rebuilt on demand | state -> in/out edges |

**M:N is never stored as such.** "A state has N transitions and a transition has two states" is M:N-via-junction-row, and the junction row already exists as a first-class entity — `Transition` *is* the join table. Document provenance is the second instance of the pattern and reads the same way: statements and include instantiations are M:N, and the entity row is the junction that carries one key from each side (§9). The state->transitions direction is not stored at all: it is a derived-scratch inversion (§7), a counting sort over `transitions`. Rejected alternatives, so they are not re-proposed:

- **Hashed GUIDs as cross-references.** 16 bytes against 4, and a hash lookup on every dereference in layout's hot path instead of an array index — plus §6 forbids a hash value escaping. §19's open question about durable GUIDs is about *cross-branch rename identity*, a version-control problem, not in-model referencing. Do not conflate them.
- **Per-column tombstones.** Strictly worse: it makes "`states[i]` alive but its geometry dead" representable, so every reader checks N flags instead of one. Liveness belongs to the **entity**, and columns parallel to that entity inherit it. That is what lockstep buys.

**Walking a span must check liveness.** A tombstoned state keeps its slot in its parent's `children` span, so a span walk yields dead ids and every consumer skips `live == 0` — the same rule as a full array scan, and it applies to the more common access pattern. Compacting the span instead would invalidate every other span into that array.

**Spans require contiguity, so they are rebuilt rather than patched.** Appending a child to any submachine but the last shifts the shared id array. Invisible while the builder is append-only (P1) and squarely a P12 problem. The answer is `O(n)` rebuild, not a chunked or linked span: `state_ids` at the 2k-state target is ~8KB, so a full rebuild per edit is microseconds, and an indirection that survives mutation would cost every read forever to save that.

Two of the three shared arrays have that rebuild today and one does not. `state_ids` and `submachine_ids` are rebuilt once per network by `model_finalize_containment`, so the loader never pays the per-insert fix-up; `attrs` is only ever grown through the builder's `insert_attr`, whose non-tail insert walks every entity's span. A document that writes an attribute after a nested block (`state On { state Idle { @x }, @doc }`) takes that walk once per such attribute, which is quadratic in the pathological case and unmeasurable on the corpus. **[OWED, with P12]**: a bulk attribute path, `model_append_attr_row` plus one rebuild, the same shape the other two arrays already have.

**Derived span targets carry no liveness at all.** `scav.geom.point` and `scav.geom.portslot` are parallel to nothing that has a `live` field; layout rebuilds them wholesale, so there is no tombstone to check and no lockstep to maintain. Tombstones exist only for authored entities.

## 8. Extensibility

**Why extension data lives *in* the model, not in app-side tables.** Two reasons:

1. **No sidecar context to thread.** The model is self-contained, so every entry point takes one thing rather than a `void* user_ctx`.
2. **Column lifetimes are locked together, which is what makes indices safe.** An index has a validity domain — the array it indexes. Co-locating app and core columns makes that domain atomic: a span either way cannot dangle. Split it and indices are as dangerous as pointers with none of the tooling.

Corollary: an app keeping data outside must mirror §7's lockstep tombstoning itself, or stale rows are silently misattributed after a delete. This is also why §4.1's rule is about long-lived heap nodes rather than container choice — indices are safe *because* lifetimes are locked, not because they are integers.

Two axes. Both round-trip losslessly, including data this build does not understand.

| | Extension columns | Attributes |
|---|---|---|
| Shape | dense, one value per entity | sparse dict per entity |
| Typed | yes, registered descriptor | no, strings |
| Use for | data most entities have (event lists, guard code, `onentry` bodies) | rare or one-off annotations |

```cpp
enum class ElemKind : uint32_t { State, Submachine, Transition, Chart, Point, PathBox, None };
struct ElemRef { ElemKind kind; uint32_t ordinal; };   // used by DrawList and diagnostics
enum class ValueKind  : uint32_t { u32, i32, u64, i64, strref, span, blob, pod };
struct AttrKeyId { uint32_t v; };   // interned attribute key, `ns:key` or bare
struct ColumnId  { uint32_t v; };   // index into Chart::columns

struct ColumnDesc {        // 28 bytes, no padding
  StrRef    name;          // "libhsm.events", "scav.geom.state"; own pool, not Chart::strings (§7)
  ElemKind  entity;        // `None` and `PathBox` never appear here
  ValueKind kind;
  uint32_t  elem_size, elem_align;
  uint32_t  flags;         // bit 0 = derived: skipped by the serializer, exempt from round-trip-unknown
};
struct Column { ColumnDesc desc; std::vector<scav_byte> bytes; };  // count * elem_size
```

Type-erased byte arrays with a stride, indexed by entity ordinal. C ABI is the three-call accessor in §16; the host casts.

**What core owes an extension:**

1. Store it, indexed by entity ordinal; keep it index-aligned under mutation and tombstoning.
2. **Round-trip its authored form losslessly, including data this build does not understand.** The on-disk carrier is **attributes, not columns**: `.scav` has no column syntax (§15) and there is no binary model format. A column is a *runtime projection* — core resolves `scav:` attributes into hint columns at load (§14), and a plugin fills its own columns from its own attributes or its own importer (§8.2). An unknown attribute round-trips as text like any other, so an older build cannot strip a colleague's data on save, and the guarantee costs no second format. Should one ever be wanted, a column block of `{name, entity, kind, elem_size, count}` plus `count * elem_size` little-endian bytes carries a column whose meaning is unknown — recorded so it is not re-derived, not because a phase owns it.
3. **Pass it through unread.** `layout` never reads extension data.
4. Let it contribute **space requests** (§8.1) and a builder function the app may call (§8.2).
5. Expose `ColumnDesc` so an editor can present unknown columns generically.

Columns are canonically ordered by name bytes, never by registration order. Attributes reserve the `scav:` namespace for core-meaningful keys; unprefixed keys belong to the user. Values are strings on disk with typed accessors; `--strict-attrs` **[OWED]** checks a known-key registry, since a typo is otherwise silent forever.

libhsm absorbs cleanly: `libhsm:handler`, `libhsm:legacy`, `libhsm:submachine_handler`, and a `libhsm.events` column. Core never learns statecharts have handlers.

### 8.1 Space requests — the only way content affects layout

Content a box must make room for has to be known **before** layout runs, so it needs a contract — the only one on the drawing side.

Derived from one question: **what geometric problem can only layout solve?** Two, neither involving appearance — size a box whose interior must fit app content *and* packed submachines (submachine sizes come from layout), and slide a rect along a route (the route does not exist yet). Hence three tables of plain integers, no variant, no enum:

```cpp
// two columns, one parallel to states and one to submachines
struct BoxSpace {
  int32_t min_w;      // interior at least this wide
  int32_t h_before;   // interior height reserved before the submachine area
  int32_t h_after;    // ... after
};

// per transition
struct PathClear { int32_t src, dst; };     // route shortening for arrowheads etc.

struct PathBox {          // 0..N per transition; layout slides these along the route
  TransId  subject;
  int32_t  w, h;
  uint32_t order;
};
```

**Domain, validated at `scav_layout_run` entry in every build** — Debug and Release must agree on which inputs are legal:

```
0 <= min_w, h_before, h_after <= COORD_MAX / 4    // §11.2
0 <= PathBox.w, PathBox.h     <= COORD_MAX / 4
0 <= PathClear.src, .dst      <= COORD_MAX / 4
PathBox.order unique per subject
```

A quarter of the domain, not all of it: §11.4's box formula *adds* to a request (`h_before + packed_subs_h + h_after + 2*pad`), so admitting `COORD_MAX` per field would let a legal input produce an illegal box. The composed box is bounds-checked as well; the input bound exists so the failure is attributed to the request that caused it.

Reject with a diagnostic, never clamp. Unbounded `int32_t` overflows §11.4's box formula into signed UB, which optimizers exploit, so Debug and Release diverge rather than both being wrong; a negative `h_before` inverts a box and breaks every orientation predicate.

**Space requests must be a pure integer function of `(model, profile, scav metrics)`.** The app is inside the determinism-critical path, and nothing otherwise forbids `min_w = int32_t(w * 1.15f)`, which differs under FMA contraction. A digest of the three tables is a hashed input (§6), so a non-conforming app fails the golden instead of silently drawing something else.

**Outputs.** Layout writes the geometry columns enumerated in §11.7a, plus a `Placed` array parallel to `PathBox`:

```cpp
struct Placed { int32_t x, y, w, h; };   // root-absolute; w/h may exceed the request
```

**The app draws its title, badges, and compartments wherever it likes inside the `scav.geom.state_before` rect** (§11.7a) — the space its own `h_before` reserved. Layout never learns what a title is.

```cpp
// before layout — the app measures and sums; composition is app-side.
// Free functions on POD, per §4: rows are data, behaviour is not on the row.
for (uint32_t i = 0; i < chart.states.size(); ++i) {
  if (!chart.states[i].live) { continue; }                       // tombstone (§7.3)
  Extent const title = measure_text(m, str(chart, chart.states[i].name), fs);
  Extent const badge = libhsm_wants_badge(chart, i) ? Extent{14, 14} : Extent{0, 0};
  Extent const body  = scxml_onentry_extent(chart, i, m);       // {0,0} if absent
  app.box_state[i] = { .min_w    = imax(title.w + badge.w + 8, body.w + 8),
                       .h_before = imax(title.h, badge.h) + 4 + body.h,
                       .h_after  = 0 };
}

for (uint32_t i = 0; i < chart.transitions.size(); ++i) {
  app.label_text[i] = join_events(chart, i);                    // app's side table
  Extent const ext  = measure_text(m, app.label_text[i], fs);
  app.path_box.push_back({ trans_id(chart, i), ext.w + 4, ext.h + 2, 0 });
  app.path_clear[i] = { 0, 8 };                                 // arrowhead room
}

scav_spaces const spaces = as_spaces(app);       // base pointers + counts
uint32_t n_placed = 0;
if (scav_layout_run(chart, &spaces, &opts,
                    app.placed.data(), app.placed.size(), &n_placed) != 0) {
  return diagnose(chart);                        // never ignore the result (§16)
}

// after layout — the app subdivides its own interior, however it likes
for (uint32_t i = 0; i < chart.states.size(); ++i) {
  Rect const r = geom_state_before(chart, i);
  push_text(dl, depth, style_title, r.x + 4, r.y + ascent(m, fs),
            str(chart, chart.states[i].name));
  if (libhsm_wants_badge(chart, i)) {
    push_circle(dl, depth, style_badge, r.x + r.w - 11, r.y + 3, 7);
  }
}
```

**`PathBox` is a slide constraint, not a label** — the one placement an app cannot do itself, since the route does not exist yet. **`h_before`/`h_after` is stacking order relative to the submachine area**, not a band taxonomy: two integers, not five names. **`Placed` may exceed the request**, so read back actual geometry; alignment inside it is the app's.

**The reserved box and the drawn box need not be the same rect.** That resolves border-attached decoration with **no composite shapes and no attachment offsets**:

- **Unprotected decoration is free** — the builder reads the box after layout, so a badge at `box.x + box.w - 6` follows it. Derivation, not lockstep movement.
- **Protected decoration is reserved then drawn inset** — reserve W×H, draw the outline at (W−12)×(H−12), badges in the margin. Visually overhanging, structurally inside, obstacle-correct. Same trick covers stroke width.

It also settles **non-rectangular shapes** — protruding tabs, concave outlines. **Layout consumes only rects**: packing is defined on rectangles, non-overlap is a rect test, the routing graph is built from rectangular obstacles, ports are per-side. So the occupied region is the composite's **AABB**, which reserve-and-inset expresses; an asymmetric tab is extra reserved width with the visual rect off-centre. Accepted artifact: avoidance is conservative and termination approximate, since a route may land on an AABB edge beside a tab. Bridge with a stub; not worth a polygon router.

**Deliberately absent**, none of which was solving a layout problem: `overlay` (content reserving no space is not layout's business), priority or composition order (the app sums before calling), alignment (the app knows the rect).

**Detached placement** — a note near an element with a leader, non-overlapping — is a real third layout problem, **deferred**: nothing in the corpus needs it, libhsm notes convert to attributes, connector glyphs are out of v1 (§11.7).

### 8.1.1 Optional helper layers

scav ships **utilities the app may call**, never machinery that calls the app (§3.0). In `libscavdraw`, all pure functions over PODs, all optional:

- **interior subdivision** — `scav_stack_v(rect, items, n, out_rects)`, `scav_row_h(...)`, `scav_align(rect, w, h, scav_anchor, out)` where `scav_anchor` is the nine-cell enum. Turns "I have a rect and three things" into positions — a convenience, not a contract.
- **text layout in a rect** — line breaking at author-supplied breaks, baseline positioning, ellipsis.
- **shape emission** — `DrawList` helpers for rounded boxes, arrowheads, dashed submachine dividers, orthogonal polylines with rounded corners.
- **the reference builder** — the standard appearance, as **per-element-kind emitters** (`emit_state`, `emit_route`, `emit_label`, …), each taking the depth to draw at, plus a convenience wrapper calling them in an order it documents. Per-kind emitters plus caller-supplied `depth` (§12) mean an app interleaves its own content without forking anything: call the emitters it wants, skip the rest, append its own primitives wherever it likes.

Nothing in scav's pipeline invokes any of these. An app that uses all of them looks like the old framework and gets the same result; an app that uses none of them is not fighting anything.

### 8.2 Plugins

A plugin is a **library the application links**, not something scav loads. Nothing in scav calls into a plugin; the plugin calls scav, and the application decides which plugins it uses. There is no plugin ABI, no registry, no dynamic loading, and no lifecycle to get wrong.

A plugin may: register extension columns; import or export its own format; validate its own semantics; measure its content and contribute space requests; contribute a builder function the app calls; and consume `(model, geometry columns)` for codegen or export.

**Shipped:**

- **`scav-libhsm`** — first-party: event-list columns, handler and legacy attributes, and a builder contribution for event labels and handler badges. Its `.puml` importer and `puml2c`-replacing codegen backend are out of scope for this document (§17) but must stay possible.
- **`scav-scxml`** — reference example. Columns for executable content, import and export, and a builder contribution showing `cond` as label text and `onentry`/`onexit` as a compartment.

The two together are the acceptance test for the design: if libhsm needs a scav change that SCXML does not, the boundary is wrong.

**Cost of this model, stated plainly:** a plugin cannot add appearance to an application that did not choose to call it. Under an inverted design libhsm could ship a badge and every scav app would show it. Here the app decides what appears in its own UI — correct, but it means "install a plugin, see new decorations" is not a thing unless the app opts in, e.g. by embedding a script host (§8.3).

### 8.3 Scripting is an application concern

The application owns the builder, so scripting is something an *application* embeds if it wants scriptable appearance. `libscavcore`, `libscavlayout`, and `libscavdraw` carry no interpreter, no sandbox, no shims, and no sol2.

**`scavview` embeds Lua**, because a viewer is where drop-in-a-`.lua`-file appearance earns its keep. Any application embedding a script host inherits these obligations; they are recorded because they were expensively derived, not because core needs them:

- **Stock Lua 5.4**, pinned `luai_makeseed`, **sol2** confined to one translation unit.
- **Sandbox**: open only `base`, `string`, `math`, `table`; nil out `load`/`dofile`/`loadfile`/`rawget`/`rawset`; `mode="t"` chunks only — the bytecode loader is not hardened against hostile input.
- **Determinism**: remove `pairs`/`next` (5.4 randomizes string-hash seeds), `math.random`, `collectgarbage`, the libm transcendentals; shim `string.format`/`tostring` to reject floats and `%p`.
- **Hot states**: precompile at init, one persistent state, userdata proxies over per-call tables, content-hash caching.

## 9. Addressing

Format-independent. A **state path** is submachine-qualified and `/`-separated:

```
On/Ready/Online      unambiguous
On:1/Idle            submachine ordinal, when a state has >1 submachine
On:main/Idle         submachine name, when named
wifi/On/Ready        cross-document, via include alias
```

- **Unnamed pseudostates get synthetic stable names** for addressing: `$initial`, `$final`, `$history`, ordinal-suffixed for uniqueness within a submachine, and exempt from §10's duplicate-name check. These are an API and diagnostic spelling only — the grammar's `ident` admits no `$`, and the format reaches them via `*` (§15). A pseudostate an author needs to name is named, like `PreConfig kind choice`.
- **Each `*` endpoint synthesizes its own pseudostate** — `initial` as a source, `final` as a target — owned by the submachine the statement lexically appears in, carrying that transition's `stmt`. One per statement, never merged per submachine: two authored `trans * -> X, trans * -> Y` are two initial arrows, which is what makes §10's more-than-one-`initial` check a reachable check rather than dead code. `trans * -> *` is rejected.
- **A path's first segment resolves innermost-outward**: from the submachine the statement appears in, outward through each enclosing submachine to the chart root, taking the nearest match; every segment after the first descends strictly. That is what the worked example already assumes — inside `submachine main`, `trans * -> Idle` names main's own `Idle`, while `trans Ready -> dock/On/Seated` starts at a chart-root alias two levels up. Lexical scoping, because it is the rule every reader already knows.
- **An include synthesizes one state, named for its alias**, in the submachine where the `include` statement appears; that state's `submachines` span gains the included document's root submachine. **The included document's chart-level attributes land on that root submachine**, since the network has one `Chart` entity and it belongs to the root document; the included chart's *name* is dropped, its alias being the name, and its label survives as the submachine's label. A plugin that reads chart-level attributes therefore reads them from two places depending on provenance: the Chart entity for the root document, the alias host's root submachine for an included one. Recorded because it is a surprise, not because it is wrong: the alternative, a Chart entity per document, would give the network several roots. A submachine's children are states, so this is the only shape that type-checks — an included root is a submachine and has nowhere else to attach. It also makes `wifi/Up/Connected` an ordinary path: `wifi` *is* a state. The host state is lowering's (P1, so §10's alias-collision check can run without a loader); filling `Include.target` and attaching the included root is resolution's (P2), and until then a path descending past an alias diagnoses as unresolvable.
- **The outward walk stops at the document it started in.** A path's first segment climbs from its own submachine to that instantiation's root and no further, so a name inside an included document may not silently bind to one in whichever host included it. Two reasons, and the second is the load-bearing one: a document is a reusable unit whose meaning must not depend on its include site, and §9 already says two instantiations of one file differ *only* by `InstId` — outward binding would make them differ structurally. Reaching the other way still works: `dock/On/Seated` is a downward descent past an alias, not an outward climb. The boundary is where a submachine and its owner state carry different `InstId`s, which is exactly the alias-host edge the loader built.
- **Resolution links; it does not flatten** (§7). Containment crosses documents because `State.submachines` holds global ids, so layout sees one containment tree with no transformation having occurred — no cross-document LCA, no splice pass, no project handle.
- **Provenance is two fields, not a computed column, because it is M:N** (§7.3). One statement declares N entities when its file is included N times; one instantiation contains the entities of N statements. So the entity row is the junction and carries both keys: `StmtId` says which authored construct produced it, `InstId` says which instantiation it belongs to. A renderer tinting sub-document submachines reads `inst`; a diagnostic or an editor reads `stmt`; layout ignores both.
- **Statements are per file, entities are per instantiation.** The parser produces `Document` and `Statement` rows once per distinct file (P0); the loader instantiates entities per include (P2). Including a file twice therefore duplicates entities — which is correct, they lay out separately — but never duplicates source bytes or statements.
- Transition endpoints are plain `StateId`s, because ids were global from the start.
- **An include alias is a bare path prefix**, not a sigil, because it is a state name. Alias uniqueness is therefore §10's ordinary duplicate-name check rather than a second rule, and duplicate top-level names in two documents cannot collide.
- **No integrity attestation.** An include names a path, not a digest: a `.scav` document network is source code under the same version control as the code it describes, so pinning content hashes would duplicate what the VCS already guarantees while adding a second thing to keep current. Fetching a document over a network is the app's policy (§16.2) and so is verifying it.
- Include cycles are a hard error.
- **A document's `DocId` is a function of the include graph, never of arrival order.** The id is fixed by the *first* include statement naming that path, ordered by `(requesting DocId, statement ordinal)` — a breadth-first walk from the root. This is §6's shard rule applied to loading: the work items are enumerated deterministically and completion order is irrelevant. It has to be stated, because §16.2 hands the app the `pending` list and invites it to resolve the batch however it likes, including in parallel — and §7's iteration order is array order is document order, which §14 then requires to survive all the way to layout. Numbering documents as they arrive would make a parallel fetch reorder `documents`, and with it reading order and the diagram. Parallel *fetch* breaks determinism on its own under an arrival-order rule; no threaded parser is needed to get there.
- **Loading is therefore parallelizable without core threading any of it.** Parsing is pure over one document's bytes and there is no library-global state (§16), so N documents parse independently; the loader stays single-threaded-per-instance and assigns ids by the rule above. Whether the app fetches serially, on a pool, or on a wasm host with no threads at all, the model is byte-identical. Splitting `scav_load_add` into a parse that the app may run off-thread and an `attach` that takes the result is a two-function addition, deliberately **not** made yet: 200 documents of a few KB parse in about 4 ms serially, which one `open` per file dominates.
- **Instantiating one document twice means two include statements**, two aliases, and two disjoint sets of entity rows distinguished by `InstId`. Renaming that file then patches one path string per instantiation. Accepted: a **global include section with a reference sigil was considered and rejected** — a sigil names a document, but an endpoint must name an instance, so the two coincide only at one instantiation and above it the section needs instance names anyway. It would also still require a statement at the host to say where the subdocument attaches, and it would mark a cross-document distinction the model does not have, since an alias is an ordinary state.
- Relative hints travel with an included chart; **absolute pins do not** — a pin is authored against a document's own frame and is meaningless in a host frame.
- Resolution is a linear scan per path level (document order forbids sorting `state_ids` by name) or via the derived sorted index.
- Paths break on rename. Renaming is a **semantic editor** operation — the editor holds the document network and rewrites every reference — not a CLI verb. **[OPEN]** whether elements also need durable GUIDs, which paths cannot supply across branches: two branches renaming the same state differently is unreconcilable when identity *is* the name.

## 10. Validation

Mandatory, in core, structural only — `layout` reads ordinals and crashes on garbage:

- dangling `StateId`/`SubmachineId`; `INVALID` where a value is required; tombstoned targets
- **containment consistency**: every relation is stored on both sides (`State.parent` against `Submachine.children`, `Submachine.owner` against `State.submachines`), so the two sides must agree row for row, and the parent-owner walk from every live state must reach a document root. `Chart` is a public struct and layout's ancestor walks trust it, so a disagreement or a cycle is a finding here rather than a hang there. The builder and the loader cannot produce either; a hand-mutated chart can
- every column covers its entity array exactly, except a self-length `Point` column
- duplicate authored names within a submachine
- include cycles and unresolvable include paths — the **loader's**, not `validate_chart`'s. Neither is representable in a finished chart, because `finish` refuses to produce one; checking for them afterwards would be checking for a state that cannot exist. Their diagnostics are therefore document-local, since they fire precisely when no chart does
- unresolvable cross-document paths, checked at the **resolution phase** (§9)
- a `Statement.src` span outside its document's `text` span
- an alias colliding with a sibling state name — the same duplicate-name check, since an alias is a state (§9)
- authored names must not contain the path metacharacters `/ : $`, nor `@` (the format's attribute sigil, §15)
- more than one `initial` per submachine. **No degree checks per pseudostate kind** — "a fork has one incoming edge" is a dialect rule, and §7.2 requires every topology to be drawable
- authored `scav:pin` coordinates outside §11.2's domain — the only authored geometry there is. **[OWED]** with the hint columns (§14); no earlier phase produces the column it checks

Semantic lint is out of scope. Identifier-sanitization collision checks belong to the codegen backend, not core.

## 11. Layout

Isolated static library, imperative entry, POD in. Writes **derived** geometry columns only and never authored data (§7).

Scale target: **2k states, 5k transitions, depth 16.**

```
decompose(Chart)                                              -> SplitGraph
phase1_order(Chart, SplitGraph, Spaces, Profile)              -> SubmachineOrders
phase2_size(Chart, SplitGraph, SubmachineOrders, Spaces, Profile) -> SizedLayout
phase3_route(Chart, SplitGraph, SubmachineOrders, SizedLayout, Spaces, Profile, Router)
                                                              -> Routes: points, slots, Placed[]
layout_run                                                    -> the geometry columns (§11.7a)
```

The four intermediates are internal POD: `Spaces` is the three §8.1 tables; `SplitGraph` is segments and ports plus the containment facts they imply (each state's depth, each border's crossing count); `SubmachineOrders` adds rank and in-rank position per node; `SizedLayout` adds box extents and node coordinates. None crosses the ABI (§16) — only geometry columns and `Placed[]` do — so they are free to change without an ABI break.

**Every phase also takes the `Chart`, for containment and liveness.** The model already holds the containment tree as columns, so copying it into `SplitGraph` would be a second spelling of the same rows; a phase reads `parent`, `owner`, `children`, `submachines`, `kind`, and `live` from the model and everything *derived* from its input intermediate. One helper pair in `decompose.h` does the ancestor walk for every phase and the cost scorer, bounded by the state count so a corrupted chain terminates.

Every stage is POD in, POD out, so any stage is testable with hand-written inputs and no font present. Hint columns are integers (§14), so layout never touches a string or resolves a path — §3's font-blindness is structural, not a convention.

**Each phase is its own translation unit, and its output intermediate is declared in that unit's header** — the shape `decompose.h` already has. That is what makes the previous paragraph a fact rather than an aspiration: a phase whose intermediate is a file-local type in a monolithic `layout.cpp` is reachable only by running every phase before it, which is a functional test wearing a unit test's name.

**Unit tests construct the input intermediate directly and never run the preceding phase.** A `SubmachineOrders` with three ranks is nine integers typed into a test; a `SizedLayout` with two overlapping boxes is a literal. Nothing is loaded from disk, no document is parsed, and no font is opened — there are in-RAM chart builders for the cases where a real model is genuinely what is under test (§5), and hand-written PODs for every case where it is not. The pipeline's own composition is then the *functional* class's job, over the corpus, end to end.

Two consequences worth stating because they are easy to erode. A phase may read the `Chart` for what the model *stores* and never for what an earlier phase *computed* — if phase 2 needs a boundary-crossing count, `SplitGraph` carries it, and if phase 3 needs to know which end of a segment meets an inner face, `SplitSegment` says so rather than phase 3 inferring it from phase 1's node tables (§11.1; the one defect of that kind found so far was exactly such an inference). And no phase may read the clock, the environment, a global, or a thread id, so a stage's output is a function of its arguments and a test needs no fixture to pin it.

Geometry columns are derived: never serialized, never authored (§7), so "layout writes the model" does not compromise round-trip stability.

### 11.1 Phase 0 — decompose

Build the containment tree. Split every transition at each boundary it crosses, terminating each segment on a **hierarchical port** on the compound state's border. Each segment is then local to one submachine, and the long-hierarchical-edge problem becomes 1D port ordering per compound side.

A long edge's weight **accumulates structurally, not in a scalar**: one port per crossed ancestor border means a state at depth 16 with a transition to a top-level state contributes an ordering constraint in *every* one of the frames between, so it exerts real pull on its whole ancestor chain. This is the differentiator, and it is why `SplitPort` needs no weight field — the accumulation is the port count along the chain, and what a consumer needs to know about congestion at one boundary is `state_crossings`.

Port order is a solver output (§11.3). Ties break on the port's stable key (§6), never on weight-insertion order.

**Each segment end is one of three things, and the segment says which.** An end is a *port* on a crossed border, the endpoint state's *box* in this frame, or the endpoint state's *inner face* when that state encloses the frame and its border is not crossed (§11.14: an `internal` or `local` source, and every destination that encloses its source). `SplitSegment` carries the inner-face flag per end explicitly. Phase 1 places a boundary node for an inner-face end; phase 3 starts or ends the route at that node and emits no port slot, because no border was crossed. Phase 3 does not deduce the end kind from which node tables phase 1 happened to fill.

### 11.2 Coordinates

Integer only, grid units of **1/16 point** — a compile-time constant, not a profile field, because §11.9's ABI takes sixteenths of a point directly.

```cpp
inline constexpr std::int32_t COORD_MAX = (INT32_C(1) << 19) - 1;   //  524'287
inline constexpr std::int32_t COORD_MIN = -COORD_MAX;               // symmetric
using Coord = std::int32_t;   using Wide = std::int64_t;
```

**Symmetric domain** — asymmetry makes `-x`, `abs(x)`, the RTL x-mirror, and subtree rotation overflow on the minimum value.

Bit budget, degree-driven (`bk + log2(terms)` for a degree-*k* polynomial over *b*-bit coordinates), `b = 20` including sign:

| Quantity | Degree | Bits | |
|---|---|---|---|
| coordinate, difference | 1 | 21 | int32 |
| `orient2d` | 2 | 41 | int64 |
| squared length | 2 | 41 | int64 |
| Σ squared length, 5k edges | 2 | 54 | int64 |
| intersection-point numerator | 3 | 61 | int64, 2 bits spare |
| degree 4 (`incircle`) | 4 | 81 | int128 required — avoid |

`int64` holds **63** magnitude bits, not 64; the previous domain overflowed this table's own worst row.

Rules:
- **Intersection *tests* are degree 2** — four `orient2d` calls, never constructing the point. Compare signs; **never multiply two determinants** (degree 4).
- Constructed points snap to grid with a documented rounding rule.
- **Widen before multiplying.** `int32 * int32` computes in 32 bits then widens. Wrap it: `cross(ax,ay,bx,by) -> int64`.
- Validate the domain at `scav_layout_run` entry in **every** build (§8.1), and again on each inflated profile copy before its retry runs: a copy out of range ends the retries, as does a `phase2_size` that overflows, and the last successful geometry stands (§11.6).
- Output is **root-absolute**, applied as one final `O(n)` transform over submachine-local internals (ELK's LCA-relative coordinates are a documented trap).

Extent estimate: 2k states ≈ 8,000 x 3,200 pt = 128,000 x 51,200 units, ~4x headroom. **Measured twice, and both numbers are worth keeping.** Under P4's deliberately fat fabricated advances a 2k-state chart came out **181,120 x 277,888** — 1.9x headroom on the tall axis, which is the conservative bound the fabricated measurement exists to produce. Under P5a's real bundled font the same shape is **152,628 x 101,044**, or **5.2x**, so the original estimate was sound and the grid decision was never close. Keep asserting the fabricated case: it is the one that trips first when a later phase grows boxes, and P6's did. **Measured a third time, under P6:** the widest fabricated `min_w` the 2k shape carries is **4768**, against P4's 3200 stand-in, at 523,584 x 417,456 — and the real font puts the same shape at **167,194 x 109,253**, 3.1x headroom. That the fabricated number went *up* is the fold of §11.4 doing its work; without it the same shape carries only 1280, because a rank run grows along one axis and nesting multiplies it by the depth. If real charts ever exceed the domain, reduce the grid to 1/8 pt rather than widening it.

Coordinate assignment uses two linear integer primitives, not a solver: **Brandes & Köpf** for cross-axis coordinates (GD 2001 — **read the erratum, arXiv:2008.01252**), and optimal topological numbering for compaction. On an integer grid with integer gaps and an acyclic constraint graph, non-overlap plus separation *is* longest-path.

### 11.3 Phase 1 — per-submachine ordering

Independent per submachine: the parallel unit and the dirty unit. Layered rank assignment, then intra-rank ordering by **median** (tight 3-approximation) or **global sifting** (5–10% fewer crossings than level-by-level sweeps, eliminates type-2 conflicts). **Not barycenter** — no constant-factor bound, ratio Ω(√n).

**The layering axis runs left to right.** Ranks are columns, nodes stack vertically within a rank, and successive ranks proceed in +x. This is not a new choice — it is what §11.4's size composition already encodes, and recording it here keeps `rank_sep` from being read as the other axis by half its readers. So `rank_sep` is a horizontal gap between adjacent columns and `node_sep` a vertical gap between adjacent nodes in one column (§11.15). The RTL mirror (§11.9) is an x-flip, which is the layering axis, so it reverses reading order exactly as intended.

**A frame's graph need not be connected, and each component is laid out on its own**, because unconnected states all rank 0 and one graph would stack them in a single column — the shape a submachine of leaves actually has. The components are then packed (§11.4), which is the treatment sibling submachines already get one level up, and the pieces are ordered by their first node so reading order survives.

**Ports are nodes in their frame's layered graph**, one per `SplitPort` whose frame this is, ranked and ordered alongside real states. That is what turns §11.1's split into 1D port ordering per compound side without a second algorithm: a port's rank fixes which side of the compound state it lands on, and its position within that rank fixes its offset along the side. Phase 2 turns those two ordinals into coordinates; nothing else assigns a port a side.

**Only the left and right sides are produced.** Ranks run in +x, so a port is a source boundary at the frame's left edge or a sink at its right, and every slot reports side 0 or 1. **[OWED]**: top and bottom sides, which need a port to be allowed onto the cross axis; the `side` field already admits 2 and 3 so the column does not change shape when they arrive. **What that costs, measured**: a disc or a diamond has one attachable point per face (§11.5), so with two sides a mark seats at most two incident transitions before an arrival and a departure double up on one point — `gauntlet/marks.scav` reads **4 such pairs** at both profiles, and the count is pinned there. A boundary node's coordinate is the frame's edge, not the edge of whichever folded piece (§11.4) it was laid out in.

**Ordering-edge weights are uniform at 1.** The known lever if the side-by-side (§11.12) wants straighter long edges is dot's schedule — real-to-real 1, real-to-dummy 2, dummy-to-dummy 8 — and it is deliberately not taken up front: it is three integers and a golden rebase whenever it is wanted, and taking it now would mean tuning against no measurement.

**Inter-rank edge labels widen the rank boundary they cross**, so rank separation accommodates them by construction (§11.9): the gap between two adjacent ranks is `rank_sep` plus the widest `PathBox` any edge crossing that gap carries. That makes **`Spaces` a phase-1 input and not only a phase-3 one** — a label wide enough to set the gap between two ranks has to be known before the ranks have coordinates — while placement along the finished route stays phase 3's, so a `PathBox` row is read twice for two different questions: how much room to leave, then where the room ended up.

The textbook alternative is a **label dummy node**, which additionally *orders* the labels sharing one gap so two cannot collide, at the cost of doubling every rank (an edge between adjacent ranks has no intervening rank to put its dummy in) and of choosing which frame of a hierarchy-crossing route owns the label. Widening buys the sizing guarantee, which is the part that cannot be repaired later; collisions within a widened gap are priced by `w_lbl` (§11.6) until P7, whose strip matching (§11.9) places path boxes properly and subsumes what the dummy would have done.

**The degenerate flat chart bounds the ordering algorithm, not just the router.** One submachine holding 2k states is legal input (§11.5), and global sifting there is `O(|V||E|)` per pass over a graph an order of magnitude larger than the n≈20–50 the per-submachine case assumes. **A long edge costs a bend in every rank it spans, and cycle breaking can make an edge long.** Chaining is `O(Σ span)`, which is the price of a proper layering and is fine while spans are short. Reversing an edge to break a cycle is what makes them long: a chain reversed in node order turns a thirteen-rank skip into a thousand-rank one. Measured, a flat frame of 2k states in a chain lays out in 2 ms with 3,953 ordering nodes; the same 2k with skips that wrap produces **a million** ordering nodes and takes seconds. The mitigation is a cycle-breaking heuristic that leaves a chain alone — Eades, Lin and Smyth's greedy removal rather than a depth-first walk in node order — and it is not bought here, because which edges it reverses instead is a quality question for the side-by-side (§11.12). A test pins the behaviour at 512 states so the fix has a target.

**Median ships; global sifting is a named lever, not a second code path.** Median's 3-approximation is bounded at every size, so one algorithm covers both the n≈20–50 nested frame and the flat 2k one, and the flat shape gets its own performance floor to keep that true. Sifting's 5–10% is worth having and its cost is `O(|V||E|)` per pass, which is affordable at the nested size and not at the flat one — so taking it means a size threshold, a profile field, and two orderings to keep deterministic. That is bought when the side-by-side (§11.12) says the crossings are what is wrong, and not before.

### 11.4 Phase 2 — sizing and sibling packing

Sizes bottom-up; port positions and hints top-down; fixed pass count.

Submachine size composition (Castelló et al., JGAA 6(3), 2002): **width = Σ over layers of (max width in layer); height = max over layers of (Σ heights in layer)**. Layers are columns, which is §11.3's left-to-right axis restated — a layer contributes its widest member to the total width and its stacked members to a candidate height. The gaps go in the same two sums: `rank_sep` once per layer boundary along the width, `node_sep` once per adjacent pair within a layer along the height. **Every node in a layer shares that layer's x origin** — left-aligned, not centred within the layer's width — which is what leaves rank recoverable from the finished coordinates (§11.7a).

Composite state box, from the requesting entity's `BoxSpace` (§8.1):

```
w = max(min_w, packed_subs_w, kind_min_w) + 2*pad
h = max(h_before + packed_subs_h + h_after, kind_min_h) + 2*pad
```

`pad` and the per-`StateKind` `kind_min_w`/`kind_min_h` are profile fields (§11.15), never hardcoded. **`pad` is interior only** — the ring between a box's border and its contents, which is why it appears exactly twice per axis in the formula above and nowhere else.

**A bare pseudostate takes no ring**, having no contents to ring. One rule, applied at sizing and again when the interior bands are placed: a non-`Normal` kind, no `h_before`, no `h_after`, and no live submachine with a nonzero extent. Padding one does not merely make a 14pt dot occupy a 30pt box: a route attaches to the *box* while the glyph is drawn inside it, so every arrow into a pseudostate stops one `pad` short of the mark. The rule is `glyph == box` — a builder fills its box, layout gives no more than the mark needs. Ordinary states keep the ring even when empty, or two same-kind states differ in size for no visible reason.

**A glyph inscribed in its box holds less than half of it.** A diamond takes a centred `w` by `h` label only where `w/2a + h/2b <= 1`, so the measurement pass asks for twice the text on both axes and the builder centres the name rather than setting it at the top of the band. Every gap *between* two things is `rank_sep`, `node_sep`, or `sub_sep`. Serving all four roles from one field is the trap here: it forces the space around a submachine title, the space between two ranks, and the space between two sibling submachines to move together, and the three want different numbers in both shipped profiles. A state with no space request passes an all-zero `BoxSpace`, so `kind_min_*` is what gives a fork bar its wide-and-thin extent (§7.2) — without that term the formula would size it `2*pad` square.

**`kind_min_*` is a floor, so a space request always wins, and the reference builder spent P5a–P7b handing it one it should not have.** Measuring every state's name gave a fork `min_w` and `h_before` from six characters of text, and the 4pt bar thickness this profile asks for lost to a 41pt title band: `ota`'s two bars drew as 1216x653 and 1434x653 slabs with their names painted black-on-black inside them. Only a rounded rect and a diamond show the name they reserve for; a bar, a dot and an `H` are marks. **A builder reserves for what it draws** — and the converse, that a mark must then fit the box `kind_min_*` gave it, is the builder's too: `H*` at title size straddles the border of a 16pt circle, so the mark is sized from the circle instead. Both are pinned by tests over every mark-drawn kind.

**Sibling submachines are packed here, not in phase 1** — packing requires the siblings already sized. **LR-rectpacking** (Domrös et al., IVAPP 2021): greedy width approximation → placement → compaction → whitespace elimination, `O(n log n)`. Take the LR variant; plain rectpacking's one-oversized-child special case was deleted by its own authors as unmaintainable and aspect-ratio-blind. The gap the packer leaves between two placed siblings, and preserves through compaction and whitespace elimination, is `sub_sep`.

Order-preserving and gap-avoiding are one constraint: restricting placement to four positions relative to the predecessor (directly right; right on the current row level; next subrow; next row) is exactly what makes local whitespace elimination always possible. **Compaction and whitespace elimination are the two steps not taken**: both reclaim leftover space, and both want the side-by-side (§11.12) to judge before they ship.

Review has now judged, and the number is `vac`. Its root frame is five components — four isolated composites and one three-rank chain — with **55% occupancy**: 64.9M of component area in a 118.7M canvas. A 4173x3783 block sits empty left of `dock` and below `On` while the 1696x1594 chain occupies a row of its own beneath everything. Order-preserving placement offers a late arrival only the four positions relative to its predecessor, and "next row" was the only one that fit; nothing then goes back for the hole. That is what these two steps are for.

**A rank run folds when folding scales larger.** A run grows along the layering axis without bound and nesting multiplies it by the depth, so sixteen levels of a fifteen-state chain draws as a strip a million units wide. Cutting the run and stacking the pieces fixes that, but not everywhere: at two ranks it makes the aspect *worse*, which a greedy fold at the target width demonstrates on the first chart it meets. So both shapes are laid out and the scale measure picks, the same way `trybox` picks a packer. An edge the cut crosses has its ends in two pieces and gets no say in either's coordinates, as a wrapped line's does not.

**The pieces a fold makes are packed, not stacked.** Stacking them left-aligned gives every piece the width of the widest, however little it holds; they are rectangles sharing an area, which is what LR-rectpacking already does for this frame's components and a state's sibling submachines. Over the corpus the Tier-2 sum falls 5.6% with area, Tier 0 unchanged at zero, `toolchanger` best at 0.79x. Under real measurements `brew` goes 35.7M to 28.9M, arriving at the arrangement a reader proposes unprompted: `Standby` beside `Brewing`, terminal below it, inside `Brewing`'s own vertical extent.

That is worth separating from the win. **Nothing generates candidates**: the pipeline ranks, orders, sizes and packs once, and `Cost` is never asked about a second arrangement. The packer found this one because one more shape happened to be in the set it already compares. §11.8's "reorder sibling submachines" is P9's, and until it exists every improvement of this kind is a special case in phase 2 rather than something the layout found.

The fold is what costs §11.7a's hash split a property it had while sizing did not feed back into shape: a size change that reflows the ranks now moves the structural hash as well as the coordinate one. That is the honest form of §11.11's limit rather than a new exception to it — what survives is that a route whose shape cannot depend on its box, a self-loop for instance, moves coordinates and no turn.

Width approximation is `target_w = isqrt(floor_div(total_area * dar_num, dar_den))` with that exact operation order, `isqrt` = floor. `DAR` is an integer pair. On same-height submachines the older `box` packer wins; `trybox` is evaluated once per layout, deterministically. A 1-unit change can flip the packer and reflow siblings; that is a boundary condition for hints (§14), not grounds for remembering the previous choice.

**Bottom-up sizing has no locality** — a leaf growing one unit resizes every ancestor to the root. Inherent, and simply paid: one pass up, one pass down, fixed count. No hysteresis; that would be hidden state (§11.11).

Not top-down layout: its central size-approximation problem is unsolved by its own authors, it introduces per-level scale factors that break port-split segment continuity, and it is mutually exclusive with cross-hierarchy edges in ELK. Cost: bottom-up sizing at depth is a readability problem on fixed media (a depth-9 SCChart lays out to 0.322 pt max font on A4). Acceptable because output is a zoomable canvas.

### 11.5 Phase 3 — routing

Axis-aligned is a **hard constraint**. States, submachine rectangles, and placed boxes are obstacles, so Tier-0 edge-through-box is unrepresentable rather than penalized.

Routing graph is an **orthogonal visibility graph** or a **channel-representative graph** (Hegemann & Wolff, GD 2023, arXiv:2309.01671). At per-submachine scale (n≈20–50) both are cheap: **choose on quality and implementation simplicity, not asymptotics.** Published full-OVG scaling failures (~30 min at 4,330 obstacles) apply only to the degenerate flat chart — one submachine holding 2k states, which is legal input.

**The sparse graph that path was reserved for is not needed, measured at P7b.** The grid is the product of two line sets rather than a function of box count, and 2k packed boxes share columns and rows, so the flat chart routes in 215 ms on the full OVG. What blows the budget is boxes at *distinct* offsets — a shape the router's own suite builds and no chart produces. Reach for a sparse graph when a chart does.

Bend cost lives **in the shortest-path metric**, via the **separated OVG** (Wybrow et al., Diagrams 2012): split each node into h-plane and v-plane copies joined by an edge whose weight *is* the bend penalty. Length and bends collapse to one uniform edge weight; unmodified Dijkstra/A* optimizes both, so bend-heavy routes are never generated.

**The separation *is* the A* state, so neither the key nor the heuristic needs a direction in it.** This section first specified state `(vertex, entry_direction)`, tie-break `(f, g, entry_direction, node_index)`, and the 5-case remaining-bend bound from GD 2009 §4 Fig 2a. A node already *is* `(vertex, plane)`, so the direction term in the key is a copy of part of the node and the key is `(f, g, node)` — total, and one comparison shorter. The bound collapses the same way: from the h-plane a goal off the current row needs at least one more turn, and from the v-plane a goal off the current column does, so Manhattan plus one bend when this plane cannot reach the goal's axis alone is admissible in two cases rather than five. Both are lower bounds, so the sum admits.

**Inter-submachine segments are routed in the parent's frame**, and every separator channel is owned by exactly one submachine (the LCA) and routed there. Without this, "independent per submachine" is false for exactly the edges this project exists to handle. **Two qualifications, both measured.** "The parent's frame" names no frame when the two submachines are concurrent regions of one state — §11.8 holds that **[OWED]** and the count the element suite pins on it. And separator channels are **not built**: they were the fix for the separator stub's Tier-0 violations, the attachment-face rule below took those to zero, and nothing in the tree now demands them, so they stay here as the design for *sharing* a corridor (§17 P7c).

Congestion is **history-based** (PathFinder, McMurchie & Ebeling, FPGA'95; TritonRoute's marker cost) and is **not built** — nothing measured demands it, and it is the expensive, sequential, iteration-count-tuned part, so it is unscheduled rather than owed (§17 P7c). The design, for the chart that does demand it: route nets in `(submachine, transition)` order within a congestion domain, double-buffer the history map so iteration *k* reads only the *k−1* snapshot, merge updates in net order, fixed iteration count from the profile, integer ramp schedule as a lookup table.

**Nudging, as built, is the room-only half of the stage below.** A lane is a run of collinear overlapping segments in one frame, keyed `(axis, coordinate)` and swept into groups by extent. **One axis is done at a time, and the members of the second are read off the moved geometry rather than the input**: a horizontal displacement drags the vertical legs either side of it, and their extents move with them. Its members are ordered by where each net was *before* it reached the lane, taken from whichever end is lower along the lane's own axis so every member is measured from the same side — a net arriving from above stays above rather than swapping and paying a crossing for it. Ordering by both ends read worse when that was measured, 145 corpus crossings against 129, because a net that changes side has no consistent answer and ends up placed by its tie-break.

**Members whose nets already run as one take one offset between them.** Two bundle when their nets are identical from the segment's far point to the net's end, or from the net's start to its near point — a fan-in and a fan-out being the same shape read from either end — and the relation is transitive because the runs are identical. A bundle counts as one member when the step is sized and the lane laid out, sits in the order at its least `toward`, and moves or stays whole. Without it the stage takes the trunk several routes reach a state along and spreads it into that many parallel lanes a gap apart, which reads worse than the one line fanning in that the router produced: four of them into `bottler`'s `Fault`. **Nothing attracts a route to a trunk.** Every net is still routed independently on its shortest path under the bend penalty, a trunk exists only where two of those coincide, and the rule only stops the pipeline pulling apart one that is already there — a detour to meet up trades length the reader pays for on every glance against a merge the reader may not notice (§11.13).

**Only a segment with a neighbour at each end moves.** An end segment is anchored on a box border and sliding it along that face is a different move; 86% of the corpus's shared length is between two interior segments, so the anchored case buys little and risks detaching a route. **And a displacement is taken only when it is known good**: the segment and the two legs it drags may not leave the region or the frame's own box, may not collapse a leg to nothing — which the arrowhead reads its direction off — and may not reverse one, which folds the polyline back over itself. **A leg's own length is therefore part of the room**, folded in before the step is sized rather than checked afterwards, so a lane spreads by what every member can drag instead of dropping the members that cannot reach: 12,905 corpus grid units of shared run between the two. The obstacle rule is per leg and in two parts: a box's raw rect is a hard wall and the rect grown by `clear` a soft one, each exempt only for the leg already inside it — a re-seated route sits inside a bumper it may not then cross. **And a leg may keep only a shared run it already had**, which is what stops a member trading the lane it left for one it lands on; the whole-cloth rule, refusing any overlap rather than a new one, refuses so much that the residual corridor nearly doubles (163,468 units against 86,310). A lane with no room keeps its members stacked, because a diagram that scores well and overlaps a box is worth less than one that scores badly and does not. The window either side is not symmetric — a box one side and open space the other is the ordinary case — so the spread sizes to the whole of it and slides back towards the lane as far as it will go. **The frame's own box bounds the room one unit inside its border**, since a lane laid on that border is drawn over it and reads as it: with a bundle taking one step where its members took several, the arithmetic reaches limits the old spread never did, and six of `mill`'s segments landed there before the unit was taken out. **A bundle is asked as a whole**: every member's checks run against the geometry the others still have, and one refusal leaves all of them where they were, since half a trunk pulled off the other half is worse than the trunk. Its own siblings are exempt from the shared-run rule, because two members of one bundle move onto each other by construction.

*Measured, nudging off against on, no space requests:* corridor **355,116 → 93,066**, and it is the first phase in which that term has a nonzero multiplicand at all. Crossings **66 → 128** and excess length **648,320 → 1,775,868**, the second following the first since excess is charged per crossing on the edge. Bends 587 → 587, aspect and area untouched. `Cost` improves overall — 1.1629e9 → 1.1549e9 — but only because area dominates the tier (§19). Under real text the corpus went from 240 shared-segment pairs over 368,902 grid units to 136 over 86,310, with no route doubling back and none collapsed. The two scales disagree in sign on the corridor term and both are honest: the readable profile's boxes sit differently under measured text, and the lanes a displacement can clear are not the same ones. **With trunks left as one and not charged** (the bundles below, §17 P7c's addendum): real text reads **80 pairs over 49,199 units** plus 90 trunks over 107,818 that are now permitted; on the tables' scale corridor 93,066 → **62,285**, crossings 128 → **104**, excess length 1,775,868 → **1,611,523**, bends 587 unchanged, total polyline length +0.07%.

**Doubling the crossings was partly honest and partly owed.** Two collinear segments do not *cross* under the predicate, so separating them reveals crossings that overlap was hiding. The rest was the ordering key being a projection of one end rather than the full stage.

**The combinatorial stage, built.** Its input is the crossings the ordering itself controls, and there is exactly one family: two lane members are parallel and their legs are perpendicular, so neither two segments nor two legs can cross, and only a leg against the other member's segment can. **A leg leaving the lane at a point strictly inside another member's extent crosses that member's segment unless the member lies on the leg's far side**, so every such incidence is one vote for the order that avoids it, and both members' legs are read into the one pair — which is what makes the matrix antisymmetric and a pair's answer independent of which end asked. A pair whose two legs disagree must cross whichever way it goes and its votes cancel, which is the honest answer rather than the first constraint found winning. Members whose extents coincide at both ends produce no incidence at all: their legs are collinear, so what they share is a run and not a crossing, and the other axis is where that is separated.

The votes are then a digraph — an edge from one bundle to another for every pair they separate that way round — and **the lane's order is a linear extension of it, by Kahn's algorithm taking the lowest key of the bundles nothing left precedes**. It contradicts no vote, and on a lane with no incidences at all every bundle is ready at once, so it is the key order the lane went in with. **Entering them one at a time and taking the position that contradicts the fewest, last of the positions that tie, is not that**: it is a linear extension only where the votes are total, and three members whose extents stagger — `a`'s high leg landing inside `b`, `b`'s inside `c`, `a` and `c` never touching — enter in the key's order `c, a, b` and come out of it in that order, every tie taken last, contradicting the one vote between `b` and `c` where `a, b, c` contradicts none. Votes that run round a cycle have no linear extension to find, and such a lane keeps the fewest-contradictions insertion, because that is the honest answer where no order satisfies the votes. **No pair can cycle on its own** — the matrix is antisymmetric — and no lane of the corpus, the element suite or the fixtures cycles at all, so the insertion is kept against a shape rather than measured on one. Segment order is a total integral key and placement is the same **integer offsets `k*gap`** as before; the pseudo-direction pass the stage was first specified with is unnecessary, because ordering both members from the lane's own low end already measures them from one side (above).

*Measured, the key alone against the full stage.* No space requests: crossings **104 → 85**, excess length **1,611,523 → 956,258**, corridor 62,285 → 62,061, `t2` 1.152747e9 → 1.150115e9. Real text: crossings **124 → 92**, excess 2,784,161 → 1,549,724, corridor 49,199 → 45,963. **Bends are 587 and 613 either side of it, unchanged on both scales**, which is the "minimum crossings with no extra bends" the stage was specified for and is the whole of what distinguishes it from moving a route.

*And the extension against the insertion*, measured on the corpus as the attachment below leaves it: the two orders differ on five lanes and the insertion contradicts no vote on any of them, so there is no crossing for the extension to remove and what moves is which of several extensions the lane gets — corridor **37,018 → 36,973** and excess length 961,229 → 960,779, crossings 102 and bends 563 unchanged, and no chart but `mill` moving at all, there over its three copies of `axis`. **The chain the two disagree on is a fixture rather than a corpus shape**, which is the honest size of it: the linear extension is now what the stage produces rather than what it produces on the easy lanes.

**The order is chosen on crossings and taken only where the room is good, and the two can disagree.** A lane whose preferred order would lay one net's leg along another's fails the shared-run rule above, and the lane then keeps its members stacked rather than taking the second-best order — a diagram that scores well and overlaps a box is worth less than one that scores badly and does not, and the same reasoning applies one level up. Two of the bundling fixtures turned out to be exactly that shape and now have a test of their own.

**Router is swappable** — internal only. Contract: pure w.r.t. its input, reentrant, no global state, called concurrently from workers, must not unwind. Router name and version are hashed inputs. The C ABI exposes routers **by name** (`scav_router_by_name`, `scav_router_list`); function pointers never cross it.

**An abstract base class, not the POD vtable this section first specified.** The boundary never crosses the ABI, so the C shape bought nothing; a `Router` has no members and one virtual call. The registry holds `Router const *` because C++ has no array of references, `reference_wrapper` needs `<functional>` (outside §6's subset), and `router_at` must answer "no such id" for an unvalidated index. `ud` goes with the function pointer.

`RouteInput`: the frame's region, obstacle rects, and nets carrying their two ends and the corridor phase 1 chose. `RouteOutput`: integer polylines plus per-net metrics — bends, length, and a named failure cause — so routers are A/B'd automatically. Internal POD, no `scav_` prefix, because neither crosses the ABI. **The bench is that A/B, and it runs over the registry rather than over two names**: every registered router is scored over the corpus at the readable profile with no space requests, one committed row per `(router, chart)` — Tier 0, the eight Tier-2 terms, the weighted sum, and the degraded and re-seated net counts — in `test_data/golden/layout/corpus_routers.txt`, with the wall clock over the corpus and both 2k shapes reported as a message and never committed.

**A route never leaves `region`**, which is what makes a per-frame obstacle set sound: every frame a decomposed transition passes through is owned by an ancestor of one of its endpoints (§11.1), so anything enclosing the region is excused by §11.14 and anything else is blocked or out of reach. Phase 3 sizes the region to the frame plus every point its nets touch — a port sits on the *crossed* box's border, outside this submachine by the owner's padding — plus the margin the router asks for, since a box flush against the frame's edge has no room for a lane otherwise. Obstacles are every live box overlapping it that does not enclose it, **outermost only**: a box contains its own descendants, so adding them blocks nothing and multiplies the grid by the subtree.

An anchor outside the region, an unreachable end, and a graph past the budget are three different failures and are reported as three: a degraded net is a straight line, and a straight line is what Tier 0 counts.

**Clearance is a bumper, not a penalty.** Obstacles block against their rect grown by `clear`, so "no segment comes within `clear` of a box" is a property of the graph. Pricing the flush lane instead was built first and is worse: it needs a constant tuned against the bend penalty — going round a box costs two turns, so anything at or below two bends leaves hugging cheaper — and it can only ever be probably right, so a test asserts nothing stronger than "not on these inputs."

The cost is over-constraint: two boxes closer than twice the clearance seal the channel between them. That is what §11.5's re-seat is for and the whole of it — the same graph without bumpers, tried once per net. A re-seated net is reported and is not a failure: it routed at spacing the profile did not ask for. **Measured at P7b: four of the corpus's 917 rendered segments run flush along a box edge**, each a leg lying on a box's own border. That is the whole visible cost of the hatch on real input, and it is the number to watch when P7c's channels change what seals. **A route's own ends are exempt by construction**: the search runs between *ring* points one clearance off the border, and the leg from ring to border is emitted rather than searched, so it is perpendicular for free.

Two numbers the orthogonal router derives from the profile rather than reads: the clearance is `node_sep / 3` and the bend penalty is one `rank_sep` (P7b's measurement, §17). Neither is a profile field until P9's calibration says what the fields should be.

**Which face an end is moved onto is the separation from the box, never the distance to a point on it.** Measuring to a candidate border point charges an exit for the run *along* the face it leaves through, so the longer a face is the worse its own perpendicular exit scores — the rule that had `ota`'s fork bar leaving through a 4pt end while a 60pt side went unused, and standing the bar up (§7.2) made it worse rather than better. `ortho_escape_box` measures how far `toward` lies outside the box on each axis instead; the dominant separation picks the axis, a tie goes to x because that is the layering axis (§11.3), and the side is the nearer border, which stays total for a target inside the span. **An end therefore leaves through the face the flow runs through unless the other end is genuinely stacked above or below it**, and a bar's long faces are its attachment faces without anything in the router knowing what a fork is.

Measured over the corpus: bends **689 to 587**, better on ten charts of eleven, with crossings, aspect and area flat. It is a better rule for ordinary states too — nothing stopped one being left through its top face when the next rank happened to sit slightly above.

**Where on the face is the same question one step finer, and it is the attachment's rather than nudging's.** Having picked the face by separation, `ortho_attach_box` puts the end at **`toward`'s own projection onto that face**, clamped into it and held one clearance off each corner — an end on a corner leaves along the face it did not pick. A box centre carries no information about where a route is going, so every net naming one face of one box was handed one point: all six of `ota`'s bar attachments landed on a long face and three of them on the same point of it, with the incoming arrowhead inked along a branch's own first leg.

**This was P7c's, and the implementation moved it.** Written as a nudging pass it cannot work: sliding an end along its face is a move on a segment anchored on a box border, which the stage above excludes for good reason, and the one shape it exists to fix — a route leaving straight out — cannot slide at all without a manufactured bend. Chosen before the search instead it costs nothing, and the search then produces the route from the right point. It lives in the router because the face rule does, and `straight` has no faces.

**Two rules keep it honest.** A glyph **inscribed** in its box — a disc or a diamond — meets an axis-aligned route at one point per face, so its ends take the face midpoint whatever they are aimed at; `RouteInput` carries the fact per obstacle and phase 3 reads it off the `StateKind`, which is the same per-kind knowledge `kind_min_*` already is (§11.4). And where two ends still want one seat, `ortho_spread_attachments` separates them **by direction, not by count**: everything arriving at a point is one fan-in and everything leaving is one fan-out, each a trunk §11.5's bundles exist to keep whole, and what no trunk explains is an arrival and a departure together — where the head is inked along the other route's own first leg and reads as belonging to it.

*Measured over the corpus.* No space requests, the seating against the box centre: bends **587 → 563**, corridor **62,061 → 37,018**, excess 956,258 → 961,229, crossings 85 → 102. Under real text, arrivals meeting a departure on one point **116 → 16**, ends of one direction sharing a point 78 → 19, route segments 917 → 868, labels over another transition's route 9 → 2, and the centred label fallbacks 14 → 12 of 192. The crossings go up because the routes that were drawn inside one another could not cross: total rendered overlap falls 157,017 → 119,671 grid units over the same charts, which is the trade taken deliberately — a crossing is legible and a hidden line is a transition the reader never sees.

**A router's polyline begins at `net.src` and ends at `net.dst`**, except that an end naming an obstacle box is moved onto that box's border. Phase 3 relies on it to lay a transition's nets end to end, dropping a net's first point only when it repeats the previous net's last, and the `straight` router's output is the reference for it.

### 11.6 Cost

Lexicographic across tiers. **No multipliers between tiers** — a dominating weight inside a sum is the cost cliff that breaks local search, and it overflows.

```cpp
struct Cost {                 // compared lexicographically, in this order
  int32_t t0_violations;      // 0 for any admissible candidate
  int64_t t1_hints;           // unsatisfied hint count, priority-weighted
  int64_t t2;                 // weighted sum below
};
```

**Tier 0 — forbidden, not priced.** Edge through a state box, submachine box, or placed box; box-box overlap. Structurally impossible via the obstacle set. The predicate survives as a net for three cases: the straight-line **surrogate** during search; **degenerate enclosure**, a net whose ends the obstacles seal apart even after §11.5's re-seat; and a marked violation with a stable code when the retries run out. Never a silent overlap. **`CostTerms` carries the net as two counts and `t0_violations` is their sum**: `through_box`, a route segment entering the interior of a state box its transition is neither an endpoint of nor a descendant of (§11.14's carve-out), and `box_overlap`, a pair of sibling state boxes sharing area. The submachine box and the placed box are the obstacle set's alone — nothing re-checks them after the fact.

**Degenerate enclosure is answered by inflation, and only `unreachable` triggers it.** `layout_run` raises `rank_sep`, `node_sep` and `sub_sep` by `spacing_inflation_increment` on a copy of the caller's profile and re-runs phases 1–3, up to `spacing_inflation_cap` times, stopping at the first attempt with nothing unreachable and keeping the attempt that degraded least — ties to the earliest. `outside_region` is a disagreement between phase 3's plan and the region it handed the router, and `too_large` is a graph budget; neither moves with spacing, so neither retries. A copy the validator rejects ends the retries, as does a `phase2_size` that leaves the domain, and the last geometry that succeeded stands. The digest hashes the caller's profile, never the copy, so a retry cannot move a golden.

Whatever is still degraded at the end is written as the straight line and marked: one `RouteDegraded` per transition, subject `(Transition, ordinal)`, ordinal-ordered, on `scav_chart_diag`, under `SCAV_OK`. **Measured: the corpus and both 2k shapes inflate zero times with no space requests, and the corpus stays at zero under real text — `scav render` writes nothing to stderr for any of the eleven.** The shape that does not is a fork bar one rank ahead of a composite with `pad` 16, `rank_sep` 0 and `node_sep` 576: the clearance is a third of `node_sep`, so the composite's frame reaches 192 units past its own border onto a bar that spans it top to bottom, and the bar walls the frame off from the route into it. Three inflations of 32 units clear it — the rank gap outgrows the clearance and the bar leaves the frame's obstacle set.

**Tier 2 —** every weight is an integer with a documented ceiling. The largest term is `w_area * bounding_box_area`; area is at most `(2*COORD_MAX)^2 ≈ 2^40`, so with weights capped at `2^10` the sum stays under `2^53` — inside `int64` with room, and the ceiling exists to keep that true rather than to tune anything:

```
w_bends      * bends                                     -- highest in tier
w_corridor   * corridor                                  -- collinear overlap, less the trunks
w_crossings  * crossings
w_excess_len * Σ (excess_length * (1 + crossings_of(e))) -- excess over min_len(e), §11.9
w_adjacency  * adjacency                                 -- §11.8; excludes fork/join fan-out
w_label      * label                                     -- three components, below
w_label_near * label_near                                -- Σ max(0, d_own + h - d_other) per placed box, below
w_aspect     * |w_actual*dar_den - h_actual*dar_num|     -- integer aspect deviation
w_area       * bounding_box_area                         -- lowest
```

Weights are integers in **named, versioned profiles**. Express each as an exchange rate ("n means an edge would rather turn n times than cross"), not a bare number. A weight change, a profile change, a font change, or a packer change is an **output-format change**: versioned, golden-tested, reviewable.

**`corridor` is the length two routes draw over each other**, taken per transition pair over every collinear overlapping pair of their segments, so a line three routes lie on is charged three times — once per pair — and a run is charged over exactly the length it is shared for. **A pair's trunk is not charged.** Two routes ending at the same point run into it as one line, and that is a fan a reader sees as one edge rather than as two edges hiding each other: the trunk is every segment inside the pair's common suffix, plus the leg each route takes into that suffix where those two legs lie along one line, and the same from the first point for a fan-out. Everything else is charged as before, including a run the two share before they merge and one they find again after they part. Measured over the corpus under real text, the exemption alone takes `corridor` from 86,310 to 53,143 grid units with no geometry moved at all, and §11.5's nudging then leaves those trunks alone rather than spreading them, for 49,199.

**`label` counts three things per placed box**, and the third is the one a reader notices most: another placed box; a state's geometry, which is its `before`/`after` text bands where that state encloses the transition's source or target and its whole rect where it does not; and a route segment of another transition, tested as a zero-thickness rect so a box that merely touches a leg is free. The ancestor carve-out is the same one §11.14 gives an edge, for the same reason: a label inside the composite its transition runs in is where it belongs, and charging it there makes zero unreachable. The band is not carved out, because that is the state's own name.

**`label_near` prices the label a reader will attach to the wrong line.** For a placed box of request height `h`, `d_own` is the Chebyshev gap from the box's rect to the nearest leg of its own route and `d_other` the gap to the nearest segment of any other transition's; the term charges `max(0, d_own + h - d_other)` per box, and nothing at all where the box's transition has no route or the chart has no other one. Overlap is not the question — `w_lbl` charges that already, and a box clear of every line can still sit in the wrong channel: PlantUML puts `brew button` between its own arc and the neighbouring `shot done` arrow, touching neither. **The margin is the box's own height because that is one line of its own text**, the smallest separation at which the eye stops having to choose. §11.9's strip matching minimises the shortfall ahead of every other component of its key, so what this term scores is the residual the five strips could not reach.

**Two terms have no nonzero multiplicand on the phase tables, and one had none before P7c.** `w_corridor` priced a shared *corridor*, and a corridor is a channel no registered router had until P7c's nudging found and measured the lanes, so that product was identically zero however the weight was set; since P7c it is the term nudging drives down. `w_label` and `w_label_near` price placed boxes and are scored from `Routes::placed`, which is empty unless the caller made `path_box` space requests — so it is zero on every phase table in §17, all of which score with no space requests, and nonzero the moment the same charts are measured under real text. The first zero is a property of the router; the second is a property of the input, and only the second can be lifted without writing code. `w_b` is not in the same position: a straight-line polyline still turns at every port and bend it passes through, and counting those direction changes is a real measure of how straight an edge came out. So a phase table with no space requests grades on six terms before P7c and seven after, and only a run under real text grades on all nine. This is a statement about the inputs, not a mode: `Cost` is one struct with the same three fields throughout, and registering a router with channels makes `corridor` nonzero with no other change anywhere.

**`Cost` is scored from PODs, never by re-running layout.** Its inputs are the geometry columns and the `SplitGraph` — rects, points, port slots, and the segment table — so a test hands it two hand-written boxes and one hand-written route and asserts a single term, with no model, no profile beyond the weights under test, and no pipeline. That also makes the P6 gate mechanical: score P4's committed geometry and P6's, compare the vectors.

Area is deliberately last: minimizing it directly produces crammed blobs with snaking edges, and narrow channels force bends. For the packing sub-problem the objective is the **scale measure** `SM = min(DAR/w, 1/h)` — held as a rational and **compared by cross-multiplication, never computed**. Its tiebreak order (area, then aspect) is a versioned profile field.

Two cost functions: a cheap **surrogate** for search (bends from port sides plus Manhattan distance, crossings from straight-line segments) and the exact one for scoring. A test asserts they agree on *ranking*; a misranking surrogate optimizes the wrong thing silently. Crossings are counted by **inversion counting** in the layered formulation, `O(|E| log|V|)` — the general straight-line formulation is `O(n^{4/3})` or worse and is not affordable inside search.

### 11.7 Long-edge escape hatch — a builder concern

At depth 16 a literal polyline crossing 15 boundaries is unreadable. The hatch is paired **off-page connector glyphs** with matching tags.

**This is a builder concern, not a layout feature**, which is why nothing needs building for it. The app requests no `PathBox` and no route for that transition, reserves a little space at each end, and draws a tagged stub pair. Layout never learns the transition is drawn differently — it has one fewer route to compute. So the model and the space tables already permit it, and `scav:render=connector` is an ordinary authored attribute the builder reads, not a layout input.

Precedent: **UML 2.5.1 §15.2.4** ActivityEdge connector — "purely notational", exactly-one matching pair — and **SDL / ITU-T Z.100** §2.6.7, which standardizes it inside a state-machine language with a textual dual. UML gives state *transitions* no such notation, so this fills a real gap. A connector must be semantically inert.

### 11.7a Geometry columns — the layout output contract

This list *is* the layout output ABI — there is no bespoke result type (§16) — and every builder consumes all of it. All are `derived-persistent` per §7 and all `pod`-typed (§8) except `scav.geom.route` (`span`) and `scav.geom.gen` (`u32`). Coordinates are root-absolute grid units.

| Column | Parallel to | Holds |
|---|---|---|
| `scav.geom.state` | `StateId` | box rect |
| `scav.geom.state_before` | `StateId` | the rect `h_before` reserved (§8.1) |
| `scav.geom.state_after` | `StateId` | the rect `h_after` reserved |
| `scav.geom.sub` | `SubmachineId` | submachine rect — needed for dividers and titles |
| `scav.geom.route` | `TransId` | `Span` into `scav.geom.point` |
| `scav.geom.point` | point ordinal | `{int32 x, y}` |
| `scav.geom.port` | `TransId` | `Span` into `scav.geom.portslot` |
| `scav.geom.portslot` | port ordinal | `{int32 x, y; uint32 side, boundary_depth}`; `side` is 0 left or 1 right today, 2 top and 3 bottom reserved (§11.3) |
| `scav.geom.chart` | chart | bounding box of everything laid out |
| `scav.geom.inputs` | chart | digest of the run's non-geometry inputs (§6) — `u32` |
| `scav.geom.gen` | chart | generation counter (§13) — `u32`, **not hashed, not serialized** |

`ElemKind::Point` exists so the point and port-slot arrays are real columns rather than side arrays outside the column rules; entity count is the column length.

**The `scav.geom.` names are layout's.** A run finds each by name and writes through it, so a column an application registered under one of these names with another entity, kind, or element size is refused before any geometry is computed, with `GeometryColumnClash` on the chart and every column left as it was. Layout does not trust a descriptor it did not write. A transition splits into one port per boundary it crosses (§11.1), not two — a depth-16 edge has up to 15, and the structural hash covers all their sides, so one row per transition cannot hold them.

`scav.geom.chart` bounds **everything laid out, not only the root submachine's extent**: a route bends into a frame's padding and a path box centres on a point of one, so either can reach past it. A consumer sizes its viewport from this rect, and one cut to the root clips whatever crossed the line.

**Hashing is by explicit allowlist, not by enumerating `Chart.columns`** — otherwise app and plugin columns perturb scav's own goldens, and the same corpus hashed through `scav` and through `scavview` would differ. The **structural hash** covers ranks, orders, port sides, and bend sequences *as direction-turn tokens*; the **coordinate hash** covers the rects and every point and port coordinate. Turn tokens rather than points is what makes a pure translation move the coordinate hash and not the structural one, which is the whole point of the split.

**Rank and in-rank position are recovered from the rects as ordinals, not stored in a column.** On an integer grid with a left-to-right layering axis (§11.3), a submachine's rank sequence is the sorted distinct x of its children and a node's in-rank position is its index by y within its column — so both are derivable, and derived *as indices* they are translation-invariant, which is exactly the property the split needs. `SubmachineOrders` therefore stays an internal intermediate (§11) with no ABI presence, and the structural hash stays recomputable from a laid-out chart alone, which is what `scav selftest` (§6) requires. A `scav.geom.rank` column would be the alternative and is rejected twice over: no builder would consume it, breaking this section's own rule, and storing an ordinal that the coordinates already determine invites the two to disagree.

### 11.8 Transitions between concurrent submachines

Supported; arbitrary topologies must be. **Drawn as a direct arrow**, not routed up through the parent — the semantics are parent-mediated (exit to just below the owning ancestor state, re-enter the source submachine at its default initial configuration, enter the target along the path from the LCA) but **the geometry does not follow the execution path.**

General principle: **scav draws topology, not execution paths.** Applies equally to history and choice.

Phase 0 still splits at both submachine borders; the middle segment crosses only the separator, routed in the parent's frame by the LCA-owning submachine (§11.5).

**[OWED] That middle segment does not route, and the corpus never asked it to.** "The parent's frame" has no referent: two concurrent submachines are siblings under a *state*, and the nearest enclosing *submachine* is the one that state itself sits in — one level further out than the state. In that frame the state is an ordinary obstacle, so it walls the route out of the space between its own two regions and the route goes the long way round it, through whatever else the frame holds and back along the line it arrived on. Measured on `gauntlet/regions.scav`: **two edges through a box and two routes doubling back**, at both shipped profiles. No corpus chart carries the shape — eleven charts and not one transition between two regions of one state — so P7b's Tier-0 zero is a zero over inputs that never exercise §11.8's own case, and it took the element suite (§5) to find that.

The fix §11.14 already implies is **per-net rather than per-frame excusal**: a box that strictly contains both of a net's ends is a box that net cannot avoid, and blocking it buys a detour rather than a diagram. The cost is that the obstacle set stops being one per frame, so the orthogonal router's one grid per frame becomes one per distinct exemption set — cheap, since a frame has one or two, but not free, and it is the router's grid cache rather than a rule change. Scheduled with P9, which is the first phase that has a reason to re-enter the router.

A direct arrow wants its two submachines **adjacent**; a third submachine between them means an edge crossing an unrelated submachine rectangle, which is Tier 0. `w_adj` prices non-adjacency and "reorder sibling submachines" is already a local-search move — no new algorithm. **Edges incident to a fork or join are excluded from `w_adj`**: adjacency is pairwise, so a fan-out above two cannot achieve it under a linear packing, and pricing an unsatisfiable constraint distorts everything else — the same reason `w_len` charges excess only (§11.9). This lets a submachine-crossing transition override source order (§14), which is correct: adjacency for a real edge beats reading order for a rare one.

**[OPEN]** whether to depict the implicit source-submachine reset (a ghost arc to its initial state). Drawing direct hides both the exit and the reset; no competitor depicts either.

### 11.9 Text metrics and labels

**Measurement is the application's; the metrics are scav's.** An app sizes its own content and fills the space tables (§8.1), but it must use **one** metrics implementation shared by its measurement pass, its builder, and its backend — scav's by default — so those three cannot disagree about how wide a string is. Substituting a shaping engine (§11.9.2) means substituting it for all three.

Three coordinate spaces, two conversions, each in one place:

| Space | Unit | Who |
|---|---|---|
| font design units | `1/units_per_em` em | inside the metrics helper only |
| **layout grid units** | 1/16 pt | the model, layout, and `DrawList` |
| output units | SVG user units, pixels | the backend only |

```
w_grid = ceil_div(sum(advance_funits) * font_size_grid, units_per_em)
```

Accumulate as `int64`, divide **exactly once**, **`ceil` never round-to-nearest** — an under-sized box is a diagram that lies. `font_size_grid` is an integer; the ABI takes `int32_t` sixteenths of a point, so no float crosses. Box height is `ceil_div(font_size_grid * k_num, k_den)`, **not** font vertical metrics — `hhea`, `OS/2.sTypo*`, and `usWin*` disagree by 10–20% within one font.

Font size is a profile field and integer rounding is nonlinear, so changing it forces relayout. Em-relative units rejected: they require *everything* including padding and glyphs to be em-relative and still do not scale exactly.

**Wrap width is always an input, never an output.** The forbidden circularity is height→width→placement→height, needing a per-box shape function and a fixpoint. An app wraps to any width it likes — it is measuring, so it decides — and layout never re-wraps. Author-controlled breaks (§15) because identifier text must break at semantic boundaries, not a pixel column, and fixed line counts keep layout stable under font-size change.

**Labels routinely dominate transition length: a constraint, not a pathology.** ``min_len(e) = max(geometric_min, Σ PathBox widths along the route)``, a hard sizing input. `w_len` charges **excess only** (§11.6) — charging raw length makes the optimizer fight an unwinnable constraint and cram everything else. Rank separation grows to fit the widest label crossing each rank boundary (§11.3).

**A `PathBox`'s size is a phase-1 input; its position is a phase-3 output** (§11.3). Sizing rank separation needs the extent before any coordinate exists, and placement needs a finished route, so the same row answers two questions at two stages. **Placing a `PathBox`** is Kakoulis & Tollis strip matching. Each leg of the route offers two sides, each side five strips one box height apart, and on each strip the box slides from the leg's low end to its high end in steps of its own height, plus the leg's exact centre. A candidate is infeasible if it leaves the chart rect or overlaps a state that encloses neither endpoint, any state's `before`/`after` bands, an already-placed box, another transition's route, or a leg of its own route other than the one it rides; touching is free throughout. The winner is the lexicographic minimum of `(label_near shortfall, Manhattan distance to the centred placement, leg, side, strip, slide)`. **The shortfall comes first** (§11.6): a box crosses to the far side of its own leg, or moves to another of that route's legs, rather than read as the neighbour's label, and the distance decides everything the shortfall ties — so a label already clear and already unambiguous does not move. Boxes are assigned transitions ascending then by `order`, a later box confined to legs at or after the earlier one's and, where that is the same leg, to slide positions further along the route than the earlier one took — further along, not higher in coordinate, since a leg is as often traversed right-to-left or bottom-to-top. NP-hard, so heuristic: a box with no feasible candidate keeps the centred placement, which is **12 of the corpus's 192** under real text. Rip-up-and-reroute of that one edge (§11.5) is the design for those and is **not built**; at 6% of boxes it buys less than the fifth strip did, and it is the only part of this that has to re-enter the router.

#### 11.9.1 Font metrics

Minimum tables: `head` (`units_per_em`, offset 18), `hhea` (`numberOfHMetrics`, offset 34), `hmtx`, `cmap` (format 4, plus 12), `maxp` (bounds-checking). Advances never come from `glyf`/`CFF`. Bundle exactly one static font. The extension path is `scav_metrics_create` with your own TTF bytes (§16), not runtime substitution — the font is a layout-hash input, so swapping it is an output change.

Three traps: the **`numberOfHMetrics` tail rule** (the last record's advance applies to all remaining glyphs — breaks monospaced fonts specifically, which is what we bundle); vertical metrics disagreeing with themselves (see above); per-glyph rounding.

**Kerning is deliberately ignored** — conservative for Latin (kerning narrows, so boxes over-size and never under-size) — and `render` emits `font-kerning: none` so both sides agree. Missing glyphs **fail loudly**; silent zero-width produces boxes narrower than their text.

#### 11.9.2 RTL and complex scripts — not v1, not blocked

Arabic advance widths are not the sum of per-codepoint `hmtx` values (mandatory cursive joining via `GSUB`, ligatures collapsing codepoints). The would-be show-stopper — measurement as a callback inside layout — is already avoided: the metrics helper is a separate POD-producing entry point, so the fix is to **swap that helper** for a shaping engine (`hamza`, MIT; or HarfBuzz, "Old MIT" — both deterministic). Do not undo that split.

Watch four things: `textLength` must become conditional (`lengthAdjust="spacing"` breaks cursive joining, so RTL leans on `--embed-font`); ignoring kerning degrades from a few percent to absurdly wide; byte-wise collation means codepoint-order sort; bidi (UAX #9) belongs in `measure` and `render` only — the pool stores logical order, which is correct.

Diagram mirroring is an **output transform**, not a second layout algorithm: lay out LTR, x-mirror at the coordinate stage, `render` flips anchors and arrowheads but not glyph order.

### 11.10 Search

Local search from a structured seed with **restricted uphill moves** — "simulated sintering", Grover, DAC 1987 (~10% better *and* 3× faster than annealing from random). Not generate-and-test: random restart of a constructive heuristic measures ~2000× worse than guided search at equal CPU (Martí & Laguna), and published metaheuristic layout throughput is ~6–54 candidates/sec at 10–100 nodes.

- **Portfolio**: K fixed strategies per submachine, K from the profile, seeded by submachine id. Results collect into a K-slot array indexed by strategy and reduce by `argmin(Cost, strategy_index)` after all shards join — never a shared best-so-far.
- **Local search**: bounded moves — swap adjacent in rank, move node across ranks, flip port side, reorder sibling submachines, rotate subtree. **Best-improvement per sweep**, moves enumerated in `(move_kind, subject_index, parameter)` order, tie-break `(delta, move_kind, subject_index)`, fixed sweep count from the profile. Parallelism evaluates deltas; **acceptance is a serial index-ordered pass**.
- **Delta-evaluate.** Never re-score a submachine after a bounded move.

### 11.11 One algorithm, stable by construction

**There is no `quick` mode and no `polish` mode.** Layout runs one algorithm, always, and is a pure function of `(model, spaces, profile)`. No warm start, no prior layout, no incremental dirty-region path, no persisted cache.

**Stability is a property of the algorithms, not a cost term.** A small model change yields a small diagram change because each stage is order-preserving and deterministic — LR-rectpacking preserves input order (§11.4), ranking and ordering have total-order tie-breaks (§6), document order drives reading order (§14).

That removes `w_st`, `PriorLayout` and its version key, per-loader hysteresis, the sticky packer bit, dirty-region tracking, and VPSC (§11.13) — plus a class of defects: layout depending on edit history, and a golden hash needing a "cold-start" qualifier to mean anything.

**Honest limit.** Stability-by-construction is not a guarantee. A one-node change can flip a crossing-minimisation decision and cascade, and no ordering discipline prevents that in general. Those are the boundary conditions **hints** exist for (§14): when the engine makes a defensible choice the author dislikes, the author pins it rather than the engine remembering what it did last time.

**This is a bet on speed**, and it should be measured rather than assumed: full layout must be fast enough at 2k states that no second mode is wanted. If it is not, the answer is to make layout faster — not to reintroduce a mode, which trades a performance problem for a correctness one.

### 11.12 Quality baseline

The likeliest failure is producing layouts that score well on `Cost` and that readers find worse than the PlantUML output they already have. Nothing in a cost vector detects this.

**A side-by-side harness ships at P5b**: the same chart through PlantUML, elkjs, and scav. PlantUML rather than the `dot -Tsvg` this first named, because the `.puml` files scav replaces are rendered by exactly that binary — the incumbent itself, not a stand-in for one. Its state-diagram syntax carries composite states, concurrent regions and every pseudostate kind, so the translation is mechanical; `--puml` points the harness at real sources to remove even that. `-Playout=smetana` keeps it off a host Graphviz, which would make the comparison depend on what the machine happens to have installed. Exit criterion is **"no worse than the incumbent on the transcribed corpus"** — not "visually reasonable" — and it is judged at **P9**, not P7, for the two reasons below.

**Tier 0 at zero is a precondition for scoring, not one of the things scored.** Both incumbents route around obstacles and sit at zero edges-through-a-box on every chart by construction, so one violation settles the comparison on the tier compared first and the scores measure the missing router rather than the layout. The review therefore cannot run before a router with an obstacle set does (§11.5).

That also blocks §11.3's global sifting and edge-weight schedule and §11.4's compaction, each of which is written to be bought when review says that term is wrong.

**Run at P7b. One finding recorded, the rest [OWED] to this document.** §11.4 carries the compaction verdict — `vac` at 55% occupancy — and that is the whole of what survives in writing. The ranking against the incumbents, and whatever the review said about §11.3's sifting and edge-weight schedule, were not written down.

**The scored review is not a P7 gate, and it was never blind.** Decided 2026-09-05, with P7c and P7d landed and the three rendering defects a reviewer would have marked first — labels over names (§11.9), routes sharing a lane (§11.5), glyphs sized by text nobody draws (§11.4) — gone from the page. Two reasons. **Blinding is nominal**: PlantUML rounds its state corners and draws curves, elkjs draws sharp right-angled polylines in a horizontally biased layout, and scav is whichever panel is neither; hiding the labels hides nothing. **And a P7 render is a single candidate, not a layout.** §11.10's search — the portfolio, the local search, the calibrated weights — does not exist yet, so every scav panel is the first admissible layout the pipeline produced, adhering to its constraints and often plainly suboptimal within them. Scoring that against two engines that do optimise measures the missing optimizer, not the design, the way scoring P6 measured the missing router. So the comparison moves to P9's exit, where it is the test of what P9 built. **The side-by-side itself stays the standing instrument**: `tools/baseline.py` and `tools/poster.py` are re-run at every phase and the page is looked at, unblinded, for the defects a cost vector cannot see — that is how the compaction finding above was made and how P7c's shared lanes and P7d's labels were found before they were priced.

### 11.13 Rejected

**Topology-shape-metrics.** Needs planarity — statecharts are non-planar, planarization is NP-complete, and the literature's ceiling is "a few hundred vertices". Compound nesting isn't in the model; the bolt-on rests on c-planarity, open 1995–2022. Three chained NP-hard problems with documented excess bends and area blowup. HOLA replaces it; CoDaFlow rejected it for compound-plus-ports specifically. Keep only compaction by topological numbering (§11.2).

**VPSC.** Coordinates are generated, not adjusted, so separation is longest-path: linear, exact, integral, ~100 lines. VPSC buys only minimum-displacement-from-prior, and §11.11 leaves no prior. If ever revisited, note the GD 2006 **Correction** — the published algorithm can return **infeasible** solutions.

**LP nudging fallback.** "Integral if coefficients are integral" is false — that needs total unimodularity, unestablished here — and simplex pivoting under degeneracy is tolerance-driven float. Deterministic degradation instead, and it is what ships: the router re-seats the net without its clearance, then `layout_run` widens all three separations by `spacing_inflation_increment` and re-runs phases 1–3 up to `spacing_inflation_cap` times; whatever is still unreachable at the cap is drawn as a straight line and diagnosed `RouteDegraded` (§11.6).

**Active bundling.** No attraction term, no inverted congestion, nothing that steers a route towards a sibling's trunk so that two edges arrive as one. A route is its own shortest path under the bend penalty and a trunk exists only where two of those coincide, because a detour to merge trades length the reader pays for on every glance against a merge the reader may not notice — and because a term that rewards two routes for touching is a term the optimiser can satisfy by drawing one edge on top of another. §11.5's bundles are a rule about what the pipeline may not pull apart and §11.6's exemption about what the scorer may not charge; neither is a rule about where a route goes.

**PRISM, GTREE, FORBID** — Delaunay plus iterative solvers, or stochastic gradient descent. **EditLens randomized nudging** — admits residual overlaps and is randomized; permitted only via the position-addressed RNG (§6), never as a determinism carve-out.

### 11.14 Transition kind — internal, external, local

`TransKind` is a **first-class layout and rendering input**, not passthrough metadata. Implementations attach materially different runtime semantics to it — under libhsm, `internal` means the source state is neither exited nor re-entered, `external` means it is exited and re-entered — so a renderer that draws them identically produces a diagram that is wrong about behavior. scav does not interpret the semantics; it preserves the distinction and gives layout the one fact that follows from it.

**The layout-relevant rule, stated without semantics: `kind` decides whether the arrow crosses the source state's border.**

| Kind | Source border | Geometry |
|---|---|---|
| `external` | crossed | ordinary edge. Self-transition (`src == dst`) is a **loop outside** the box, leaving and re-entering the border |
| `internal` | **not** crossed | source endpoint sits on the border's **inner** face. Self-transition is an arrow **entirely inside** the source box |
| `local` | not crossed | as `internal` at the source; differs only at intermediate boundaries for a composite source (does not exit the composite, does exit substates) |

**`internal` does not imply a self-transition.** A transition from a composite state to one of its own descendants can be `internal` — libhsm's `Online --> online_idle : internal` is exactly this. So the rule is about the *source border*, not about `src == dst`.

**The mirror case: a destination that encloses its source.** `trans Idle -> On` from inside `On` exits every state between `Idle` and `On` and then has nowhere left to cross: `On`'s own border is not on the route. The arrow therefore **terminates on `On`'s inner face**, at the boundary node phase 1 placed for it, with no port slot, whatever the transition's kind. It does not end at `On`'s centre, and `On`'s border is never counted crossed. Found by review rather than by the corpus, which has no such transition: phase 3 was inferring the inner-face end from phase 1's tables and mistook the destination's boundary node for the source's, drawing the arrow from a point on `On`'s border to `On`'s centre and touching `Idle` nowhere. The fix is §11.1's explicit end kinds, pinned by unit tests on both routers. **[OWED]**: a corpus chart carrying the shape, so blind review and the goldens see it too.

Consequences:

- **Phase 0 (§11.1) suppresses the source-boundary split** for `internal` and `local`. One fewer segment, one fewer port. The derived boundary-crossing count (§7) must reflect this, or `w_len`'s per-crossing multiplier (§11.6) miscounts.
- **Tier 0 (§11.6) carve-out:** an edge may occupy the interior of a state whose border it does not cross, and **only** that state. Every other state and submachine rectangle remains an obstacle.
- **Internal self-loops are the app's, end to end.** The app sums the band it needs into `h_before`/`h_after` and its builder draws the glyphs inside the returned `scav.geom.state_before`/`_after` rect. There is no route, so `PathBox`/`PathClear`/`min_len` do not apply and the router is not involved. Layout sees only two integers.
- **The reference builder distinguishes the three kinds**, pinned in the `drawlist/` golden. scav cannot mandate what a custom builder draws (§2, §3).

### 11.15 The profile

A versioned, hashed artifact (§6), so it needs a field list rather than thirteen scattered references. All integers.

| Group | Fields |
|---|---|
| geometry | `pad` — a box's interior ring only (§11.4) |
| separation | `rank_sep` between adjacent ranks, `node_sep` between adjacent nodes in a rank (§11.3), `sub_sep` between packed sibling submachines (§11.4). Each `[0, COORD_MAX/4]`. Distinct from `pad` because that one is interior and these are between things |
| type | `font_size_grid`, `line_height_k_num`/`_k_den` (`k_den >= 1`) |
| pseudostate sizes | per-`StateKind` min extent. `fork`/`join` are wide-and-thin boxes; nothing scales with arity (§7.2) |
| packing | `dar_num`/`dar_den` (each in `[1, 2^10]`), `trybox`, SM tiebreak order |
| cost | the nine Tier-2 weights, each with a ceiling keeping `Σ Tier-2` inside §11.2's budget |
| search | portfolio `K`, `sweep_count`, congestion iterations, rip-up cap, spacing-inflation cap and increment. `sweep_count` bounds every fixed-count improvement loop, phase 1's crossing-minimisation sweeps (§11.3) included — one knob because they are one question, splittable at P9 if calibration wants different numbers |
| format | `print_columns` — the canonical printer's line-break budget (§15) |
| id | `profile_id`, `profile_version` — 4 on both shipped profiles, `w_label_near` being the field that last moved it |

Profile load **validates every bound and rejects out of range** — weight ceilings give the Tier-2 sum a proven bound, and bounded `dar_num` keeps `total_area * dar_num` under `2^50` before `isqrt`.

`print_columns` arrives ahead of the rest, with the printer (P3) rather than with layout (P4), because canonical output is part of the contract from the moment `fmt` exists. It is the one field the printer reads and the only one nothing else does.

Named profiles ship as data: `compact`, `readable`. There is no `print` profile — fit-to-page would need the top-down layout §11.4 rejects.

## 12. `DrawList` and rendering

**`DrawList` is the render IR and the one drawing contract.** A builder produces it from model columns; a backend consumes it. Neither knows about the other, and neither is required to be scav's.

```cpp
enum class PrimKind : uint32_t {
  rect, rrect, line, polyline, path, text, circle, arc, image
};

struct Style {                   // interned; primitives index a style table
  uint32_t stroke_rgba, fill_rgba;
  int32_t  stroke_w;             // grid units
  uint32_t dash;                 // 0 = solid; app-defined otherwise
  int32_t  font_size_grid;       // same width as the ABI (§11.9)
};                               // 20 bytes, no padding — see below

struct Prim {
  PrimKind kind;
  int32_t  depth;                // draw order; see below
  uint32_t style;                // -> styles[]
  uint32_t clip;                 // -> clips[]; INVALID = unclipped
  ElemRef  origin;               // back-reference to the defining model entity
  Span     points;               // -> points[]; meaning per kind
  StrRef   payload;              // text, or an image id; empty otherwise
  int32_t  a, b;                 // kind-specific scalars: corner radius, angles
};

struct DrawList {
  std::vector<Prim>  prims;
  std::vector<Style> styles;
  std::vector<Point> points;     // absolute grid units
  std::vector<Rect>  clips;
  StringPool         text;
};
```

`points` and the scalars are fixed per kind, so a backend switches once and never guesses: `rect`/`rrect` 2 points (corners, `a` = radius) · `line` 2 · `polyline`/`path` N >= 2 (`path` closes, `polyline` does not) · `text` 1 (baseline origin, `payload` = the string) · `circle` 1 + `a` = radius · `arc` **2** (the corners of its bounding box, as `rect`) + `a`/`b` = start/sweep in 1/64 degree · `image` 2 + `payload` = registered id. Arc takes two points rather than one because a centre plus two angles has nowhere to put a radius, and a bounding box is how SVG, Qt and Cairo all spell an arc — it makes the elliptical case free and needs no field packed two ways. Any other count is invalid, and the `DrawList` validator rejects it.

**Draw order is an explicit `depth`, not array position**, which makes `DrawList`s **appendable**: an app appends the reference builder's output to its own and depth resolves interleaving — no splice, no forking the builder to reach the middle of its stack. Append is not raw concatenation, because `style`, `clip`, `points`, and `payload` are indices into per-list arrays; `scav_drawlist_append` rebases all four, which is the whole reason it is a shipped function rather than a documented `insert()` call.

A backend either **orders by `(depth, emission_index)`** for painter's algorithm or writes depth as z under orthographic projection. That key is a total order, so §6's comparator rule holds without relying on sort stability. Depth-as-z covers opaque content only; a blended backend still sorts, using the same integer.

**scav reserves no depth bands and assigns no depth semantics.** Emitters take depth as a parameter — `emit_state(dl, chart, depth)` — so the caller owns the numbering. Reserved bands would have been scav deciding an ordering the app should own, and they are meaningless to an app that writes its own builder. The convenience wrapper picks *some* defaults, documented as that one function's choice rather than as a namespace: if you need to interleave, call the emitters and pass your own numbers.

**Clipping is a per-primitive index, not a `clip_push`/`clip_pop` pair.** Stateful scope primitives cannot survive a depth sort — sorting separates a pair from the primitives it was scoping. So a `Prim` names its clip rect directly, which also lets a GPU backend batch by scissor rather than replaying a stack.

**Identity is a back-reference, not a class string.** `Prim.origin` is an `ElemRef`, with a `none` kind for primitives belonging to no entity. A backend wanting CSS classes *synthesizes* them — `class="scav-state scav-id-1234"` (§12.1) is the SVG backend's projection, not IR content. String classes would leak an SVG concept into an IR that also feeds ImGui, and add interning to a hot path.

**Style is a separate table, which is what makes §13 cheap.** Live recoloring mutates `styles[]` and leaves `prims`, `points`, and `text` cached. Fat per-primitive style forces a full rebuild every frame.

**Coordinates are absolute grid units**, one frame, no per-primitive frame tag — a builder reads geometry columns and knows where things are.

```
builder:  (model columns, incl. geometry) -> DrawList     // app's; scav ships a reference one
backend:  DrawList -> ImGui calls | SVG text | PDF | ...  // app's; scav ships SVG + ImGui
```

**The application owns the builder and the render function.** How it organizes them — one function, a list of passes, a class hierarchy — is its business and not scav's concern. A builder that also draws threat radii, a timeline, or annotations linking distant states needs no scav change, because it has the whole model and all the geometry.

Two properties worth keeping:

**Golden-test the `DrawList`, not the SVG.** It is canonical POD with no formatting degrees of freedom, a strictly better comparison surface than serialized text. What the `drawlist/` golden pins is the *reference* builder's output — a regression test on shipped code, not a claim on what any builder must draw (§2). Canonical form sorts by **`(depth, prim_bytes)`** — content, not emission order — with `styles[]` and `clips[]` deduplicated and sorted by field bytes, and `style`/`clip` indices rewritten to the deduplicated tables. Every field of `Style` and `Prim` is therefore 4 bytes wide: §6 forbids byte-comparing a struct with padding, whose contents are unspecified. Sorting on content is what makes two builders that draw the same picture in different orders compare equal; an `emission_index` tiebreak would not. Sorting the golden means it compares *what gets drawn*, so two builders that produce the same picture by different emission orders compare equal. SVG emission then gets a thin serializer test rather than carrying the whole rendering contract.

**One metrics implementation** (§11.9), with a golden test asserting builder and backend agree for every box.

**Images: the app registers, the `DrawList` references.** `scav_image_register(images, id, bytes, len, w, h, mime)`. Raster only — arbitrary SVG fragments would be unimplementable in an ImGui backend and would break the one-IR property; vector content is primitives. Dimensions come from registration, not decoding, so no backend needs a decoder to *size* an image and the SVG backend needs none at all (base64 the bytes with their MIME type). Bytes hash into the SVG golden.

**No backend imposes an extent limit of its own.** Diagram size is bounded by the layout grid on the way in (§11.2) and by the output format on the way out, and by nothing in between — no configured maximum, no page, and no writer's own bookkeeping narrower than the format it targets. A format's own ceiling is the only one that may reject: SVG has none, and PNG's `IHDR` is `uint32` capped at 2^31-1 by spec. Where such a ceiling exists, exceeding it is a diagnostic naming the format — never a silent clamp and never a quietly scaled-down diagram, both of which produce a picture that lies about the model.

**A raster backend streams.** Whole-image residency is a memory bound with no format behind it: a 2k-state chart at print resolution is gigabytes of framebuffer that nothing needs at once. Emit row bands instead, which is what PNG's `IDAT` chunk sequence already is, so peak memory tracks the band and not the diagram. v1 ships SVG and ImGui, so this binds whichever raster writer lands later rather than describing code that exists.

### 12.1 The reference SVG backend

Headless `scav render` is the first user-visible deliverable (P5b), so this one ships.

**Emit the body in integer grid units with the entire scale in one integer `viewBox`.** Float-to-decimal conversion is not portable (MSVC UCRT, glibc, musl, and Apple libc disagree on the last digit) and `-ffp-contract=fast` is the default, so `grid * scale` differs by 1 ULP between Debug and Release. **No float is printed, ever.** SVG sets no extent ceiling, so neither does `render`.

Renderer-vs-metrics agreement, in order: one bundled font, named with a fallback · `textLength` with `lengthAdjust="spacing"` from our own advance sum, turning overflow into slightly loose spacing (Graphviz emits none, which is why its SVG overflows under substitution) · `font-kerning: none` per §11.9.1 · explicit padding, never sizing to exactly the text width · `--embed-font` base64ing the bundled TTF whole into `<defs><style>@font-face`, the only exact agreement that keeps text selectable — whole, not subsetted, because a subsetter is the expensive part of the PDF backend and v1 does not have one. **Never convert text to paths** — needs the outline stack we avoid, discards selection and accessibility.

Emit a stable `class` per element, synthesized from `Prim.origin`: `scav-state scav-id-1234`. External CSS can then restyle a static SVG.

**`arc` is the one kind this backend refuses.** An `A` command needs endpoint coordinates, and deriving those from a start-and-sweep angle needs trigonometry that no integer path in scav supplies — a table at 1/64-degree resolution would be ~23,000 entries, and no shipped builder emits an arc. So `svg_write` reports the offending primitive rather than approximating it, and the table arrives with the first builder that needs one. Eight of nine kinds render.

**Opacity is the one ratio that reaches the output**, because SVG has no integer spelling for it. `fill-opacity="0.501"` is assembled digit by digit from `alpha * 1000 / 255` in integer arithmetic — not a float-to-decimal conversion, so every platform emits the same bytes. Colours stay `#rrggbb` with a separate opacity attribute rather than CSS Color 4's `#rrggbbaa`, which older consumers ignore silently instead of refusing.

PDF is out of v1: xref tables, content streams, and a real TTF subsetter, ~1,500–3,000 LOC, most of it duplicating `--embed-font`. SVG→PDF via any converter covers it.

## 13. Live highlighting

Static layout, dynamic appearance: a viewer highlighting active states and recently-taken transitions at frame rate over a layout that never moves.

**This needs almost nothing from scav, which is the point.** Geometry is in model columns and does not change, so the app rebuilds its `DrawList` each frame, or caches `prims`/`points`/`text` and mutates only `styles[]` (§12). No overlay channel, no command vocabulary, no scav-side animation state.

Two rules that are scav's:

**Appearance must not change metrics.** Recolor freely; changing font, weight, size, or content resizes boxes and forces relayout. If bold-for-active is wanted, measure at bold *always*.

**Geometry columns carry a generation counter**, so a builder cannot run against a partially-updated model. Cheap, but it must exist. Stroke clearance is not a scav concern — an app reserves it via `BoxSpace` and draws inset (§8.1).

**scav models no time, activity, or recency (§2).** The active configuration is a *set* — one leaf per active submachine plus ancestors — computed by the application. Immediate-mode: the app recomputes appearance from `(events, now)` every frame and scav retains nothing. A retained fade would put animation policy and mutable per-element state in scav.

### 13.1 Debugger glue is the application's

For a running target pushing events over a socket or UART: the app opens the socket, buffers asynchronously on its own thread, decodes with its plugin, and each frame folds the event window into the colors it passes to its builder.

**scav defines no event vocabulary.** There is no `kind == "entered"` or `"took"` — runtime semantics are exactly what scav does not model, and the real space is far larger than any schema scav could guess: entered-via-history, guard-evaluated-false, choice-resolved, submachine-forked, deferred-event-consumed. Every dialect differs and the meaning lives in the plugin.

Testability is why: appearance is a pure function of `(events, now)`, so a recorded log with a fixed `now` reproduces a frame exactly.

If an app embeds a script host (§8.3), the natural split is transport and decode native, event-to-appearance scripted — with **trace scripts in a separate interpreter state from appearance scripts**, since trace input is wall-clock-dependent and must be structurally unable to contaminate anything feeding a hashed layout.

## 14. Layout hints

**[OWED in full: nothing here is built.]** No hint column exists, no `scav:` attribute is resolved at load, and layout reads only the space tables and the model. What follows is the design the tree is expected to grow into.

**Hints are columns, like geometry.** There is no separate `HintTable` input: layout reads hint columns the way it reads space and model columns. The only distinction that matters is the one §7 already draws — **authored hints persist and serialize; app-computed hints are derived and get overwritten.** That falls out of the column classes rather than needing a mechanism.

Absolute pins fall out of the same rule: a pin is an **authored** column, so it round-trips. `scav:pin` alongside `scav:right-of` and the rest, all resolved from `scav:` attributes at load (§8) into integer columns so layout never sees a string or a path.

**Source order is the primary hint and costs no syntax.** LR-rectpacking is order-preserving, so model order maps to reading order. Consequently **document order must survive parse → model → layout, and the canonical printer must never reorder states or submachines.** Attributes may be sorted; structure may not. (This is the opposite of `puml2c`, which sorts states alphabetically — that sort belongs in the codegen backend.) `w_adj` (§11.8) may override source order for submachine-crossing transitions.

Deliberately not designed further until the engine runs on the real corpus. Known needed: relative position across a containment boundary; sibling submachine stacking direction. Structural requirements that must hold now: hints live inline next to their subject; priority is source order; over-constrained sets emit a stable diagnostic and **drop the lowest-priority hint**, never failing the render; Tier 1 dominates Tier 2.

## 15. The `.scav` format

**Decided, and it is the first thing built** (P0). A terse block-structured DSL, LL(1), whitespace-insensitive.

```ebnf
document   := chart
chart      := 'chart' ident [ string ] block
block      := '{' [ item ( ',' item )* [ ',' ] ] '}'
item       := include | state | submachine | trans | attr
include    := 'include' string 'as' ident
state      := ('state'|'s') ident [ state_kind ] [ string ] [ block ]
submachine := ('submachine'|'m') [ ident ] [ string ] block
trans      := ('trans'|'t') [ trans_kind ] endpoint '->' endpoint [ string ] [ block ]
attr       := '@' key [ '=' value ] | '@' ident datablock
datablock  := '{' [ entry ( ',' entry )* [ ',' ] ] '}'
entry      := ident [ '=' value ]
value      := string | '[' [ string ( ',' string )* [ ',' ] ] ']'
endpoint   := '*' | path
path       := seg ( '/' seg )*
seg        := ident [ ':' ( ident | digit+ ) ]   -- submachine qualifier, §9
key        := ident [ ':' ident ]
ident      := [A-Za-z_][A-Za-z0-9_]*
digit      := [0-9]
string     := '"' char* '"' | '"""' rawchar* '"""'
state_kind := 'normal'|'choice'|'junction'|'fork'|'join'|'history'|'deephistory'
trans_kind := 'external'|'internal'|'local'
comment    := '//' <to end of line>          -- trivia; lexed, not parsed
```

`//` to end of line is the only comment form — no block comments, so there is no nesting rule and no unterminated-comment failure mode, and the printer's position classification (leading / trailing / own-line) stays a line-relative question.

```
chart vac "robot vacuum" {
  include "dock.scav" as dock,

  state Off "powered down",
  state Booting,
  state PreConfig choice,

  trans * -> Off,
  trans Off -> Booting "POWER_ON",

  state On {
    @doc = "Enter: publishes EVT_POWERED_ON",
    @nav { uses_lidar, follow_walls = "false" },

    submachine main {
      state Idle { @nav:retry = "false" },
      state Ready,
      trans * -> Idle,
      trans internal Ready -> Ready "BUMP_RETRY",
      trans Ready -> dock/On/Seated "battery low",
    },
    submachine aux "sweeps while main drives" {
      state Idle,
      trans * -> Idle,
    },
  },
}
```

**Design rules**, each fixing a defect found by writing examples:

- **Keyword-led statements** (`include` `state` `submachine` `trans` `@`) — dispatch is one token. Identifier-led transitions parse but break skimmability.
- **States directly inside a block belong to an implicit submachine, ordinal 0, unnamed.** `chart` and every `state` block get one; `submachine` is only written when there is a second, or when it needs a name or label. Without this, the common single-region state would need a wrapper line, and the printer would have to decide whether to emit one — so the implicit form is also the canonical one, and printing an explicit sole unnamed submachine is not canonical.
- **`,` separates every list, statements included** — juxtaposed statements are illegible on one line.
- **`=` anchors key to value, `[...]` delimits lists** — variadic values without delimiters are LL(1) and unreadable.
- **Positional string is the label**; everything else goes in the block.
- **`*` is initial or terminal by position** (source or target). Bare, not `[*]`, keeping `[` for lists.
- **A kind is a bare word in both `state` and `trans`.** `state PreConfig choice`, not `state PreConfig kind choice` — the name slot is mandatory and first, so a bare ident after it can only be a kind, and the two statements then spell the same concept the same way. This is also why `kind` is not a reserved word.
- **`s`, `m`, `t` are one-letter aliases for `state`, `submachine`, `trans`** — authoring convenience, for typing and for packing a dense chart while drafting. They are recognized only in statement-leading position, so they are *not* reserved and `state s` is a normal state named `s`. **Canonical form always emits the long spelling** (below), so an alias survives until the next `scav fmt` and never appears in a committed file. No alias for `chart` or `include`: once and rarely per document. Drafting a region on one line stays legible:
  ```
  m main { s Idle, s Ready, t * -> Idle, t internal Ready -> Ready "RETRY", }
  ```
- **Newlines carry nothing** — whitespace-insensitive outside strings, whole file legal on one line. Line breaking is the printer's, which is what makes byte-identical output achievable.

Reserved: `chart` `include` `state` `submachine` `trans` `external` `internal` `local`. Everything else is contextual, so a state may be named `choice`, `history`, `as`, `kind`, `s`, `m`, or `t`.

**Strings.** `"..."` takes `\\ \" \n \t \uXXXX`. `"""..."""` is raw with no escapes — which is its purpose, and why it cannot contain `"""`. Indentation is stripped to the closing delimiter's column; a line indented *less* than the closing delimiter is an error, not silently clamped.

**Canonical form.** A model always emits byte-identical text. Seven rules, because each is a place the format can say the same thing twice:

| | Canonical |
|---|---|
| keyword spelling | long form always — `s`/`m`/`t` normalize to `state`/`submachine`/`trans` |
| repeated key vs list | list form whenever count > 1 |
| `@k` vs `@k = "true"` | flag form iff the value is exactly `"true"` |
| `@ns:k` vs `@ns { k }` | block form iff ≥2 keys share the namespace |
| trailing comma | present iff the printer broke the block across lines |
| attribute order | sorted by key bytes; within one repeated key, insertion order |
| line breaking | by a column budget — **a versioned profile field** (§11.15), since it is part of the output contract |

**Structure is never reordered** (§14). Comments carry position (leading, trailing, own-line) on `Statement.comments` (§7), and are the expensive half of the printer.

Two consequences of the rules above, stated because each looks like a defect until it is read as canonical form doing its job. **The `chart` block always breaks**, whatever the budget says: a document is a file, and a one-line file makes every edit a whole-file diff. **Blank lines are the one whitespace the model records**, as `Statement.blank_before` — a bit rather than a count, so a run of them collapses to one, and suppressed wherever it would open or close a block. Source order is a layout hint (§14), and grouping is how an author writes that hint down; a printer that ran the groups together would be discarding it. A blank *after* a comment is `CommentPos::OwnLine` instead, which is why the two spellings around a heading comment each keep their own shape.

**Two more spellings collapse, for the same reason the seven rules exist.** A `"""` raw string prints escaped, since both spellings decode to the same text and canonical means one of them. And `state Foo {}` prints as `state Foo`: an empty block says exactly what leaving it out says. A `submachine` keeps its empty block, the grammar requiring one.

**One printer, always reconstructing.** Stored source bytes (§7) are **not** a printing shortcut: emitting untouched statements verbatim preserves their formatting, so two semantically identical models from differently-formatted files print differently — breaking the canonicity the format hash and merges rest on. Print reconstructs, gofmt-style; a repo is expected canonical (`scav fmt` pre-commit). Source bytes are for diagnostics and source mapping.

**The printer's input is a `ParsedDocument`, not a `Chart`.** Everything the seven rules need lives in the statement stream and only there: `AttrValueKind` separates `@k` from `@k = "true"` before lowering collapses both to `"true"`, `AttrStmt.ns` records the block spelling, and `PathSeg` holds the endpoint text an author wrote. A `Chart` holds *resolved* `StateId` endpoints, so printing from one reprints `trans Ready -> dock/On/Seated` as `trans On:main/Ready -> dock/On/Seated` — canonical enough, but different bytes, and nothing above says which spelling wins.

Model-to-text is therefore **P12's**, with the editor that first needs it, and it owes two things this printer does not: an **eighth canonical rule fixing endpoint spelling** — root-absolute, the only spelling that is a pure function of the model — and a dedup pass, since a document included twice has one statement stream and two sets of entity rows. Both are cheap to add and expensive to guess at now, and `fmt` needs neither.

Also required: text normalized on read (§6).

**Cost.** Lexer ~400 LOC including `"""` handling and comment capture, parser ~500, comment-preserving printer 3,000–5,000. The printer is the expensive half and a simpler grammar barely helps it.

**JSON survives as an output-only projection** (`scav dump --json`, §3.2) for programmatic consumers. Mechanical over columnar data, and not a format: it has no comments, so §15's trivia cannot round-trip, and it has no canonical form, so two encoders disagree on byte output.

A program that returns the graph (Lua etc.) is not the on-disk format: not diffable, no round-trip, reading it requires executing it, and a program can fail to terminate. The generative case is the C ABI plus bindings.

## 16. C ABI

Flat `extern "C"`, opaque handles, POD structs, out-params, error enums, `scav_abi_version()`.

```c
typedef int32_t  scav_result;                // 0 = ok; negative = error enum
typedef uint32_t scav_column_id;
typedef uint32_t scav_router_id;
typedef struct { uint32_t off, len; } scav_span;   // StrRef and Span both
typedef struct { int32_t w, h; } scav_extent;
typedef struct { int32_t x, y; } scav_point;
typedef struct { int32_t x, y, w, h; } scav_rect;   // also the Placed type (§8.1)
typedef scav_rect scav_placed;
```

**"ABI" names the property, not a component.** The component is each library's C API — `src/<lib>/c_api.cpp` against `src/<lib>/include/scav/scav_<lib>_c.h` — every library's headers install into one `include/scav/`, so the C header carries the library's name — and the ABI is what that surface guarantees: calling convention, struct layout, the extracted JSON. Every library projects its own C API at its own root, and the shared object links them; there is no directory that owns "the ABI".

**A slice of this lands with P2, ahead of the rest.** §17's P2 gate requires the loader driven from Python over ctypes, so `scav_load_*`, `scav_chart_destroy`, and enough of a chart to compare two — counts, the structural hash, the digest under the out-param protocol — ship then, along with the one shared object a binding can actually load. The reason is not schedule: if driving a no-callback loader from a foreign runtime were awkward, §16.1's central claim would be wrong, and that is worth learning before four more phases are built on it. The lifecycle rules below bind from **P3**, though only two of the five handles exist to obey them there. Column access lands with the geometry columns a binding must read (**P4**), and the extracted ABI JSON with the surface it describes (**P5c**) — generating bindings against a surface four phases from complete means generating them four more times.

**Handles: five, each with a create and a destroy.** `scav_chart` (the model), `scav_load` (a multi-document loader, §16.2), `scav_metrics` (font tables), `scav_images` (the raster registry a backend reads), and `scav_drawlist` — which exists because `DrawList` is five `std::` containers (§12) and §16.1 requires the reference builder and SVG backend to be reachable from a binding. Its arrays are read out with the same span accessors as a column. Destroy is idempotent on `NULL`; a `scav_chart` outlives every `scav_span` handed out from it, and nothing else owns model memory. `scav_metrics_create(const scav_byte* ttf, uint32_t len, scav_metrics** out)` — the bundled font is embedded, so `NULL` selects it. `scav_metrics` is immutable after create, so it is shared across threads without locking; the other three are single-threaded-per-instance, and any number of instances may be used concurrently. There is no library-global state and no init call.

**What the chart handle exposes today, and what it owes.** Built: counts, the structural hash and digest, diagnostics, the three-call column accessor, `scav_str`, layout, and everything in `libscavdraw` and `libscavsvg` a binding needs to run the *reference* pipeline end to end. **[OWED]**, and the largest gap against §16.1's "extending scav means writing an application": a binding cannot read one entity row. No state name, no transition endpoint, no attribute, no path resolution, no validation, no builder, no column registration. A Python app can therefore run scav's builder but cannot write its own, and cannot construct a model except by loading text. The shape decided for closing it:

- **Entity arrays as read-only columns.** `State`, `Submachine`, `Transition`, `Attr`, and `Include` are already flat records of `uint32_t` with no padding, so they are exposed through the *existing* three-call accessor under reserved names (`scav.model.state`, `scav.model.submachine`, `scav.model.transition`, `scav.model.attr`, `scav.model.include`), stride equal to the record size, plus the two id arrays `scav.model.state_ids` and `scav.model.submachine_ids`. No new accessor, no per-field getters, and the ABI JSON already describes a column read. The chart's own `name`, `label`, `root_submachine`, and `chart_attrs` come back through one small `scav_chart_header` POD.
- **The builder, projected.** `scav_chart_create`, `scav_build_chart`, `scav_build_state`, `scav_build_submachine`, `scav_build_trans`, `scav_build_attr`, `scav_build_include`: one C function per §7's builder function, returning the ordinal or `UINT32_MAX`.
- **`scav_chart_validate`**, reporting through the handle's diagnostics like layout does.
- **`scav_column_register`** and **`scav_column_data_mut`**, so a plugin written in a binding can own a column; **`scav_attr_find`** and **`scav_attr_key`**; **`scav_resolve_path`**.

Each is a projection of a function that exists in C++ today; none changes the model. They land together, because a binding that can read rows but not write them is a viewer and §16.1 promises an application.

**Operations on an existing chart report through the chart handle.** Validation and layout findings land in a diagnostics vector the handle owns, overwritten at each operation's entry and read back with `scav_chart_diag_count` / `scav_chart_diag` — each a flat `scav_diag` of code, subject kind and ordinal, document, and source span, rendered with `scav_diag_message` like any other code. The loader keeps its own diagnostics (§16.2's calls), because a cycle or a missing document leaves no chart to carry them.

**The profile reaches layout inside `scav_layout_opts`**, as a `scav_profile` POD by value plus the `scav_router_id` — not a handle, not a file path, so its bytes hash into the golden (§6) directly. `scav_profile_named(const char*, scav_profile* out)` fills it from a shipped profile; `scav_profile_validate` is called by `scav_layout_run` regardless (§11.15).

**Column access** needs three calls, not one: `scav_column_find(chart, name, scav_column_id* out)`, `scav_column_data(chart, id, const scav_byte** out, uint32_t* stride)`, `scav_column_count(chart, id, uint32_t* out)`. A builder cannot walk a column without the row count.

**Out-param protocol**, uniform: pass `cap = 0` with a non-null `out_count` to query the required count, then call again with a buffer. `cap` too small returns `SCAV_E_CAPACITY` and writes the required count; it never truncates silently.

**Allocation is the system allocator's, and there is no injection hook.** No scav target supplies its own — desktop hosts, wasm, and a ctypes binding all have `malloc` — and injection is not free. Under `-fno-exceptions` an allocator that can fail puts an error path on every `push_back` in core; one that cannot fail is a pointer threaded through every container for no buyer. Honoring it literally would also mean `std::pmr` throughout, which costs `Chart c;` — §7's usable empty chart — since a pmr container needs its resource at construction. `scav_abi_version()` is the escape hatch if a host that needs it ever appears.

**ABI type rule:** every type crossing the boundary is either an opaque handle or a fixed-layout POD whose only variable-length members are `{uint32 off, len}` spans into separately-exposed flat arrays. **No `std::` type ever crosses** — `struct Chart` is C++-internal and reaches the ABI only as `scav_chart*`. Padding is pinned; single-field id structs must not be flattened to ints by the binding generator (golden ABI case).

Strings come out as spans, never `char*`: `scav_str(const scav_chart*, scav_span, const scav_byte** out, uint32_t* len)` — the pool is not NUL-terminated. The error enum is owed; every other ABI obligation is settled above.

Key entry points:

```c
scav_result scav_layout_run(scav_chart*, const scav_spaces*,
                            const scav_layout_opts*,
                            scav_placed* out_placed, uint32_t cap, uint32_t* out_count);
// geometry lands in derived columns, read with the ordinary column accessor:
scav_result scav_column_data(const scav_chart*, scav_column_id,
                             const scav_byte** out, uint32_t* stride);
scav_result scav_measure_text(const scav_metrics*, const scav_byte* utf8_nfc, uint32_t len,
                              int32_t font_size_grid, scav_extent* out);
scav_result scav_image_register(scav_images*, const char* id, const scav_byte*, uint32_t len,
                                int32_t w, int32_t h, const char* mime);
scav_result scav_router_by_name(const scav_byte* name, uint32_t len, scav_router_id* out);
```

```c
typedef struct {                    // the app owns every array; scav only reads
  const scav_box_space*  box_state;   uint32_t n_box_state;    // parallel to states
  const scav_box_space*  box_sub;     uint32_t n_box_sub;      // parallel to submachines
  const scav_path_clear* path_clear;  uint32_t n_path_clear;   // parallel to transitions
  const scav_path_box*   path_box;    uint32_t n_path_box;     // 0..N per transition
} scav_spaces;
typedef struct { scav_profile profile; scav_router_id router; uint32_t threads; } scav_layout_opts;
```

`scav_box_space`, `scav_path_clear`, and `scav_path_box` are the ABI spellings of §8.1's structs, field-for-field. `threads` affects scheduling only (§6). `scav_profile` is the §11.15 field list as a flat POD of `int32_t`.

**No bespoke layout-result type.** Geometry is columns (§11.7a); edge polylines are a `Span` into a points column, already the model's idiom. `Placed[]` stays an out-param only because `PathBox` is 0..N per transition and cannot be a dense per-entity column.

**Routers are exposed by name only.** Function pointers cannot cross: `void* ud` is undescribable in the ABI JSON, routers run on worker threads, and `-fno-exceptions` makes a binding-language exception crossing back UB.

**Machine-readable ABI:** the header is the source of truth; a build-time tool extracts functions, structs, enums, and field offsets to a committed JSON sidecar. Bindings are generated; a golden test asserts extraction matches, so an ABI break is a review diff rather than a downstream segfault.

**The extraction tool is the scraper plus a probe, chosen at P5c.** libclang was the alternative and it lost on the cost §16 already named: provisioning LLVM on six triples that carry a compiler and little else. That cost turned out to be worse than estimated — on darwin the lint gate's clang-tools package *compiles clang from source*, which is what an ABI extractor would have inherited on every row. The scraper needs nothing new, and the probe reports the layout the shipping compiler really produces rather than a second parser's model of one.

**Accepted cost, stated plainly: it cannot answer for a target it cannot execute.** `wasm32-wasi` therefore needs either a runner in the P11 row or a declared fallback, and that is a P11 decision rather than a gap here.

The scraper **fails closed** — a declaration form it does not model is an error, never a silent skip, because skipping one would leave exactly the drift the golden exists to catch. It reads each header the way a C compiler does, with `__cplusplus` undefined, so the `extern "C"` braces and any C++-only section drop out together; a namespace body scraped as if it were ABI is the failure mode that rule prevents. The probe compiles as C++ with the project's own compiler, since that is the toolchain that ships and these are standard-layout PODs; that the headers *also* compile as C is a separate claim, checked by a separate C11 compile of all of them together.

**Padding is recorded, not inferred.** Every struct carries its size, alignment, per-field offsets and total padding, so a struct that grows a hole reads as an ABI break in the diff. Only `scav_spaces` has any — 16 bytes, from four pointer-and-count pairs on LP64 — which is exactly the case an inferring reader would have got wrong.

**No single-field id struct crosses this ABI**, so the flatten-to-int hazard has nothing to bite: `scav_router_id` and `scav_column_id` are `typedef uint32_t` and every id-shaped C++ type (`StateId`, `TransId`) stays C++-internal. The rule stands for whenever one appears.

**One ABI field was renamed for the bindings' sake**: `scav_pending.from` became `from_doc`. `from` is a keyword in Python and several other binding languages, so no generated attribute could name it — a permanent wart in exchange for one rename at the phase that first generates bindings.

Editor commands do not cross the C boundary as objects; that layer's API is opcodes. (Note `virtual Command Inverse()` returning an abstract base by value does not compile — the editor's inverse is a command buffer append.)

### 16.1 Distribution and bindings

**Extending scav means writing an application** (§3), so bindings must cover the whole pipeline — model, format, metrics, space tables, layout, geometry columns, `DrawList`, SVG — not a plugin corner.

**No extension point is a callback** — everything is data in, data out. So a binding is pure marshalling, with no host-language function invoked from a worker thread across an `-fno-exceptions` boundary. That is what makes bindings tractable.

**One redistributable shared library** — `libscav` = core + layout + draw + svg. The static libraries are a build-time decomposition; the distribution unit is one shared object. `libscavimgui` stays out of it: it needs an ImGui context, which only the host application has. The batteries are everything except the interactive viewer, so the reference builder and SVG backend must be reachable through the C ABI rather than being C++-only conveniences.

- **Generated, not hand-written.** ABI JSON (§16) → generated low-level layer, plus a thin hand-written idiomatic wrapper per language. The generated half never drifts.
- **Prebuilt binaries**: macOS arm64/x86_64, Linux x86_64/aarch64 (manylinux), Windows x64, plus wasm. No compiler required to `pip install`.
- **Self-contained**, because there are no runtime dependencies. The bundled font is **embedded in the library**, not loaded from a path — it is a layout-hash input and must travel with the code.

**One hazard:** Python makes §8.1's integer purity easy to violate (`/` yields float), so setters reject non-integers and range-check, and space-computation helpers live in the shared library. Handle lifecycle was the other, and §16 now specifies it; the rules bind from **P3**, and each handle inherits them as it lands.

### 16.2 Loading and parsing are separate systems

**Parsing takes a byte span. Acquiring those bytes is a different system.** Core may ship a helper that does both — and it does — but **no API forces a caller through a filesystem**, and no entry point that needs bytes will only accept a path. That is the invariant, not an abstinence from `fopen`: a browser host, a binding, a zip reader, and an editor holding unsaved buffers must all be first-class, and fusing the two systems is what would demote them.

The two are separable in both directions. Parse bytes you got anywhere; drive the loader without parsing anything yet.

Include resolution is therefore **iterative and data-driven, not a callback** — a loader accumulating documents and reporting what it still needs:

```c
scav_result scav_load_begin(scav_load** out);
scav_result scav_load_add(scav_load*, const scav_byte*, uint32_t len, const char* name);
scav_result scav_load_pending(const scav_load*, const scav_pending** out, uint32_t* n);
scav_result scav_load_finish(scav_load*, scav_chart** out);
void        scav_load_destroy(scav_load*);

// batteries, in core, written against the calls above and skippable in full.
// [OWED] as C: today they are the C++ read_file and load_file, and the CLI is their caller.
scav_result scav_read_file(const char* path, const scav_byte** out, uint32_t* len);
scav_result scav_load_file(const char* path, scav_chart** out);

typedef uint32_t scav_doc_id;              // ABI spellings of DocId / InstId / StmtId (§7)
typedef uint32_t scav_inst_id;
typedef uint32_t scav_stmt_id;
struct scav_pending {                      // 16 bytes, no padding
  scav_span   path;                        // into the loader's own byte pool
  scav_doc_id from;                        // the document whose include statement claimed it
  uint32_t    stmt_row;                    // that statement's row within `from`
};
scav_result scav_load_path(const scav_load*, scav_span, const scav_byte** out, uint32_t* len);
```

`add` the root, read `pending`, resolve each however you like, `add` each, repeat until empty, `finish`. The app owns fetch policy, caching, and parallelism; cycles and unresolvable paths are core's errors; and `name` makes diagnostics say `wifi.scav:12` rather than `<buffer>:12`. Resolving a `pending` batch concurrently is expected, which is exactly why §9 fixes `DocId` from the include graph rather than from the order documents come back.

Of the block above, the loader calls and `scav_load_path` (the C spelling of `scav_load_bytes`) exist; `scav_read_file` and `scav_load_file` exist in C++ (`read_file`, `load_file`) and are **[OWED]** as C projections. The CLI is their only caller today, and it is C++.

**`from` is a `DocId`, not an `InstId`.** Pending is reported before anything is instantiated — the loader's first walk is over *documents*, and no entity row exists yet to have an `InstId`. It is also the right key: a file included N times is fetched once, so an `InstId` there would mean N pendings for one document and defeat parse-once. `stmt_row` accompanies it so "cannot resolve this path" names a line rather than only a file.

**Two walks, over two graphs, and keeping them apart is the design.** The first is over documents: as each parses, its include paths resolve to keys and an unseen key claims the next `DocId`. That is what `pending` reports, and it needs no entities. The second runs at `finish` and is over *instantiations* — breadth-first from the root, each job creating one document's entity rows under its alias host. Only when every host has its target attached does anything resolve a transition endpoint, which is what lets a path descend through an include.

**Document names are keys, not filesystem queries.** `path_resolve(base, ref)` is pure and byte-wise — no `realpath`, no case folding, no symlink walk — because those answer differently on a filesystem, in a zip, and over HTTP, and the answer decides whether two include statements name one document or two. That is a structural difference in the model, so it may not vary by transport. Names are `/`-separated everywhere and a backslash is an ordinary byte; converting a native path is the caller's job at its own boundary. A ref that is absolute or carries a scheme passes through verbatim, because whether `https://x/a.scav` and `/srv/a.scav` are one file is fetch policy. Two accepted consequences, both stated rather than discovered: on a case-insensitive filesystem `Dock.scav` and `dock.scav` are two documents, and `scheme://x` makes `x` an authority, so a sibling of it lands under it.

**The instantiation walk states its cap.** A DAG is not a cycle and still expands exponentially — N documents each including the next twice is 2^N instantiations from a few KB — so the queue is bounded and overrunning it is a diagnostic, not a hang (§6's fixed-iteration rule).

Works identically over a filesystem, HTTP, a zip, or memory, and preserves §16.1's no-callback property.

**Nothing is hidden, and nothing is mandatory.** `scav_parse` on a byte span and the loader calls above are the primitives, always available and never bypassed internally. `scav_read_file` and `scav_load_file` ship in core, compose those primitives, and are skippable in full — `scav_load_file("root.scav", &chart)` is the one-liner most callers want, and it is implemented in terms of the API it wraps, with no private path. Same layering as the reference builder (§8.1.1): primitives below, batteries on top, and the batteries buy nothing you could not have written yourself. They use `<cstdio>` rather than an `ifstream`, to keep the global stream objects out of every consumer's static-init (§4) — a preference, not a portability constraint.

No stream type: a `.scav` file is kilobytes, so bytes are the simpler composition point. Revisit only if incremental parse becomes an editor-responsiveness requirement.

### 16.3 The path to a browser viewer

Not a v1 deliverable; what matters is that nothing precludes it. Four conditions, all already required for other reasons:

| Condition | Status |
|---|---|
| single-threaded execution produces byte-identical output | **already required** (§6's null shim backend, in the matrix) |
| every entry point accepts bytes, so nothing needs a filesystem | **§16.2** |
| the font is embedded bytes, not a path | **§16.1** |
| the viewer's platform layer is swappable | ImGui's own concern; it ships SDL and GLFW emscripten backends |

The scav-specific part of a viewer is only `DrawList` → draw calls, so a browser viewer is an emscripten build of the *viewer*, not a change below it.

**It may not be the right web front end anyway.** A web app can run core+layout+draw in wasm and render the `DrawList` in JS to SVG DOM or Canvas — beating ImGui-in-canvas on text selection, copy, accessibility, zoom, printing, and bundle size. Two backends is §3's intended shape.

One nuance: a JS emitter is a second implementation the goldens do not cover. So the wasm build exports the `DrawList` **and** the C++ SVG backend — interactive rendering is JS, static SVG comes from the code CI pins.

## 17. Phases

Where a phase states production LOC, multiply by 1.5–2 for the mandated test classes.

**PB — bootstrap.** No scav code, which is why it is lettered rather than numbered: it builds the harness every later phase is measured on. Doing it first means P0's exit gate is a CI result rather than a claim.

- **envy provisions everything**: compilers, cmake, ninja, doctest, clang-format, clang-tidy. Cache at `out/.envy` (§4.2). CI runs on a **bare** runner with nothing preinstalled but a system compiler, because that is the only way provisioning is actually tested — and CI overrides `ENVY_CACHE_ROOT` to a shared path so the cache stays warm across jobs.
- **A toy static library and a doctest executable**, nothing more: `libscavtoy` with one function, one unit test, one golden, one deliberately-failing test held behind a flag to prove failures are actually reported.
- **`build.sh` / `build.bat`**: one command from a clean checkout to a green test run. No arguments required, no environment to set up, no README steps.
- **Tests are build steps, not a second command.** Every test is an `add_custom_command` whose output is a stamp file, wired into `ALL`: building *is* testing, a green build cannot hide a red test, and a second build back to back is a no-op because every stamp is newer than its inputs. CTest is deliberately absent — it has no notion of a test being up to date, so it re-runs the whole suite on every invocation and "build, then test" can never be incremental. ctest's `noTestsAction=error` has a configure-time equivalent that fires earlier and cannot be skipped by forgetting a command.
- **`CMakePresets.json` expresses §6's matrix directly** — that was the argument for CMake over GN, so it gets exercised here rather than asserted. Three configs per triple: `Debug`, `Release`, `testable` (`-DSCAV_TESTING`, §5).
- **Sanitizers as a mutually-exclusive enum**, not booleans: `SCAV_SANITIZER=NONE|ASAN|UBSAN|TSAN|MSAN`. ASan and TSan cannot coexist, so a boolean pair invites an unbuildable combination. **MSan needs an instrumented `libc++`** and is therefore Linux/clang only — build it in PB or MSan silently reports false positives from uninstrumented standard-library code for the life of the project.
- **Warnings are errors**, with the per-compiler set pinned in one place. Cheap now, a week of cleanup later.
- **`install()` + an export config package, verified by a separate consumer project** doing `find_package(scav)` against the installed tree. §4.2 rejected GN specifically because sharing is awful; leaving this untested makes that a preference rather than a finding.
- **Native six only** — the `wasm32-wasi` row lands with P11 (§6).

*Exit:* green on all six triples × three configs; each sanitizer green on every platform supporting it; `build.sh` works on a machine with no scav-specific setup; the consumer project links an installed scav; the deliberately-failing test fails.

**P0 — the language, the lexer, and the parser.** Validate the format before anything depends on it. Recursive-descent parser over §15's grammar, one document, byte span in. Produces the **front-end slice of the model only** — `src_bytes`, `Document`, `Statement`, trivia, and the string pool — because a statement stream is all a parser owes (§7). No entity arrays, no includes, no resolution. NFC normalization (§6) lands here since it happens at parse. Plus the **in-RAM synthetic document generator** (harness-only, §3.2) and **2–3 hand-transcribed real charts** — synthetic input has uniform branching and no accidental structure, so validating on it alone is a trap.

**Recursive descent needs an explicit depth cap** with a diagnostic, not a stack overflow: nesting depth is attacker-controlled and 16 is the *design* target, not a limit the grammar enforces.

**Performance is a P0 test class, not a later concern.** Generate documents in RAM — never on disk, which measures the wrong thing — and assert a throughput floor plus peak-memory-to-input ratio for lex and parse separately. The point is catching accidental `O(n²)`: string-pool growth, per-statement rescans, long comment runs, wide sibling lists. Timing is machine-dependent, so these are floors on a named machine and **not** part of §6's matrix or any golden.
*Exit:* every corpus file parses; every diagnostic locates to a `Statement.src` span; fuzz clean on the lexer and parser; a hostile depth-10,000 document is rejected rather than crashing; throughput floors met at 100 MB in RAM.

**P1 — model spine.** Entity arrays, ids as ordinals with tombstones, spans, extension columns and `ColumnDesc`, append-only builder API, structural validation (§10). Lowering from statements to entities — an `include` statement included, which lowers to its `Include` row and its alias host state with `target` left unresolved, because the host is an ordinary state (§9) and §10's alias-collision check cannot run without it. Determinism discipline (§6) is in force from the first commit; it cannot be retrofitted.
*Exit:* build, validate, and walk a depth-16 / 2k-state chart from code with no text involved; then the same chart via P0's parser, structurally identical.

**P2 — the loader.** The iterative loader (§16.2): pending list, app-supplied bytes, alias resolution (§9 — the host state exists from P1's lowering; P2 fills `Include.target` and attaches the included root submachine), cross-document path resolution, cycle detection. Separate system from the parser, and no callbacks (§16.2). Includes `read_file`/`load_file`, the composing helpers — written against the same public primitives, so they demonstrate the layering rather than shortcutting it. `Include.target`, `InstId`, and every entity row are the loader's; the parser produces `Document`, `Statement`, and `src_bytes` and stops (§7.3).

Three things P2 turned out to own that the phase list did not name. **Lowering splits in four** — attach a file's front-end slice, instantiate its entities, rebuild containment, resolve transitions — because a file is parsed once and instantiated once per include (§9), and because nothing may resolve an endpoint until every alias host has its target. **Path resolution is core's** (§16.2), since a transport-dependent answer changes how many documents the model holds. **The structural digest** arrives here rather than with layout (§6), because the exit gate compares one network three ways and there are no coordinates yet.

*Exit:* one 3-document network resolved **three ways** — from memory, through the CLI over a filesystem, and from Python/ctypes faking a network fetch — yielding the same chart and the same hash.

**P3 — the printer.** The comment-preserving canonical printer over a `ParsedDocument` and the seven canonical rules (§15), plus the CLI surface that falls out of having one: `fmt` and `fmt --check`, `deps`, `check`, and `dump --json`. One large thing and a handful of small ones, and the ratio is the point — §15 budgets the printer at 3,000–5,000 LOC, half again the production code standing after P2, with comments the expensive half of that. Everything else here is already sitting in the model: `deps` is `documents` plus `Include.target`, `check` is `validate_chart` behind an exit code, and `dump --json` is a mechanical projection of columnar data (§15) whose shape is pinned by a golden the first time it runs.

The printer's line-break budget is `print_columns` (§11.15); P3 ships that field and its bound check ahead of the rest of the profile. **Handle lifecycle** — create and destroy per handle, destroy idempotent on `NULL`, a `scav_chart` outliving every span it handed out, no library-global state, no init call, single-threaded per instance with any number of instances concurrent — is **stated and tested here**, against the two handles that exist; §16's other three inherit it as they land. There is no allocator injection (§16) and no ABI JSON (P5c), so the C surface P2 shipped is unchanged by this phase.

*Exit:* `print(parse(bytes))` is idempotent for every corpus document and for a depth-16 / 2k-state document, comments and attribute forms included; the corpus is committed in canonical form and `fmt --check` is green over it; `deps` output feeds a real `ninja` build that rebuilds a diagram when an included document changes.

**P4 — space requests and the layout skeleton.** The space tables and their domain checks, the profile, Phase 0 splitting — including §11.14's source-boundary suppression for `internal` and `local` and the crossing counts that follow from it — trivial placement, straight-line routes, the geometry columns. Validate the coordinate extent estimate (§11.2). The ABI's three-call column accessor plus `scav_str` (§16) land with the columns — geometry *is* columns, so P4 is the first phase where a binding has one to read.

**No metrics and no font.** Layout is font-blind by construction (§3, §11): text reaches it only as integers in the space tables, measured upstream by the app. Metrics' real consumers — the reference builder's measurement pass and the SVG backend's `textLength` — arrive at P5a, and §6's corpus goldens are stated against the reference builder's measurement policy, which cannot exist earlier. Until then the CLI passes all-zero spaces, which is the specified no-request semantics (§11.4) rather than a degenerate mode, and the test harness uses a fabricated integer measurement — a pure function of model and profile, so it digests and goldens like a real one. The extent estimate is validated with deliberately fat fabricated advances, so the grid decision errs conservative; P5a re-asserts it under the real font, and the geometry goldens restate their measurement policy once, there.

*Exit:* geometry columns populated for every chart, no overflow at depth 16; a Python caller reads a geometry column through the accessor.

**P5a — metrics, `DrawList`, the reference builder.** The font metrics helper and the bundled font (§11.9.1; license resolved per §18 before this phase starts, since the font is a layout-hash input), the `scav_metrics` handle, the `DrawList` type, and a builder covering standard appearance — including the measurement pass that becomes §6's stated policy for the corpus goldens.
*Exit:* the reference builder emits a pinned `drawlist/` golden for every corpus chart; the extent estimate holds under real metrics.

**P5b — SVG backend and the baseline harness.** The SVG backend with integer body and single `viewBox`, `textLength`, per-element classes, `scav render`, `scav_images`, the golden harness, and the PlantUML/elkjs/scav side-by-side (§11.12).
*Exit:* `scav render` produces a readable diagram; baseline harness runs.

**P5c — ABI JSON and generated bindings.** `scav_drawlist` completes §16's five handles — and with the surface finally whole, **ABI JSON extraction, its golden, and the generated binding layer** (§16.1). Held to here on purpose: the JSON describes a surface, and generating against one still moving means generating it again per phase.
*Exit:* extracted ABI JSON matches its golden, and a generated Python layer drives model, layout, `DrawList`, and SVG end to end.

**P6 — real layout.** Layered rank, median ordering (sifting deferred as a lever, §11.3), Brandes & Köpf coordinates, bottom-up sizing (fixed pass count, no hysteresis), LR-rectpacking with `box` fallback. Everything P4 stood in for gets replaced: document-order row wrapping by real ranks and real packing, dominant-axis port midpoints by ports ordered as nodes in their own frame (§11.3). Straight-line routes survive to P7 unchanged — this phase moves nodes, not edges.

**Read the Brandes & Köpf erratum (arXiv:2008.01252) before implementing it, not after.** The GD 2001 paper's algorithm is wrong as published; this is the single most likely way this phase ships a subtle defect that goldens happily pin.

**Four things this phase owes beyond the algorithms.** The phases stop being file-local functions in one `layout.cpp` and become a translation unit each with its intermediate in a header (§11), because P4's three stages are today reachable only by running the whole pipeline and P6 quadruples what needs isolating. `Cost` arrives here rather than with search (P9), since the exit gate is a comparison of cost vectors and cannot be stated without one — the **surrogate** and its ranking test stay with P9, which is the only thing that consumes a surrogate. The profile gains `rank_sep`, `node_sep`, and `sub_sep` (§11.15) and so bumps `profile_version`, which rebases every geometry, `DrawList`, and SVG golden — take that churn here, in the phase that was going to move every coordinate anyway, rather than dribbling it across P6a..d. And the ordering stage gets a performance floor at the **flat 2k-state** shape, not only the nested one (§11.3).

*Exit:* better than P4 on the six Tier-2 terms straight-line geometry and no space requests leave nonzero (§11.6) **and no worse than the incumbent** on blind review of the corpus. The second half was misplaced: it belongs to P9 and has moved there (§11.12).

**Measured, and neither clause came back clean.** The corpus is scored term by term into a committed golden, and the same scorer was built against the P4 tree so the two are on one scale:

| | P4 | P6 |
|---|---|---|
| Tier 0, edges through a box | 383 | **186** |
| geometric crossings | 251 | 282 |
| bends | 48 | 129 |
| aspect deviation | 630,976 | **455,408** |
| bounding-box area | 5.63e8 | 1.30e9 |

**Tier 0 is better on ten charts of eleven and worse on none**, and Tier 0 is the tier compared first, so on `Cost` as defined P6 wins outright. **Tier 2 is worse on every chart**, and the reason is that the Tier-2 sum is `w_area * area` to within a rounding error (§19), so "the Tier-2 vector" is an area comparison — and area is what a packer optimises and what layering spends. Aspect — the one Tier-2 term whose scale is comparable to the others — improves. The Tier-2 half of the gate is therefore not met, and the two things that would meet it are P9's weight calibration and §11.4's unspent compaction, neither of which belongs to this phase.

**The other half was unscoreable, not merely unscored.** `straight` has no obstacle set, so Tier 0 is nonzero on all eleven charts — 79 on `mill`, 46 on `bottler`, 1 even on `led` — where both incumbents are at zero everywhere. There is nothing for a reviewer to weigh (§11.12), so the review moves to P7. 383 → 186 is a P4-to-P6 measurement and never an incumbent comparison: the property being compared is that the count is *zero*.

**P7 — orthogonal routing.** Router behind its own boundary, separated OVG, A* with bend state, obstacles including submachines and placed boxes, LCA-owned separator channels, combinatorial nudging with integer offsets, `PathBox` strip placement, bench harness over ≥2 routers. It split in five: four planned, and a fifth for what the first four measured and left owed.

**P7a — the boundary, and nothing through it.** `RouteInput`/`RouteOutput`, the `Router` base class, the registry rebuilt on it, and phase 3 restructured into one net per segment routed in that segment's frame. `straight` moves behind the boundary and produces the same points, so *every geometry, cost, `DrawList` and SVG golden holds unchanged* — the whole exit criterion.

**P7b — the orthogonal router.** An orthogonal visibility graph per frame, separated into h-plane and v-plane copies joined by an edge whose weight is the bend penalty, and A* over that with the total tie-break key `(f, g, node)`. It becomes registry index 0, so it is what a caller with no opinion gets.

*Exit, met:* **Tier 0 is zero on all eleven corpus charts**, asserted by a test that rewrites the predicate rather than asking the scorer whether it is happy. That is §11.12's precondition; the scored comparison itself waits on P9 (§11.12).

| | P6 | P7b |
|---|---|---|
| Tier 0, edges through a box | 186 | **0** |
| geometric crossings | 282 | **63** |
| excess length | 2,367,494 | **618,899** |
| bends | 129 | 684 |
| aspect deviation | 455,408 | **305,920** |
| bounding-box area | 1.30e9 | **1.14e9** |

Bends up and everything else down is the trade an orthogonal router is: it buys the forbidden tier and the crossings with turns.

**Aspect and area moved, and the router did not move them.** The phase was scoped to move edges and not nodes, and three sizing changes rode along with it: a cut rank run's chunks go through the packer instead of stacking vertically, a bare pseudostate takes no padding, and a `Choice` reserves twice its label extent so the text fits the inscribed diamond. Every coordinate in the corpus moved. Credit the 33% and the 12% to those, not to routing.

**Every measurement in these phase tables is scored with no space requests** — §11.6's terms over `readable()` and an empty `scav_spaces`, which is what `golden/layout/corpus_cost.txt` holds and what makes the P4, P6 and P7b columns one scale. Rendered-quality numbers below are the other scale: real text through `measure_chart`, which is what `scav render`, `dump --layout` and `tools/audit.py` produce. The two are close in route count and far apart in coordinates, so a number from one never checks a claim from the other. Say which scale a number is on, every time.

On the table's scale the corpus routes 257 transitions over **893 segments**, every one of them axis-aligned, and **68%** of routes turn twice or fewer — 587 turns after §11.5's attachment-face rule, against the 684 in the table's `bends` row at P7b. Under real text: 917 segments, 613 turns, 71%.

**Three things measured that the phase list did not name.** The **bend penalty is one rank separation**, not §11.6's exchange rate — that is sixteen grid units on both profiles and buys a staircase wherever the grid offers one. A profile field for it is P9's, so no `profile_version` bump and the golden rebase is the router's alone. **The grid is the product of two line sets, not a function of box count**, so the flat 2k chart needs no sparse graph after all: a packed grid shares columns and rows. What exceeds the budget is boxes at *distinct* offsets, which the router's suite builds deliberately. And routing costs what it costs: the nested 2k went 8 ms to **47 ms**, the flat one to 215 ms, both floors raised to match.

**The separator port is the one shape P7b does not route properly.** It sits on a submachine rect and so inside that submachine's owner, and §11.5 gives the segment to the *parent* frame, where the owner is an obstacle walling off its own port. P7b stubs from the port out to the owner's border and the stub crosses whatever lies between; §11.14 excuses the owner and nothing else. The corpus paid nothing and a synthetic 2k chart of eight depth-16 chains scored **496** violations from these stubs. **The LCA-owned separator channel was to be the fix and turned out not to be needed:** the stub crossed the owner because it left through whichever face sat nearest its target, and §11.5's attachment-face rule sends it out of the flow-facing face instead, which crosses nothing. The count is **0**, asserted as zero rather than as a ceiling. Channels are still §11.5's design for *sharing* a separator corridor; nothing in the tree now demands them for Tier 0.

**P7c — nudging. Landed, and closed.** The phase was three coupled subsystems and is one. **Nudging is built** (§11.5): lanes detected per frame, members ordered by the side they arrive from, integer offsets taken only where the room is known good. **Separator channels are unmotivated** — they were the fix for 496 Tier-0 violations on a synthetic depth-16 shape and the attachment-face rule took those to zero (§11.5). **History-based congestion is unmotivated** — nothing measured demands it, and it is the expensive, sequential, iteration-count-tuned part. Both stay in §11.5 as design; neither is scheduled. The attachment-face rule this phase was going to owe landed early, with P7b's bar transposition that exposed it (§11.5). `w_par` gets its first nonzero multiplicand here: a corridor is a channel the current graph does not have, so `corridor` is identically zero on every scale until this lands (§11.6).

**Two routes sharing a run was what that zero cost.** Nothing separated two edges reaching the same lane, so they drew as one polyline fanning out at its ends — **240 pairs over 368,902 grid units** under real text, invisible to `Cost` because `corridor` was the term for exactly this and was zero on every chart. Nudging fixed and priced it in one move: **136 pairs over 86,310 units**, and `corridor` carries a real number for the first time. Of that residual, 1,152 units are lanes nudging made rather than found, down from 11,909 before a displacement was made to keep only the shared runs its legs already had.
*Exit, met:* the corridor term nonzero and then driven down — 355,116 to 93,066 on the scale these tables use. Tier 0 stays zero on the corpus and on both synthetic 2k shapes, every route keeps its arrowhead, no leg collapsed, and no route doubles back over itself. The price of that last one is legs a displacement shortened to nothing much: 11 of the corpus's 917 rendered segments are under two stroke widths against 3 before, four of them a single grid unit, which is a lane still drawn as one lane and counted as two.

**P7c addendum — the routes that already ran as one.** Nudging spread every member of a lane, including the members that were one line by construction: several transitions into one state converge onto a shared trunk, and the stage took that trunk apart into parallel lanes a gap each side — four of them into `bottler`'s `Fault` — while `corridor` charged every unit of it. Both are now keyed on the same fact. §11.6 exempts a pair's trunk — the segments in their common suffix or prefix, plus the two legs into it where those lie along one line — and §11.5 bundles the lane members those runs belong to, so a bundle takes one offset and moves or stays whole. Nothing was added that pulls a route towards a trunk (§11.13): the router is untouched, and total polyline length over the corpus moves **1,123,695 → 1,124,511** grid units, +0.07%, all of it legs a displacement lengthened or shortened.

Three configurations, over the corpus under real text — `scav render` read back by `tools/audit.py`, and `corpus_cost_measured.txt` beside it. The audit's shared run and the scorer's `corridor` are the same number on every row, computed twice from opposite ends; charging the trunks too would add the trunk column back.

| | shared run, pairs/units | merged trunks, pairs/units | `corridor` | `crossings` | `bends` | `excess_len` | `label_near` | U-turns |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| nudging off | 154 / 261,468 | 86 / 107,434 | 261,468 | 67 | 613 | 914,921 | 42,707 | 0 |
| nudging, as it landed | 92 / 53,143 | 44 / 33,167 | 53,143 | 125 | 613 | 2,801,993 | 42,934 | 0 |
| with bundles | **80 / 49,199** | **90 / 107,818** | **49,199** | 124 | 613 | 2,784,161 | 43,082 | 0 |

On the no-space-requests scale the tables above use, `corridor` **93,066 → 62,285**, of which 18,800 is the exemption and the rest the geometry; `crossings` **128 → 104** and `excess_len` **1,775,868 → 1,611,523**, the second following the first; `bends` 587 either way, aspect and area untouched, `t2` 1.15488e9 → 1.15275e9. **Keeping a trunk together removes crossings rather than adding them** — the lanes the old spread cut across were its own. Tier 0 stays zero on both scales, no route doubles back and none collapsed, and the corpus's flush segments stay at 4 of 917 rendered and at the pinned 2 with no space requests: a bundle takes a whole step where its members took a fraction of one, which walked `mill`'s trunks onto their frame's own border until the room stopped one unit inside it (§11.5).

Nudging finds **30 bundles over the corpus and refuses none of them** as rendered — `axis` 3, `bottler` 4, `dock` 1, `mill` 13, `ota` 2, `tcp` 2, `toolchanger` 4, `vac` 1, and none in `brew`, `estop` or `led`. A refusal is a member whose own checks fail, which the unit tests build and the corpus does not produce.

**P7d — labels, inflation, and the bench. Landed.** `PathBox` strip placement by Kakoulis & Tollis matching is built (§11.9) and called from phase 3 once the routes are final and nudged. **The degenerate-enclosure inflation landed** (§11.6): `layout_run` widens all three separations by `spacing_inflation_increment` and re-runs phases 1–3 up to `spacing_inflation_cap` times when a net comes back unreachable, keeping the attempt that degraded least; what is left is written as a straight line and marked `RouteDegraded` under `SCAV_OK`. The corpus and both 2k shapes inflate zero times, and the synthetic that does not takes three — and that synthetic is not a sealed channel but a frame's region reaching past its owner's border onto an adjacent-rank sibling when `rank_sep` is under the router's clearance, the same margin strip §11.5's nudging had to be walled out of. **The bench landed**: `src/layout/bench_tests.cpp` runs every router the registry holds over the corpus at the readable profile with no space requests, commits the scored table to `golden/layout/corpus_routers.txt`, reports each router's wall clock over the corpus and both 2k shapes as a message under per-router floors, and checks the `RouteOutput` end-to-end join against `straight` — so a third router joins the bench by being registered and nothing else.

**The predicate was wrong before the placement was.** `label` charged a placed box against every live state, the composite its own transition runs inside included, so a label could not score zero wherever it sat — and `tools/audit.py` asked the same question the same way. Under real text that read **155 of 203 labels over a state box**; with the carve-out §11.14 already gives an edge, and with the reserved band of a routeless transition's source excused because that band was reserved for exactly that label, the honest count at the same commit is **32 of 203**. The other 123 were labels sitting inside their own composite, which is where they belong. Two counts were missing beside it: a label over **another transition's route, 77 of 203**, and the corpus's **47 of 403 overprinting strings** (52 before P7c's nudging moved them).

**What the placement bought, on the rendered scale.** Labels over a state box **32 → 9 of 203**, over another transition's route **77 → 9**, overprinting strings **47 → 9 of 403**. `bottler` went 7/12/9 to zero on all three, `toolchanger` 4/7/3 to zero, `mill` 10/32/19 to 3/0/0. What is left is `estop` and `led` — three labels each, on charts small enough that the label is wider than every gap the chart has — plus single boxes on five others.

**The cost golden has its second row**, `golden/layout/corpus_cost_measured.txt`: §11.6's terms over the corpus scored from the geometry columns with the reference builder's measurement and the placed boxes, which is the scale `label` is nonzero on. Placement moves no route and no box, so seven of the eight Tier-2 terms are identical either side of it and the comparison is one column: **`label` 429 → 45**, `t2` 1,480,030,752 → 1,480,021,536. It is not zero because **14 of the corpus's 192 path boxes found no feasible strip** and kept the centred placement; those 14 are what the residual counts above are made of.

**Rip-up was measured rather than built.** The fallback rate decided it: five strips either side of a leg took it from 47 of 192 to 14, and each strip beyond the second bought more than a reroute plausibly would. Re-routing one edge with the placed boxes as obstacles is the only part of §11.9 that has to re-enter the router, it is capped by `ripup_cap` for a reason, and 7% of boxes on charts whose labels are wider than their gaps is not the evidence to spend that on. It stays design in §11.9.
*Exit, met on the term placement moves.* The exit named all eight Tier-2 terms under real text, and there was no real-text row to compare against until this phase wrote one — so it is stated against the pre-placement row of the same golden, where seven terms are unchanged by construction and `label` falls by 90%. Tier 0 stays zero, no coordinate moved, and the drawlist keeps its primitive count on every chart.

**P7d addendum — the label a reader ties to the wrong line.** Nothing priced a box sitting nearer some other transition's route than its own, and nothing in placement preferred the side of its own leg that kept strangers away; PlantUML's `brew button` between its own arc and the neighbouring `shot done` arrow is the shape of it. §11.6 gains a ninth Tier-2 term, `label_near`, and §11.9's key minimises it ahead of every other component — `w_label_near` at `w_corridor`'s value, `profile_version` 4, and a rebase of every digest that hashes the profile. **The term landed one commit before the placement did**, so the real-text golden holds both ends: `label_near` **62,689 → 42,934** grid units over the corpus, `t2` 1,483,030,608 → 1,482,082,344, `label` 45 → 44, and the other seven Tier-2 terms bit-identical, because placement moves no route and no box. Tier 0 stays zero and the centred fallbacks stay at 14 of 192. `tools/audit.py` asks the same question on the rendered scale — a label not one line of its own text nearer its own route than any other — and reads **129 → 104 of 203**, of which the sharp case, strictly nearer somebody else's line, is **62 → 37**. Overprinting strings went 47 → 9 → **8** of 403 and no other count moved on any chart. **What is left is structural.** Of the 104, thirty-four sit equidistant from both routes, which is what a shared lane or a crossing produces and no strip can fix — §11.5's residual is 136 shared-run pairs — thirty-three are nearer their own by less than a full line, and the remaining thirty-seven are the sharp case above. Placing 2k boxes went **16 ms to 66 ms** against its 200 ms floor, the key's first component being a distance query per candidate; dropping the out-of-reach segments once per strip band rather than once per candidate is what keeps it there.

**P7e — the combinatorial stage, the attachment, and the element suite. Landed.** What P7c and P7d each left owed, and the suite that says which element owns the next one.

**The combinatorial ordering stage** (§11.5) replaces the low-end projection nudging ordered lanes by. A leg leaving the lane strictly inside another member's extent crosses that member's segment unless the member lies on the leg's far side, so each incidence votes; the lane's order is **a linear extension of the votes**, Kahn's algorithm over their digraph taking the lowest key of the bundles nothing left precedes, and lanes whose votes run round a cycle keep the fewest-contradictions insertion. No space requests: crossings **104 → 85**, excess **1,611,523 → 956,258**. Real text: crossings **124 → 92**, excess 2,784,161 → 1,549,724. Bends 587 and 613, **unchanged on both scales** — the property the stage was specified for. The pseudo-direction pass it was first written with turned out unnecessary, and the case where the crossing order is not a known-good displacement turned out to exist and now has a test. **Insertion one at a time was the linear extension only where the votes are total**, which three staggered extents are not; taking the extension instead reads corridor **37,018 → 36,973** and excess 961,229 → 960,779 over the corpus, crossings and bends unchanged, and the chain that separates the two orders is a nudging fixture rather than a chart.

**The attachment moved off the box centre** (§11.5). `ortho_attach_box` seats an end at its target's projection onto the face the escape rule chose, held one clearance off the corners; an inscribed glyph keeps the face midpoint, `RouteInput` carrying that per obstacle; and `ortho_spread_attachments` separates an arrival from a departure that still want one seat while leaving a fan-in or a fan-out whole. **This was P7c's "spread the ends out" and it is not nudging's**: sliding an end along its face is a move on an anchored segment, and the shape it exists to fix — a route leaving straight out — cannot slide without a manufactured bend. Chosen before the search it costs nothing. Bends **587 → 563**, corridor **62,061 → 37,018**; under real text arrivals meeting a departure on one point **116 → 16**, route segments 917 → 868, labels over another route 9 → 2, centred label fallbacks 14 → **12** of 192.

**Over the whole of P7e**, against the phase's start: no space requests, bends 587 → **563**, corridor 62,285 → **36,973**, crossings 104 → **102**, excess 1,611,523 → **960,779**, `t2` 1.152747e9 → 1.148927e9, aspect and area untouched. Real text: bends 613 → **563**, corridor 49,199 → **30,410**, crossings 124 → **98**, excess 2,784,161 → **1,567,636**, `label` 42 → **26**. Tier 0 stays zero on the corpus and both 2k shapes.

**The element suite** (§5) is the durable half. Nine charts under `test_data/charts/gauntlet/`, one shape each, held to reader-visible properties at both shipped profiles — Tier 0, axis alignment, no leg reversed, an end on an inscribed glyph at a face midpoint, an endpoint that is also a crossing met at one point rather than two, no arrowhead inked over another route's end, a fork's bar used along its long faces, a fan-in with none of its arrivals hidden inside another. The directory and the array the suite iterates are held to the same list by `functional_tests/test_gauntlet.py`, which also puts every chart through `fmt --check`, `check` and `render`, and `tools/baseline.py --gauntlet` and `tools/audit.py --gauntlet` read the same charts on the rendered scale. It paid for itself on the first run: **§11.8's own case does not route** (two edges through a box, two routes doubling back, and no corpus chart carrying the shape at all), a mark with more than two incident transitions doubles a seat because only left and right port sides exist (§11.3), and a two-state cycle sends one route the long way round the frame at the compact profile. Reviewing it found a fourth: one fork branch leaves through the bar's own 64-unit cap while the 960-unit face beside it goes unused, which is §11.5's face rule reading a dominant y separation exactly as specified. **Every shape a property carves out carries its count and its owning section beside it**, so the next change to one of them is a number that moved; the cycle's shared run excuses no property and is reported as a measurement instead.
*Exit, met:* the two stages P7 named and did not build are built, every corpus golden rebased, and the element suite green with every carve-out in it counted rather than excused.

**P8 — determinism infrastructure. [OWED]** Thread shim, model-derived sharding, counter-based RNG, index-ordered reduction, tiered matrix, sanitizer configs, scheduling-delay injector, and **`scav selftest`** — the command that makes §6's compiler-independence claim checkable by a user rather than only by our CI. Today `threads` is accepted and ignored, every phase runs inline, and the six-triple matrix is CI's alone. The standard-library-subset include check is **not** this phase's and never was: it landed with the directory it guards and runs as `functional_tests/test_include_subset.py`, which is what §6 means by enforced.
*Exit:* one structural hash and one coordinate hash across the blocking matrix; full grid green nightly. The `wasm32-wasi` row lands with P11 — until then the matrix is the six native triples, and §6's discipline is what makes adding the row a build change rather than a redesign.

**P9 — search and calibration.** Portfolio, local search, surrogate with ranking test, weight calibration, versioned profiles.
*Exit:* full-layout latency at 2k states measured and published (§11.11's bet); on the corpus, a one-state edit produces a visually small diagram change **in the common case** — §11.11's honest limit means a cascade is a hint's job, not a gate failure; **and no worse than the incumbent on the corpus side by side** (§11.12), the clause P6 and P7 each carried and neither could be judged on, because a render with no search behind it is one candidate rather than a layout.

**P10 — `scavview`.** `libscavimgui`, pan, zoom, linear-scan hit test, hover/select, live highlighting (§13), relayout on request, and the Lua host (§8.3) with its sandbox and determinism obligations. Metrics-parity golden against the builder.
*Exit:* navigate 2k states smoothly; drive highlighting from an external process with no relayout.

**P11 — browser.** Core, layout, draw, and SVG to `wasm32-wasi`, single-threaded on the null shim. A JS host reads the `DrawList` and renders it, or calls the wasm SVG backend.
*Exit:* same chart in a browser, hashes identical to native.

**P12 — editor.** In-place mutation, undo/redo. **[OPEN]** arena snapshot vs command buffer with inverse.

**Plugin work is not a phase.** Column registration and the builder API land in P1; space requests need P4; builder contributions need P5a. `scav-scxml` should be built incrementally alongside, because it is the acceptance test for the extension boundary (§8.2) — deferring it means discovering the boundary is wrong after everything is built against it.

**Out of scope for this document, but must not be precluded:** `.puml` importer; libhsm codegen backend; PDF; layout hints beyond the minimum.

**Importer compatibility is a standing constraint, not a future feature.** Nothing in the design may make a `.puml` importer impossible. Verified against libhsm's corpus — everything a `.puml` chart carries is already representable:

| `.puml` construct | Lands in |
|---|---|
| nested states, concurrent submachines (`\|\|`, `--`) | `State.submachines`, §7 |
| `<<choice>>`, history, terminal, initial | `StateKind` |
| `internal` markers, out-of-machine transitions | `TransKind`, §11.14 / Phase 0 |
| state descriptions, transition names | `label`, attributes |
| event lists, `note on X`, handler markers, legacy mode | extension columns and attributes, §8 |
| direction hints (`-u-`, `-d-`) | `scav:` layout hints, §14 |

**No display-name-vs-identifier split is required.** libhsm's `state on_idle as "Idle"` exists to dodge C identifier collisions between same-named states in different submachines; scav addresses by path (`On:main/Idle` vs `On:aux/Idle`), so the collision does not arise. A `.puml` state description lands in `State.label` (§7), which is a *description*, not a second identifier — do not add one. Codegen identifier uniqueness is a `libhsm:ident` attribute owned by that backend.

When written, the importer should be Python against `fi.hsm`'s existing lexer rather than a C++ PlantUML parser — throwaway code, runs once per chart, and `fi.hsm` already encodes the accepted grammar subset including the non-obvious rules (column-0-only comments, `note on X : handler`, legacy mode).

## 18. Licensing

scav is MIT or Apache-2.0. Verified from LICENSE bytes; GitHub's detected field is wrong for all four.

| | License | |
|---|---|---|
| ELK | `EPL-2.0 OR GPL-3.0-or-later` | Java only, no native port |
| Adaptagrams (libavoid, libcola, libvpsc, …) | LGPL-2.1-or-later, uniformly | dynamic link only, or buy Monash's commercial license |
| OGDF | GPL-2.0/3.0 | **blocker** — its exception is outbound-only |
| Graphviz ≥14.1.4 | EPL-2.0, no Secondary License | cleanest, but `dot` is weak on compound graphs |
| the bundled TTF | JetBrains Mono 2.304 Regular, OFL-1.1, verified from `assets/font/OFL.txt` bytes | a layout-hash input, so it is redistributed inside the library (§16.1). `head`/`hhea`/`hmtx`/`maxp` plus cmap formats 4 and 12 verified; upem 1000, 1743 glyphs, 268 KB |

**Read the permissive reimplementations, not Adaptagrams** — Dwyer released the same algorithms twice:

| What | Where | License | Size |
|---|---|---|---|
| Rectilinear routing | MSAGL `Routing/Rectilinear` (C#) / msagljs (TS) | MIT | ~13k |
| Grid routing, small | WebCola `gridrouter.ts` (Dwyer) | MIT | 675 |
| VPSC + overlap removal | WebCola `vpsc.ts` + `rectangle.ts` (Dwyer, author of LGPL libvpsc) | MIT | 1,136 |
| VPSC/QPSC, cluster-aware | MSAGL `ProjectionSolver` + `OverlapRemoval` | MIT | ~7k |
| Sugiyama in C++ | bigno78/drag (no compound support) | BSD-3 | 3,456 |
| Compound layered layout | dagre (JS) | MIT | — |
| Polygon offsetting | Clipper2 | Boost-1.0 | — |

`stb_rect_pack.h` is MIT/public-domain but **skyline packing is not order-preserving**, so it cannot serve §11.4 — do not reach for it.

Two verified negatives, which justify building this: **no maintained permissively-licensed native C/C++ orthogonal router exists**, and **nothing permissive in C++ does compound layout**.

Never vendor: `libnest2d` (LGPL-3.0), OGDF (GPL), Graphviz's `textspan_lut.c` (EPL-2.0 — read the approach, not the table).

## 19. Open questions

**Decisions owed:** §11.8 whether to depict the implicit submachine reset · §9 durable per-element GUIDs for cross-branch rename identity · §17 P12 undo/redo mechanism · §16 whether the entity-row columns are the permanent ABI or a bridge to a generated per-record accessor once the binding generator can emit one.

**Unverified claims, flagged not smoothed:**
- No diagram-routing work found doing history-based negotiated congestion with rip-up-and-reroute. Unconfirmed absence, **not** novelty.
- No published TSM runtime at 1000–2000 nodes; §11.13's rejection rests on a survey statement plus absence of data at our scale.
- No published integer or combinatorial reformulation of VPSC.
- `textLength` support is patchy in non-browser SVG consumers (Inkscape, librsvg, resvg). Test before relying on it.
- Total pairwise rectangle overlap area has no published complexity bound; the `O(n log n)` sweep is our derivation. Union area at `O(n log n)` is published and optimal. Moot while Tier 0 forbids overlap.
- ~~The coordinate extent estimate (§11.2) is derived, not measured.~~ **Settled by measurement**, three times: 1.9x headroom under P4's fabricated advances, 5.2x under P5a's real font, and under P6 a fabricated bound of `min_w` 4768 against P4's 3200 (§11.2). The fabricated case stays asserted as a bisection rather than a fixed number, so the bound is what the test reports instead of what it was written against.
- **The shipped Tier-2 weights make the sum an area measurement.** On the corpus `w_area * area` is within a rounding error of the whole of Tier 2, because area is `10^8` while every other term weighted is `10^3` to `10^6`. §11.6's ordering says area is *lowest*, and that is true of the multiplier and false of the influence. Calibration is P9's, and this is the first datum it has.
