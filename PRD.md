# scav — PRD / ERD

Statechart authoring, layout, rendering. Normative. Terse by intent: read by agents and by humans ramping up.

`[OPEN]` marks unsettled decisions. Everything else is settled; changing it needs new evidence, not a new opinion.

**A living document, and a peer of the source, not its upstream.** The two co-evolve: a design decision lands here first, an implementation finding lands in the code first, and each review reconciles the pair. Where this document describes something the tree does not yet do, it says so with `[OWED]`; where the tree taught the design something, the section records the measurement. A claim here that the tree contradicts is a defect in one of them, and the review decides which.

## 0. Status ledger

Reconciled against the tree on 2026-09-08. Phase names are §17's.

| Built and green | Owed, with an owner section |
|---|---|
| PB bootstrap, P0 language, P1 model spine, P2 loader, P3 printer and CLI, P4 space tables, P5a metrics + `DrawList` + reference builder, P5b SVG + baseline harness, P5c ABI JSON + Python binding, P6 real layout, P7a router boundary, P7b orthogonal router, P7c nudging, P7d label placement, inflation, and the bench, P7e the combinatorial stage, the attachment, and the element suite, P8 the thread shim, the sharded phases, the seeded structural hash, the determinism class, and `selftest`, P9a the docs commit, `crowd.scav`, the hierarchy-descent scorer, Tier 2 in ems, and profile v5, P9b Level 2 of the portfolio, the phase-1 hoist, DAR as a capability, and profile v6, P9c the spot list, whitespace elimination always, and compaction as Level 2's bit 1 | **the incumbent comparison**, rescheduled to P9d's exit because a render measures the search behind it, and at P9b that is one of two levels with the weights still unfitted (§11.12) · **routing between two concurrent submachines of one state** (§11.8), which no corpus chart exercises and the element suite pins · rip-up-and-reroute for the boxes no strip fits (§11.9) · **P9d–P9e calibration and the fine-grained level** (§11.10, §11.4), the order **set 2026-09-08** so the level that chooses between whole charts was built first and the fine-grained one is built last: P9d the fitted weights and the side-by-side · P9e Level 1 and local search, **gated on a measurement taken there** · **P9c's shipped `portfolio_m` of 2** (§11.15), decided on the renders once compaction was a table row and not yet set in the profiles · P10 viewer · P11 wasm · P12 editor |
| | **ABI: entity rows, builder, validate, column registration and mutable column data, attrs, and path resolution from C** (§16) — the three-call *read* accessor is built; registration and mutable data are not · **hints** (§14) · **top/bottom port sides** (§11.3) · **a bulk attribute path** (§7.3) · **`scav_read_file`/`scav_load_file` in C** (§16.2) · **`--strict-attrs`** (§8) · **ImGui backend** (§3.1) |

The bottom-left cell is empty on purpose: everything built is in the top row, and everything owed names the section that owns it.

**Three search fields are validated and read by nothing.** `portfolio_k`, `congestion_iterations` and `ripup_cap` are bounds-checked at load and hashed into the inputs digest (§6), so changing one moves a golden's inputs column without moving a coordinate. `portfolio_m` was the fourth: it landed at P9a on the profile bump the em already forced and went live at P9b with Level 2, shipping at 4 and re-bounded to `[1, 8]`, and **P9c takes the shipped value to 2**, rows 2 and 3 being compaction's and unjudgeable until the weights are fitted (§11.15). `portfolio_k` waits for P9e with Level 1; the other two belong to stages §11.5 and §11.9 hold as design and nothing has scheduled. `sweep_count`, `portfolio_m` and the two spacing-inflation fields are what the tree reads of that group (§11.15).

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
| `selftest` | lay the embedded corpus out on this toolchain at every thread count in §6's matrix and diff all three hashes against the embedded goldens. Charts and goldens are compiled in, so it takes no paths and an installed binary checks the claim from anywhere; `--against FILE` substitutes a golden |

**No `gen`.** Synthetic chart generation is test tooling (PB, P0) and lives in the harness; shipping it as a verb would imply a user need nobody has. Same reasoning retired a separate `layout` verb — dumping geometry is `dump --layout`, not a second command.

### 3.3 Repository layout

Mirrors `~/src/envy`: SHA-pinned deps under `cmake/deps/`, unit tests adjacent to sources, Python functional tests, sanitizer suppressions, presets.

```
include/scav/          the cross-library vocabulary: POD spellings, no functions
src/<lib>/include/scav/  that library's public API: `scav_<lib>.h`, plus `scav_<lib>_c.h`
                       where it projects a C surface. A C header is a second language,
                       not a second place to look for the same symbol
src/scav_*.h           determinism primitives that belong to no subsystem: the vendored
                       stable sort and hash §6 mandates in place of the standard library's,
                       `scav_rnd.h`'s position-addressed randomness, `scav_shard.h`'s
                       shard count and range, and `scav_thread.h` — whose one selected
                       backend, `scav_thread_{pthread,win32,null}.cpp`, is the only .cpp
                       at this level besides the hash's.
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
src/<lib>/tests/       suite-level test classes (functional_*, fuzz_*, perf_*,
                       determinism_*) + shared fixtures; unit tests stay adjacent to
                       their subject as <foo>_tests.cpp
src/layout/            space requests, phases 0-3, routers, cost; shard.h, the one
                       spelling of the shard count both sharded phases use (§6).
                       gauntlet_tests.cpp is the element suite over the charts above,
                       and tests/ holds the determinism class (§5) beside the 2k and
                       sealed-channel fixtures more than one suite builds
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

**The corpus says whether a diagram comes out well; it cannot say which element was wrong.** Every layout defect this project has found was found on a corpus chart and then bisected by hand to one shape — a fork bar, a mark, a pair of states each other's target — because a chart with sixty transitions in it answers about all sixty at once. So there is a third body of input beside the unit tests and the corpus: `test_data/charts/gauntlet/`, one element per chart, small enough that every route in it can be named, and `src/layout/gauntlet_tests.cpp`, which holds each to the properties a reader checks rather than to a hash. A chart earns its place there by isolating a *shape*, and adding one means adding the assertion that would have caught the defect. Where a property does not hold yet the chart is carved out **with a count pinned beside it and the section that owns it named**, because a carve-out with no number on it is an excuse. **And the suite scores what ships.** `lay` runs `layout_run` at the shipped profile, re-runs the phases for the row §11.10's portfolio kept — the nudge statistics and the per-transition fallback flag have no column of their own — and holds all six geometry columns it produced to the ones the run wrote, word for word, so a property here is a property of the picture rather than of a candidate nobody draws. One carve-out closed on that change and every pinned count stayed on the record: `regions.scav` is held to the no-edge-through-a-box property like every other chart now, because what ships routes through nothing at either profile, and the **2** §11.8 owns is pinned against row 0 alone, where it still reads; `fork`'s capped branch reads **0** at `readable` against row 0's 1, and 1 at `compact`, which ships row 0; and `regions`' reversing legs went **2 → 4**, worse rather than better, which is a carve-out earning its keep rather than closing (§17 P9b). **And not every pinned defect is a carved-out property.** `crowd.scav`, the tenth chart, holds every property the suite has at both profiles with no space requests; what it pins instead is a pair of real-text terms in `src/draw/tests/functional_drawlist_tests.cpp` — `label` 4 and `label_near` 442 — because the packing that wins on area is the one whose labels collide, which is §11.6's and P9d's to move (§17 P9a) — and which Level 2 did not move: the tenth chart's pick is row 0 at the shipped M, so both pins hold unchanged (§17 P9b). The same file pins one number about the corpus rather than about an element — the centred label fallbacks, **16 of 192** at the shipped M against 20 at M = 4 — because what a strip has room for moves with the portfolio's pick and not only with §11.9's five strips (§17 P9c). **The directory and both arrays that iterate it are held to the same list** — the suite's and the bench's, the second pinning the same charts' terms and having already drifted a chart behind the first — by a functional test that also runs `fmt --check`, `check` and `render` over every chart in it: a chart nobody asserts anything about and a chart nobody ever renders are the same gap, and both are the kind a hand-maintained list opens.

Required test classes: **unit** (every internal function, doctest) · **functional** (full pipeline over the corpus) · **gauntlet** (one layout element per chart, held to reader-visible properties) · **golden** (canonical serialization, structural hash, coordinate hash, **`DrawList`** — the primary surface, §12 — plus a thin SVG serializer check, ABI JSON) · **property** (round-trip identity, all refs resolve, zero box overlap, zero edge-through-box, surrogate cost ranks like exact cost `[OWED, P9e, gated]`, which §11.6 specifies both halves of — a Kendall τ on `t2` is the test it is not, area being exact in the surrogate and, even after P9a's em cut it to a third of the sum, the largest term in it) · **determinism** (§6) · **sanitizer** (UBSan signed-overflow+shift, ASan, TSan, and MSan on Linux/clang, which is the only place an instrumented `libc++` exists — see PB) · **fuzz** (deserializer and reference resolver — untrusted input) · **binding** (drive the C ABI from Python/ctypes in CI) · **baseline** (§11.12) · **performance** (see below) · **regression** (every fixed bug leaves a test).

**The determinism class is `src/layout/tests/determinism_tests.cpp`**, layout being what §6's matrix makes its claim about: every `scav.geom.*` column's bytes, all three hashes and the portfolio row that produced them (§11.10) over the eleven corpus charts and both 2k synthetic charts at 2, 3, 5, 8, 13 and 16 workers against 1, both 2k charts again under the scheduling-delay injector, the spacing-inflation retry at 8 workers against 1, and a chart every row of the portfolio ties on so the reduction's index order is what answers — 7 cases, 3,091 assertions. **The chosen row is in the snapshot beside the geometry** because a worker count that moved the pick would move every column under it, and a named row says which of the two happened instead of leaving a reflow to bisect. Eight corpus charts shard to one. Both 2k charts shard to 128, but the count comes from every entity and the ranges are cut over submachines, so they cover different paths: the nested chart's 129 submachines put frames on every shard and run them concurrently, while the flat chart's one submachine busies one shard and leaves 127 with nothing to do. Each case asserts which of the two it is, so neither can quietly cover the other's.

**Performance tests assert floors, not times.** Inputs are generated **in RAM** — a disk-backed benchmark measures the filesystem. Each asserts a throughput floor and a peak-memory-to-input ratio, per stage, on a named machine. Their job is catching accidental `O(n²)`, not tracking milliseconds, so they are **not** matrix rows and **not** goldens (§6): timing is not reproducible and must never gate a determinism claim.

Measure branch coverage; an untested file fails the build. No percentage target.

## 6. Determinism contract

**Layout output is a function of the C++ abstract machine, not of any implementation.** That is the claim, stated stronger than a test matrix: *any* conforming C++20 toolchain must produce the same bytes, because the code depends on nothing an implementation is free to choose. **Users bring their own compilers** — the six triples below are *evidence* for the property, not its definition, and a seventh toolchain is expected to agree without anyone having tried it.

Two obligations follow. **No implementation-defined or unspecified behaviour may reach output** — the banned list below is that rule enumerated, and it is closed rather than advisory. And **`scav selftest`** recomputes the inputs digest, the structural hash and the coordinate hash over the corpus at every thread count below and diffs them against the committed goldens, so a user on an untested compiler verifies the claim in one command instead of trusting it. A failure is a scav bug until proven otherwise, and the report says which — a column that moved against the golden is a toolchain finding, a column that moved against `threads = 1` is a sharding defect, and they are separate line classes.

**Canonical matrix**, defined once and referenced everywhere:

```
{ macOS/clang/libc++, Linux/clang/libc++, Linux/clang/libstdc++,
  Linux/gcc/libstdc++, Windows/MSVC/MSVC STL, Windows/clang/MSVC STL }
  x {Debug, Release, testable} x {1,2,3,5,8,13,16} threads
  + wasm32-wasi single-threaded
```

Six realizable platform triples, not a 27-cell cross product — most combinations of that product do not exist. They are chosen to span the axes that historically diverge: three standard libraries, three vendors' codegen, LP64 vs LLP64 vs ILP32, and x86_64 vs arm64 vs wasm32.

Odd and prime thread counts are mandatory — they expose reduction-shape bugs powers of two hide. Tier it: a small blocking subset per PR, full grid nightly and advisory, or a trickle of one-cell failures halts velocity.

**Measured at P8, and nothing is tiered, because nothing is slow enough to tier.** The thread axis is not a CI dimension at all: the determinism class (§5) and `scav selftest` each walk the whole of `{1,2,3,5,8,13,16}` in-process, and both run as build steps in every row, so every `triple × config × threads` cell is covered on every PR. A full run costs about ten minutes wall with sccache — 10:30, 9:49 and 10:30 on recent pushes to `main` — which is not a budget worth splitting. So nothing is advisory and the blocking subset is the whole grid, and what the nightly buys is time rather than breadth: a `schedule:` trigger runs the same grid on `main` daily whether or not anything was pushed, which is what catches drift in the weekly-rebuilt toolchain image.

**The scheduling-delay injector is how the thread axis is more than seven runs of the same interleaving.** Behind `SCAV_TESTING`, `thread_test_delay_seed` makes each worker yield `rnd(seed, 0, shard, 0) % 64` times and spin `rnd(seed, 1, shard, 0) % 4096` iterations before the shard's body, so one shard always waits the same amount and only the order the workers finish in moves; `thread_test_spawn_limit` makes creation past the first N attempts report failure, which is how the fallback below is reached on a machine where spawning never fails. Both are written on the calling thread before any worker exists.

**Rules:**

- **Integer only in the metrics helper and `layout`.** The space tables are `int32_t` by construction. Float appears only in a backend's geometry, and never in emitted text (§12.1).
- Shard count is `clamp(bit_ceil(ceil_div(entity_count, 64)), 1, 256)` — a pure function of the model, never `hardware_concurrency()`, never tunable. Ceiling division, so a shard holds at most 64 entities; floor division would put 65 of them in one. `entity_count` is every row of the three entity arrays, tombstones included, so the count describes the model's shape rather than what a phase decided to skip. `shard_range(shard, shards, items)` then cuts `[0, items)` into contiguous index-ordered ranges no two of which differ in length by more than one. **Phase 1 (per-submachine ordering) and phase 3 (per-frame routing, nudging included) shard over submachines**, each shard writing only its own frames' result slots and its own scratch, with a serial index-ordered pass emitting and merging afterwards exactly as the serial code did; phase 0, phase 2's bottom-up fold, and label placement stay serial. Shards are work items; worker count never affects results, and the null backend runs the same shards inline in index order.
- Randomness is **stateless and position-addressed**: `rnd(seed, phase, item_index, step)` via the splitmix64 finalizer, `constexpr` and pinned against an independent reference by known-answer vectors. **Never** a per-shard stream and **never** keyed on shard index — that would couple output to the decomposition. It is built and its only consumer today is the delay injector above; §11.10's search is the production one.
- Synthesized ids derive from a **global stable key**, never from shard ranges. Per synthesized kind: port = `(compound_state, side, transition, crossing_depth)`; split segment = `(transition, segment_ordinal)`; label dummy = `(transition, rank)`; routing-graph node = rank under a total sort on `(x, y, plane, kind)`.
- Reductions merge in **index order**. Combine operators must be **associative**; commutativity is not required. `+`/`min`/`max`/`xor` qualify outright. List append and `argmin(value, index)` are associative but not commutative — legal, and index order is what makes them well-defined, which is why §11.10 reduces a portfolio that way. **Saturating add is banned**: it is not associative, so a shard split changes the answer.
- Every comparator is a **total order** ending in a stable input-derived key. Ties are forks.
- **Vendored stable merge sort** for any sort whose result reaches output. `std::sort` is permitted only in tests.
- All rectangles are **half-open**: `[x0,x1) x [y0,y1)`.
- Fixed iteration counts. Every retry loop states its cap, its integer increment schedule, its subject order, and its terminal diagnostic. **An *improvement* loop — a search sweep — owes a counted statistic instead** (§11.10): sweeps used, moves accepted, and the frames that hit the cap while still improving. A retry loop ends because something is wrong, and a sweep cap ends a loop that was working, so a `StoppedAtCap` code would fire on well-behaved charts; and diagnostics are part of the golden artifact, so it would rebase every golden on each calibration pass. A retry loop still owes its diagnostic.
- Diagnostics are collected per shard, concatenated in shard order, then sorted by `(code, subject_kind, subject_index)`. They are part of the golden artifact. **A diagnostic carries nothing but that triple** — file, line, column, and the offending text are derived by walking to the statement's `src` span (§7), so no layer threads positions through its call stack. Diagnostics that precede the model — normalization, lexing, parsing — have no entity to name, so they carry `subject_kind = None` plus a raw source span directly: no statement exists yet to walk to. **An ordinal of `INVALID` under a real kind names the kind and no row** — a column whose count disagrees with its entity array reports that entity's kind and `INVALID`, since `ElemKind` has no `Column` and cannot grow without an ABI break — and `None` carries `INVALID` and nothing else. So a reader never has to know, per code, what an ordinal means. On a model mutated since load that span is cleared, and a diagnostic degrades to the triple alone; goldens run on unmutated models, so the artifact is unaffected.
- **Text is normalized at parse**: LF-only, BOM stripped, NFC. Ship `.gitattributes` with `*.scav -text`. Without this, `core.autocrlf` on Windows and NFD on macOS change the name and label *bytes* — same commit, different canonical print, and different text metrics, so different space requests and different coordinates. NFC needs a table: it is a **P0** dependency.
- Threading via a shim over pthreads / Win32 / **null (inline)**, `parallel_for(shards, threads, fn)` over a function pointer or a functor, one backend chosen by the CMake cache variable `SCAV_THREAD_BACKEND` (`AUTO|NULL|PTHREAD|WIN32`, `AUTO` being `WIN32` on Windows and `PTHREAD` elsewhere). **Static striping**: `threads` 0 means 1, `W = min(threads, shards)`, worker `w` runs shards `w, w+W, w+2W, …`, and the whole assignment is fixed by `(shards, W)` before any shard runs — no queue, no work stealing, no atomics, so there is nothing for a worker count to change. The caller is worker 0 and joins every other before returning. **A spawn failure degrades rather than fails**: a worker `pthread_create` or `CreateThread` will not start has its stripe run on the caller over the same indices, so output is unchanged by construction. The pthread backend links `Threads::Threads` PUBLIC, since a static archive carries no link flags of its own, and the exported package config carries the matching `find_dependency(Threads)`.
- **Not `std::thread`, and the reason is `-fno-exceptions` (§4).** `std::thread`'s only failure channel is a thrown `std::system_error`, which under that flag is `std::terminate` — a library that aborts its host process on a transient thread-creation failure is worse than one that runs the stripe inline, and only the OS APIs return the error code that makes the inline fallback expressible. Not C11 `<threads.h>` either. Header weight is not the argument: the backend `.cpp`s sit at `src/` root, outside the include-subset scan the layout library is held to. The null backend makes WASI and the matrix work — either design needs it — and it is compiled by no CI row until P11's `wasm32-wasi` one, so it is built by hand with `-DSCAV_THREAD_BACKEND=NULL`. The Win32 backend mirrors the pthread one line for line and gets its first compile on the Windows rows.

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

**Scope of this section: anything that can reach layout geometry or rendered output.** A structure that only ferries data inside one call is outside it, and `std::unordered_map` is the right choice there — deterministic *by usage*, because a key lookup has no order and the hash value never escapes as a bucket index. Enforce that structurally: `HashMap<K,V>` exposes `find`/`contains`/`insert` and **no `begin()`/`end()`**, so "never iterated" is a compile error rather than a review comment.

**Golden hash.** Split into a **structural hash** (ranks, orders, port assignments, bend sequences) and a **coordinate hash**, so a translation-only change is a reviewable diff instead of a global reflow.

**The structural hash is seeded with the model's own structural digest** (below), so it answers "this model laid out this way" rather than "some model did". The seed is what separates two charts whose geometry happens to agree: `estop.scav` and `led.scav` collided on **both** hashes at once, being different models that lay out to identical ranks, sides, depths and turn tokens, and the seed splits the structural pair while leaving the coordinate pair colliding — which is exactly what the split is for, since their coordinates really are the same. Only the structural column of any golden moved when it landed; every inputs digest and every coordinate hash is byte-identical either side.

**The inputs digest is a third value beside them, not a seed for them.** It covers profile id, packer choice, router name and version, **and the space-request columns** — a golden is reproducible only against a stated measurement policy, and the corpus goldens use the reference builder's. It lands in `scav.geom.inputs` (§11.7a) so it round-trips the model and a binding can read it, and a golden row is `inputs structural coordinate`. Seeding the two geometry hashes with it was the obvious shape and is wrong: the space tables move whenever a label's width does, so the structural hash would move on every remeasure and the split would report a global reflow for a change that reordered nothing. Three values say *which inputs* and *what moved* separately, which is what a reviewer needs.

**Font identity and version reach that digest through the space tables, not as an argument.** Layout is font-blind by construction — text arrives only as integers the app measured — so a font field on `scav_layout_opts` would be a knob layout never reads and a caller could set wrongly. Two fonts that measure one corpus identically *should* hash identically at this stage, because layout genuinely produced the same geometry; the difference is real at the `DrawList`, where glyph advances and `textLength` live, and that is where font identity is hashed explicitly.

**The hash is xxHash32, ours rather than the standard library's.** `std::hash` is permitted to differ between runs of one binary, let alone between implementations. xxhash is not cryptographic and does not need to be: what a golden wants is speed and even distribution over inputs measured in kilobytes. Lane reads are assembled from bytes rather than cast, so a big-endian host agrees, and rotation goes through `<bit>` because `__builtin_rotl` is not standard C++ and a hand-rolled shift pair is UB at a rotation of zero.

**The model's structural digest arrives before layout does** (P2), because P2's exit gate has to compare one network loaded three ways and there are no coordinates yet to compare. It is a *serialization* first and a hash second: field by field in array order, never a struct's bytes — padding is unspecified — and length-prefixed on every string, so two adjacent names cannot spell one. The bytes are exposed alongside the hash because when two models disagree, diffing them says where and a hash only says that. Excluded: document names, which differ legitimately between a filesystem, a buffer, and a URL; statement ids and source spans, which say where a thing was written rather than what it is; and columns, which are the extension's to hash. The layout structural hash is seeded from it (above).

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

**The bar is a fixed-size box.** Layout's only fork-specific behaviour is negative: it takes the bar's extent from the profile like any other pseudostate, and `w_adjacency` excludes fork edges because adjacency above arity 2 is unsatisfiable (§11.8) — no fan-out algorithm, no arity scaling. A fork/join pseudostate is an ordinary small box — wide and thin — from the profile's per-`StateKind` min extent (§11.15). Layout places it and routes N edges out of it; the builder draws a filled rect. That is what PlantUML does, and it is enough: the bar is the **same size for two branches as for five**, with the routes simply fanning out, including sideways.

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
decompose(Chart)                                                  -> SplitGraph
order_submachines(Chart, SplitGraph, Spaces, Profile, Threads)    -> SubmachineOrders
size_layout(Chart, SplitGraph, SubmachineOrders, Spaces, Profile) -> SizedLayout
route_transitions(Chart, SplitGraph, SubmachineOrders, SizedLayout, Spaces, Profile, Router, Threads)
                                                                  -> Routes: points, slots, Placed[]
layout_run                                                        -> the geometry columns (§11.7a)
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
- Validate the domain at `scav_layout_run` entry in **every** build (§8.1), and again on each inflated profile copy before its retry runs: a copy out of range ends the retries, as does a `size_layout` that overflows, and the last successful geometry stands (§11.6).
- Output is **root-absolute**, applied as one final `O(n)` transform over submachine-local internals (ELK's LCA-relative coordinates are a documented trap).

Extent estimate: 2k states ≈ 8,000 x 3,200 pt = 128,000 x 51,200 units, ~4x headroom. **Measured twice, and both numbers are worth keeping.** Under P4's deliberately fat fabricated advances a 2k-state chart came out **181,120 x 277,888** — 1.9x headroom on the tall axis, which is the conservative bound the fabricated measurement exists to produce. Under P5a's real bundled font the same shape is **152,628 x 101,044**, or **5.2x**, so the original estimate was sound and the grid decision was never close. Keep asserting the fabricated case: it is the one that trips first when a later phase grows boxes, and P6's did. **Measured a third time, under P6:** the widest fabricated `min_w` the 2k shape carries is **4768**, against P4's 3200 stand-in, at 523,584 x 417,456 — and the real font puts the same shape at **167,194 x 109,253**, 3.1x headroom. That the fabricated number went *up* is the fold of §11.4 doing its work; without it the same shape carries only 1280, because a rank run grows along one axis and nesting multiplies it by the depth. If real charts ever exceed the domain, reduce the grid to 1/8 pt rather than widening it.

Coordinate assignment uses two linear integer primitives, not a solver: **Brandes & Köpf** for cross-axis coordinates (GD 2001 — **read the erratum, arXiv:2008.01252**), and optimal topological numbering for compaction. On an integer grid with integer gaps and an acyclic constraint graph, non-overlap plus separation *is* longest-path.

### 11.3 Phase 1 — per-submachine ordering

Independent per submachine: the parallel unit and the dirty unit. Layered rank assignment, then intra-rank ordering by **median** (tight 3-approximation) or **global sifting** (5–10% fewer crossings than level-by-level sweeps, eliminates type-2 conflicts). **Not barycenter** — no constant-factor bound, ratio Ω(√n).

**The layering axis runs left to right.** Ranks are columns, nodes stack vertically within a rank, and successive ranks proceed in +x. This is not a new choice — it is what §11.4's size composition already encodes, and recording it here keeps `rank_sep` from being read as the other axis by half its readers. So `rank_sep` is a horizontal gap between adjacent columns and `node_sep` a vertical gap between adjacent nodes in one column (§11.15). The RTL mirror (§11.9) is an x-flip, which is the layering axis, so it reverses reading order exactly as intended.

**A frame's graph need not be connected, and each component is laid out on its own**, because unconnected states all rank 0 and one graph would stack them in a single column — the shape a submachine of leaves actually has. The components are then packed (§11.4), which is the treatment sibling submachines already get one level up, and the pieces are ordered by their first node so reading order survives.

**Ports are nodes in their frame's layered graph**, one per `SplitPort` whose frame this is, ranked and ordered alongside real states. That is what turns §11.1's split into 1D port ordering per compound side without a second algorithm: a port's rank fixes which side of the compound state it lands on, and its position within that rank fixes its offset along the side. Phase 2 turns those two ordinals into coordinates; nothing else assigns a port a side.

**Only the left and right sides are produced.** Ranks run in +x, so a port is a source boundary at the frame's left edge or a sink at its right, and every slot reports side 0 or 1. **[OWED]**: top and bottom sides, which need a port to be allowed onto the cross axis; the `side` field already admits 2 and 3 so the column does not change shape when they arrive. **What that costs is not the marks case, and is not yet measured**: `gauntlet/marks.scav` holds no compound state, so phase 0 splits nothing there and creates no port at all — the arrival and departure that met on one point of a disc were §11.5's seating, answered there by moving one of them onto another face of the same glyph. What two sides do cost is a decomposed transition that would enter or leave a submachine from above or below, which no corpus chart asks for, so nothing has counted it. A boundary node's coordinate is the frame's edge, not the edge of whichever folded piece (§11.4) it was laid out in.

**Ordering-edge weights are uniform at 1.** The known lever if the side-by-side (§11.12) wants straighter long edges is dot's schedule — real-to-real 1, real-to-dummy 2, dummy-to-dummy 8 — and it is deliberately not taken up front: it is three integers and a golden rebase whenever it is wanted, and taking it now would mean tuning against no measurement.

**Inter-rank edge labels widen the rank boundary they cross**, so rank separation accommodates them by construction (§11.9): the gap between two adjacent ranks is `rank_sep` plus the widest `PathBox` any edge crossing that gap carries. That makes **`Spaces` a phase-1 input and not only a phase-3 one** — a label wide enough to set the gap between two ranks has to be known before the ranks have coordinates — while placement along the finished route stays phase 3's, so a `PathBox` row is read twice for two different questions: how much room to leave, then where the room ended up.

The textbook alternative is a **label dummy node**, which additionally *orders* the labels sharing one gap so two cannot collide, at the cost of doubling every rank (an edge between adjacent ranks has no intervening rank to put its dummy in) and of choosing which frame of a hierarchy-crossing route owns the label. Widening buys the sizing guarantee, which is the part that cannot be repaired later; collisions within a widened gap are priced by `w_label` (§11.6) until P7, whose strip matching (§11.9) places path boxes properly and subsumes what the dummy would have done.

**The degenerate flat chart bounds the ordering algorithm, not just the router.** One submachine holding 2k states is legal input (§11.5), and global sifting there is `O(|V||E|)` per pass over a graph an order of magnitude larger than the n≈20–50 the per-submachine case assumes. **A long edge costs a bend in every rank it spans, and cycle breaking can make an edge long.** Chaining is `O(Σ span)`, which is the price of a proper layering and is fine while spans are short. Reversing an edge to break a cycle is what makes them long: a chain reversed in node order turns a thirteen-rank skip into a thousand-rank one. Measured, a flat frame of 2k states in a chain lays out in 2 ms with 3,953 ordering nodes; the same 2k with skips that wrap produces **a million** ordering nodes and takes seconds. The mitigation is a cycle-breaking heuristic that leaves a chain alone — Eades, Lin and Smyth's greedy removal rather than a depth-first walk in node order — and it is not bought here, because which edges it reverses instead is a quality question for the side-by-side (§11.12). A test pins the behaviour at 512 states so the fix has a target.

**Median ships; global sifting is a named lever, not a second code path.** Median's 3-approximation is bounded at every size, so one algorithm covers both the n≈20–50 nested frame and the flat 2k one, and the flat shape gets its own performance floor to keep that true. Sifting's 5–10% is worth having and its cost is `O(|V||E|)` per pass, which is affordable at the nested size and not at the flat one — so taking it means a size threshold, a profile field, and two orderings to keep deterministic. That is bought when the side-by-side (§11.12) says the crossings are what is wrong, and not before.

**The layered count is right in aggregate and wrong on two charts, and the mechanism is measured.** At `sweep_count = 0` against the shipped 8, routed crossings on the no-space scale read 99 against **91** over the corpus, so sweeping helps overall; on `axis` they read 2 against 4 and on `mill` 38 against 43, so on those two the sweeping is what costs. On `axis` the root frame's layered count is 0 at both settings — `minimize_crossings` never sweeps a frame it already scores at zero — while the drawing has 4 crossings, 8 under real text: uninformative rather than misranking. The whole `axis` delta is one adjacent swap in the frame of its `estop` instance, rank 2 going `{bend, Tripped}` to `{Tripped, bend}` at zero sweeps, where the bend chains the reversed `Latched → Clear` edge across three ranks; taking that frame's layered count 1 → 0 costs the drawing two crossings, and `estop` is instantiated six times in `mill`.

Two mechanisms are primary, and both are structural. **Pairs sharing an endpoint are uncrossable to the count** — the south tiebreak in `south_of`, which is correct about the graph — while the router seats them on different faces of the shared box (§11.5) and they then do cross: under real text 5 of `axis`'s 8 crossings and 21 of `mill`'s 48 are between routes sharing an endpoint state. **And a rank gap is not a channel**, because §11.4's fold cuts rank runs, which it does on 59 of the corpus's 62 components. Two more are live: a frame's components are packed as disjoint rects while `total_crossings` buckets every node of one rank together, so an inversion between two of them is phantom (`vac`'s root frame has four); and pairs in two different frames, which phase 1 never prices at all and which §11.8 measures the cost of.

**The fix is a portfolio row with zero sweeps (§11.10), not a new phase-1 objective.** Phase 1 has no extents, no fold and no faces, so any replacement objective is still a proxy, and this one is right on average. What a portfolio must not read is `Cost` alone on this question: at zero sweeps the corpus's `t2` is the *better* of the two, 1.0814e9 against 1.1487e9, because `mill`'s ranks reorder into 11% less area and area dominated the tier (§19). **Both sums are the pre-em scoring** and the pair has not been re-measured since P9a cut area to a third of the tier (§11.6); what the argument rests on is that phase 1's own objective is not the diagram's, which no unit changes.

### 11.4 Phase 2 — sizing and sibling packing

Sizes bottom-up; port positions top-down; fixed pass count. The hints half of that descent is **[OWED]** with §14: no hint column exists, nothing resolves a `scav:` attribute, and `Cost::t1_hints` is therefore always zero.

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

**Sibling submachines are packed here, not in phase 1** — packing requires the siblings already sized. **LR-rectpacking** (Domrös et al., IVAPP 2021): greedy width approximation → placement → compaction → whitespace elimination, `O(n log n)`. Take the LR variant; plain rectpacking's one-oversized-child special case was deleted by its own authors as unmaintainable and aspect-ratio-blind. **All four steps are in `pack.cpp`, and P9c landed the last two as two different kinds of thing** (§17 P9c). Placement produces a *spot list* — one of `{Right, Subrow, Level, Row}` per rect, which is the whole of its freedom — and the geometry is a pure function of that list, so every structure the packer can produce is a cut of the input sequence at three levels and order, non-overlap and the `sub_sep` between two placed siblings are properties of the structure rather than of the code that walks it. **Whitespace elimination is unconditional**: every row grows to the drawing's width, every block to its row's height, every subrow to its block's width and every rect to its subrow's height, the slack floor-apportioned from a running prefix so the shares sum to exactly the slack and every gap stays the gap placement gave it. It moves neither extent, so `pack_better` and `Cost`'s area and aspect cannot see it, and as a row of a table that ranks candidates it could only ever tie. **Compaction is a knob** — bit 1 of §11.10's Level 2 table — because where it fires it is a smaller drawing at the price of a reflowed one, and only the whole chart's `Cost` weighs those two against each other; the shipped `portfolio_m` of 2 does not run it (§11.15). Of the 90 multi-rect packings the corpus and the element suite run, **76 are a fold's pieces, 12 a state's sibling submachines and 2 a frame's components**, and only the sibling site has a rect a reader sees — `size.cpp` reads a component's and a piece's position and nothing else — so that is the one site whose grown extents are written back, the sink boundary node on the old trailing edge moving with them.

Order-preserving and gap-avoiding are one constraint: restricting placement to four positions relative to the predecessor (directly right; right on the current row level; next subrow; next row) is exactly what makes local whitespace elimination always possible. **And what those four positions find is better than this section gave them credit for**: exhaustive enumeration over all 90 packings finds no order-preserving structure inside the target width shorter than the one placement found, on 18 of the 21 charts. So what compaction can reach is narrow, and narrow in a particular direction. **"Nothing then goes back for the hole" is not reclaimable at the target width** — placement wraps to a new row only when no earlier position fits, and the target bounds the achieved width, so a wrapped rect is a fixed point of any rule that cannot lose area. The "up" half of compaction fires only where an earlier edit has already narrowed a row, about 1 in 300 random 3–5-rect lists; what fires on the corpus is the "left" half, a rect pulled back beside its predecessor (§17 P9c).

Review has now judged, and the number is `vac`. Under real text its root frame's content sits in five rectangles — `lamp`, `PreConfig`, `On`, `dock`, and the `$initial → Off → Booting` chain — for **54.7% occupancy**: 64,883,482 of a 10,785 x 11,004 canvas. The frame's graph is four components rather than five — the middle segment of `Ready → dock/On/Seated` joins `On` to `dock` — and the fold cuts that one between its two ranks, so the packing the frame itself does places four rects covering **68.6%** and `pack_lr` places `On`'s piece and `dock`'s inside one of them. A 4173x3783 block sits empty left of `dock` and below `On` — between those two pieces, which is this same packing one level down — while the 1696x1594 chain occupies a row of its own beneath everything. Order-preserving placement offers a late arrival only the four positions relative to its predecessor, and "next row" was the only one that fit; nothing then goes back for the hole. That was read as what these two steps are for, and half of it was misread: the 4173x3783 block is inside a *component* rect, which is not a box a reader sees, so it was never the packer's to fill (§17 P9c).

**That reading is P9a's one candidate, and `vac` is one of the charts whose pick moved at P9b.** Its real-text canvas goes 118,678,140 to 103,808,360, 12.5% less of it and packed by `pack_lr` alone rather than against the row packer, so the 54.7% describes a drawing that no longer ships and the occupancy is re-taken below on the row the portfolio keeps (§17 P9b). What the finding says does not move with the number: order-preserving placement still offers a late arrival no way back to a hole behind it, whichever row places the rects. **Re-taken at P9c on the row that ships**: the root frame is 69.98% full over its four packed components and 60.24% over the five rectangles a reader sees, of 11380 x 9122, and neither step moves either number — compaction cannot improve that packing and expansion grows only rects nobody draws. So the occupancy clause P9c carried is met by Level 2's pick rather than by a packer step, and the whitespace the legible candidates pay for is inside their components' own layouts rather than between them: **it is P9d's weights that carry that trade, not another step here** (§17 P9c).

**A rank run folds when folding scales larger.** A run grows along the layering axis without bound and nesting multiplies it by the depth, so sixteen levels of a fifteen-state chain draws as a strip a million units wide. Cutting the run and stacking the pieces fixes that, but not everywhere: at two ranks it makes the aspect *worse*, which a greedy fold at the target width demonstrates on the first chart it meets. So both shapes are laid out and the scale measure picks, the same way `trybox` picks a packer. An edge the cut crosses has its ends in two pieces and gets no say in either's coordinates, as a wrapped line's does not.

**The pieces a fold makes are packed, not stacked.** Stacking them left-aligned gives every piece the width of the widest, however little it holds; they are rectangles sharing an area, which is what LR-rectpacking already does for this frame's components and a state's sibling submachines. Over the corpus the Tier-2 sum falls 5.6% with area, Tier 0 unchanged at zero, `toolchanger` best at 0.79x. Under real measurements `brew` goes 35.7M to 28.9M, arriving at the arrangement a reader proposes unprompted: `Standby` beside `Brewing`, terminal below it, inside `Brewing`'s own vertical extent.

That is worth separating from the win. **Nothing generated candidates before P9b**: the pipeline ranked, ordered, sized and packed once, and `Cost` was never asked about a second arrangement — the packer found this one because one more shape happened to be in the set it already compares. §11.10's Level 2 is what asks now, and it asks at chart scale: M whole layouts, scored on exact `Cost`, the pick recorded per chart (§17 P9b). §11.8's "reorder sibling submachines" is still P9e's, so a *frame-local* improvement of this kind remains a special case in phase 2 rather than something the layout found.

The fold is what costs §11.7a's hash split a property it had while sizing did not feed back into shape: a size change that reflows the ranks now moves the structural hash as well as the coordinate one. That is the honest form of §11.11's limit rather than a new exception to it — what survives is that a route whose shape cannot depend on its box, a self-loop for instance, moves coordinates and no turn.

**Phase 2 costs five to six times phase 1 on the corpus, and half of that is the fold's second candidate.** Measured on this Mac, release, one thread, `readable`, no space requests, and informational rather than a floor (§5): 4.9x to 7.5x chart by chart over the corpus, and about 4.5x on the nested 2k target, with the per-phase absolutes in §11.10. `lay_out` runs twice per component, once unwrapped and once with the run cut at the target, and each run rebuilds a `View` per Brandes–Köpf pass, four per chunk. **The second run earns its half, which is the finding**: the cut fires on 59 of the corpus's 62 components and on all 128 of the nested target's, so the folded candidate is a different shape almost everywhere. Skipping it only where it would not fire is byte-identical — every layout golden and all 493,371 assertions of `scav_layout_tests` hold — and saves nothing measurable, 584 µs against 577. Dropping the candidate outright is what the half buys — phase 2 halves on the corpus and on the nested target alike — and it is not bought, because the candidate is what §11.4's fold finding was. **[OWED, P9e]**: cache what a candidate cannot move, rather than skip a layout. Under any in-rank permutation of one frame the child extents, `layer_w`, `layer_h`, the area and the target, the chunk boundaries, phase 1's `gaps` and the component decomposition are all invariant; only `cross_coordinates` over the chunk that moved is per-candidate.

**Every frame aims at the profile's ratio, whichever hole it is filling.** `pack_best` hands `pack_lr` and `pack_better` the profile's `dar_num`/`dar_den` at every depth — 16:10 on `readable` — so a frame packs for 16:10 whether or not its owner's remaining hole is tall and narrow. That is `vac`'s finding one level down, and it is cheap to answer: DAR is already a parameter of both, and `pack_best` is the only caller that hardcodes the profile's, so handing a frame its owner's hole aspect is an argument change rather than an algorithm.

**Landed at P9b as a capability, and unshipped.** `pack_best` takes the pair explicitly and hands that same pair to `pack_lr`, to the fold's target width and to the scale-measure compare beside it, so the three packings inside one state's interior all read the one ratio that state carries; `size_layout` takes a `DarSource` of `Profile` or `OwnerHole`, and `Profile` is what every caller passes today. **The hole is the aspect of the owner's interior box between its `before` and `after` bands** — the rect its submachines pack into, which is the whole interior where the state requests neither band and carries whatever slack `kind_min_h` left. That aspect is reduced into the profile's own `[1, 1024]`, the bound `dar_num` already carries and where `pack.cpp` proved its products fit `int64` (§11.15), with the longer axis taking the cap and the shorter floored at 1: a hole a million times longer than it is wide reads 1024:1 rather than as no ratio at all. A root frame has no owner and a state with no live submachine has no hole; both keep the profile's. And it cannot be read before its owner is sized while sizing is bottom-up (below), so **`OwnerHole` sizes twice**: once at the profile's ratio to find the holes, then again against them, with a first pass that leaves the domain ending the second so a failing state is diagnosed once. Rows 4–7 of §11.10's table are what would ship it and the shipped `portfolio_m` — 4 at P9b, 2 from P9c — does not reach them; §17 P9b records what M = 8 measures and P9d judges whether to buy it.

Width approximation is `target_w = isqrt(floor_div(total_area * dar_num, dar_den))` with that exact operation order, `isqrt` = floor, over an area carrying one gap per rect, then floored at the widest rect and capped at `COORD_MAX` — a target under the widest rect is unsatisfiable. `DAR` is an integer pair. On same-height submachines the older `box` packer wins; **`trybox` is evaluated per packing call**, not once per layout: `pack_best` compares the row packing against `pack_lr` every time it is asked, which is once per frame's components, once per fold's pieces, and once per state's sibling submachines, deterministically each time. A 1-unit change can flip the packer and reflow siblings; that is a boundary condition for hints (§14), not grounds for remembering the previous choice. **And the choice decides legibility, not only extent.** `gauntlet/crowd.scav` is the element case (§5): under real text at `readable`, `trybox = 1` sits a composite's two concurrent regions side by side for an area of 16,144,032 and leaves the labelled pair into and out of it printing over itself and over an arrow, while `trybox = 0` stacks them, takes every one of `tools/audit.py`'s counts on that chart to zero, and costs 56% more area. `sweep_count` moves neither that chart nor `brew` over {0, 1, 2, 4}, so the lever is chart-global and phase-2's, which is what §11.10's Level 2 chooses over. **Level 2 landed at P9b and every pick it makes is the same direction**: on both scales, on every corpus chart whose drawing moved, the winning row is `trybox` flipped off — `pack_lr` alone, with the row packer never tried — and the scale-measure tiebreak wins nothing anywhere, which is why P9c took its bit for compaction (§17 P9b, §17 P9c). A knob evaluated per packing call is still a knob one number per chart sets.

**Fitting the domain (§11.2) is a precondition of the comparison, not a check after it.** Every packing comparison — a frame's components, the pieces a fold makes, and a state's sibling submachines — measures each candidate against `COORD_MAX` first, so a better-scoring packing that does not fit never displaces one that does, and `CoordinateOverflow` is reported only where no candidate fit. A packing outside the domain cannot compose a box inside it, so nothing viable is discarded by asking.

**Bottom-up sizing has no locality** — a leaf growing one unit resizes every ancestor to the root. Inherent, and simply paid: one pass up, one pass down, fixed count. No hysteresis; that would be hidden state (§11.11).

Not top-down layout: its central size-approximation problem is unsolved by its own authors, it introduces per-level scale factors that break port-split segment continuity, and it is mutually exclusive with cross-hierarchy edges in ELK. Cost: bottom-up sizing at depth is a readability problem on fixed media (a depth-9 SCChart lays out to 0.322 pt max font on A4). Acceptable because output is a zoomable canvas.

### 11.5 Phase 3 — routing

Axis-aligned is a **hard constraint**. States, submachine rectangles, and placed boxes are obstacles, so Tier-0 edge-through-box is unrepresentable rather than penalized.

Routing graph is an **orthogonal visibility graph** or a **channel-representative graph** (Hegemann & Wolff, GD 2023, arXiv:2309.01671). At per-submachine scale (n≈20–50) both are cheap: **choose on quality and implementation simplicity, not asymptotics.** Published full-OVG scaling failures (~30 min at 4,330 obstacles) apply only to the degenerate flat chart — one submachine holding 2k states, which is legal input.

**The sparse graph that path was reserved for is not needed, measured at P7b.** The grid is the product of two line sets rather than a function of box count, and 2k packed boxes share columns and rows, so the flat chart routes in 215 ms on the full OVG. What blows the budget is boxes at *distinct* offsets — a shape the router's own suite builds and no chart produces. Reach for a sparse graph when a chart does.

Bend cost lives **in the shortest-path metric**, via the **separated OVG** (Wybrow et al., Diagrams 2012): split each node into h-plane and v-plane copies joined by an edge whose weight *is* the bend penalty. Length and bends collapse to one uniform edge weight; unmodified Dijkstra/A* optimizes both, so bend-heavy routes are never generated.

**The separation *is* the A* state, so neither the key nor the heuristic needs a direction in it.** This section first specified state `(vertex, entry_direction)`, tie-break `(f, g, entry_direction, node_index)`, and the 5-case remaining-bend bound from GD 2009 §4 Fig 2a. A node already *is* `(vertex, plane)`, so the direction term in the key is a copy of part of the node and the key is `(f, g, node)` — total, and one comparison shorter. The bound collapses the same way: from the h-plane a goal off the current row needs at least one more turn, and from the v-plane a goal off the current column does, so Manhattan plus one bend when this plane cannot reach the goal's axis alone is admissible in two cases rather than five. Both are lower bounds, so the sum admits.

**Inter-submachine segments are routed in the parent's frame**, and every separator channel is owned by exactly one submachine (the LCA) and routed there. Without this, "independent per submachine" is false for exactly the edges this project exists to handle. **Two qualifications, both measured.** "The parent's frame" names no frame when the two submachines are concurrent regions of one state — §11.8 holds that **[OWED]** and the count the element suite pins on it. And separator channels are **not built**: they were the fix for the separator stub's Tier-0 violations, the attachment-face rule below took those to zero, and nothing in the tree now demands them **for Tier 0** — `layout_tests.cpp` still names the LCA channel as the fix for the three flush lanes it pins, every one of them a separator port — so they stay here as the design for *sharing* a corridor (§17 P7c).

Congestion is **history-based** (PathFinder, McMurchie & Ebeling, FPGA'95; TritonRoute's marker cost) and is **not built** — nothing measured demands it, and it is the expensive, sequential, iteration-count-tuned part, so it is unscheduled rather than owed (§17 P7c). The design, for the chart that does demand it: route nets in `(submachine, transition)` order within a congestion domain, double-buffer the history map so iteration *k* reads only the *k−1* snapshot, merge updates in net order, fixed iteration count from the profile, integer ramp schedule as a lookup table.

**Nudging, as built, is the room-only half of the stage below.** A lane is a run of collinear overlapping segments in one frame, keyed `(axis, coordinate)` and swept into groups by extent. **One axis is done at a time, and the members of the second are read off the moved geometry rather than the input**: a horizontal displacement drags the vertical legs either side of it, and their extents move with them. Its members are ordered by where each net was *before* it reached the lane, taken from whichever end is lower along the lane's own axis so every member is measured from the same side — a net arriving from above stays above rather than swapping and paying a crossing for it. Ordering by both ends read worse when that was measured, 145 corpus crossings against 129, because a net that changes side has no consistent answer and ends up placed by its tie-break.

**Members whose nets already run as one take one offset between them.** Two bundle when their nets are identical from the segment's far point to the net's end, or from the net's start to its near point — a fan-in and a fan-out being the same shape read from either end — and the relation is transitive because the runs are identical. A bundle counts as one member when the step is sized and the lane laid out, sits in the order at its least `toward`, and moves or stays whole. Without it the stage takes the trunk several routes reach a state along and spreads it into that many parallel lanes a gap apart, which reads worse than the one line fanning in that the router produced: four of them into `bottler`'s `Fault`. **Nothing attracts a route to a trunk.** Every net is still routed independently on its shortest path under the bend penalty, a trunk exists only where two of those coincide, and the rule only stops the pipeline pulling apart one that is already there — a detour to meet up trades length the reader pays for on every glance against a merge the reader may not notice (§11.13).

**Only a segment with a neighbour at each end moves.** An end segment is anchored on a box border and sliding it along that face is a different move; 86% of the corpus's shared length is between two interior segments, so the anchored case buys little and risks detaching a route. **And a displacement is taken only when it is known good**: the segment and the two legs it drags may not leave the region or the frame's own box, may not collapse a leg to nothing — which the arrowhead reads its direction off — and may not reverse one, which folds the polyline back over itself. **A leg's own length is therefore part of the room**, folded in before the step is sized rather than checked afterwards, so a lane spreads by what every member can drag instead of dropping the members that cannot reach: 12,905 corpus grid units of shared run between the two. The obstacle rule is per leg and in two parts: a box's raw rect is a hard wall and the rect grown by `clear` a soft one, each exempt only for the leg already inside it — a re-seated route sits inside a bumper it may not then cross. **And a leg may keep only a shared run it already had**, which is what stops a member trading the lane it left for one it lands on; the whole-cloth rule, refusing any overlap rather than a new one, refuses so much that the residual corridor nearly doubles (163,468 units against 86,310). A lane with no room keeps its members stacked, because a diagram that scores well and overlaps a box is worth less than one that scores badly and does not. The window either side is not symmetric — a box one side and open space the other is the ordinary case — so the spread sizes to the whole of it and slides back towards the lane as far as it will go. **The frame's own box bounds the room one unit inside its border**, since a lane laid on that border is drawn over it and reads as it: with a bundle taking one step where its members took several, the arithmetic reaches limits the old spread never did, and six of `mill`'s segments landed there before the unit was taken out. **A bundle is asked as a whole**: every member's checks run against the geometry the others still have, and one refusal leaves all of them where they were, since half a trunk pulled off the other half is worse than the trunk. Its own siblings are exempt from the shared-run rule, because two members of one bundle move onto each other by construction.

*Measured, nudging off against on, no space requests:* corridor **355,116 → 93,066**, and it is the first phase in which that term has a nonzero multiplicand at all. Crossings **66 → 128** and excess length **648,320 → 1,775,868**, the second following the first since excess is charged per crossing on the edge. Bends 587 → 587, and the stage moves neither aspect nor area — though the commit that landed it did, unattributed (§17 P7c). `Cost` improves overall — 1.1629e9 → 1.1549e9 — but only because area dominates the tier (§19). Under real text the corpus went from 240 shared-segment pairs over 368,902 grid units to 136 over 86,310, with no route doubling back and none collapsed. The two scales disagree in sign on the corridor term and both are honest: the readable profile's boxes sit differently under measured text, and the lanes a displacement can clear are not the same ones. **With trunks left as one and not charged** (the bundles below, §17 P7c's addendum): real text reads **80 pairs over 49,199 units** plus 90 trunks over 107,818 that are now permitted; on the tables' scale corridor 93,066 → **62,285**, crossings 128 → **104**, excess length 1,775,868 → **1,611,523**, bends 587 unchanged, total polyline length +0.07%.

**Doubling the crossings was partly honest and partly owed.** Two collinear segments do not *cross* under the predicate, so separating them reveals crossings that overlap was hiding. The rest was the ordering key being a projection of one end rather than the full stage.

**The combinatorial stage, built.** Its input is the crossings the ordering itself controls, and there is exactly one family: two lane members are parallel and their legs are perpendicular, so neither two segments nor two legs can cross, and only a leg against the other member's segment can. **A leg leaving the lane at a point strictly inside another member's extent crosses that member's segment unless the member lies on the leg's far side**, so every such incidence is one vote for the order that avoids it, and both members' legs are read into the one pair — which is what makes the matrix antisymmetric and a pair's answer independent of which end asked. A pair whose two legs disagree must cross whichever way it goes and its votes cancel, which is the honest answer rather than the first constraint found winning. Members whose extents coincide at both ends produce no incidence at all: their legs are collinear, so what they share is a run and not a crossing, and the other axis is where that is separated.

The votes are then a digraph — an edge from one bundle to another for every pair they separate that way round — and **the lane's order is a linear extension of it, by Kahn's algorithm taking the lowest key of the bundles nothing left precedes**. It contradicts no vote, and on a lane with no incidences at all every bundle is ready at once, so it is the key order the lane went in with. **Entering them one at a time and taking the position that contradicts the fewest, last of the positions that tie, is not that**: it is a linear extension only where the votes are total, and three members whose extents stagger — `a`'s high leg landing inside `b`, `b`'s inside `c`, `a` and `c` never touching — enter in the key's order `c, a, b` and come out of it in that order, every tie taken last, contradicting the one vote between `b` and `c` where `a, b, c` contradicts none. Votes that run round a cycle have no linear extension to find, and such a lane keeps the fewest-contradictions insertion, because that is the honest answer where no order satisfies the votes. **No pair can cycle on its own** — the matrix is antisymmetric — and no lane of the corpus, the element suite or the fixtures cycles at all, so the insertion is kept against a shape rather than measured on one. Segment order is a total integral key and placement is the same **integer offsets `k*gap`** as before; the pseudo-direction pass the stage was first specified with is unnecessary, because ordering both members from the lane's own low end already measures them from one side (above).

*Measured, the key alone against the full stage.* No space requests: crossings **104 → 85**, excess length **1,611,523 → 956,258**, corridor 62,285 → 62,061, `t2` 1.152747e9 → 1.150115e9. Real text: crossings **124 → 92**, excess 2,784,161 → 1,549,724, corridor 49,199 → 45,963. **Bends are 587 and 613 either side of it, unchanged on both scales**, which is the "minimum crossings with no extra bends" the stage was specified for and is the whole of what distinguishes it from moving a route.

*And the extension against the insertion*, measured on the corpus as the attachment below leaves it: the two orders differ on five lanes and the insertion contradicts no vote on any of them, so there is no crossing for the extension to remove and what moves is which of several extensions the lane gets — corridor **37,018 → 36,973** and excess length 961,229 → 960,779, crossings 102 and bends 563 unchanged, and no chart but `mill` moving at all, there over its three copies of `axis`. **The chain the two disagree on is a fixture rather than a corpus shape**, which is the honest size of it: the linear extension is now what the stage produces rather than what it produces on the easy lanes.

**The order is chosen on crossings and taken only where the room is good, and the two can disagree.** A lane whose preferred order would lay one net's leg along another's fails the shared-run rule above, and the lane then keeps its members stacked rather than taking the second-best order — a diagram that scores well and overlaps a box is worth less than one that scores badly and does not, and the same reasoning applies one level up. Two of the bundling fixtures turned out to be exactly that shape and now have a test of their own.

**Router is swappable** — internal only. Contract: pure w.r.t. its input, reentrant, no global state, called concurrently from workers, must not unwind. Router name and version are hashed inputs. The C ABI exposes routers **by name** (`scav_router_by_name`, `scav_router_list`); function pointers never cross it.

**An abstract base class, not the POD vtable this section first specified.** The boundary never crosses the ABI, so the C shape bought nothing; a `Router` has no members and one virtual call. The registry holds `Router const *` because C++ has no array of references, `reference_wrapper` needs `<functional>` (outside §6's subset), and `router_at` must answer "no such id" for an unvalidated index. `ud` goes with the function pointer.

`RouteInput`: the frame's region, obstacle rects, and nets carrying their two ends and the corridor phase 1 chose. `RouteOutput`: integer polylines plus per-net metrics — bends, length, and a named failure cause — so routers are A/B'd automatically. Internal POD, no `scav_` prefix, because neither crosses the ABI. **The bench is that A/B, and it runs over the registry rather than over two names**: every registered router is scored over the corpus at the readable profile with no space requests, one committed row per `(router, chart)` — Tier 0, the nine Tier-2 terms, the weighted sum, and the degraded and re-seated net counts — in `test_data/golden/layout/corpus_routers.txt`, with the wall clock over the corpus and both 2k shapes reported as a message and never committed.

**A route never leaves `region`**, which is what makes a per-frame obstacle set sound: every frame a decomposed transition passes through is owned by an ancestor of one of its endpoints (§11.1), so anything enclosing the region is excused by §11.14 and anything else is blocked or out of reach. Phase 3 sizes the region to the frame plus every point its nets touch — a port sits on the *crossed* box's border, outside this submachine by the owner's padding — plus the margin the router asks for, since a box flush against the frame's edge has no room for a lane otherwise. Obstacles are every live box overlapping it that does not enclose it, **outermost only**: a box contains its own descendants, so adding them blocks nothing and multiplies the grid by the subtree.

An anchor outside the region, an unreachable end, and a graph past the budget are three different failures and are reported as three: a degraded net is a straight line, and a straight line is what Tier 0 counts.

**Clearance is a bumper, not a penalty.** Obstacles block against their rect grown by `clear`, so "no segment comes within `clear` of a box" is a property of the graph. Pricing the flush lane instead was built first and is worse: it needs a constant tuned against the bend penalty — going round a box costs two turns, so anything at or below two bends leaves hugging cheaper — and it can only ever be probably right, so a test asserts nothing stronger than "not on these inputs."

The cost is over-constraint: two boxes closer than twice the clearance seal the channel between them. That is what §11.5's re-seat is for and the whole of it — the same graph without bumpers, tried once per net. A re-seated net is reported and is not a failure: it routed at spacing the profile did not ask for. **Measured at P7b: four of the corpus's 917 rendered segments run flush along a box edge**, each a leg lying on a box's own border. That is the whole visible cost of the hatch on real input, and it is the number to watch when P7c's channels change what seals. **At P8 it reads 3 of 731** under real text (`tools/audit.py`), with the no-space count pinned at 3 in `layout_tests.cpp`. **A route's own ends are exempt by construction**: the search runs between *ring* points one clearance off the border, and the leg from ring to border is emitted rather than searched, so it is perpendicular for free.

Two numbers the orthogonal router derives from the profile rather than reads: the clearance is `node_sep / 3` and the bend penalty is one `rank_sep` (P7b's measurement, §17). Neither is a profile field until P9's calibration says what the fields should be.

**Which face an end is moved onto is the separation from the box, never the distance to a point on it.** Measuring to a candidate border point charges an exit for the run *along* the face it leaves through, so the longer a face is the worse its own perpendicular exit scores — the rule that had `ota`'s fork bar leaving through a 4pt end while a 60pt side went unused, and standing the bar up (§7.2) made it worse rather than better. `ortho_escape_box` measures how far `toward` lies outside the box on each axis instead; the dominant separation picks the axis, a tie goes to x because that is the layering axis (§11.3), and the side is the nearer border, which stays total for a target inside the span. **An end therefore leaves through the face the flow runs through unless the other end is genuinely stacked above or below it**, and a bar's long faces are its attachment faces without anything in the router knowing what a fork is.

Measured over the corpus: bends **689 to 587**, better on ten charts of eleven, with crossings, aspect and area flat. It is a better rule for ordinary states too — nothing stopped one being left through its top face when the next rank happened to sit slightly above.

**Where on the face is the same question one step finer, and it is the attachment's rather than nudging's.** Having picked the face by separation, `ortho_attach_box` puts the end at **`toward`'s own projection onto that face**, clamped into it and held one clearance off each corner — an end on a corner leaves along the face it did not pick. A box centre carries no information about where a route is going, so every net naming one face of one box was handed one point: all six of `ota`'s bar attachments landed on a long face and three of them on the same point of it, with the incoming arrowhead inked along a branch's own first leg.

**But the other box's centre is still not the other end.** Each end projects a centre onto its own face, so a net whose two faces are parallel with overlapping usable runs comes out on two coordinates — `clamp_A(B.cy)` at one end, `clamp_B(A.cy)` at the other — and the search then pays two bends for a jog where one straight segment fits. `ortho_align_attachments` seats such a pair on one coordinate before the search: halfway between the two centres, clamped into what both faces can hold, which is symmetric in the pair, so a net and its reverse ask for the same line rather than two a step apart. An inscribed end can hold one coordinate and no other, so it is the far end that comes to the glyph. A net phase 1 gave a corridor is left alone, because the corridor is the shape phase 1 asked for.

**This was P7c's, and the implementation moved it.** Written as a nudging pass it cannot work: sliding an end along its face is a move on a segment anchored on a box border, which the stage above excludes for good reason, and the one shape it exists to fix — a route leaving straight out — cannot slide at all without a manufactured bend. Chosen before the search instead it costs nothing, and the search then produces the route from the right point. It lives in the router because the face rule does, and `straight` has no faces.

**Three rules keep it honest.** A glyph **inscribed** in its box — a disc or a diamond — meets an axis-aligned route at one point per face, so its ends take the face midpoint whatever they are aimed at; `RouteInput` carries the fact per obstacle and phase 3 reads it off the `StateKind`, which is the same per-kind knowledge `kind_min_*` already is (§11.4). **A midpoint an arrival and a departure both want is then moved a face rather than slid along one**, because sliding it off the midpoint is the one thing the rule above forbids: a mark has four attachable points and two directions, so `ortho_reface_attachments` sends the direction with the most to gain on the axis the escape rule did not pick — a tie going to the departures — onto the nearer of the other axis's two faces, tries the remaining faces in a fixed order where that one is mixed as well, and leaves the pair stacked where every other face is taken. Deciding it from the set of seats on a face rather than from whichever seat asked is what keeps it independent of the order the nets arrive in.

And where two ends of a filled box still want one seat, `ortho_spread_attachments` separates them **by the direction each net travels through the face**: out through a right or a bottom face runs +, in through a left or a top face runs + as well, and the net running + takes the lower seat at both of its ends. At any one face that is exactly an arrival against a departure — everything arriving at a point is one fan-in and everything leaving is one fan-out, each a trunk §11.5's bundles exist to keep whole, and what no trunk explains is an arrival and a departure together, where the head is inked along the other route's own first leg and reads as belonging to it. What keying it to the travel direction buys over keying it to the end is that a pair seated apart at one box is seated the same way apart at the other, so two states each other's target come out two parallel lines and not two jogs. **And one sweep is not enough**: several projections clamp to the same corner inset, so a sweep can seat one group's arrivals exactly where another group's departure sits, and the pass repeats until nothing moves.

*Measured over the corpus.* No space requests, the seating against the box centre: bends **587 → 563**, corridor **62,061 → 37,018**, excess 956,258 → 961,229, crossings 85 → 102. Under real text, arrivals meeting a departure on one point **116 → 16**, ends of one direction sharing a point 78 → 19, route segments 917 → 868, labels over another transition's route 9 → 2, and the centred label fallbacks 14 → 12 of 192. The crossings go up because the routes that were drawn inside one another could not cross: total rendered overlap falls 157,017 → 119,671 grid units over the same charts, which is the trade taken deliberately — a crossing is legible and a hidden line is a transition the reader never sees.

*Measured again, the three seating passes above against the projection alone.* No space requests: bends **563 → 497**, corridor 37,018 → 34,586, crossings 102 → **91**, excess 961,229 → **924,810**, `t2` 1.148931e9 → 1.148664e9, Tier 0 zero throughout and nothing degraded or re-seated. Under real text: bends 563 → **426**, corridor 30,410 → **16,494**, `label_near` 41,853 → **28,247**, crossings 98 → 101, excess 1,567,636 → 1,583,984. On the rendered scale `tools/audit.py` reads **an arrowhead over another route's end 16 → 0 of 257** — the whole of the class, on every corpus chart and every element chart — routes sharing a run 19 → 12, route segments 868 → **731**, and a label nearer a stranger's line than its own 95 → 84 of 203. **What it costs is the label placement's room.** A straight net offers one leg where a jogged one offered three, so §11.9's five strips a leg run out sooner: the centred fallbacks go 12 → **25** of 192 and `label` 26 → 51, of which the rendered part is labels over another route 2 → 8 while labels over a state box fall 7 → 5. Corpus area moves 0.04% with them, since the chart rect covers every route point and placed box. That is a trade P9's calibration weighs and this phase does not: a jog the reader has to follow is worse than a label the reader finds in the middle of its own line.

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

**Degenerate enclosure is answered by inflation, and only `unreachable` triggers it.** `layout_run` raises `rank_sep`, `node_sep` and `sub_sep` by `spacing_inflation_increment` on a copy of the caller's profile and re-runs phases 1–3, up to `spacing_inflation_cap` times, stopping at the first attempt with nothing unreachable and keeping the attempt that degraded least — ties to the earliest. `outside_region` is a disagreement between phase 3's plan and the region it handed the router, and `too_large` is a graph budget; neither moves with spacing, so neither retries. A copy the validator rejects ends the retries, as does a `size_layout` that leaves the domain, and the last geometry that succeeded stands. The digest hashes the caller's profile, never the copy, so a retry cannot move a golden.

Whatever is still degraded at the end is written as the straight line and marked: one `RouteDegraded` per transition, subject `(Transition, ordinal)`, ordinal-ordered, on `scav_chart_diag`, under `SCAV_OK`. **Measured: the corpus and both 2k shapes inflate zero times with no space requests, and the corpus stays at zero under real text — `scav render` writes nothing to stderr for any of the eleven.** The shape that does not is a fork bar one rank ahead of a composite with `pad` 16, `rank_sep` 0 and `node_sep` 576: the clearance is a third of `node_sep`, so the composite's frame reaches 192 units past its own border onto a bar that spans it top to bottom, and the bar walls the frame off from the route into it. Three inflations of 32 units clear it — the rank gap outgrows the clearance and the bar leaves the frame's obstacle set.

**Tier 2 —** every weight is an integer with a documented ceiling, and every term is scored in the unit the profile already names it in: `em` below is `font_size_grid`, `/` is `ceil_div`, and P9a is where that landed (below). The largest term is `w_area * bounding_box_area / em²`; area is at most `(2*COORD_MAX)^2 ≈ 2^40` before the division, so with weights capped at `2^10` the sum stays under `2^53` — inside `int64` with room, and the ceiling exists to keep that true rather than to tune anything:

```
w_bends      * bends                                          -- highest in tier
w_corridor   * corridor / em                                  -- collinear overlap, less the trunks
w_crossings  * crossings
w_excess_len * Σ (excess_length * (1 + crossings_of(e))) / em -- excess over min_len(e), §11.9
w_adjacency  * adjacency                                      -- §11.8; excludes fork/join fan-out
w_label      * label                                          -- three components, below
w_label_near * label_near / em                                -- Σ max(0, d_own + h - d_other) per placed box, below
w_aspect     * |w_actual*dar_den - h_actual*dar_num| / em     -- integer aspect deviation
w_area       * bounding_box_area / em²                        -- lowest
```

Weights are integers in **named, versioned profiles**. Express each as an exchange rate ("n means an edge would rather turn n times than cross"), not a bare number. A weight change, a profile change, a font change, or a packer change is an **output-format change**: versioned, golden-tested, reviewable.

**The Tier-2 sum was an area measurement, and that was not a tuning problem.** On the corpus totals `w_area * area` was **99.48%** of `t2` with no space requests and **99.38%** under real text. Every other weighted term rounded to nothing: bends 0.00% on both scales, corridor 0.14% and 0.05%, crossings 0.00%, excess 0.32% and 0.43%, aspect 0.05% and 0.04%, and `label_near` 0.09% on the one scale where it had a multiplicand at all. Over the whole of P7e `t2` moved **−0.35%** — 1.152747e9 → 1.148664e9 — while bends fell 15%, corridor 45%, crossings 12% and excess 43%, with aspect and area untouched. Those four terms were the entire change and the objective reported nothing. §19 and §17 P6 hold that datum; what follows is what P9a did about it.

**It was untunable within the caps, because the terms are not the same degree in the coordinates.** Counts are degree 0 — bends, crossings, adjacency, `label`. Lengths are degree 1 — corridor, `excess_len`, `label_near`, and the aspect deviation. Area is degree 2. So the two ends of the vector sit 10⁷ to 10⁸ apart on real input: area 1.14e9 against 91 crossings with no space requests, 1.46e9 against 51 `label` under real text, and area against bends alone is 2.3×10⁶. One weighted sum over three degrees needs weights spanning that, and the ceiling above is 1024 — three orders of magnitude to cover seven. Raising the ceiling is not the fix; it is the only thing holding the sum inside `int64`.

**Determination, landed at P9a: score every term in the unit the profile already names it in, so a weight is the exchange rate this section says it is.** A count is a count, a length is grid units and the profile states its separations in them, and an area is a product of two lengths — so normalising each term by its own unit before weighting leaves the weights as ratios between comparable quantities, which is the only reading under which "n means an edge would rather turn n times than cross" is a true sentence, and it is what makes the 1024 ceiling mean something. **The unit is the em.** `cost_of` divides the four lengths — corridor, `excess_len`, `label_near`, the aspect deviation — by `font_size_grid`, and the area by its square; counts stay counts, `CostTerms` stays raw grid units, and no weight moved. Both constraints on the unit are met structurally rather than by measurement. **Every division is `ceil_div`**, so a term nonzero in grid units is nonzero after it and no difference `tools/audit.py` can see is quantised away — floor-dividing `label_near` had produced a tie on `dock.scav` between two candidates the audit distinguishes. **And `font_size_grid` is bounded `[1, COORD_MAX/4]` rather than `[0, COORD_MAX/4]`** (§11.15), so unlike the three separations it is not a divisor a valid profile can zero; `cost_of` floors it at 1 besides, its profile arriving unvalidated on that path. **The `rank_sep × node_sep` illustration was rejected** on the second constraint, needing a zero floor of its own, and on a design one: it couples the objective to spacing knobs calibration may want to move independently of it.

**What the em reads at the shipped weights, per scale.** At P9a, over the one candidate the pipeline then produced — no space requests: bends **32.78%**, area **31.95%**, `excess_len` 19.88%, corridor 9.05%, aspect 3.34%, crossings 3.00%; real text: area **33.44%**, `excess_len` **27.78%**, bends 22.94%, `label_near` 6.10%, corridor 3.55%, crossings 2.72%, aspect 2.44%, `label` 1.03%; corpus `t2` 97,039 and 118,851. **P9b's portfolio moved both columns, and moved them the way a chooser would.** No space requests: area **38.45%**, bends **33.65%**, `excess_len` 18.89%, crossings 3.79%, aspect 3.30%, corridor 1.91%. Real text: area **39.51%**, `excess_len` **24.44%**, bends 23.24%, `label_near` 5.59%, crossings 2.92%, aspect 2.36%, corridor 1.30%, `label` 0.63%. Corpus `t2` is **82,736** and **110,705** (§17 P9b). Corridor's share falls by a factor of five on the first scale and area's rises on both, because what a pick buys is the terms a reader reads and what it pays in is canvas — the trade §11.4's compaction is meant to make cheaper and P9d's weights have to price. `adjacency` still has no multiplicand on either scale and no corpus chart to give it one. **`cost_shares` is that table per chart**, in floored basis points — a row sums to 9,995–9,998 rather than 10,000 — committed as `corpus_cost_shares.txt` and `corpus_cost_shares_measured.txt` beside the two cost goldens and written by the same two runs, so the next change to a weight is a number a reviewer watches move rather than a sum they decompose by hand. It owes §6 an overflow guard the sum itself does not: `term × 10,000` against a sum the ceiling above bounds only near `2^53` does not fit `int64`, so both sides shift down above `2^48` — never on real data, and proven rather than argued. Re-scored, the whole of P7e reads **−22.04%** of Tier 2 against the old weights' **−0.354%**, 124,301 → 96,905 on the corpus totals taken as one vector, where the `node_sep` illustration had predicted −24.6%: the same conclusion at a different granularity. It is an output-format change like every other weight change — `profile_version` 4 → 5 on both shipped profiles, and the `t2` column of every cost golden rebased and nothing else in any of them (§17 P9a).

**And the weights decide real trade-offs, wrongly.** The 28-point grid of knobs that already exist — `sweep_count` ∈ {0,1,2,4,8,16,32} × `sm_tiebreak` × `trybox` — says so twice. With no space requests **no grid point Pareto-dominates the shipped setting on any chart**, so the frontier is real and the objective is what chooses on it; an area-normalised objective then picks a different candidate on **7 of 11** charts. Under real text the current weights already pick a different candidate than what ships on **4 of 11** — `axis`, `mill`, `ota`, `vac` — and `axis` is the plainest of them: its shipped candidate is beaten on every term but crossings by `sweep_count = 1, trybox = 0`, at bends 25 → 16, corridor 3,796 → 0, crossings 8 → 9, excess 91,133 → 32,779, `label` 2 → 0, `label_near` 2,061 → 1,319, aspect 76,102 → 32,364, area 71.2M → 61.7M.

Corpus totals under real text — what ships, what the current weights pick over that grid, and what the normalised objective picks:

| | shipped at P9a | current-weight argmin | normalised argmin |
|---|---|---|---|
| bends | 426 | 410 | **393** |
| corridor | 16,494 | 14,372 | **7,184** |
| crossings | 101 | 94 | **92** |
| `excess_len` | 1,583,984 | 1,458,604 | **1,197,094** |
| `label` | 51 | 42 | **13** |
| `label_near` | 28,247 | 27,135 | **23,394** |
| aspect | 277,362 | **196,378** | 277,584 |
| area | 1.465G | **1.440G** | 1.623G, +10.8% |

Rendered and looked at (§11.12), the normalised winners are the more legible drawings, and their visible price is the whitespace in that last row. **Both argmin columns predate the em**: the middle one is the grid-unit scoring P9a replaced, and the right one is the `node_sep` illustration rather than the unit that shipped. **And the first column predates P9b.** What ships now is the portfolio's pick per chart, which takes that column to bends **402**, corridor **5,510**, crossings 101, `excess_len` **1,298,172**, `label` **29**, `label_near` **23,945**, aspect **250,192** and area **1.612G** — six of the nine terms better than the column it replaces, crossings unchanged at 101, `adjacency` still without a multiplicand, and area **+10.1%**, which is about the whitespace the right-hand column priced at +10.8%. Reached by four rows of a phase-2 table and the weights exactly as they were (§17 P9b). The trade the columns price is what P9d's weights have to fit; Level 2 is what chose on it.

**`corridor` is the length two routes draw over each other**, taken per transition pair over every collinear overlapping pair of their segments, so a line three routes lie on is charged three times — once per pair — and a run is charged over exactly the length it is shared for. **A pair's trunk is not charged.** Two routes ending at the same point run into it as one line, and that is a fan a reader sees as one edge rather than as two edges hiding each other: the trunk is every segment inside the pair's common suffix, plus the leg each route takes into that suffix where those two legs lie along one line, and the same from the first point for a fan-out. Everything else is charged as before, including a run the two share before they merge and one they find again after they part. Measured over the corpus under real text, the exemption alone takes `corridor` from 86,310 to 53,143 grid units with no geometry moved at all, and §11.5's nudging then leaves those trunks alone rather than spreading them, for 49,199 — which is where P7c left it. P7e's seating took the same corpus to **16,494** and P9b's picks to **5,510**, which is what the real-text golden and `tools/audit.py` read today, the two computing it from opposite ends (§17 P7c, §17 P9b).

**`label` counts three things per placed box**, and the third is the one a reader notices most: another placed box; a state's geometry, which is its `before`/`after` text bands where that state encloses the transition's source or target and its whole rect where it does not; and a route segment of another transition, tested as a zero-thickness rect so a box that merely touches a leg is free. The ancestor carve-out is the same one §11.14 gives an edge, for the same reason: a label inside the composite its transition runs in is where it belongs, and charging it there makes zero unreachable. The band is not carved out, because that is the state's own name.

**`label_near` prices the label a reader will attach to the wrong line.** For a placed box of request height `h`, `d_own` is the Chebyshev gap from the box's rect to the nearest leg of its own route and `d_other` the gap to the nearest segment of any other transition's; the term charges `max(0, d_own + h - d_other)` per box, and nothing at all where the box's transition has no route or the chart has no other one. Overlap is not the question — `w_label` charges that already, and a box clear of every line can still sit in the wrong channel: PlantUML puts `brew button` between its own arc and the neighbouring `shot done` arrow, touching neither. **The margin is the box's own height because that is one line of its own text**, the smallest separation at which the eye stops having to choose. §11.9's strip matching minimises the shortfall ahead of every other component of its key, so what this term scores is the residual the five strips could not reach.

**Three terms have no nonzero multiplicand on the phase tables, and a fourth had none before P7c.** `w_corridor` priced a shared *corridor*, and a corridor is a channel no registered router had until P7c's nudging found and measured the lanes, so that product was identically zero however the weight was set; since P7c it is the term nudging drives down. `w_label` and `w_label_near` price placed boxes and are scored from `Routes::placed`, which is empty unless the caller made `path_box` space requests — so it is zero on every phase table in §17, all of which score with no space requests, and nonzero the moment the same charts are measured under real text. The first zero is a property of the router; the second is a property of the input, and only the second can be lifted without writing code. `w_adjacency` is the third and the hardest: it prices two sibling submachines a transition joins without their being adjacent, which is §11.8's shape, and its multiplicand is 0 on both scales and for both routers — no corpus chart carries a transition between two concurrent regions of one state, so the shape lives in `gauntlet/regions.scav` and in nothing the cost goldens cover. `w_bends` is not in the same position: a straight-line polyline still turns at every port and bend it passes through, and counting those direction changes is a real measure of how straight an edge came out. **Count them: a phase table with no space requests grades on five of the nine terms before P7c and six after, and a run under real text grades on eight.** Real text lifts `label` and `label_near` and cannot lift `adjacency`. This is a statement about the inputs, not a mode: `Cost` is one struct with the same three fields throughout, and registering a router with channels makes `corridor` nonzero with no other change anywhere.

**`Cost` is scored from PODs, never by re-running layout.** Its inputs are the geometry columns and the `SplitGraph` — rects, points, port slots, and the segment table — so a test hands it two hand-written boxes and one hand-written route and asserts a single term, with no model, no profile beyond the weights under test, and no pipeline. **A cost over incomplete geometry is zero by definition, not an error**: a column shorter than the entities it parallels, or a route span reaching past the points, means there is nothing to score rather than something to diagnose, so a chart that was never laid out scores zero instead of reading past the columns. That also makes the P6 gate mechanical: score P4's committed geometry and P6's, compare the vectors.

**The exact scorer was the pipeline's most expensive stage, and the cheap form of it is exact.** `cost_terms` took **672 ms** on the nested 2k chart, against 28.5 ms for routing, 4.8 ms for sizing and 1.8 ms for ordering — the three of them being the whole of §17 P8's 35.3 ms `layout_run`, so the scorer cost nineteen times the layout it scored. On the flat 2k it was 54 ms against 124 ms of routing, and over the whole corpus 723 µs against 700 µs. **508–599 ms of the nested figure was the Tier-0 `through_box` loop alone**, which is O(pieces × live states × depth): sixteen million `ancestor_or_self` climbs, every one of them evaluated before any geometry test ran. Nothing had noticed because `layout_run` never scores — `cost_terms` is the caller's, and §11.10's search is the first caller that wants it per candidate.

**Scoring per frame is the obvious cut, and it is refuted**, measured on 88 chart × profile × router runs. A frame's segments against that frame's own children misses three classes, all of them on real input. **Pieces leave their frame's rect routinely** — 13 of `axis`'s 49, 50 of `bottler`'s 150, 120 of the nested 2k's 7,840, by up to 2,688 units. **They enter grandchildren**: `gauntlet/regions.scav`'s only two Tier-0 violations are against `Running:motor/Spinning` and `Running:light/Dark`, reached by §11.8's unrouted separator segment, and frame scoping scores them zero — it would delete the one count the element suite exists to hold. **And they enter a sibling region's subtree**: `tcp`'s `Established:inbound/HalfClosed → Closing` clips `Established:outbound/FinSent`, and the same shape appears in `vac` and `bottler`. Two invariants do hold on all 88 runs, and they are what the fix rests on instead: **no piece leaves the box of its frame's owner state, and every child box lies inside its parent's.**

**Determination: hierarchy descent. Landed at P9a.** Ancestry is flattened once per chart — `tin`/`tout` per state from one DFS, after which `cost_ancestor` is two comparisons, 14–22 µs at 2k — and one uniform grid is built per submachine over its live children, side `isqrt(n) + 1` capped at 64, siblings being disjoint by Tier 0, at 25–33 µs. Tier 0 then descends from the root submachines and prunes a subtree the moment the piece misses a child's box, **exact under child ⊆ parent alone**, assuming nothing of §11.5 or §11.8, and it catches `regions.scav`. `corridor` buckets by `(axis, coordinate)`, a shared run implying collinearity; `crossings` takes, per vertical, the band of horizontals whose y lies strictly inside its span, with a brute-force fallback for the degraded diagonal pieces; and `box_overlap` goes through the same grid — O(Σ kids²), which is what the flat 2k is left paying. Every sum stays order-independent, so §6 is untouched, and nothing new lands in `Routes`, so `cost_columns` keeps scoring another build's output. Every one of them is `SCAV_INTERNAL`, with ten unit tests on them, and `cost.cpp` is at 100% of lines and 97% of branches. **Measured, median of 5, release, one thread: nested 2k 655 → 3.2 ms, flat 2k 51.4 → 0.62 ms, corpus ×10 5.78 → 1.22 ms, with identical `CostTerms` on all 88 chart × profile × router runs** — `straight`'s nonzero `through_box` included — and the 508 ms was `ancestor_or_self` evaluated before any geometry test, as predicted. The floors behind `SCAV_PERF_ASSERT_FLOOR` are 20 ms on each 2k chart, which a per-piece sweep over every state fails; the times themselves are informational (§5).

**The 3.2 ms is the orthogonal router's, and the same chart under `straight` scores in about 160 ms** — measured at P9b, and the scorer's one known quadratic case. A chord router draws every net as a straight line, so nearly every piece is diagonal and `crossings` falls back to the brute force it keeps for the pieces no axis sorts: 7,840² comparisons rather than a band per vertical. It is bench-only in both senses — every segment the shipped router draws on the corpus is axis-aligned (§17), so the fallback stays empty there, and §11.10's Level 2, the one caller that scores per candidate, gives that chart a single row it never has to rank. Cutting it would mean an index over diagonals, which nothing is asking for.

**Two pieces of the descent did not survive contact with the tree.** **It cannot prune on `enters`.** `enters` answers false for a piece whose two endpoints lie exactly on a box's border while it crosses the interior between them, so an enclosing box can answer false where its own child answers true: the predicate is not monotone under child ⊆ parent, which is the one property the descent needs of it. What prunes is the piece's bounding box against the rect — `overlaps(span_rect(a, b), r)`, monotone by construction rather than by measurement — and `enters` still decides the charge. **And a tombstone cannot be descended to.** The old flat loop treated a live state under a dead ancestor as an obstacle; a tombstone's rect is all zeros, so a descent prunes its whole subtree before reaching one. `Ancestry::detached` carries those, and any live state no document root reaches, and Tier 0 tests them outright.

**Cross-frame Tier-2 pairs exist on the shipped router, so this is not only a Tier-0 argument.** One crossing and 1,312 corridor units on `readable` — `bottler`'s `valve`↔`cell` and `tcp`'s `inbound`↔`outbound` — every one of them tracing to §11.8's hole, so a per-frame sweep would silently move numbers this document publishes. P9b's pick takes `tcp`'s side of that to zero and leaves `bottler`'s 1,216 standing (§11.8), which changes the number and not the argument. **`label` and `label_near` are what this leaves unbounded**, at O(placed × states) + O(placed × pieces), and the phase tables have no placed boxes to score them on. **Measured at P9a under real text**: `cost_columns` over the corpus is 0.30 ms against 1.0–1.7 ms before, about half of it those two terms — the next term to cut, and not disproportionate at some 14 µs a chart.

Area is deliberately last: minimizing it directly produces crammed blobs with snaking edges, and narrow channels force bends. **Before P9a that was true of the multiplier and false of the influence** — `w_area` is 1, the smallest number in the list, and `w_area * area` was 99% of the sum (above); in ems it is 32% with no space requests and 33% under real text, the largest term but no longer the only one, and the ordering states an intent the arithmetic now respects. For the packing sub-problem the objective is the **scale measure** `SM = min(DAR/w, 1/h)` — held as a rational and **compared by cross-multiplication, never computed**. Its tiebreak order (area, then aspect) is a versioned profile field.

**The surrogate is one object and this section described two** `[OWED, P9e, gated]`. "Crossings from straight-line segments" is geometric; "inversion counting in the layered formulation" is combinatorial, and valid only for a pair of edges sharing one rank gap in one frame. A search cannot have both under one name, and what a search needs is the first. **The surrogate is specified against `SubmachineOrders` and `SizedLayout` and no `Routes`.** Phase 3's planning walk — `route.cpp`'s net planning, which turns a transition into its segments' ends and bend waypoints — is extracted into a `plan_nets()` that both the surrogate and `route_transitions` call, so the surrogate's ports are the router's rather than a second guess at them. Every end is then seated with the router's own exposed primitives, each already pure: `ortho_attach_box`, `ortho_reface_attachments`, `ortho_align_attachments`, `ortho_spread_attachments`, `ortho_clearance`. Each net is then one straight chord, seat → bend waypoints → seat. Whole-chart complexity `O(|E| log|V| + |E| log|B|)`. **It is built only if P9e's gate says so** (§17): Level 1's strategy table is scored on the exact function first, and this object and its ranking test below are bought only where that measurement leaves a defect a per-frame ordering choice reaches.

**Which terms that gets, per term rather than as "cheap".** **Exact**: `adjacency`, `aspect`, `area`, `box_overlap` — none of them reads a route. **Approximated**: `bends`, from a port-face table over the seats, exact for an unobstructed chord and blind to a detour's turns; `crossings`, by inversions over the seats' **coordinates** per rank gap rather than over `pos`, and without `south_of`'s shared-north tiebreak where the pair's seats sit on different faces — those two changes are what let the count see a folded rank run and the attachment faces (§11.3); `excess_len`, as `Manhattan(chord) − max(direct, Σ path_box.w)`; and `through_box`, the chord predicate, which is this surrogate's own stated purpose above. **Dropped**: `corridor`, `label`, `label_near` — there are no lanes before nudging and no placed boxes before routes. Those three are exactly where the real-text wins above live, which is why §11.10's Level 2 scores the exact function over real routing.

**Three crossing counts exist and they are not interchangeable.**

| count | where | scope | what it drives |
|---|---|---|---|
| layered inversions over `pos` | `order.cpp` | one rank gap in one frame | `reorder_rank`, at O(deg) per delta |
| straight-line between seats | the surrogate | one frame, per candidate | §11.10's Level 1 |
| routed orthogonal | `cost_terms` | whole chart, after nudging | every number this document commits |

**Rule: no decision is taken on the first unless the third can be checked against it.** §11.3's sweep regression is what that rule would have caught.

**The ranking test is not a Kendall τ on `t2`.** Area is exact in the surrogate and, since P9a's em, 31.95% of `t2` with no space requests rather than 99.48% — still its largest single term, and with `aspect` and `adjacency` exact beside it about a third of the sum is the surrogate's for nothing — so that correlation over the knob grid is carried by terms the surrogate does not approximate and says nothing about the four it does. It is a property test (§5) over a seeded in-RAM generator. Candidates are the seed order plus its bounded-move neighbourhood, enumerated in `(move_kind, subject_index, parameter)` order and truncated to K = 32. Every statistic is integer: **Tier-0 agreement is exact or the test fails**; top-1 regret is asserted as a rank, the surrogate's argmin lying inside the exact top-⌈K/8⌉; a Kendall τ-b floor over the modelled sub-vector alone — {`bends`, `crossings`, `excess_len`, `adjacency`, `aspect`, `area`} — asserted by cross-multiplication rather than computed (§6); and **per-term τ for `bends` and `crossings` separately, which is the only place the two approximations are actually tested**. On disagreement it prints both `CostTerms` and names the term that changed sign. Beside it, **one committed golden row per chart** — τ and top-1 regret — next to `corpus_routers.txt`: the property test catches the class, the golden catches the drift, and neither answers the other's question (§5).

**And "delta-evaluate, never re-score a submachine after a bounded move" is unachievable for any surrogate that reads `SizedLayout`.** Bottom-up sizing has no locality (§11.4): one bounded move resizes every ancestor to the root and moves every seat under it, so there is no bounded region left to re-score. §11.10 says what replaces it.

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

**[OWED] That middle segment does not route, and the corpus never asked it to.** "The parent's frame" has no referent: two concurrent submachines are siblings under a *state*, and the nearest enclosing *submachine* is the one that state itself sits in — one level further out than the state. In that frame the state is an ordinary obstacle, so it walls the route out of the space between its own two regions and the route goes the long way round it, through whatever else the frame holds and back along the line it arrived on. Measured on `gauntlet/regions.scav`: **two edges through a box and two routes doubling back**, at both shipped profiles. **P9b's portfolio routes around that count rather than closing it**: at the shipped M the packer choice puts the two regions where the long way round clips nothing, so what ships reads **0** edges through a box at both profiles and **row 0 alone still reads 2** — pinned there, and read back by `cost_columns` from the other end, because the shape is untouched and only the arrangement moved. The doubling back went the other way: the arrangement the packer picked folds the route back twice each way instead of once, so the reversing legs are **4** as shipped against row 0's 2, and that count is what the axis-aligned property still carves this chart out for (§5, §17 P9b). A hole a pick can step over is a hole. No corpus chart carries the shape — eleven charts and not one transition between two regions of one state — so P7b's Tier-0 zero is a zero over inputs that never exercise §11.8's own case, and it took the element suite (§5) to find that.

The fix §11.14 already implies is **per-net rather than per-frame excusal**: a box that strictly contains both of a net's ends is a box that net cannot avoid, and blocking it buys a detour rather than a diagram. The cost is that the obstacle set stops being one per frame, so the orthogonal router's one grid per frame becomes one per distinct exemption set — cheap, since a frame has one or two, but not free, and it is the router's grid cache rather than a rule change. Scheduled with P9, which is the first phase that has a reason to re-enter the router.

**The corpus carries a milder form of the same hole, and that one is charged.** A transition *leaving* a concurrent region crosses no separator, so phase 0 splits it at the owner *state's* border — and that border lies outside the region's own rect, so the segment is routed in the region's frame and has to cross whatever the sibling region holds to reach it. The sibling's routes are in another frame and so in no obstacle set this one was given, which leaves the charge to the scorer: measured with no space requests at `readable`, **every cross-frame charge in the corpus is this shape — 1 crossing and 1,312 corridor units**, `bottler`'s `valve` against `cell` for 1,216 of it and `tcp`'s `inbound` against `outbound` for 96, and no other pair of frames in the corpus is charged at all. **P9b moved that measurement and not the shape**: `tcp`'s pick takes its corridor and its crossings to zero outright, so its 96 units are gone with whatever crossing they carried, while `bottler`'s row is byte-identical either side and its 1,216 stands. §11.6 compares every route segment against every other regardless of frame, so the hole is priced rather than invisible to the gate; the fix stays owed, and the pair count is re-taken when it lands. `gauntlet/regions.scav` is the same hole at full strength, and its two pinned Tier-0 violations say where it lands: both are the `Lit → Stopped` route entering `Spinning` and `Dark`, which are grandchildren of the frame routing it and so were never in the obstacle set — at both shipped profiles, on the profile's own tuple (above).

A direct arrow wants its two submachines **adjacent**; a third submachine between them means an edge crossing an unrelated submachine rectangle, which is Tier 0. `w_adjacency` prices non-adjacency and "reorder sibling submachines" is already a local-search move — no new algorithm. **Edges incident to a fork or join are excluded from `w_adjacency`**: adjacency is pairwise, so a fan-out above two cannot achieve it under a linear packing, and pricing an unsatisfiable constraint distorts everything else — the same reason `w_excess_len` charges excess only (§11.9). This lets a submachine-crossing transition override source order (§14), which is correct: adjacency for a real edge beats reading order for a rare one.

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

**Labels routinely dominate transition length: a constraint, not a pathology.** ``min_len(e) = max(geometric_min, Σ PathBox widths along the route)``, a hard sizing input. `w_excess_len` charges **excess only** (§11.6) — charging raw length makes the optimizer fight an unwinnable constraint and cram everything else. Rank separation grows to fit the widest label crossing each rank boundary (§11.3).

**A `PathBox`'s size is a phase-1 input; its position is a phase-3 output** (§11.3). Sizing rank separation needs the extent before any coordinate exists, and placement needs a finished route, so the same row answers two questions at two stages. **Placing a `PathBox`** is Kakoulis & Tollis strip matching. Each leg of the route offers two sides, each side five strips one box height apart, and on each strip the box slides from the leg's low end to its high end in steps of its own height, plus the leg's exact centre. A candidate is infeasible if it leaves the chart rect or overlaps a state that encloses neither endpoint, any state's `before`/`after` bands, an already-placed box, another transition's route, or a leg of its own route other than the one it rides; touching is free throughout. The winner is the lexicographic minimum of `(label_near shortfall, Manhattan distance to the centred placement, leg, side, strip, slide)`. **The shortfall comes first** (§11.6): a box crosses to the far side of its own leg, or moves to another of that route's legs, rather than read as the neighbour's label, and the distance decides everything the shortfall ties — so a label already clear and already unambiguous does not move. Boxes are assigned transitions ascending then by `order`, a later box confined to legs at or after the earlier one's and, where that is the same leg, to slide positions further along the route than the earlier one took — further along, not higher in coordinate, since a leg is as often traversed right-to-left or bottom-to-top. NP-hard, so heuristic: a box with no feasible candidate keeps the centred placement, which is **25 of the corpus's 192** under real text, 13% — up from the 12 P7e's aimed attachment left, since its seating passes then straightened the routes further and a straight net offers one leg where a jogged one offered three (§17 P7e). Rip-up-and-reroute of that one edge (§11.5) is the design for those and is **not built**: it is the only part of this that has to re-enter the router, and §11.10's search reaches some of these without doing so, a different admissible arrangement giving a strip the room this one does not. So rip-up is judged after P9b's Level 2, the first stage that offers a box a second arrangement, on what the search leaves rather than on what a single candidate leaves — and these cases are one of the two residues P9e's gate names (§17).

#### 11.9.1 Font metrics

Minimum tables: `head` (`units_per_em`, offset 18), `hhea` (`numberOfHMetrics`, offset 34), `hmtx`, `cmap` (format 4, plus 12), `maxp` (bounds-checking). Advances never come from `glyf`/`CFF`. Bundle exactly one static font. The extension path is `scav_metrics_create` with your own TTF bytes (§16), not runtime substitution — the font is a layout-hash input, so swapping it is an output change.

Three traps: the **`numberOfHMetrics` tail rule** (the last record's advance applies to all remaining glyphs — breaks monospaced fonts specifically, which is what we bundle); vertical metrics disagreeing with themselves (see above); per-glyph rounding.

**Kerning is deliberately ignored** — conservative for Latin (kerning narrows, so boxes over-size and never under-size) — and `render` emits `font-kerning: none` so both sides agree. Missing glyphs **fail loudly**; silent zero-width produces boxes narrower than their text.

#### 11.9.2 RTL and complex scripts — not v1, not blocked

Arabic advance widths are not the sum of per-codepoint `hmtx` values (mandatory cursive joining via `GSUB`, ligatures collapsing codepoints). The would-be show-stopper — measurement as a callback inside layout — is already avoided: the metrics helper is a separate POD-producing entry point, so the fix is to **swap that helper** for a shaping engine (`hamza`, MIT; or HarfBuzz, "Old MIT" — both deterministic). Do not undo that split.

Watch four things: `textLength` must become conditional (`lengthAdjust="spacing"` breaks cursive joining, so RTL leans on `--embed-font`); ignoring kerning degrades from a few percent to absurdly wide; byte-wise collation means codepoint-order sort; bidi (UAX #9) belongs in `measure` and `render` only — the pool stores logical order, which is correct.

Diagram mirroring is an **output transform**, not a second layout algorithm: lay out LTR, x-mirror at the coordinate stage, `render` flips anchors and arrowheads but not glyph order.

### 11.10 Search

**Level 2 is built; Level 1, the three bounded moves and §11.6's surrogate are not.** What the tree does today is P9b's, with P9c's compaction in bit 1 of its table: the fixed table of chart-global phase-2 tuples, the closed-form rule that says how many of its rows a chart runs, the `argmin(Cost, row)` reduction over what they produced, and the phase-1 hoist — each of them below, with what they measured in §17 P9b and §17 P9c. What is **[OWED]** is the per-frame level and everything under it, **gated at P9e** on a measurement taken there. `portfolio_m` is read by Level 2 and `sweep_count` by phase 1's crossing-minimisation sweeps (§11.3); `portfolio_k` is still validated and read by nothing (§0, §11.15). The order — **Level 2 first and Level 1 last** — is this section's, set 2026-09-08: every improvement measured to date is a whole-chart strategy choice rather than a fine-grained ordering move, so the level that chooses between whole charts was built first and the per-frame level and its moves are built only if a measurement says a reader can see what they reach. What P9a landed of all this is the two-level shape itself, written here before anything was built against it.

Local search from a structured seed with **restricted uphill moves** — "simulated sintering", Grover, DAC 1987 (~10% better *and* 3× faster than annealing from random). Not generate-and-test: random restart of a constructive heuristic measures ~2000× worse than guided search at equal CPU (Martí & Laguna), and published metaheuristic layout throughput is ~6–54 candidates/sec at 10–100 nodes. So what ships is a small fixed portfolio around the one constructive pipeline plus bounded moves off what it produced, never a candidate generator with a scorer bolted to its output. **The uphill half is not bought**: acceptance below is strictly improving, because an uphill rule needs a temperature schedule and this section has no measurement to set one from. It is a named lever beyond P9e, wanted only if the bounded moves are built at all and then stall in a local minimum a reader can see.

**The portfolio has two levels, because `Cost` has no per-frame form.** Area and aspect are the root's; routing one frame reads the owner's rect, a region grown past the frame by its own port slots, and an obstacle scan over every state; and §11.4's bottom-up sizing has no locality, so both frame-local moves below resize the frame's box and every ancestor to the root. "`argmin(Cost, strategy_index)` per submachine", which this section specified first, therefore names a quantity that does not exist. Two levels replace it:

- **Level 1 — per frame, on the surrogate (§11.6).** A frame evaluates its own K strategy rows inside its own shard body into a K-slot array and takes `argmin(surrogate, k)` in index order, never against a shared best-so-far (§6). Every candidate gets its own `Frame` and `SegPort` vectors, and the per-shard scratch resets after every *candidate* rather than after the frame — a stale frame-local index read by the next frame in the same shard range would make output a function of shard count. **This whole level is behind P9e's gate**: the table is scored on the exact function over the corpus and the element suite first, and the surrogate is bought only if that measurement finds defects a per-frame ordering choice reaches (§17 P9e).
- **Level 2 — whole chart, on exact `Cost` with real routing. Landed at P9b.** A fixed eight-row table of deltas from the profile the caller passed, over the chart-global phase-2 knobs: **bit 0 of the row index flips the packer choice (`trybox`), bit 1 turns §11.4's compaction on, bit 2 hands each frame its owner's hole** instead of the global `dar_num`/`dar_den`, which `pack_better` otherwise measures every frame at every depth against — `vac`'s 55% occupancy one level down (§11.4). The packer is bit 0 because it is the bit that moves a chart. **Bit 1 was the scale-measure tiebreak until P9c took it**: the tiebreak won on no chart at either scale, so the table was spending a bit on a knob that decided nothing, and `sm_tiebreak` is a profile field no row flips (§11.15). **M = 2 ships today** — {as given, `trybox`} — with compaction's rows 2–3 and the ratio's rows 4–7 built, measured and waiting for weights that can judge what they trade (§17 P9c). **Row 0 is therefore the profile as given**, which is what makes `portfolio_m` of 1 the pipeline as it ran before the portfolio existed, and what a failure is reported for: a row whose sizing leaves the coordinate domain is no candidate, row 0 leaving it is the run's failure reported before anything else was tried, and row 0's diagnostics are the run's. Each row runs phases 2 and 3 whole, with the spacing-inflation retry a lone run has, so what is scored is the retried geometry rather than the attempt that asked for it — and scored against the caller's profile rather than the row's copy, so two rows are compared on one scale even where one of them inflated. The M results are **collected and reduced afterwards**, never folded into a best-so-far a row could mutate (§6), and `cost_less` being strict keeps a tie at the lower row. **The chart-rect cover pass runs inside a candidate, ahead of its score**: `SizedLayout::chart` bounds the root submachine, while a route bends into a frame's padding and a path box centres on one, so `area` and `aspect` have to price the grown rect that `cost_columns`, every golden and `tools/audit.py` read. `bottler` with no space requests is the chart that says why — scored before the grow, it prefers a row whose canvas is larger. `gauntlet/crowd.scav` is the element case for bit 0: `trybox` is the whole lever on it, and on `brew`, while `sweep_count` moves neither (§11.4, §17 P9a).

**Level 2 is affordable, and it is where the real-text wins are.** Exact scoring is 3.2 ms at 2k since §11.6's restructuring landed and routing is 0.7 ms over the whole corpus, so a handful of exact evaluations cost about what one used to. It has to be the exact scorer: the surrogate cannot see `corridor`, `label` or `label_near` at all, and those are the terms a reader reads. Over the 28-point grid of knobs that already exist, under real text, the normalised argmin takes label overlaps **51 → 13**, `corridor` **16,494 → 7,184** and excess **1,583,984 → 1,197,094** over the corpus at **+10.8% area** — which is the trade P9d's weights have to price, not a free win. **What four rows and the shipped weights took, at P9b**: label **51 → 29**, corridor **16,494 → 5,510**, excess **1,583,984 → 1,298,172**, at **+10.1% area** — the same trade at the same price, chosen by the objective the tree has rather than by an illustration of one (§17 P9b).

**What search runs inside, informational** (one thread, `readable`, no space requests, this machine): the nested 2k `layout_run` is 35.3 ms — order 1.8, size 4.8, route 28.5 — and the flat 2k routes in 124 ms; over the whole corpus order is 192 µs, size 779 µs, route 700 µs. `cost_terms` was **655 ms** on the nested 2k before §11.6's restructuring and **3.2 ms** after, which is what makes level 2 a design rather than a wish. §5 keeps a clock out of a gate, so these are reported; what P9b publishes is the closed form below rather than these numbers.

**And what it cost, measured at P9b** (median of 5, quiet machine, one thread, `orthogonal`, `layout_run` end to end): the corpus goes **2.11 → 6.30 ms** at M = 4, which is 3.0× rather than 4× because the load, `decompose` and the hoisted phase 1 are all outside the loop; the nested 2k reads 33.5 → 31.5 ms and the flat 2k 121 → 116 ms, **both unchanged**, because the closed form below gives them one row and one row is never scored — `argmin` over a single candidate is that candidate. No performance floor moved (§5). The 2k pair paying nothing is the whole point of a model-derived M. The shipped M is 2 from P9c, so what the corpus pays today is two of those rows rather than four (§17 P9c).

**The K strategies are the first K rows of a fixed, versioned table, and nothing is seeded.** A row is a phase-1 knob tuple: sweep count (0 included), sweep start direction (predecessors or successors), the pull-right ranking pass on or off, and initial in-rank order (document order or DFS discovery). Changing the table bumps `profile_version`, and **no search constant lives outside the profile or that table** — the inputs digest hashes all 48 profile fields (§6), so a constant that is neither is one no golden can see. **"Seeded by submachine id" is dropped**: `SubmachineId` is an ordinal, so a re-parse after inserting one state renumbers every later submachine, every later frame then draws a different seed, and the diagram cascades — against §11.11 and against P9's own exit clause about a one-state edit. If randomness is ever wanted it is `rnd(seed = the model's structural digest, phase = a constant per kind, item = submachine ordinal, step = candidate index)`, seeded from the model and never from a build constant (§6).

**How much search a frame gets is closed form and model-derived — never a wall clock, never a thread count.** With n the frame's ordering-node count after chaining (§11.3), `k_frame = max(1, portfolio_k >> max(0, ilog2(n) - 6))` and `sweeps_frame` the same, so a frame of ≤64 nodes gets the full K and the flat 2k frame gets one candidate. **M is the built half of the rule** (P9b): `m_chart = max(1, portfolio_m >> max(0, ilog2(entity_count) − 9))`, capped at the table's row count, `entity_count` being every row of the three entity arrays as §6's sharding reads it. So a chart under 1,024 entities runs the whole of M — every corpus chart, the largest of them an order of magnitude short of the first shift — and either 2k shape runs one row, the flat one carrying 4,223 entities. **Said plainly: the largest chart, like the largest frame, gets the least search.** That is backwards for quality and right for latency, and it is the honest form of §11.11's bet — the shape that cannot be searched fast is searched least, rather than given a second mode. Timing it instead would put one chart at two costs on two machines.

**Three moves survive.** Frame-local: **swap adjacent in rank**, and **move a node across ranks**, the second of which can flip §11.4's fold, so its frame is re-sized. Chart-level and serial: **reorder sibling submachines**, subject the owner state's ordinal, parameter an adjacent-transposition index in the submachine span order phase 2 feeds the packer, scored exact after phase 2 — the move §11.8 has wanted since `w_adjacency` was written. **All three are behind P9e's gate too**, and for a blunter reason than the surrogate: nothing measured so far says a bounded move buys a reader anything, so they are built on the strength of that measurement rather than ahead of it (§17 P9e). Per sweep:

- **Enumerate into a dense index** by prefix sum over `(move_kind, subject_index, parameter)`, **every slot written**, an infeasible move writing a sentinel, so the array is shard-split-proof; `parallel_for` over `layout_shard_count(c)` shards, each writing only its own slot range, and no reduction.
- **Then one serial index-ordered pass accepts**: a move is taken iff its delta is negative and its subject set is disjoint from every move already accepted this sweep, per-subject best-improvement choosing the parameter, ties broken `(delta, move_kind, subject_index)`. That is what reconciles "best-improvement per sweep" with "acceptance is a serial index-ordered pass", which as first written contradicted — one accepts one move per sweep and the other accepts many.
- **The empty-rank squeeze re-runs at end of sweep**, in index order, after any cross-rank move; without it phase 2 sizes a phantom rank gap. Rank is recovered from the finished rects as sorted distinct x (§11.7a), so a sweep must not leave two ranks at one x either.

**The inner loop delta-evaluates the coordinate-free order cost, and that replaces "never re-score".** Inversions on `pos`, `O(deg)` per adjacent swap and exact rather than approximate, with the frame re-sized once per accepted sweep instead of once per move. "Never re-score a submachine after a bounded move" was written against a per-frame `Cost` that does not exist; a per-sweep resize is the buildable form of the same intent, and it is what holds a sweep at `O(Σ deg)`.

**Two moves are deleted rather than deferred.** **Flip port side** goes because a port's side is derived and never chosen: `size.cpp` puts a boundary node's x at 0 or the frame width by out-degree, and `route.cpp` reads side 0 or 1 off `source_node`. The only lever near it is the `reversed` bit, which is cycle breaking's — §11.3's unbought Eades–Lin–Smyth — so if it is wanted it is a cycle-breaking row in the strategy table and not a move. **Rotate subtree** goes because ranks run in +x by construction (§11.3) and §11.7a's rank recovery from rects depends on that; the one rotation-shaped choice the pipeline has, §11.4's fold, is already a two-way candidate in phase 2 arbitrated by the scale measure.

**Phase 1 and its search run once, outside both loops. Landed at P9b.** Phase 1 reads exactly one profile field (`sweep_count`) and no extent, so it is separation-invariant: `layout_run` used to recompute an identical `SubmachineOrders` on each of the eight spacing-inflation attempts it is allowed (§11.6), and a candidate loop would have recomputed it M times besides. It is computed once now, ahead of both, and every attempt and every row sizes and routes against that one. **The property is its own test rather than a golden's**: `order_tests.cpp` orders one chart at `readable` and at `readable` with all three separations widened by eight increments and holds the two results equal vector by vector — nodes, edges, both spans, the rank counts, the boundary gaps and the three parallel index arrays. No golden moved on either scale when it landed, which is the byte-level proof that it had been invariant all along. Chart-level moves come after phase 2 and so live inside an attempt: they are **replayed on each inflated attempt, not re-searched**, which keeps "the attempt that degraded least" a comparison between attempts of one topology instead of between two different diagrams.

**An improvement loop owes a counted statistic, not a diagnostic** (§6): sweeps used, moves accepted, and the frames that hit the cap while still improving. A cap reached is the normal end of a loop that was working, and diagnostics are part of the golden artifact, so a code for it would fire on well-behaved charts and rebase every golden on each calibration pass. The cap is still a profile field, still fixed, and still stated.

### 11.11 One algorithm, stable by construction

**There is no `quick` mode and no `polish` mode.** Layout runs one algorithm, always, and is a pure function of `(model, spaces, profile)`. No warm start, no prior layout, no incremental dirty-region path, no persisted cache.

**Stability is a property of the algorithms, not a cost term.** A small model change yields a small diagram change because each stage is order-preserving and deterministic — LR-rectpacking preserves input order (§11.4), ranking and ordering have total-order tie-breaks (§6), document order drives reading order (§14).

That removes `w_st`, `PriorLayout` and its version key, per-loader hysteresis, the sticky packer bit, dirty-region tracking, and VPSC (§11.13) — plus a class of defects: layout depending on edit history, and a golden hash needing a "cold-start" qualifier to mean anything.

**Honest limit.** Stability-by-construction is not a guarantee. A one-node change can flip a crossing-minimisation decision and cascade, and no ordering discipline prevents that in general. Those are the boundary conditions **hints** exist for (§14): when the engine makes a defensible choice the author dislikes, the author pins it rather than the engine remembering what it did last time. **§11.10's portfolio is a second source of the same discontinuity**, landed at P9b: an `argmin` over M candidates means one unit in one term can move the pick and reflow the whole chart rather than one frame. That is the same bet and the same remedy — the pick is a function of the model, so it is reproducible rather than sticky — and P9d's exit clause about a one-state edit is where it is measured.

**This is a bet on speed**, and it should be measured rather than assumed: full layout must be fast enough at 2k states that no second mode is wanted. If it is not, the answer is to make layout faster — not to reintroduce a mode, which trades a performance problem for a correctness one.

### 11.12 Quality baseline

The likeliest failure is producing layouts that score well on `Cost` and that readers find worse than the PlantUML output they already have. Nothing in a cost vector detects this.

**A side-by-side harness ships at P5b**: the same chart through PlantUML, elkjs, and scav. PlantUML rather than the `dot -Tsvg` this first named, because the `.puml` files scav replaces are rendered by exactly that binary — the incumbent itself, not a stand-in for one. Its state-diagram syntax carries composite states, concurrent regions and every pseudostate kind, so the translation is mechanical; `--puml` points the harness at real sources to remove even that. `-Playout=smetana` keeps it off a host Graphviz, which would make the comparison depend on what the machine happens to have installed. Exit criterion is **"no worse than the incumbent on the transcribed corpus"** — not "visually reasonable" — and it is judged at **P9**, not P7, for the two reasons below.

**Tier 0 at zero is a precondition for scoring, not one of the things scored.** Both incumbents route around obstacles and sit at zero edges-through-a-box on every chart by construction, so one violation settles the comparison on the tier compared first and the scores measure the missing router rather than the layout. The review therefore cannot run before a router with an obstacle set does (§11.5).

That also blocks §11.3's global sifting and edge-weight schedule and §11.4's compaction, each of which is written to be bought when review says that term is wrong. **Compaction was bought that way and came back a knob**: the finding the review made turned out to be inside a rect nobody draws, and where the step did fire the page got worse, so it ships as a row of §11.10's table rather than as a step of the packer (§17 P9c).

**Run at P7b. One finding recorded, the rest [OWED] to this document.** §11.4 carries the compaction verdict — `vac` at 55% occupancy — and that is the whole of what survives in writing. The ranking against the incumbents, and whatever the review said about §11.3's sifting and edge-weight schedule, were not written down.

**The scored review is not a P7 gate, and it was never blind.** Decided 2026-09-05, with P7c and P7d landed and the three rendering defects a reviewer would have marked first — labels over names (§11.9), routes sharing a lane (§11.5), glyphs sized by text nobody draws (§11.4) — gone from the page. Two reasons. **Blinding is nominal**: PlantUML rounds its state corners and draws curves, elkjs draws sharp right-angled polylines in a horizontally biased layout, and scav is whichever panel is neither; hiding the labels hides nothing. **And a P7 render is a single candidate, not a layout.** §11.10's search — the portfolio, the local search, the calibrated weights — does not exist yet, so every scav panel is the first admissible layout the pipeline produced, adhering to its constraints and often plainly suboptimal within them. Scoring that against two engines that do optimise measures the missing optimizer, not the design, the way scoring P6 measured the missing router. So the comparison moves to P9's exit, where it is the test of what P9 built. **The side-by-side itself stays the standing instrument**: `tools/baseline.py` and `tools/poster.py` are re-run at every phase and the page is looked at, unblinded, for the defects a cost vector cannot see — that is how the compaction finding above was made and how P7c's shared lanes and P7d's labels were found before they were priced.

**Looked at again on 2026-09-07, before P9, with the search still unbuilt.** The instrument was the existing knob grid rendered under real text through the Python binding, next to the incumbents. Canvas area normalised to points over the eleven charts: scav **5.94M pt²**, 0.43× PlantUML's 13.78M and 0.91× elkjs's 6.54M. Aspect: scav 0.80–1.81 against a DAR of 1.60, PlantUML 0.41–1.81 and mostly around 0.6, elkjs 2.6–8.9. Approximate, since each engine sets its own text — but the gap is outside any font difference, and what it says is that **scav has area to spend and §11.6's weights forbid spending it.** The candidates a normalised objective prefers (§11.6) are the more legible drawings: `brew`'s overprinted `brew button`/`shot done` and its colliding `Standby`↔`Brewing` arrows gone, `toolchanger`'s routes no longer riding the border of the `arm` include's box, `tcp`'s top cluster untangled. The visible price is whitespace, which is what §11.4's compaction and whitespace elimination were for and is why **P9c built them before the weights were fitted**. They reclaimed no canvas: whitespace elimination moves neither extent by construction, and compaction moves two charts at M = 4 and is not a row the shipped M runs, so the canvases P9d fits against are these ones rather than smaller ones (§17 P9c). `tools/audit.py` at that reading, real text: 731 route segments; a label nearer another route than its own 84 of 203; a label over another route 8 of 203; a label over a state box 5 of 203; texts overprinting each other 5 of 403; routes sharing a run 12 of 731 over 16,494 units; a segment flush along a box 3 of 731; and every Tier-0-shaped count zero. **Every count in that list moved at P9b and every one of them down**, the Tier-0-shaped zeros having nowhere to go; §17 P9b carries the current reading, P9c having moved none of them. **And that is the instrument earning its keep**: expansion changed no count, the compaction rows P9c could have shipped changed three of them upward, and looking at the page is what set the shipped M rather than a sum in a golden (§17 P9c). **The scored comparison stays P9d's exit** — this is the standing instrument reporting, not the review.

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

- **Phase 0 (§11.1) suppresses the source-boundary split** for `internal` and `local`. One fewer segment, one fewer port. The derived boundary-crossing count (§7) must reflect this, or `w_excess_len`'s per-crossing multiplier (§11.6) miscounts.
- **Tier 0 (§11.6) carve-out:** an edge may occupy the interior of a state whose border it does not cross, and **only** that state. Every other state and submachine rectangle remains an obstacle.
- **Internal self-loops are the app's, end to end.** The app sums the band it needs into `h_before`/`h_after` and its builder draws the glyphs inside the returned `scav.geom.state_before`/`_after` rect. There is no route, so `PathBox`/`PathClear`/`min_len` do not apply and the router is not involved. Layout sees only two integers.
- **The reference builder distinguishes the three kinds**, pinned in the `drawlist/` golden. scav cannot mandate what a custom builder draws (§2, §3).

### 11.15 The profile

A versioned, hashed artifact (§6), so it needs a field list rather than thirteen scattered references. All integers.

| Group | Fields |
|---|---|
| geometry | `pad` — a box's interior ring only (§11.4) |
| separation | `rank_sep` between adjacent ranks, `node_sep` between adjacent nodes in a rank (§11.3), `sub_sep` between packed sibling submachines (§11.4). Each `[0, COORD_MAX/4]`. Distinct from `pad` because that one is interior and these are between things |
| type | `font_size_grid`, `line_height_k_num`/`_k_den` (`k_den >= 1`). `font_size_grid` is also the em every Tier-2 length and the area are scored in (§11.6), and its `[1, COORD_MAX/4]` bound — the one length field whose floor is not zero — is what makes it a legal divisor |
| pseudostate sizes | per-`StateKind` min extent. `fork`/`join` are wide-and-thin boxes; nothing scales with arity (§7.2) |
| packing | `dar_num`/`dar_den` (each in `[1, 2^10]`), `trybox`, SM tiebreak order — honoured exactly as a caller gives it and flipped by no row of §11.10's table since P9c took bit 1 for compaction (§17 P9c). Compaction is not a field here: it is an argument to `size_layout`, the way the ratio's source is (§11.4) |
| cost | the nine Tier-2 weights, each with a ceiling keeping `Σ Tier-2` inside §11.2's budget. Each term is converted to the unit the profile names it in before its weight applies (§11.6) |
| search | `portfolio_k`, `portfolio_m`, `sweep_count`, `congestion_iterations`, `ripup_cap`, `spacing_inflation_cap` and `spacing_inflation_increment`. **Four of the seven are read today**: `sweep_count`, which phase 1 spends on crossing-minimisation sweeps (§11.3), `portfolio_m`, which is how many rows of §11.10's Level 2 table a chart's size admits, and the two spacing-inflation fields, which drive §11.6's retry. `portfolio_k`, `congestion_iterations` and `ripup_cap` are validated and hashed and consumed by nothing — `portfolio_k` until §11.10's Level 1 at **P9e**, the other two by the stages §11.5 and §11.9 hold as design. **`portfolio_m`** landed at P9a in `[1, 64]` and shipped at 1, riding the `profile_version` bump the em already forced rather than paying for a second output-format change; **P9b is the change that pays** — 4 on both profiles, and the bound tightened to `[1, 8]`, the table's row count, since this section's discipline is reject-out-of-range rather than silently cap and a ninth row would repeat a tuple already run. **P9c takes the shipped value to 2** and leaves the bound where it is: bit 1 is compaction now, and rows 2 and 3 buy `dock` and `vac` a fifth and a twentieth of their Tier 2 at the price of twelve label collisions a reader sees, which is a trade only fitted weights can make (§11.6, §17 P9c). **[OWED]**: the profiles still carry 4, and setting 2 moves geometry, so it carries a `profile_version` bump and the real-text goldens `dock` and `vac` sit in. The cap inside the scaling rule stays as the belt for a profile that path never saw validated, the way `cost_of` floors the em (§11.6). No bound crosses the ABI: the extracted JSON and the generated ctypes carry a field's name, type, offset and size and never its range, so tightening one is not a binding change (§16). `sweep_count` bounds every fixed-count improvement loop, phase 1's sweeps included — one knob because they are one question, splittable at **P9d** if calibration wants different numbers |
| format | `print_columns` — the canonical printer's line-break budget (§15) |
| id | `profile_id`, `profile_version` — **6** on both shipped profiles, `portfolio_m`'s value being the field that last moved it: a shipped number that moves geometry is a version (§17 P9b), and `portfolio_m` moves it again when P9c's shipped 2 lands (above) |

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

**Source order is the primary hint and costs no syntax.** LR-rectpacking is order-preserving, so model order maps to reading order. Consequently **document order must survive parse → model → layout, and the canonical printer must never reorder states or submachines.** Attributes may be sorted; structure may not. (This is the opposite of `puml2c`, which sorts states alphabetically — that sort belongs in the codegen backend.) `w_adjacency` (§11.8) may override source order for submachine-crossing transitions.

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

**Every caller-owned POD crosses with its own size beside it, and a disagreement is `SCAV_E_ABI`.** `scav_profile_named(name, out, out_size)`, `scav_profile_validate(p, p_size)`, `scav_layout_run(chart, spaces, spaces_size, opts, opts_size, placed, placed_cap, placed_size, out_count)`, `scav_measure_chart`, `scav_emit_chart`, `scav_svg_write`, `scav_svg_bounds`, `scav_chart_diag`, `scav_measure_text`, `scav_measure_block`, `scav_image_extent`, `scav_palette_standard`: wherever the library reads or writes a struct or a row the caller allocated, the caller states `sizeof` as it compiled it, and the check runs **before any other argument is looked at and whether or not the pointer is `NULL`** — a count query with `rows = NULL, cap = 0` still declares the stride it will read with, and a caller whose header disagrees has said nothing scav can act on, so nothing is written and no finding is recorded. A nested POD is covered by the outer size (`scav_layout_opts` holds the profile; one header, one layout). `scav_spaces` declares the stride of each of its four tables in four members rather than eight more parameters on the two entry points that read it: a member the caller's header lacks reads as zero and is refused, and the four landed in the padding the pointer-and-count pairs left, so on LP64 the struct is the 64 bytes it was and **no struct in this ABI has padding anywhere**. Strides are ABI facts, not layout inputs — `spaces_digest` never sees one and no golden moved. In the other direction, every array scav hands out carries `out_stride` the way `scav_column_data` always did — `scav_drawlist_prims/styles/points/clips`, `scav_load_pending` — so a binding asserts it against its own `sizeof` at the call. Byte buffers (`scav_chart_digest`, `scav_svg_write`'s output, `scav_load_add`) take no size: a `scav_byte` has no layout to disagree about. The Python binding passes `ctypes.sizeof(T)` by hand at every call site and the generator is unchanged — injecting the sizes would hide an ABI parameter and make the refusal path unreachable from Python. **Why, measured on 2026-09-08:** `functional_tests/test_layout.py` hand-rolled `scav_layout_opts` with a profile one field short; `scav_profile_named` wrote 192 bytes into 184 and `scav_layout_run` read `router` from the bytes past the struct — zero on one Mac, garbage on twelve CI rows, surfacing as `SCAV_E_INVALID_ARG` two calls later. The same struct, passed with its own size, is now `SCAV_E_ABI` on every platform before a byte moves, and that call is the regression test. `scav_abi_version()` → 5.

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

`scav_box_space`, `scav_path_clear`, and `scav_path_box` are the ABI spellings of §8.1's structs, field-for-field. `threads` is the worker count over the sharded phases (§6): 0 means 1, every value is legal so `scav_layout_run` validates it against nothing, and none of them reaches the output. `scav_profile` is the §11.15 field list as a flat POD of `int32_t`.

**No bespoke layout-result type.** Geometry is columns (§11.7a); edge polylines are a `Span` into a points column, already the model's idiom. `Placed[]` stays an out-param only because `PathBox` is 0..N per transition and cannot be a dense per-entity column.

**Routers are exposed by name only.** Function pointers cannot cross: `void* ud` is undescribable in the ABI JSON, routers run on worker threads, and `-fno-exceptions` makes a binding-language exception crossing back UB.

**Machine-readable ABI:** the header is the source of truth; a build-time tool extracts functions, structs, enums, and field offsets to a committed JSON sidecar. Bindings are generated; a golden test asserts extraction matches, so an ABI break is a review diff rather than a downstream segfault.

**The extraction tool is the scraper plus a probe, chosen at P5c.** libclang was the alternative and it lost on the cost §16 already named: provisioning LLVM on six triples that carry a compiler and little else. That cost turned out to be worse than estimated — on darwin the lint gate's clang-tools package *compiles clang from source*, which is what an ABI extractor would have inherited on every row. The scraper needs nothing new, and the probe reports the layout the shipping compiler really produces rather than a second parser's model of one.

**Accepted cost, stated plainly: it cannot answer for a target it cannot execute.** `wasm32-wasi` therefore needs either a runner in the P11 row or a declared fallback, and that is a P11 decision rather than a gap here.

The scraper **fails closed** — a declaration form it does not model is an error, never a silent skip, because skipping one would leave exactly the drift the golden exists to catch. It reads each header the way a C compiler does, with `__cplusplus` undefined, so the `extern "C"` braces and any C++-only section drop out together; a namespace body scraped as if it were ABI is the failure mode that rule prevents. The probe compiles as C++ with the project's own compiler, since that is the toolchain that ships and these are standard-layout PODs; that the headers *also* compile as C is a separate claim, checked by a separate C11 compile of all of them together.

**Padding is recorded, not inferred.** Every struct carries its size, alignment, per-field offsets and total padding, so a struct that grows a hole reads as an ABI break in the diff. Only `scav_spaces` had any — 16 bytes, from four pointer-and-count pairs on LP64, which is exactly the case an inferring reader would have got wrong — until its four stride members (above) filled them on 2026-09-08; the extraction now reads zero padding on every struct, and a nonzero total anywhere is a break.

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
| single-threaded execution produces byte-identical output | **built** — §6's null shim backend, and the determinism class asserts every geometry column identical at one worker and at sixteen |
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

**Every measurement in these phase tables is scored with no space requests** — §11.6's terms over `readable()` and an empty `scav_spaces`, which is what `golden/layout/corpus_cost.txt` holds and what makes the P4, P6 and P7b columns one scale. **From P9b that golden is `cost_columns` over a whole `layout_run`**, portfolio and all, so a row describes the drawing that ships; the columns before it are the single candidate the pipeline then produced. That is a change in what the pipeline is, not in the scale. **Every `t2` in §17 before P9a is the pre-em weighted sum**: the raw terms compare across the whole list, that sum only within the phase that reported it, and `corpus_cost.txt`'s `t2` column reads in ems from P9a on (§11.6). Rendered-quality numbers below are the other scale: real text through `measure_chart`, which is what `scav render`, `dump --layout` and `tools/audit.py` produce. The two are close in route count and far apart in coordinates, so a number from one never checks a claim from the other. Say which scale a number is on, every time.

On the table's scale the corpus routes 257 transitions over **893 segments**, every one of them axis-aligned, and **68%** of routes turn twice or fewer — 587 turns after §11.5's attachment-face rule, against the 684 in the table's `bends` row at P7b. Under real text: 917 segments, 613 turns, 71%. **Those are P7b's counts.** At P8 the same scale reads **803 segments and 497 turns**, with 179 of 257 routes at two turns or fewer (70%), and real text reads **731 segments and 426 turns**, 186 of 257 (72%); the transition count is 257 throughout. At P9b the portfolio's picks take real text to **709 segments and 402 turns** and the no-space turn count to **435** against P8's 497 (§17 P9b).

**Three things measured that the phase list did not name.** The **bend penalty is one rank separation**, not §11.6's exchange rate — that is sixteen grid units on both profiles and buys a staircase wherever the grid offers one. A profile field for it is P9's, so no `profile_version` bump and the golden rebase is the router's alone. **The grid is the product of two line sets, not a function of box count**, so the flat 2k chart needs no sparse graph after all: a packed grid shares columns and rows. What exceeds the budget is boxes at *distinct* offsets, which the router's suite builds deliberately. And routing costs what it costs: the nested 2k went 8 ms to **47 ms**, the flat one to 215 ms, both floors raised to match.

**The separator port is the one shape P7b does not route properly.** It sits on a submachine rect and so inside that submachine's owner, and §11.5 gives the segment to the *parent* frame, where the owner is an obstacle walling off its own port. P7b stubs from the port out to the owner's border and the stub crosses whatever lies between; §11.14 excuses the owner and nothing else. The corpus paid nothing and a synthetic 2k chart of eight depth-16 chains scored **496** violations from these stubs. **The LCA-owned separator channel was to be the fix and turned out not to be needed:** the stub crossed the owner because it left through whichever face sat nearest its target, and §11.5's attachment-face rule sends it out of the flow-facing face instead, which crosses nothing. The count is **0**, asserted as zero rather than as a ceiling. Channels are still §11.5's design for *sharing* a separator corridor; nothing in the tree now demands them for Tier 0.

**P7c — nudging. Landed, and closed.** The phase was three coupled subsystems and is one. **Nudging is built** (§11.5): lanes detected per frame, members ordered by the side they arrive from, integer offsets taken only where the room is known good. **Separator channels are unmotivated** — they were the fix for 496 Tier-0 violations on a synthetic depth-16 shape and the attachment-face rule took those to zero (§11.5). **History-based congestion is unmotivated** — nothing measured demands it, and it is the expensive, sequential, iteration-count-tuned part. Both stay in §11.5 as design; neither is scheduled. The attachment-face rule this phase was going to owe landed early, with P7b's bar transposition that exposed it (§11.5). `w_corridor` gets its first nonzero multiplicand here: a corridor is a channel the current graph does not have, so `corridor` is identically zero on every scale until this lands (§11.6).

**Two routes sharing a run was what that zero cost.** Nothing separated two edges reaching the same lane, so they drew as one polyline fanning out at its ends — **240 pairs over 368,902 grid units** under real text, invisible to `Cost` because `corridor` was the term for exactly this and was zero on every chart. Nudging fixed and priced it in one move: **136 pairs over 86,310 units**, and `corridor` carries a real number for the first time. Of that residual, 1,152 units are lanes nudging made rather than found, down from 11,909 before a displacement was made to keep only the shared runs its legs already had.
*Exit, met:* the corridor term nonzero and then driven down — 355,116 to 93,066 on the scale these tables use. Tier 0 stays zero on the corpus and on both synthetic 2k shapes, every route keeps its arrowhead, no leg collapsed, and no route doubles back over itself. The price of that last one is legs a displacement shortened to nothing much: 11 of the corpus's 917 rendered segments are under two stroke widths against 3 before, four of them a single grid unit, which is a lane still drawn as one lane and counted as two.

**P7c addendum — the routes that already ran as one.** Nudging spread every member of a lane, including the members that were one line by construction: several transitions into one state converge onto a shared trunk, and the stage took that trunk apart into parallel lanes a gap each side — four of them into `bottler`'s `Fault` — while `corridor` charged every unit of it. Both are now keyed on the same fact. §11.6 exempts a pair's trunk — the segments in their common suffix or prefix, plus the two legs into it where those lie along one line — and §11.5 bundles the lane members those runs belong to, so a bundle takes one offset and moves or stays whole. Nothing was added that pulls a route towards a trunk (§11.13): the router is untouched, and total polyline length over the corpus moves **1,123,695 → 1,124,511** grid units, +0.07%, all of it legs a displacement lengthened or shortened.

Three configurations, over the corpus under real text — `scav render` read back by `tools/audit.py`, and `corpus_cost_measured.txt` beside it. The audit's shared run and the scorer's `corridor` are the same number on every row, computed twice from opposite ends; charging the trunks too would add the trunk column back.

| | shared run, pairs/units | merged trunks, pairs/units | `corridor` | `crossings` | `bends` | `excess_len` | `label_near` | U-turns |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| nudging off | 154 / 261,468 | 86 / 107,434 | 261,468 | 67 | 613 | 914,921 | 42,707 | 0 |
| nudging, as it landed | 92 / 53,143 | 44 / 33,167 | 53,143 | 125 | 613 | 2,801,993 | 42,934 | 0 |
| with bundles | **80 / 49,199** | **90 / 107,818** | **49,199** | 124 | 613 | 2,784,161 | 43,082 | 0 |

On the no-space-requests scale the tables above use, `corridor` **93,066 → 62,285**, of which 18,800 is the exemption and the rest the geometry; `crossings` **128 → 104** and `excess_len` **1,775,868 → 1,611,523**, the second following the first; `bends` 587 either way, aspect and area untouched by the bundles, `t2` 1.15488e9 → 1.15275e9. **Keeping a trunk together removes crossings rather than adding them** — the lanes the old spread cut across were its own. Tier 0 stays zero on both scales, no route doubles back and none collapsed, and the corpus's flush segments stay at 4 of 917 rendered and at the then-pinned 2 with no space requests: a bundle takes a whole step where its members took a fraction of one, which walked `mill`'s trunks onto their frame's own border until the room stopped one unit inside it (§11.5). At P8 the pair reads 3 of 731 rendered against a pin of 3.

**These two phases moved aspect and area, and nothing here says why.** The committed golden goes aspect 305,920 → 310,208 and area 1,140,618,240 → 1,142,649,856 across the commit, all of it `ota` — 25,024 → 29,312 and 21,665,792 → 23,697,408 — with the other ten charts bit-identical on both terms. Every A/B this phase and P7d recorded leaves both terms alone and the label placement moves no coordinate at all, so whatever resized `ota` went unrecorded: written down unattributed rather than guessed at.

Nudging finds **30 bundles over the corpus and refuses none of them** as rendered — `axis` 3, `bottler` 4, `dock` 1, `mill` 13, `ota` 2, `tcp` 2, `toolchanger` 4, `vac` 1, and none in `brew`, `estop` or `led`. A refusal is a member whose own checks fail, which the unit tests build and the corpus does not produce.

**P7d — labels, inflation, and the bench. Landed.** `PathBox` strip placement by Kakoulis & Tollis matching is built (§11.9) and called from phase 3 once the routes are final and nudged. **The degenerate-enclosure inflation landed** (§11.6): `layout_run` widens all three separations by `spacing_inflation_increment` and re-runs phases 1–3 up to `spacing_inflation_cap` times when a net comes back unreachable, keeping the attempt that degraded least; what is left is written as a straight line and marked `RouteDegraded` under `SCAV_OK`. The corpus and both 2k shapes inflate zero times, and the synthetic that does not takes three — and that synthetic is not a sealed channel but a frame's region reaching past its owner's border onto an adjacent-rank sibling when `rank_sep` is under the router's clearance, the same margin strip §11.5's nudging had to be walled out of. **The bench landed**: `src/layout/bench_tests.cpp` runs every router the registry holds over the corpus at the readable profile with no space requests, commits the scored table to `golden/layout/corpus_routers.txt`, reports each router's wall clock over the corpus and both 2k shapes as a message under per-router floors, and checks the `RouteOutput` end-to-end join against `straight` — so a third router joins the bench by being registered and nothing else.

**The predicate was wrong before the placement was.** `label` charged a placed box against every live state, the composite its own transition runs inside included, so a label could not score zero wherever it sat — and `tools/audit.py` asked the same question the same way. Under real text that read **155 of 203 labels over a state box**; with the carve-out §11.14 already gives an edge, and with the reserved band of a routeless transition's source excused because that band was reserved for exactly that label, the honest count at the same commit is **32 of 203**. The other 123 were labels sitting inside their own composite, which is where they belong. Two counts were missing beside it: a label over **another transition's route, 77 of 203**, and the corpus's **47 of 403 overprinting strings** (52 before P7c's nudging moved them).

**What the placement bought, on the rendered scale.** Labels over a state box **32 → 9 of 203**, over another transition's route **77 → 9**, overprinting strings **47 → 9 of 403**. `bottler` went 7/12/9 to zero on all three, `toolchanger` 4/7/3 to zero, `mill` 10/32/19 to 3/0/0. What is left is `estop` and `led` — three labels each, on charts small enough that the label is wider than every gap the chart has — plus single boxes on five others.

**The cost golden has its second row**, `golden/layout/corpus_cost_measured.txt`: §11.6's terms over the corpus scored from the geometry columns with the reference builder's measurement and the placed boxes, which is the scale `label` is nonzero on. Placement moves no route and no box, so seven of the eight Tier-2 terms are identical either side of it and the comparison is one column: **`label` 429 → 45**, `t2` 1,480,030,752 → 1,480,021,536. It is not zero because **14 of the corpus's 192 path boxes found no feasible strip** and kept the centred placement; those 14 are what the residual counts above are made of.

**Rip-up was measured rather than built.** The fallback rate decided it: five strips either side of a leg took it from 47 of 192 to 14, and each strip beyond the second bought more than a reroute plausibly would. Re-routing one edge with the placed boxes as obstacles is the only part of §11.9 that has to re-enter the router, it is capped by `ripup_cap` for a reason, and 7% of boxes on charts whose labels are wider than their gaps is not the evidence to spend that on. It stays design in §11.9.
*Exit, met on the term placement moves.* The exit named all eight Tier-2 terms under real text, and there was no real-text row to compare against until this phase wrote one — so it is stated against the pre-placement row of the same golden, where seven terms are unchanged by construction and `label` falls by 90%. Tier 0 stays zero, no coordinate moved, and the drawlist keeps its primitive count on every chart.

**P7d addendum — the label a reader ties to the wrong line.** Nothing priced a box sitting nearer some other transition's route than its own, and nothing in placement preferred the side of its own leg that kept strangers away; PlantUML's `brew button` between its own arc and the neighbouring `shot done` arrow is the shape of it. §11.6 gains a ninth Tier-2 term, `label_near`, and §11.9's key minimises it ahead of every other component — `w_label_near` at `w_corridor`'s value, `profile_version` 4, and a rebase of every digest that hashes the profile. **The term landed one commit before the placement did**, so the real-text golden holds both ends: `label_near` **62,689 → 42,934** grid units over the corpus, `t2` 1,483,030,608 → 1,482,082,344, `label` 45 → 44, and the other seven Tier-2 terms bit-identical, because placement moves no route and no box. Tier 0 stays zero and the centred fallbacks stay at 14 of 192. `tools/audit.py` asks the same question on the rendered scale — a label not one line of its own text nearer its own route than any other — and reads **129 → 104 of 203**, of which the sharp case, strictly nearer somebody else's line, is **62 → 37**. Overprinting strings went 47 → 9 → **8** of 403 and no other count moved on any chart. **Those audit counts are P7d's**; at P8 the same tool reads **84 of 203** nearer a stranger's line and **5 of 403** overprinting, the seating of §17 P7e having moved the routes underneath them. **What is left is structural.** Of the 104, thirty-four sit equidistant from both routes, which is what a shared lane or a crossing produces and no strip can fix — §11.5's residual is 136 shared-run pairs — thirty-three are nearer their own by less than a full line, and the remaining thirty-seven are the sharp case above. Placing 2k boxes went **16 ms to 66 ms** against its 200 ms floor, the key's first component being a distance query per candidate; dropping the out-of-reach segments once per strip band rather than once per candidate is what keeps it there.

**P7e — the combinatorial stage, the attachment, and the element suite. Landed.** What P7c and P7d each left owed, and the suite that says which element owns the next one.

**The combinatorial ordering stage** (§11.5) replaces the low-end projection nudging ordered lanes by. A leg leaving the lane strictly inside another member's extent crosses that member's segment unless the member lies on the leg's far side, so each incidence votes; the lane's order is **a linear extension of the votes**, Kahn's algorithm over their digraph taking the lowest key of the bundles nothing left precedes, and lanes whose votes run round a cycle keep the fewest-contradictions insertion. No space requests: crossings **104 → 85**, excess **1,611,523 → 956,258**. Real text: crossings **124 → 92**, excess 2,784,161 → 1,549,724. Bends 587 and 613, **unchanged on both scales** — the property the stage was specified for. The pseudo-direction pass it was first written with turned out unnecessary, and the case where the crossing order is not a known-good displacement turned out to exist and now has a test. **Insertion one at a time was the linear extension only where the votes are total**, which three staggered extents are not; taking the extension instead reads corridor **37,018 → 36,973** and excess 961,229 → 960,779 over the corpus, crossings and bends unchanged, and the chain that separates the two orders is a nudging fixture rather than a chart.

**The attachment moved off the box centre** (§11.5). `ortho_attach_box` seats an end at its target's projection onto the face the escape rule chose, held one clearance off the corners; an inscribed glyph keeps the face midpoint, `RouteInput` carrying that per obstacle; and `ortho_spread_attachments` separates an arrival from a departure that still want one seat while leaving a fan-in or a fan-out whole. **This was P7c's "spread the ends out" and it is not nudging's**: sliding an end along its face is a move on an anchored segment, and the shape it exists to fix — a route leaving straight out — cannot slide without a manufactured bend. Chosen before the search it costs nothing. Bends **587 → 563**, corridor **62,061 → 37,018**; under real text arrivals meeting a departure on one point **116 → 16**, route segments 917 → 868, labels over another route 9 → 2, centred label fallbacks 14 → **12** of 192.

**And the seating finished, in three parts the review found** (§11.5). A projection of the other box's *centre* is not a projection of the other *end*, so two parallel faces with a run in common still produced two coordinates and a jog between them: `ortho_align_attachments` seats such a pair on one. A mark's mixed midpoint is not §11.3's missing port side but a face the seating never used: `ortho_reface_attachments` moves one direction onto another face of the same glyph. And the spread's sign, keyed to which end a seat was rather than to which way the net runs through the face, seated a pair low at one box and high at the other; keyed to the travel direction it comes out two parallel lines, and the sweep now repeats because one group's move can land on another group's seat. No space requests, against the tree the stage above left: bends **563 → 497**, corridor 36,973 → 34,586, crossings 102 → **91**, excess 960,779 → **924,810**. Real text: bends 563 → **426**, corridor 30,410 → **16,494**, `label_near` 41,853 → **28,247**. `tools/audit.py` reads **an arrowhead over another route's end 16 → 0 of 257**, and the element suite drops the carve-out that counted them. The bill is §11.9's strips: a straight net has one leg where a jogged one had three, so the centred fallbacks go 12 → **25** of 192 and `label` 26 → 51.

**Over the whole of P7e**, against the phase's start: no space requests, bends 587 → **497**, corridor 62,285 → **34,586**, crossings 104 → **91**, excess 1,611,523 → **924,810**, `t2` 1.152747e9 → 1.148664e9, aspect and area untouched. Real text: bends 613 → **426**, corridor 49,199 → **16,494**, crossings 124 → **101**, excess 2,784,161 → **1,583,984**, and `label` 42 → **51** — the one term the phase leaves worse than it found it, which is what the straighter routes cost §11.9's strips and is weighed against `label_near` falling with them. Tier 0 stays zero on the corpus and both 2k shapes.

**The element suite** (§5) is the durable half. Nine charts under `test_data/charts/gauntlet/`, one shape each, held to reader-visible properties at both shipped profiles — Tier 0, axis alignment, no leg reversed, an end on an inscribed glyph at a face midpoint, an endpoint that is also a crossing met at one point rather than two, no arrowhead inked over another route's end, a fork's bar used along its long faces, a fan-in with none of its arrivals hidden inside another. The directory and the array the suite iterates are held to the same list by `functional_tests/test_gauntlet.py`, which also puts every chart through `fmt --check`, `check` and `render`, and `tools/baseline.py --gauntlet` and `tools/audit.py --gauntlet` read the same charts on the rendered scale. It paid for itself on the first run: **§11.8's own case does not route** (two edges through a box, two routes doubling back, and no corpus chart carrying the shape at all), a mark with more than two incident transitions doubled a seat, and a two-state cycle sends one route the long way round the frame at the compact profile. The middle one was misread as §11.3's owed port sides and is not: `marks.scav` has no compound state and so no port, the seat was §11.5's, and the reface above closed it — the property now holds on every chart at both profiles with no carve-out. Reviewing the suite found a fourth: one fork branch leaves through the bar's own 64-unit cap while the 960-unit face beside it goes unused, which is §11.5's face rule reading a dominant y separation exactly as specified. **Every shape a property carves out carries its count and its owning section beside it**, so the next change to one of them is a number that moved; the cycle's shared run excuses no property and is pinned per profile all the same.
*Exit, met:* the two stages P7 named and did not build are built, every corpus golden rebased, and the element suite green with every carve-out in it counted rather than excused.

**P8 — determinism infrastructure. Landed.** The thread axis stops being a column nothing writes to: `threads` reaches two sharded phases, the structural hash gains its seed, and the claim §6 makes about other people's compilers becomes a command they can run.

**The shim** (§6) is `parallel_for(shards, threads, fn)` over pthreads, Win32 or null, chosen by `SCAV_THREAD_BACKEND`. Static striping, the assignment fixed by `(shards, W)` before any shard runs, the caller as worker 0, and a worker whose spawn fails running its own stripe inline — no queue, no atomics, nothing a worker count can reach. **`std::thread` was rejected on `-fno-exceptions`**: its only failure channel is a thrown `std::system_error`, so a transient thread-creation failure would `std::terminate` the host process, and only the OS APIs return the error code the inline fallback is written against. The null backend runs everything inline; it is compiled by no CI row until P11's `wasm32-wasi` one and was built by hand at `-DSCAV_THREAD_BACKEND=NULL` against the full suite, and the Win32 backend, written line for line against the pthread one, gets its first compile on the Windows rows.

**`rnd` and the sharding primitives** landed with it — `rnd(seed, phase, item, step)` over the splitmix64 finalizer, pinned by known-answer vectors against an independent reference and consumed today only by the delay injector, and `shard_count`/`shard_range`, both `constexpr` and both pure functions of the model. §6's shard rule was written `bit_ceil(entity_count / 64)` and is built as `bit_ceil(ceil_div(entity_count, 64))`: the two differ (65 entities is one shard under floor and two under ceiling), and ceiling is the reading that holds a shard to at most 64 entities; §6 now spells it that way.

**Two phases shard.** Phase 1's per-submachine ordering and phase 3's per-frame routing, nudging included, run their bodies through `parallel_for` over `layout_shard_count(c)` shards of `submachines.size()`; each shard writes only its own frames' slots and its own scratch, and a serial index-ordered pass emits and merges exactly as the serial code did. `threads` arrives from `scav_layout_opts` on the first attempt and every spacing-inflation retry. **No golden moved**: every hash and every geometry column is byte-identical at `threads ∈ {0,1,2,3,5,8,13,16}`.

**The structural hash is seeded from the model's digest**, which §6 had owed to this phase in its last sentence. Only the structural column moved, in `corpus_hashes.txt`, `corpus_measured.txt` and the `vac_layout` dumps. It found a collision: `estop.scav` and `led.scav` agreed on **both** hashes, being different models with identical geometry, and the seed splits the structural pair while the coordinate pair still collides — the split working as specified rather than a defect in either.

**The determinism class** is `src/layout/tests/determinism_tests.cpp` (§5): 6 cases, 2,769 assertions over every `scav.geom.*` column's bytes and all three hashes. Corpus shard counts are axis 1, bottler 2, brew 1, dock 1, estop 1, led 1, mill 4, ota 1, tcp 1, toolchanger 2, vac 1 — eight of eleven at one shard, so the corpus alone never reaches the multi-shard path. Both 2k charts shard to 128, and they are not the same test: the nested chart's 129 submachines are what runs frames concurrently, and the flat chart's single submachine is what covers a shard with an empty range — 127 of them, which now return before allocating scratch. Each case asserts which it is. **It has teeth**: making phase 3 skip nudging when `threads > 1` fails four of the six cases on 144 assertions. The 2k and sealed-channel fixtures move to `tests/test_synth.{h,cpp}`, one definition where `layout_tests.cpp` and `bench_tests.cpp` each had their own.

*Timing, informational and not a floor* (testable build, this Mac): the nested 2k chart's `layout_run` goes **35.3 ms at one thread to 16.5 ms at eight** — a little over 2x across its 129 frames, the root one holding eight times the states of any other and bounding what the rest can hide behind it. §5 keeps a clock out of a gate, so this is reported and not asserted.

**`scav selftest [--against FILE]`** (§3.2) embeds the corpus charts and `golden/layout/corpus_hashes.txt` at configure time, loads them through the in-memory transport, and lays each out at `{1,2,3,5,8,13,16}` threads on `readable` with no space requests: `ok <chart> <inputs> <structural> <coordinate>` per chart, or a `FAIL` line naming the column that moved against the golden or the thread count that diverged from `threads = 1` — two line classes, because they are two different bugs. 77 layout runs in about 13 ms wall, 18 functional tests. One incidental fix beneath it: `scav_embed_bytes` now registers its source in `CMAKE_CONFIGURE_DEPENDS`, so an edited chart or font can no longer leave stale bytes in a binary, which was true of the embedded font too.

**Sanitizer configs are not this phase's**, and neither is the standard-library-subset include check. Both predate it — the sanitizer presets and their CI rows landed with PB and the presets generator, and the include check landed with the directory it guards and runs as `functional_tests/test_include_subset.py`, which is what §6 means by enforced. What P8 owes the harness is smaller and it is paid: TSan is green over the whole suite with no addition to `tsan.supp`, and the sweep — testable, release, debug, TSan, UBSan, ASan, coverage, CI-equivalent clang-tidy, and the null backend — is green, with `order.cpp`, `route.cpp` and `scav_thread_pthread.cpp` at 100% of lines and 97.4%, 95.9% and 96.7% of branches.

**Nothing is tiered** (§6). The thread axis is exercised in-process by the determinism class and by `selftest`, both of which are build steps in every row, so every `triple × config × threads` cell is covered on every PR without a CI dimension; a full run is about ten minutes wall with sccache, which is not a budget worth splitting. What the phase adds to `ci.yml` is a nightly `schedule:` trigger, so the same grid runs on `main` daily regardless of pushes — toolchain image drift being the thing it catches, the image being rebuilt weekly.
*Exit, met.* "One structural hash and one coordinate hash across the blocking matrix": the determinism class asserts it inside every row and `selftest` asserts it again from the installed binary. "Full grid green nightly": the schedule trigger, and the grid is the blocking set rather than a tier of it. The `wasm32-wasi` row is still P11's — until then the matrix is the six native triples, and §6's discipline is what makes adding the row a build change rather than a redesign.

**P9a — the docs commit, the scorer, the em, and `crowd.scav`. Landed.** The objective stops being an area measurement and the stage that computes it stops costing nineteen times the layout it scores, in that order and for that reason: calibration cannot start from a scalar one term owns, and a search cannot call a scorer that takes 655 ms.

**The phase opened with documentation and no code**, because §11.10 as first written specified a `flip port side` move over a side nothing chooses, a `rotate subtree` move over an axis §11.7a's rank recovery depends on, a portfolio seeded on `SubmachineId` — an ordinal that renumbers under exactly the one-state edit P9's own exit clause is about — and a per-submachine `argmin(Cost)` for a cost with no per-frame form. Two of the five moves, the per-submachine `argmin(Cost)` and the never-re-score rule were struck and the two-level design written in their place before anything was built against them (§11.10, §11.6, §0).

**The scorer descends the hierarchy** (§11.6). `cost_flatten_ancestry` takes `tin`/`tout` off one DFS, so §11.14's carve-out is two comparisons; `cost_child_grid` builds one uniform grid per submachine over its live children, side `isqrt(n) + 1` capped at 64; `cost_through_boxes` descends from the root submachines; `cost_corridor` buckets by `(axis, coordinate)` with the trunk exemption unchanged; `cost_crossings` takes, per vertical, the band of horizontals whose y lies strictly inside its span and brute-forces the degraded diagonals; `cost_box_overlaps` goes through the same grid. All of them `SCAV_INTERNAL`, ten unit tests, `cost.cpp` at 100% of lines and 97% of branches. Median of 5, release, one thread: `cost_terms` on the nested 2k **655 → 3.2 ms**, flat 2k **51.4 → 0.62 ms**, corpus ×10 **5.78 → 1.22 ms**; under real text `cost_columns` over the corpus is **0.30 ms**, about half of it `label` and `label_near` at O(placed × states) + O(placed × pieces) — the next term to cut, and some 14 µs a chart rather than a disproportion. Floors behind `SCAV_PERF_ASSERT_FLOOR`, not times: `cost_terms` under 20 ms on each 2k chart. **Two pieces of the design did not survive contact, and §11.6 records both.** The descent cannot prune on `enters` — it answers false for a piece whose two endpoints lie exactly on a box's border while it crosses the interior between them, so an enclosing box can answer false where its own child answers true, and the predicate is not monotone under child ⊆ parent; the prune is the piece's bounding box against the rect, monotone by construction, and `enters` still decides the charge. And the descent cannot reach a tombstone, whose rect is all zeros and whose subtree it therefore prunes, so the live states standing under one — and any live state no document root reaches — ride `Ancestry::detached` and are tested outright. The 508 ms was `ancestor_or_self` evaluated before any geometry test, exactly as §11.6 predicted.

**Nothing moved, and 88 runs say so.** The scorer had 22 of its 88 chart × profile × router runs pinned, in `corpus_routers.txt`; the other 66 moved with nothing watching. 58 of them land in `golden/layout/cost_terms.txt` — the corpus at `compact` and the element suite at both profiles, raw terms and the Tier-0 count with **no weighted sum, so a weight change cannot move a row** — and the last 8 are literals in `layout_tests.cpp` for the two scale targets, with the two Tier-0 counts apart rather than summed: `straight` carries `through_box` **11,464 and 12,944** on the nested 2k and **1,996 and 1,998** on the flat one, the counts that say a Tier-0 rewrite still catches what it used to. Every one of the 88 is byte-identical across the restructuring, and `cost_terms.txt` is 62 rows once `crowd.scav` joins the suite.

**Tier 2 is scored in ems** (§11.6, §11.15). `cost_of` divides the four lengths by `font_size_grid` and the area by its square before the weight applies; counts stay counts, `CostTerms` stays raw grid units, and no weight moved. `ceil_div` is what keeps a term nonzero in grid units nonzero after it, and `font_size_grid` is the one length field a valid profile cannot zero — §11.6's two constraints on the unit, met structurally rather than by measurement. What moves is each term's share of the sum. No space requests: bends **32.78%**, area **31.95%**, excess 19.88%, corridor 9.05%, aspect 3.34%, crossings 3.00%. Real text: area **33.44%**, excess **27.78%**, bends 22.94%, `label_near` 6.10%, corridor 3.55%, crossings 2.72%, aspect 2.44%, `label` 1.03%. Against 99.48% and 99.38% for area alone the day before. `cost_shares` writes that table per chart in floored basis points — rows sum to 9,995–9,998 — as the new goldens `corpus_cost_shares.txt` and `corpus_cost_shares_measured.txt`, and it needed the one overflow guard the sum itself does not, `term × 10,000` against the ceiling's bound not fitting `int64`. Corpus `t2` is **97,039** and **118,851**. `profile_version` 4 → 5 on both profiles with `portfolio_m` riding the bump at `[1, 64]`, shipped 1 because Level 2 has one candidate until P9b, which puts the profile at 48 fields and 192 bytes and `scav_layout_opts` at 200 with no padding; both `static_assert`s and the inputs digest read 48, and the ABI JSON and the Python binding were regenerated. **Five golden columns moved and no others, proven column by column**: the `inputs` column of `corpus_hashes.txt` and `corpus_measured.txt`, and the `t2` column of `corpus_cost.txt`, `corpus_cost_measured.txt` and `corpus_routers.txt`. Every structural hash, every coordinate hash and every raw term column is byte-identical; `cost_terms.txt` carries no sum and so does not move at all; the drawlist, SVG and dump goldens are untouched, the dumps carrying structural and coordinate and no inputs digest.

**`crowd.scav` is the element the normalisation is about** (§5), and the suite's tenth chart: a state carrying three labelled transitions, two of them the pair to and from a composite holding two concurrent regions — `Loaded → Ready` "door closed", `Ready → Running` "start button", `Running → Ready` "cycle done", with `drum` and `valve` inside `Running`. Real text at `readable`, which is what ships: `tools/audit.py` reads texts overprinting each other **1 of 10**, a label over another route **1 of 3** and a label nearer another route than its own **2 of 3**, while `cost_columns` reads `label` **4**, `label_near` **442** and area **16,144,032**. The candidate at `sweep_count = 0, trybox = 0` takes every one of those audit counts to 0, `label` to 0 and `label_near` to 172, for **56% more area** at 25,188,336. Under the em it scores `t2` **1,438** against the shipped **1,230**: the conclusion survives — the objective still buys the cramped drawing — but the margin falls from 1.56× to 1.17×, the old one having been area. The two real-text terms are pinned in `functional_drawlist_tests.cpp` as a carve-out owned by §11.6 and P9d; every suite property holds on the chart at both profiles with no space requests, so it carries no carve-out in `gauntlet_tests.cpp`. **`trybox` is the whole lever, here and on `brew`** — `sweep_count` moves neither over {0, 1, 2, 4} — so the fix is a chart-global phase-2 knob, which is exactly what §11.10's Level 2 chooses over. Three things the shrink turned up: the third transition is load-bearing and has to be labelled; it has to *arrive* at the state, `brew`'s departing `Standby → *` reproducing the defect without offering the fix, because only an arriving third label gives the packer two arrangements that differ; and the concurrent regions are load-bearing for the fix rather than for the defect.
*Exit, met:* the exit said the nested 2k scores in **≤3 ms** and the median reads **3.2** — the floor asserted is 20 ms and the number is informational (§5), so it is stated rather than rounded down. `CostTerms` identical to the old scorer on all **88** chart × profile × router runs, a restructuring and not a re-specification, and every one of them now held by a golden or a literal. The share table is committed, one per scale and one row per chart. P6 through P7e re-scored: **−22.04%** of Tier 2 against the old weights' **−0.354%**, 124,301 → 96,905 over the corpus totals taken as one vector, where the illustrative `node_sep` normalisation had read −24.6% (no space requests, the scale these tables use).

**P9b — Level 2 of the portfolio, the hoist, and DAR as a capability. Landed.** Something in this pipeline finally generates candidates: `layout_run` sizes and routes every row of a fixed table of chart-global phase-2 tuples and keeps the one exact `Cost` ranks first. That is §11.10's Level 2, and it is the first time `Cost` has been asked about a second arrangement (§11.4).

**The table is eight rows of deltas from the profile as given.** Bit 0 of the row index flips `trybox`, bit 1 the scale-measure tiebreak — compaction from P9c — and bit 2 hands each frame its owner's hole instead of the profile's ratio — so **row 0 is the caller's own tuple**, and `portfolio_m` of 1 is the pipeline as it ran before any of this. The shipped `portfolio_m` of 4 runs the first four, the two packer knobs crossed with no ratio handed down (2 from P9c): `profile_version` 5 → 6 on both profiles, because a shipped number that moves geometry is a version, and the bound tightened to `[1, 8]`, the table's own row count (§11.15). **The packer takes bit 0 rather than bit 1** because it is the bit that moves a chart; rows 0–3 are the same four tuples either way, so the reorder moved no geometry and moved only the row index a test reads. **P9c retired bit 1 and gave it to compaction**, and took the shipped M down to 2 with it (§17 P9c).

**How much of the table a chart runs is closed form and model-derived** — never a wall clock, never a thread count (§11.10): `m_chart = max(1, portfolio_m >> max(0, ilog2(entity_count) − 9))`, capped at the row count. Every corpus chart runs 4, the largest of them an order of magnitude short of the first shift, and both 2k shapes run 1, the flat one carrying 4,223 entities. Each row's phases shard exactly as they did and the M loop is serial; the reduction **collects** the M results and takes `argmin(Cost, row)` over them in index order, never a best-so-far a row could mutate (§6), with `cost_less` strict so a tie keeps the lower row. A row whose sizing leaves the coordinate domain is no candidate, row 0 leaving it is the run's failure reported before anything else was tried, and row 0's diagnostics are the run's. **One row is not scored at all**, `argmin` over one candidate being that candidate, which is what leaves a 2k chart costing what it did. `layout_run` gained a `tuple` out-param beside `inflations` — no column and no ABI change, nothing across the C boundary asking yet. **And the chart-rect cover pass moved inside a candidate, ahead of its score**, so `area` and `aspect` price the canvas that ships rather than the root submachine's box; `bottler` with no space requests is the chart that says why, preferring a row whose canvas is larger when scored before the grow.

**Phase 1 is hoisted out of both loops, and it moved no byte.** It reads `sweep_count` and no extent, so the eight attempts the inflation loop is allowed had been computing eight identical `SubmachineOrders` and throwing seven away, and a candidate loop would have done that M times over. Byte-identical by construction rather than by measurement, and the property is its own test rather than a golden's: `order_tests.cpp` holds one chart's ordering at `readable` equal, vector by vector, to its ordering at `readable` with all three separations widened by eight increments. Every layout golden held on both scales and the sealed-channel fixture still takes its three inflations — the byte-level proof that separation-invariance was true before anything relied on it. The fold's second layout is not the same kind of win, §11.4 having measured the cut firing on 59 of the corpus's 62 components, and what phase 2 can save across candidates is P9e's caching.

**DAR landed as a capability and is not shipped** (§11.4). `pack_best` takes the ratio explicitly and hands the same pair to `pack_lr`, to the fold's target width and to the scale-measure compare beside it; `size_layout` takes a `DarSource`, and `Profile` is what every caller passes. `OwnerHole` reads the aspect of the owner's interior box between its `before` and `after` bands, reduces it into the profile's own `[1, 1024]` with the longer axis at the cap, leaves root frames and states with no live submachine on the profile's ratio, and **sizes the chart twice**, a hole being unknowable before its owner is sized. Five tests, all hand-built: the ratio's cap, floor and degenerate cases; which states leave a hole and which do not, a tombstoned frame included; a `Choice` owner reserving 100,000 units of height for four unconnected components, which come out two by two at the profile's ratio and in a column at its hole's; a root-only chart identical under both sources rect for rect; and the first pass failing. Rows 4–7 are what would ship it and the shipped M — 4 here, 2 from P9c — does not reach them.

**What Level 2 bought, no space requests: six of the eleven charts move.** `axis`, `brew`, `mill`, `tcp`, `toolchanger` and `vac`, on `corpus_hashes.txt` and `corpus_cost.txt` alike. Corpus Tier 2 **97,039 → 82,736**, −14.7%, with corridor **34,586 → 6,176**, bends 497 → 435, excess 924,810 → 748,946, aspect 310,208 → 261,728 — and the two terms the picks pay in, crossings 91 → 98 and area +2.6%. Per chart, bends / corridor / crossings / excess / aspect / area / `t2`: `axis` 31→23 / 11,744→0 / 4→10 / 30,657→22,468 / 64,704→28,032 / 32.22M→31.40M / 7,277→3,408; `brew` 21→11 / — / 3→1 / 8,519→1,618 / 11,584→384 / 13.26M→27.00M / 2,102→1,509; `mill` 182→150 / 6,506→1,152 / 43→49 / 292,812→180,319 / 88,576→66,208 / 646.3M→619.7M / 39,217→32,717; `tcp` 26→20 / 96→0 / 1→0 / 24,122→4,329 / 8,896→33,408 / 25.20M→36.92M / 3,026→2,722; `toolchanger` 55→53 / 11,216→0 / 8→6 / 59,207→41,460 / 4,224→13,760 / 102.7M→118.9M / 10,674→7,817; `vac` 34→30 / — / 4→4 / 37,319→26,578 / 47,808→35,520 / 65.41M→81.20M / 5,357→5,177.

**Under real text the movers are `axis`, `bottler`, `toolchanger` and `vac` — exactly the four P9a predicted, at the numbers it predicted.** P9a had scored `trybox` × `sm_tiebreak` by hand and named those four and no others, all four to the LR packer alone, at `axis` −13% area, `bottler` +39%, `toolchanger` +35% and `vac` −13%; the portfolio reproduced the list and the terms under it. Corpus `t2` **118,851 → 110,705**, −6.9%, and the whole vector is in §11.6 beside the grid it was compared against. Per chart, bends / corridor / crossings / excess / `label` / `label_near` / aspect / area / `t2`: `axis` 25→16 / 3,796→0 / 8→9 / 91,133→32,779 / 2→0 / 2,061→1,319 / 76,102→32,364 / 71.2M→61.7M / 8,019→4,345; `bottler` 93→84 / 97→0 / 18→16 / 702,368→465,706 / 18→3 / 5,354→2,640 / 29,788→33,828 / 331.9M→462.7M / 32,304→29,243; `toolchanger` 56→52 / 8,653→1,562 / 10→11 / 145,125→157,571 / 4→1 / 2,287→910 / 22,188→70,778 / 115.9M→156.9M / 13,185→12,654; `vac` 28→26 / — / 2→2 / 41,157→37,915 / 2→0 / 1,488→2,019 / 68,214→32,152 / 118.7M→103.8M / 7,080→6,200. **`toolchanger` is the row that says a weighted sum is doing something**: it pays in excess, in aspect and in 35% more canvas and still scores better, because corridor, `label` and `label_near` fell further than those three rose. Golden by golden: `inputs` moves on all eleven charts because the profile did, the structural and coordinate hashes move only where a pick moved, and the drawlist, SVG and dump goldens move with the real-text four — `vac` being the chart the SVG and dump goldens hold whole.

**And a reader can see it.** `tools/audit.py`, real text: routes sharing a run **12 → 6** over **16,494 → 5,510** units, a label over a state box 5 → 3, a label over another route 8 → 4, a label nearer a stranger's line than its own 84 → 82, texts overprinting each other 5 → 4, a segment flush along a box 3 → 1, route segments 731 → 709, and every Tier-0-shaped count still zero. The centred label fallbacks read **16 of 192** against 25, nine boxes having got a clear strip out of the picks (§11.9). None of that is the weights moving: it is four rows of a phase-2 table under the objective the tree already had.

**Every pick that changed is row 1** — `trybox` flipped off, `pack_lr` alone with the row packer never tried — on both scales and on every chart that moved. **`sm_tiebreak` won nothing anywhere**: it decides an exact scale-measure tie and no real chart produces one, so **M = 2 would reach every pick M = 4 finds**. That was recorded here rather than acted on, and **P9c acted on it** — bit 1 is compaction now, so neither half of that sentence describes the table any more: rows 2 and 3 are live under real text, on `dock` and `vac`, and M = 2 is what ships because the trade they make is one the weights price wrongly (§17 P9c). `gauntlet/crowd.scav`, the tenth element chart, is the one whose pick did not change: `label` **4** and `label_near` **442** stand exactly as pinned, so the packing that wins on area is still the one whose labels collide, and that remains the weights' fault rather than the search's (§5, §11.6, P9d).

**M = 8, informational and not shipped.** The DAR rows win on four charts with no space requests — `bottler`, `brew`, `tcp`, `vac`, for −4.1% of the corpus sum — and on five under real text — `axis`, `dock`, `mill`, `toolchanger`, `vac`, for −2.3%. What they buy is area and excess and what they pay in is `label`: `dock` 0 → 12, `mill` 7 → 12, `axis` 0 → 4. `crowd` at M = 8 reads `t2` 1,230 → 983 with `label_near` 442 → 602, which is the trade the weights already make wrongly, made twice. So the capability is built, measured, and left for P9d to judge against fitted weights rather than against these; P9c's canvases turned out to be these same canvases (§17 P9c).

**What it costs** (median of 5, quiet machine, one thread, `orthogonal`, `layout_run` end to end): the corpus goes **2.11 → 6.30 ms** at M = 4, 3.0× rather than 4× because the load, `decompose` and the hoisted phase 1 all sit outside the loop; the nested 2k reads 33.5 → 31.5 ms and the flat 2k 121 → 116 ms, **both unchanged**, because the scaling rule gives them one row and one row is never scored. No floor moved; the 200 ms and 500 ms router floors stand (§5). One measurement for §11.6 came out of the same bench: `cost_terms` on the nested 2k under `straight` is about **160 ms** against 3.2 ms under `orthogonal`, a chord router leaving nearly every piece diagonal so that `crossings` falls back to O(loose × pieces) — 7,840² rather than a band per vertical. Bench-only, and Level 2 pays it nowhere today because that chart runs one row.

**What the portfolio taught the suites** (§5, §11.8). A golden or a suite that claims to describe the diagram has to score what ships; one that pins a component scores the component. Two bodies of tests were describing a candidate nobody draws. **`gauntlet_tests.cpp` ran the phases directly**, so it held its properties to row 0 while `layout_run` delivered another row: `lay` now runs `layout_run` at the shipped profile, re-runs the phases for the row the portfolio kept — the nudge statistics and the per-transition fallback flag have no column of their own — and holds all six geometry columns it produced to the ones the run wrote, word for word, so a property there is a property of the picture. **`corpus_cost.txt` and `corpus_cost_shares.txt` are now `cost_columns` over a whole `layout_run`**, no space requests, `readable`, the shipped M; six rows move, the same six the hash golden moved, and the other five are byte-identical because a chart whose pick is row 0 grows no canvas either. `corpus_routers.txt` and `cost_terms.txt` stay on the raw phases with a one-line reason each — the first is an A/B between routers, so both sides have to be the same candidate, and the second pins the scorer over a fixed candidate so that neither a weight nor a pick can move it — and both are byte-identical across the whole phase, which is what says the routers and the scorer are untouched by the search wrapped around them.

**Four consequences pinned, and one of them is worse.** `regions.scav`'s Tier-0 count is **0 at the shipped M** on both profiles, `cost_columns` agreeing from the other end, while **row 0 alone still reads 2**: §11.8's hole is routed around by a packer choice and not closed, and the row-0 pin is what keeps it visible. Its reversing legs go **2 → 4**, worse rather than better, and are pinned too — the count the axis-aligned property still carves that chart out for. `fork`'s capped branch reads **0** at `readable`, where the pick stacks the branch beside the bar instead of below it, and **1** at `compact`, which ships row 0; §11.5's face rule is unchanged and the chart now states it twice. And `test_render.py`'s oversize chart needed bigger names to reach the last `2*pad` of the coordinate domain, the portfolio keeping the better-scoring row and area being a term.
*Exit, met:* the four real-text charts moved as predicted — `axis`, `bottler`, `toolchanger` and `vac`, all four to the LR packer alone, at P9a's per-term numbers. The determinism class and `selftest` are green at {1,2,3,5,8,13,16} with M = 4, the chosen row carried in the snapshot beside every geometry column so that a worker count which moved the pick is a named failure rather than a reflow to bisect, plus the fixture where every row of the table ties and the reduction's index order is what answers. **The pick is asserted as an index, not a hash** — end to end on `axis` against its four rows run one at a time, each scored the way the driver scores them — a hash saying that something moved and localising nothing. And **2k latency is published as the closed form above** rather than as a number repeated per machine, which is the first of P9's original three clauses met.

**P9c — the spot list, whitespace elimination, and compaction as a table row. Landed.** §11.4's last two LR-rectpacking steps (Domrös et al., IVAPP 2021) were taken before calibration because **the visible price of every legible candidate is whitespace** — under real text `brew` +40%, `crowd` +56%, `toolchanger` +35% and `bottler` +39% area — and these two steps are the published, bounded, search-independent answer to exactly that. Both are built, and the premise did not survive them: whitespace elimination cannot move an extent, compaction reaches three packings in ninety, and the whitespace those charts pay for turns out to be inside their components rather than between them. So one step ships unconditionally, the other ships as a knob the shipped M does not turn on, and the canvases P9d fits against are the ones P9b left.

**The placement is a spot list now, and the geometry is a pure function of it.** `pack_lr` is a target width, then one of `{Right, Subrow, Level, Row}` per rect — the whole of the placement's freedom — then geometry over that list. Every structure the packer can produce is therefore a cut of the input sequence at three levels, and order, non-overlap and `sub_sep` are properties of the structure rather than of the code that walks it: rows of blocks of subrows over consecutive runs cannot express an inversion, so reading order is placement order by construction. That restructuring is what makes the third step expressible at all — compaction is one rect changing its spot and the drawing being re-laid.

**Whitespace elimination is unconditional, and that is a consequence rather than a preference.** Every row grows to the drawing's width, every block to its row's height, every subrow to its block's width and every rect to its subrow's height, with the slack along each axis floor-apportioned from a running prefix so the shares sum to exactly the slack, no two differ by more than one, and every gap stays the gap placement gave it. A rect with no extent is left alone and takes no share, growing an empty submachine into a visible band being an invention rather than a fill. **The step moves neither extent**, so area, aspect, `SM`, the domain check and `pack_better` cannot see it — which is why it is not a table row: `Cost` cannot rank it, so as a row it could only ever tie. `pack_box` is the same three functions over an all-`Right` list, so its rects come back levelled to the tallest.

**Only one of the three call sites has a rect a reader sees, and the census is the finding.** Of the 90 multi-rect packings the corpus and the element suite run, **76 are a fold's pieces, 12 a state's sibling submachines and 2 a frame's components**. A component's rect and a piece's are not boxes — `size.cpp` reads their positions and nothing else — so growing them is invisible, and growing them is the whole of what this step does. The sibling site is the one whose rect is drawn, so only there are the grown extents written back, with the sink boundary node that sat on the old trailing edge moved to the new one.

**What expansion measured is routing, not area.** No space requests, corpus at `readable`: corridor **6,176 → 4,960** (−19.69%), crossings 98 → 97, `excess_len` 748,946 → **729,285** (−2.63%), `t2` 82,736 → **81,960** (−0.94%), area and aspect identical to the digit. `bottler` is the only chart whose score moved and the only structural hash that moved, its grown frames having changed a turn sequence; five coordinate hashes move, because a grown frame is a bigger obstacle and a bigger region wherever it grows. Real text: `excess_len` 1,298,172 → 1,298,856 (+0.05%), `label` 29 → **28**, `label_near` 23,945 → 23,914, `t2` 110,705 → 110,697, with `brew` and `tcp` the only two charts whose score moved and seven coordinate hashes under them. Tier 0 zero on both scales, `cost_terms.txt` byte-identical term for term, and `corpus_routers.txt` moves its one `orthogonal bottler.scav` row — it scores `size_layout` at the caller's own profile, which is what makes it Level-2-proof, and expansion lives inside `size_layout`. `tools/audit.py` changes no count at all. **On the page it is quiet and it is better**: `brew`'s two concurrent regions come out equal width with the divider near the middle of `Brewing` instead of two-thirds across, `vac`'s `PreConfig` diamond sits in the middle of the slack its row had going spare, and `mill` and `toolchanger` are indistinguishable at any zoom that fits the page.

**Compaction is Level 2's bit 1, and `sm_tiebreak` is not a row any more.** Each rect gets one chance, in index order, to take one of the other three spots; the drawing is re-laid and the move is kept only where neither extent grew and one shrank. Dominance is what makes the properties provable rather than tested — area falls, `SM` rises, a packing inside the domain stays inside it, occupancy is monotone because the numerator never changes — while order, non-overlap and `sub_sep` come from the spot list's structure and not from the rule, so they hold whichever row ran. An `O(1)` gate rejects a candidate whose own edge already clears the drawing, that candidate being unable to shrink it, which keeps a packing with nothing to move linear; the worst case is `O(n²)` in one packing's rect count and unmeasurable on either 2k shape. `enum class Compaction { Off, On }` lives in `pack.h` and is threaded through `pack_best` and `size_layout` beside Level 2's `DarSource`, so the knob is an argument and not a profile field — which is what makes the table's own test score a row by running its phases 2 and 3 directly, two of the three knobs being arguments no one-row run could carry. `search_tuple` reads bit 0 as `trybox`, **bit 1 as compaction** and bit 2 as the ratio's source, so the eight rows are those three knobs crossed. The bit compaction took was the scale-measure tiebreak, which P9b measured as winning on no chart at either scale: the table was spending a bit on a knob that decided nothing. `sm_tiebreak` stays a profile field a caller may set, honoured exactly as row 0 gives it and flipped by no row (§11.15).

**Why a knob and not a step.** Unconditional, compaction fires on **2 of the 11 corpus charts and 3 of the 90 multi-rect packings** — `dock`, `vac`'s copy of the same frame, and `gauntlet/mutual`, every one of them a fold's pieces — takes 0.79% off corpus area under real text, and makes the page worse where it fires: `label` over the corpus 29 → 43 and `tools/audit.py`'s overprints 4 → 7, all of it `dock`'s reflow. The thing that got better and the thing that got worse are two terms of one objective and only the whole chart's `Cost` weighs them, which is not a call a packer can make. As a row it costs the nine charts it cannot help nothing at all — they take row 0 or row 1 as they did — and the two it changes become the objective's call.

**Measured at M = 4, and not shipped.** No space requests: byte-identical, every pick the row it was. Compaction fires nowhere on that scale, so rows 2 and 3 tie with rows 0 and 1 and `argmin(value, index)` keeps the lower — `corpus_hashes.txt`, `corpus_cost.txt`, `corpus_cost_shares.txt` and `corpus_routers.txt` all unmoved, the last of them scoring row 0 and unable to see the knob at all. **Real text moves two charts and no others.** Per chart, no space requests: `axis` 1, `bottler` 0, `brew` 1, `dock` 0, `estop` 0, `led` 0, `mill` 1, `ota` 0, `tcp` 1, `toolchanger` 1, `vac` 1 — every one of them a row M = 2 already reaches. Real text: `axis` 1, `bottler` 1, `brew` 0, **`dock` 2**, `estop` 0, `led` 0, `mill` 0, `ota` 0, `tcp` 0, `toolchanger` 1, **`vac` 3**, which at the shipped M of 2 reads `dock` 0 and `vac` 1, exactly P9b's picks. Corpus totals `t2` 110,697 → **109,706** (−0.90%), area −0.79%, `excess_len` −2.28%, bends 402 → 398, `label_near` −1.74% — against **`label` 28 → 42** and aspect +1.33%, Tier 0 zero throughout. `dock` reads `t2` 3,271 → **2,588** (−20.9%) at area −34.4%, which is a knock-on rather than the packing: 3072x2458 → 2810x2458 one level down flips the fold above it, and the chart goes 6548x5646 → 7796x3111. It pays `label` 0 → 12 and aspect +13%. `vac` reads `t2` 6,200 → **5,892** (−5.0%) at identical area for `label` 0 → 2. The audit at M = 4: 705 route segments, 32 merged trunks over 66,683 units, a label over a state box 5, over another route 5, texts overprinting 7, a label nearer a stranger's line than its own 82, and every Tier-0-shaped count zero — every regression in that list `dock`'s.

**The objective bought twelve label collisions for a fifth of `dock`'s Tier 2, and the renders say it bought wrong.** A label overlap costs 24 and an em² of canvas 1 (§11.6), so `dock`'s row-2 drawing wins the sum while printing `vacuum docked` and `vacuum left` over each other and over their own arrows, and `charged` over `Blinking`. So the shipped `portfolio_m` is **2** rather than 4: the table runs {as given, `trybox`}, every pick is the row P9b kept, and no picture P9c ships reads worse than P9b's. Compaction's rows 2–3 are built, measured and unshipped exactly as P9b left the ratio's rows 4–7, and P9d raises M when fitted weights can judge the trade — with `dock` the tripwire beside `crowd.scav`, one chart per direction of the same wrong weight (§11.15, §0). **This is the trade §11.6's weights exist to make**, and the one P9d re-judges once they are fitted.

**Four things the code taught §11.4, and all four are recorded there.** **"Nothing then goes back for the hole" is not reclaimable at the target width**: placement wraps to a new row only when no earlier position fits, and the target bounds the achieved width, so a wrapped rect is a fixed point of any rule that cannot lose area — the "up" half of compaction fires only after an earlier edit narrowed a row, about 1 in 300 random 3–5-rect lists, and what fires on the corpus is the "left" half. **`vac`'s 4173x3783 hole is inside a *component* rect**, which is not a drawn box, so it was never the packer's to fill: its root frame reads **69.98%** over the four packed components and **60.24%** over the five rectangles a reader sees, of 11380 x 9122 on the shipped candidate, and neither step moves either number. The call-site census above is the third. And the placement is better than this document gave it credit for: **exhaustive enumeration over all 90 packings finds no order-preserving structure inside the target width shorter than the one placement found, on 18 of the 21 charts.**
*Exit, met, with one clause not met in the shape it was written:* **`vac`'s root frame occupancy is above 54.7%** — 69.98% over its four packed components, 60.24% over its five reader-visible rects — but the honest reading is that the 54.7% was row 0's canvas and **Level 2's pick at P9b is what moved it**, not either step here, both of which leave that packing exactly where they found it (§11.4). **Area is not down on every corpus chart, and the exception is every chart but one**: whitespace elimination cannot move an extent, so at the shipped M no chart's area moves on either scale, and at M = 4 area falls on `dock` alone, `vac` compacting at identical area — the clause was written for a step that turned out to be a knob. **Every element-suite property still holds** at both profiles, `crowd.scav`'s pinned `label` 4 and `label_near` 442 included, and the corpus's centred label fallbacks read 16 of 192 at the shipped M against 20 at M = 4 (§5). **Rendered and looked at** (§11.12), and looking is what set the shipped M rather than any number in a golden.

**P9d — the weights fitted, and the side-by-side that has waited since P6.** The weights are fitted to `tools/audit.py`'s reader-visible counts — a label over a state or another route, a label nearer a stranger's line than its own, an overprinted string, an arrowhead over another route's end — over the candidate set P9b's Level 2 generates and **on the canvases P9c left**, which are the canvases it started with — whitespace elimination moved no extent and compaction is a row the shipped M of 2 does not run — so this is also where the shipped M goes back up if the fitted weights say rows 2–3 are worth running (§17 P9c). Then the unblinded page decides (§11.12): the counts choose which candidates are worth looking at, and looking is what settles it. **`crowd.scav` is the case that says the weights are wrong and that no search fixes it.** A label overlap costs 24 and an em² of canvas costs 1, so under the em the cramped drawing still wins — `t2` **1,230** shipped against **1,438** for the candidate whose every audit count is zero — and `brew` loses the same way. That is the weight fit, so it is this sub-phase: the pinned `label` **4 must go to 0**, and the carve-out in `functional_drawlist_tests.cpp` moves with it (§5). `sweep_count` splits here if calibration wants a different number for phase 1's sweeps than for search's (§11.15), and §11.3's global sifting and edge-weight schedule becomes judgeable for the first time.
*Exit:* the weights fitted and reviewed as above; **no worse than the incumbent on the transcribed corpus side by side** (§11.12), the clause P6 and P7 each carried and neither could be judged on, because a render with no search behind it is one candidate rather than a layout; and on the corpus a one-state edit produces a visually small diagram change **in the common case** — §11.11's honest limit means a cascade is a hint's job, not a gate failure.

**P9e — Level 1 and local search, gated on a measurement.** Level 1's strategy table needs three phase-1 toggles that do not exist — sweep start direction, the pull-right ranking pass, initial in-rank order (§11.10) — so those are built first, and the table is then scored **exact** over the corpus and the element suite, per frame's best composed, with no surrogate anywhere in it. That is affordable for the reason Level 2 was: 3.2 ms a candidate on the nested 2k and 0.7 ms of routing over the whole corpus (§11.6). **That measurement is the gate.** §11.6's surrogate, its ranking test (§5) and §11.10's three bounded moves are built only if it shows residual defects a per-frame ordering choice or a bounded move reaches. If what remains instead is §11.8's regions hole and §11.9's rip-up cases, this sub-phase closes with Level 1 on exact scoring at a small K and the surrogate stays design — the most expensive piece of P9 left unbought, a defect class it cannot see being one it cannot fix. If the moves are built they are §11.10's three, with that section's dense enumeration, its serial index-ordered acceptance and its end-of-sweep squeeze, plus a third 2k fixture of near-tied symmetric frames so the tie-break rules are what decides them and not the input's asymmetry.
*Exit:* **the measurement recorded here either way**, a gate whose negative answer goes unwritten being a gate nobody took; and if built, the ranking property test green with its floors pinned, **two named mutations failing** — acceptance in shard order and a shared best-so-far, the two ways this stage stops being a function of the model — and the per-frame chosen-strategy arrays byte-identical across thread counts rather than hashed.

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
- ~~**The shipped Tier-2 weights make the sum an area measurement.**~~ **Settled at P9a.** On the corpus `w_area * area` was within a rounding error of the whole of Tier 2, because area is `10^8` while every other term weighted is `10^3` to `10^6`. §11.6's ordering says area is *lowest*, and that is true of the multiplier and false of the influence. **Quantified since, and no longer only a datum**: 99.48% with no space requests and 99.38% under real text; untunable within the weight caps, because counts, lengths and area are three different degrees in the coordinates and the caps give three orders of magnitude to cover seven; and shown to decide real trade-offs wrongly — over a 28-point grid of knobs that already exist, an area-normalised objective picks a different candidate on 7 of 11 charts with no space requests, the current weights already disagree with what ships on 4 of 11 under real text, and `label` 51 → 13 is on offer for +10.8% area. The design answer was §11.6's commensurate units, taken as **P9a's precondition rather than P9's last step**, since a search that minimises the sum as weighted minimises area. **Landed as the em**: every term scored in the unit the profile names it in, area at **31.95%** with no space requests and **33.44%** under real text — the largest single term and no longer the sum — and P7e re-scoring at −22.04% where the old weights reported −0.354% (§11.6, §17 P9a). What that does not settle is whether the weights are right, which is P9d's, on a frontier P9b's Level 2 is what walks.
