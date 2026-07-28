# scav — PRD / ERD

Statechart authoring, layout, rendering. Normative. Terse by intent: read by agents and by humans ramping up.

`[OPEN]` marks unsettled decisions. Everything else is settled; changing it needs new evidence, not a new opinion.

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
| `scav` | CLI: `render` (chart -> SVG), `layout` (dump geometry columns), `fmt` (canonical print), `gen` (synthetic charts), `check` (validate), `dump --json`, `selftest` (verify §6's hashes on this toolchain) |
| `scavview` | ImGui viewer [P10]. Embeds Lua so users can script appearance without rebuilding it (§8.3) |
| *yours* | e.g. an enemy-AI editor: links core+layout+draw, writes a builder that also draws threat radii, writes its own ImGui backend. No scav change required |

`libscavdraw` depends on `libscavcore` but **not** on `libscavlayout`: a builder reads geometry columns, and does not care who wrote them. Enforced in CI, along with the rule that no library links anything above it.

Internally each library is built from per-subsystem CMake `OBJECT` libraries with their own usage requirements, so subsystem boundaries stay declared and lintable inside one shipped artifact.

### 3.2 Repository layout

Mirrors `~/src/envy`: SHA-pinned deps under `cmake/deps/`, unit tests adjacent to sources, Python functional tests, sanitizer suppressions, presets.

```
include/scav/          public C ABI headers, one per library
src/core/model/        columnar aggregates, ids, string pool, builder, validation
src/core/format/       .scav lexer, parser, canonical printer, JSON dump, includes, resolution
src/layout/            space requests, phases 0-3, routers, cost, thread shim
src/draw/              DrawList type, text metrics, helpers, reference builder
src/svg/               reference SVG backend
src/imgui/             reference ImGui backend
apps/cli/              the scav executable
apps/view/             ImGui viewer + its Lua host [P10]
plugins/libhsm/        columns, attributes, builder contribution (importer + codegen: §17)
plugins/scxml/         reference example: importer, exporter, builder contribution
assets/font/           the bundled TTF — a layout-hash input, so versioned here
abi/scav_abi.json      committed golden ABI description (§16)
test_data/charts/      corpus: synthetic fixtures + hand-transcribed real charts
test_data/golden/      drawlist/, svg/, layout/ — see below
functional_tests/      Python, drives the CLI and the C ABI via ctypes
cmake/deps/*.cmake     one file per third-party dep
cmake/Dependencies.cmake   SHA pins
tools/                 abi extraction (libclang), corpus tooling
docs/
out/                   gitignored: all build output + the envy cache (§4.2)
```

`apps/` is separate from `src/` because an application is a *consumer* — that keeps the CLI and viewer from quietly becoming privileged layers.

**Unit tests are adjacent** and compile library sources with `-DSCAV_TESTING`, which is how `SCAV_INTERNAL` (§5) drops `static`. Each library builds twice, shipping and testable; both are matrix rows (§6).

**Goldens are layered by stage**: `layout/` (structural + coordinate hashes), `drawlist/` (the canonical render IR, §12, the primary surface), `svg/` (thin serializer check). A layout change moves the first two; an SVG-writer change moves only the third. `svg/` alone → serializer bug; all three → review starts at `layout/`.

Third-party, all permissive and SHA-pinned, none vendored by copy: **doctest** (MIT), **ImGui** (MIT, native viewer only), **stb_image** (MIT/public domain, native viewer only), **lua** + **sol2** (viewer only — see §8.3). Plus the bundled TTF, which is a layout-hash input (§18). The toolchain itself is provisioned by envy (§4.2), not by CMake.

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
| Unavailable | RTTI, exceptions, modules, coroutines, `std::format`, `<iostream>`, `<regex>`, virtual inheritance. Enforced by build flags or by portability — not a style question |
| Discouraged | `<ranges>` and view pipelines · clever `<algorithm>` compositions · stateful classes · work in constructors · operator overloading beyond id comparison · SFINAE, CRTP, type-list computation · `auto` where the type isn't locally obvious. Usable with a comment saying why; the default answer is the plain loop |
| Build | CMake + Ninja, toolchain pinned by envy — see §4.2 |

The bar for a discouraged construct is that a reader can still follow control flow from the source. That's what rules them out by default, and it's also what makes exceptions legitimate when the construct genuinely reads better.

**Preferred idioms:**

- **Snap-together function templates over function-pointer indirection.** The useful template work here is not containers of `T` — it's algorithms parameterized on a functor, monomorphized so the functor **inlines**:
  ```cpp
  template <typename T, typename Less>
  void stable_sort_by(std::vector<T>& v, Less less);   // less() inlines
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

**Runtime polymorphism has exactly one permitted site:** the internal router vtable (§11.5, a POD struct of function pointers, never crossing the C ABI). The P12 editor may add a second if the command-buffer mechanism wins over arena snapshots (§17). Everywhere else: static selection, separate binaries, or link-time choice.

### 4.1 Data structure discipline

**About the kind of data, not the layer.** One test:

> Does this outlive the call, get serialized, get hashed into output, or get addressed by path?

**Yes to any** → core model: columnar POD aggregates cross-indexed by ordinal (§7). No pointers between records, no hash maps, no nodes.

**No to all** → transient scratch, whatever is convenient: `unordered_map`, priority queues, visibility graphs, sweep structures, union-find. Not an exception grudgingly granted to `layout` — the normal treatment of data built and discarded inside one call.

Two constraints survive on scratch (§6), both structurally enforceable: **never iterate an unordered container where order reaches output**, and **never let a hash value escape**. `lookup_map` makes the first a compile error.

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
-- @envy cache-posix "out/.envy"
-- @envy cache-win   "out\.envy"
```

Precedence is `ENVY_CACHE_ROOT` > manifest directive > platform default, so **project-local is the default and a shared global cache is one env var away** — which is what CI wants, where a cache warm across jobs is the point. Cost of the default: one toolchain copy per checkout, the right trade against a machine-global directory a second checkout can invalidate.

**Not a build prerequisite, and deliberately so.** §6's claim is about the C++ abstract machine, so any conforming C++20 toolchain is supported and `scav selftest` is how a user confirms theirs agrees. Requiring envy to build would contradict that. Standard CMake: `find_package`, no vendored toolchain assumptions, no compiler-specific flags in the exported targets.

## 5. Testing

**All code exhaustively tested, unit and functional. A phase is not done until its tests are.**

Testability comes from pure functions, not seams. **Mocks and interface seams are rejected** — they cost an indirection and make control flow unreadable without running it. Intrusive access instead:

```cpp
#ifdef SCAV_TESTING
#  define SCAV_INTERNAL           // external linkage; tests declare the prototype and link
#else
#  define SCAV_INTERNAL static
#endif
```

All layout arithmetic is integer, so inlining cannot change results: the `testable` build must be byte-identical to release, and is a row in the matrix. Divergence means UB.

Required test classes: **unit** (every internal function, doctest) · **functional** (full pipeline over the corpus) · **golden** (canonical serialization, structural hash, coordinate hash, **`DrawList`** — the primary surface, §12 — plus a thin SVG serializer check, ABI JSON) · **property** (round-trip identity, all refs resolve, zero box overlap, zero edge-through-box, surrogate cost ranks like exact cost) · **determinism** (§6) · **sanitizer** (UBSan signed-overflow+shift, ASan, TSan, MSan) · **fuzz** (deserializer and reference resolver — untrusted input) · **binding** (drive the C ABI from Python/ctypes in CI) · **baseline** (§11.12) · **performance** (see below) · **regression** (every fixed bug leaves a test).

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
- Diagnostics are collected per shard, concatenated in shard order, then sorted by `(code, subject_kind, subject_index)`. They are part of the golden artifact. **A diagnostic carries nothing but that triple** — file, line, column, and the offending text are derived by walking to the statement's `src` span (§7), so no layer threads positions through its call stack. On a model mutated since load that span is cleared, and a diagnostic degrades to the triple alone; goldens run on unmutated models, so the artifact is unaffected.
- **Text is normalized at parse**: LF-only, BOM stripped, NFC. Ship `.gitattributes` with `*.scav -text`. Without this, `core.autocrlf` on Windows and NFD on macOS change the string pool — same commit, different hash. NFC needs a table: it is a **P0** dependency.
- Threading via a shim over pthreads / Win32 / **null (inline)**. Not C11 `<threads.h>`. The null backend makes WASI and the matrix work.

**Banned constructs.** Unlike §4's discouraged list, these are correctness bans with no escape hatch — each is a documented cross-platform divergence, not a readability preference:

| | Why |
|---|---|
| `int`, `long`, `unsigned`, `short`, plain `char` | `long` is 32-bit on MSVC (LLP64); plain `char` signedness differs x86 vs ARM, corrupting id hashing. `const char*` is permitted at the C ABI for NUL-terminated *input* names only, never stored in a record and never hashed |
| `size_t` in value computation | 32-bit on wasm32; unsigned wrap is *defined*, so it silently gives a different correct answer per platform |
| `std::hash` | Permitted to differ **between runs of the same binary** |
| **Iterating** `unordered_map`/`_set` where order can reach output | Iteration order varies across all three standard libraries. Key lookup is fine and fast — see below |
| Pointer-keyed containers | Address order; ASLR randomizes run to run |
| `<random>` — engines **and** distributions | Only the statistical requirement is standardized; implementations and versions differ; distributions are stateful |
| Raw `/` `%` on possibly-negative values | Truncates toward zero, so grid bucketing breaks asymmetrically about the origin. Use `floor_div`/`floor_mod`/`ceil_div`, defined once, negatives specified. These are the **only** division primitives |
| `__builtin_clz`/`ctz` | UB at 0; x86 `BSR` and ARM `CLZ` disagree. Use `<bit>` |
| `<cmath>` in the core | libm differs across glibc/musl/Apple/UCRT. Integer helpers only: `isqrt` (floor), `ilog2 = bit_width(x)-1`, ratio compare by cross-multiplication |
| Side effects in function arguments | Argument evaluation order is unspecified |
| `memcmp`/`memcpy`-hashing a struct | Reads padding, whose values are unspecified |
| Locale-aware compare, `setlocale` | Environment-dependent. Byte-wise only — Hebrew/Arabic therefore sort in codepoint order, a permanent accepted trade |
| `directory_iterator` order | Unspecified. Sort collected paths byte-wise |
| **Bitfields** | Allocation order, straddling, and padding are all implementation-defined. Use explicit masks on a fixed-width integer |
| **`__int128`, `__builtin_*`, `#pragma pack`, attributes outside `[[...]]`** | Not standard C++, so they defeat the whole claim. §11.2's budget exists to keep degree-4 arithmetic — and therefore `int128` — off the table |
| Shifting by `>= ` the operand width, or left-shifting a negative | UB. Shift counts are asserted in range; `<bit>` covers the rest |
| Narrowing without an explicit range check | Implementation-defined before C++20 and easy to get wrong after. One `narrow<T>()` helper, checked in every build |
| `enum` without an explicit underlying type | The type is otherwise the implementation's choice, which changes struct layout and wire size |

C++20's P0907 fixed two's-complement *representation* but kept signed overflow UB, and optimizers exploit it. Do not use `-fwrapv` — MSVC has no equivalent, so it would introduce a platform semantic difference. Prove no overflow (§11.2), net it with UBSan.

**Flags are belt-and-braces, never semantics.** `-fno-exceptions`, `-fno-rtti`, and their absence must all produce the same bytes: the code never throws and never asks a type its identity, so a compiler lacking those switches is still a supported compiler. Same for optimization level — with no UB, `-O0` and `-O3` cannot disagree, which is why the `testable` build is a matrix row rather than a trusted equivalence.

**`libscavlayout` uses a documented standard-library subset** — `<cstdint>`, `<bit>`, `<limits>`, `<vector>`, `<array>`, `<utility>`, `<type_traits>`, `<cstring>` — and nothing else. Every sort, hash, and container with iteration order that reaches output is scav's own (above). The subset is enforced by an include-check in CI, so "bring your own compiler" does not quietly mean "bring your own conforming `<algorithm>`".

**Scope of this section: anything that can reach layout geometry or rendered output.** A structure that only ferries data inside one call is outside it, and `std::unordered_map` is the right choice there — deterministic *by usage*, because a key lookup has no order and the hash value never escapes as a bucket index. Enforce that structurally: `lookup_map<K,V>` exposes `find`/`at`/`insert` and **no `begin()`/`end()`**, so "never iterated" is a compile error rather than a review comment.

**Golden hash.** Split into a **structural hash** (ranks, orders, port assignments, bend sequences) and a **coordinate hash**, so a translation-only change is a reviewable diff instead of a global reflow. Hashed inputs: font identity and version, profile id, packer choice, router name and version, **and the space-request columns** — a golden is reproducible only against a stated measurement policy, and the corpus goldens use the reference builder's.

## 7. Data model

**Not object-oriented. Flat arrays of POD aggregates linked by ordinal.** Column boundaries follow natural groupings, not individual fields.

**The point is end-to-end traceability, and that is the real payoff of columnar storage** — ahead of serialization mechanics, determinism, or cache behaviour. The model is the single place to look. Any function anywhere in the pipeline can walk from a rendered primitive back to the source bytes that produced it, by following columns:

```
Prim.origin  ->  entity  ->  DocId + Statement  ->  source span  ->  the authored text
     (§12)                        (below)              (below)
```

No context object, no side table, no callback. That is why source documents live in columns rather than being discarded at parse time.

**Terminology: `state` and `submachine`**, never "region". A composite state holds one or more submachines; more than one makes it concurrent. Applies to the ERD, ABI, diagnostics, and format.

```cpp
struct DocId    { uint32_t v; };   // include-instance provenance, not addressing (§9)
struct StateId  { uint32_t v; };
struct SubmachineId { uint32_t v; };
struct TransId  { uint32_t v; };
struct StrRef   { uint32_t off, len; };            // into StringPool
struct Span     { uint32_t off, len; };            // into a side array
struct Point    { int32_t x, y; };                 // grid units (§11.2)
struct Extent   { int32_t w, h; };
struct Rect     { int32_t x, y, w, h; };           // half-open (§6)
// Ids are global from the start, so endpoints are plain StateIds (§9).

constexpr uint32_t kInvalid = 0xFFFF'FFFFu;        // per-id sentinel

enum class StateKind : uint32_t {   // names match the DSL's state_kind (§15)
  normal, initial, final, choice, junction, fork, join, history, deephistory
};
// `initial` and `final` are reachable only via `*` in the format (§15), never `kind`.
// Load-bearing for layout and rendering, not passthrough metadata. See §11.14.
enum class TransKind : uint32_t { external, internal, local };

struct State {
  StrRef        name;        // empty for pseudostates; see §9
  StrRef        label;       // the positional string (§15); opaque, may be empty
  SubmachineId  parent;
  StateKind     kind;
  Span          submachines; // -> submachine_ids
  Span          attrs;
  DocId         doc;         // provenance (§9)
  uint32_t      gen;         // 0 = tombstone
};
struct Submachine {
  StateId       owner;       // kInvalid for a document root
  uint32_t      ordinal;
  StrRef        name;
  StrRef        label;
  Span          children;    // -> state_ids, document order
  Span          attrs;
  DocId         doc;
  uint32_t      gen;
};
struct Transition {
  StateId       src, dst;
  TransKind     kind;
  StrRef        label;       // opaque; see §7.1
  Span          attrs;
  DocId         doc;
  uint32_t      gen;
};
struct Include  { StrRef path, alias; DocId target; StateId host; };  // host: the synthesized alias state
struct Attr     { AttrKeyId key; StrRef value; };

struct Document {            // one per loaded document
  StrRef   path, alias;
  Span     text;             // -> src_bytes
  Span     statements;       // -> stmts, authored source order
  Span     includes;         // -> includes
};
struct Statement {           // one per authored construct
  ElemKind kind;
  uint32_t ordinal;          // the entity it declares
  Span     src;              // -> src_bytes; valid iff unmutated since load
  Span     comments;         // trivia: leading, trailing, own-line
};

struct StringPool { std::vector<scav_byte> bytes; };   // StrRef carries off+len

struct Chart {
  std::vector<Document>    documents;
  std::vector<Statement>   stmts;
  std::vector<scav_byte>   src_bytes;    // normalized source (§6); never canonicalized
  std::vector<State>       states;       // indexed by StateId
  std::vector<Submachine>  submachines;
  std::vector<Transition>  transitions;
  std::vector<Include>     includes;
  std::vector<Attr>        attrs;
  std::vector<Column>      columns;      // §8
  StringPool               column_names; // separate pool — see below
  std::vector<StateId>     state_ids;    // Span targets
  std::vector<SubmachineId> submachine_ids;
  StringPool               strings;      // interned names; canonically re-sorted
  StrRef                   name, label;
  SubmachineId             root_submachine;
  Span                     chart_attrs;
};
// No Project type. Documents are rows in one model, not charts to be merged.
```

**All documents share the same arrays**, each row tagged with its `DocId` — so no flattening step and no second model shape (§9).

`src_bytes` is a **separate pool from `strings`**: one is interned and canonically re-sorted before hashing, the other is never touched after load. "Verbatim" means post-normalization — §6 normalizes at parse (LF, no BOM, NFC), and `Statement.src` offsets index the **normalized** bytes, so reported columns are stable across platforms.

**Load-established, not serialized, not hashed.** Writing a document *produces* text; the format hash covers canonical output, not the possibly-non-canonical bytes loaded. `DocId` is implied by which file an entity is written into.

**`Statement.src` is valid iff the statement is unmutated since load.** Mutation clears it, so source mapping degrades gracefully rather than lying.

**A model is one document network rooted at one document.** Unrelated charts go in separate models; nothing structurally prevents intermingling, and the result is meaningless.

**Ids are append-only with tombstones.** `StateId` is ordinal *and* array index, so compaction invalidates every app-side column keyed by it. Deletion tombstones (`gen = 0`), ids never reused, **all columns tombstone in lockstep**. Compaction is explicit and an output change.

**Three column classes, not two.** Conflating the last two is a licensed determinism break, so they are named separately:

| Class | Serialized | Hashed | Container | Written by |
|---|---|---|---|---|
| **authored** | yes | format hash | columnar POD | builder API, editor |
| **derived-persistent** — the geometry columns layout writes | **no** | **layout hash**, by explicit allowlist (§11.7a) | **columnar POD; tombstones in lockstep** | layout only |
| **derived-scratch** — name→id and path→id indices, state→in/out edges, containment depth, LCA table, per-transition crossing counts and flags, each submachine's initial state | no | no | §4.1 convenience; `lookup_map` where lookup-only | anyone, rebuilt freely |

Only **derived-scratch** gets §4.1's container latitude. Geometry is hashed and read across frames, so it is columnar POD — a route polyline in a `lookup_map`, iterated for the coordinate hash, is the §6 failure this split exists to forbid.

`ColumnDesc` carries a `derived` flag (§8). The serializer skips derived columns and they are **exempt from round-trip-unknown**, or a stale geometry snapshot survives a save and gets trusted instead of recomputed.

**Column names live in their own pool, not `Chart.strings`.** Interning `scav.geom.state` into the authored pool would make every authored `StrRef` offset — and the format hash — depend on whether layout had run. Registration takes a `char const*` and interns it there; no pointer is stored in a row, so a `ColumnDesc` is hashable and serializable like any other record.

Serialization is mechanical (write each vector). Iteration order is array order is document order.

**Canonical ordering is by name or key *bytes*, never by id or interning order** — those are first-encounter, so two producers building the same model would emit different bytes. Re-intern into a sorted pool before serializing or hashing. `StrRef` and `AttrKeyId` are never comparison or tie-break keys.

### 7.1 No events in the core

There is no event entity, no event table, no trigger. A transition carries an opaque `label`. Semantics are that transitions are taken programmatically; scav models no triggering mechanism.

Event lists, guard expressions, executable content, and source spans are **extension data** (§8). This also removes any question of event-vocabulary unification across documents.

### 7.2 Fork, join, and semantic neutrality

Fork/join is the case that most tempts scav into having an opinion, so the boundary is worth stating explicitly.

**What the model holds:** a pseudostate of kind `fork` or `join`, and ordinary transitions. Nothing else. Arity is *derived* by counting incident edges; there is no grouping record, because the pseudostate is the grouping.

**What scav does not decide.** Exit, reset, and sibling-deactivation semantics under partial fork deactivation are dialect-specific, projects answer them incompatibly, and scav answers none of them. **It must nonetheless draw every one of those topologies**, which it does, because all of them are a pseudostate plus transitions.

**The bar is a fixed-size box.** Layout's only fork-specific behaviour is negative: it takes the bar's extent from the profile like any other pseudostate, and `w_adj` excludes fork edges because adjacency above arity 2 is unsatisfiable (§11.8) — no fan-out algorithm, no arity scaling. A fork/join pseudostate is an ordinary small box — wide and thin — from the profile's per-`StateKind` min extent (§11.15). Layout places it and routes N edges out of it; the builder draws a filled rect. That is what PlantUML does, and it is enough: the bar is the **same size for two branches as for five**, with the routes simply fanning out, including sideways.

Bar orientation is also not scav's: an app wanting a vertical bar requests a tall narrow `BoxSpace`.

**Validation is structural only** (§10): in/out degree per kind. No check that branches land in distinct submachines, no reachability, no concurrency reasoning — those are dialect rules and belong to a plugin.

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
enum class ElemKind : uint32_t { state, submachine, transition, chart, point, path_box, none };
struct ElemRef { ElemKind kind; uint32_t ordinal; };   // used by DrawList and diagnostics
enum class ValueKind  : uint32_t { u32, i32, u64, i64, strref, span, blob, pod };
struct AttrKeyId { uint32_t v; };   // interned attribute key, `ns:key` or bare
struct ColumnId  { uint32_t v; };   // index into Chart::columns

struct ColumnDesc {        // 28 bytes, no padding
  StrRef    name;          // "libhsm.events", "scav.geom.state"; own pool, not Chart::strings (§7)
  ElemKind  entity;        // `none` and `path_box` never appear here
  ValueKind kind;
  uint32_t  elem_size, elem_align;
  uint32_t  flags;         // bit 0 = derived: skipped by the serializer, exempt from round-trip-unknown
};
struct Column { ColumnDesc desc; std::vector<scav_byte> bytes; };  // count * elem_size
```

Type-erased byte arrays with a stride, indexed by entity ordinal. C ABI is the three-call accessor in §16; the host casts.

**What core owes an extension:**

1. Store it, indexed by entity ordinal; keep it index-aligned under mutation and tombstoning.
2. **Round-trip it losslessly, including unknown columns** — otherwise an older build silently strips a colleague's data on save. Wire encoding: all scalars **little-endian**, `strref`/`span` as two `u32`, `blob` as `u32` length then bytes, `elem_align` ignored on the wire. `pod` is `elem_size` opaque bytes whose interior the serializer does not interpret — the escape hatch a struct-valued column needs, and what every geometry column uses (§11.7a), which is sound because those columns are `derived` and never written to the wire at all. A column block is `{name, entity, kind, elem_size, count}` then `count * elem_size` bytes — enough to round-trip a column whose meaning is unknown.
3. **Pass it through unread.** `layout` never reads extension data.
4. Let it contribute **space requests** (§8.1) and a builder function the app may call (§8.2).
5. Expose `ColumnDesc` so an editor can present unknown columns generically.

Columns are canonically ordered by name bytes, never by registration order. Attributes reserve the `scav:` namespace for core-meaningful keys; unprefixed keys belong to the user. Values are strings on disk with typed accessors; `--strict-attrs` checks a known-key registry, since a typo is otherwise silent forever.

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
0 <= min_w, h_before, h_after <= kCoordMax / 4    // §11.2
0 <= PathBox.w, PathBox.h     <= kCoordMax / 4
0 <= PathClear.src, .dst      <= kCoordMax / 4
PathBox.order unique per subject
```

A quarter of the domain, not all of it: §11.4's box formula *adds* to a request (`h_before + packed_subs_h + h_after + 2*pad`), so admitting `kCoordMax` per field would let a legal input produce an illegal box. The composed box is bounds-checked as well; the input bound exists so the failure is attributed to the request that caused it.

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
  if (!alive(chart.states[i])) { continue; }                    // tombstone (§7)
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
- **An include synthesizes one state, named for its alias**, in the submachine where the `include` statement appears; that state's `submachines` span gains the included document's root submachine. A submachine's children are states, so this is the only shape that type-checks — an included root is a submachine and has nowhere else to attach. It also makes `wifi/Up/Connected` an ordinary path: `wifi` *is* a state.
- **Resolution links; it does not flatten** (§7). Containment crosses documents because `State.submachines` holds global ids, so layout sees one containment tree with no transformation having occurred — no cross-document LCA, no splice pass, no project handle.
- **Provenance is a field, not a computed column.** `DocId` on each row records the **include instance** — instance rather than path, so including one document twice yields two distinguishable sets. A renderer tinting sub-document submachines reads it; layout ignores it.
- Transition endpoints are plain `StateId`s, because ids were global from the start.
- **An include alias is a bare path prefix**, not a sigil, because it is a state name. Alias uniqueness is therefore §10's ordinary duplicate-name check rather than a second rule, and duplicate top-level names in two documents cannot collide.
- **No integrity attestation.** An include names a path, not a digest: a `.scav` document network is source code under the same version control as the code it describes, so pinning content hashes would duplicate what the VCS already guarantees while adding a second thing to keep current. Fetching a document over a network is the app's policy (§16.2) and so is verifying it.
- Include cycles are a hard error.
- **Instantiating one document twice means two include statements**, two aliases, and two disjoint sets of rows distinguished by `DocId`. Renaming that file then patches one path string per instantiation. Accepted: a **global include section with a reference sigil was considered and rejected** — a sigil names a document, but an endpoint must name an instance, so the two coincide only at one instantiation and above it the section needs instance names anyway. It would also still require a statement at the host to say where the subdocument attaches, and it would mark a cross-document distinction the model does not have, since an alias is an ordinary state.
- Relative hints travel with an included chart; **absolute pins do not** — a pin is authored against a document's own frame and is meaningless in a host frame.
- Resolution is a linear scan per path level (document order forbids sorting `state_ids` by name) or via the derived sorted index.
- Paths break on rename. Renaming is a **semantic editor** operation — the editor holds the document network and rewrites every reference — not a CLI verb. **[OPEN]** whether elements also need durable GUIDs, which paths cannot supply across branches: two branches renaming the same state differently is unreconcilable when identity *is* the name.

## 10. Validation

Mandatory, in core, structural only — `layout` reads ordinals and crashes on garbage:

- dangling `StateId`/`SubmachineId`/`ColumnId`; `kInvalid` where a value is required; tombstoned targets
- duplicate authored names within a submachine
- include cycles, unresolvable include paths
- unresolvable cross-document paths, checked at the **resolution phase** (§9)
- a `Statement.src` span outside its document's `text` span
- an alias colliding with a sibling state name — the same duplicate-name check, since an alias is a state (§9)
- authored names must not contain the path metacharacters `/ : $`, nor `@` (the format's attribute sigil, §15)
- more than one `initial` per submachine. **No degree checks per pseudostate kind** — "a fork has one incoming edge" is a dialect rule, and §7.2 requires every topology to be drawable
- authored `scav:pin` coordinates outside §11.2's domain — the only authored geometry there is

Semantic lint is out of scope. Identifier-sanitization collision checks belong to the codegen backend, not core.

## 11. Layout

Isolated static library, imperative entry, POD in. Writes **derived** geometry columns only and never authored data (§7).

Scale target: **2k states, 5k transitions, depth 16.**

```
phase0_split(model)                              -> SplitGraph
phase1_order(SplitGraph, Spaces)                 -> SubmachineOrders
phase2_size(SubmachineOrders, Spaces)            -> SizedLayout
phase3_route(SizedLayout, Router)                -> geometry columns + Placed[]
```

The four intermediates are internal POD: `Spaces` is the three §8.1 tables; `SplitGraph` is segments, ports, and the containment tree; `SubmachineOrders` adds rank and in-rank position per node; `SizedLayout` adds box extents and port coordinates. None crosses the ABI (§16) — only geometry columns and `Placed[]` do — so they are free to change without an ABI break.

Every stage is POD in, POD out, so any stage is testable with hand-written inputs and no font present. Hint columns are integers (§14), so layout never touches a string or resolves a path — §3's font-blindness is structural, not a convention.

Geometry columns are derived: never serialized, never authored (§7), so "layout writes the model" does not compromise round-trip stability.

### 11.1 Phase 0 — decompose

Build the containment tree. Split every transition at each boundary it crosses, terminating each segment on a **hierarchical port** on the compound state's border. Each segment is then local to one submachine, and the long-hierarchical-edge problem becomes 1D port ordering per compound side.

A port carries the **accumulated weight** of every edge through it, so a state at depth 16 with a transition to a top-level state exerts real pull on its ancestor chain. This is the differentiator.

Port order is a solver output. Ties break on the port's stable key (§6), never on weight-insertion order.

### 11.2 Coordinates

Integer only, grid units of **1/16 point** — a compile-time constant, not a profile field, because §11.9's ABI takes sixteenths of a point directly.

```cpp
inline constexpr std::int32_t kCoordMax = (INT32_C(1) << 19) - 1;   //  524'287
inline constexpr std::int32_t kCoordMin = -kCoordMax;               // symmetric
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
- Validate the domain at `scav_layout_run` entry in **every** build (§8.1), and after each retry inflation.
- Output is **root-absolute**, applied as one final `O(n)` transform over submachine-local internals (ELK's LCA-relative coordinates are a documented trap).

Extent estimate: 2k states ≈ 8,000 x 3,200 pt = 128,000 x 51,200 units, ~4x headroom. **Validate at P4**; if real charts exceed it, reduce the grid to 1/8 pt rather than widening the domain.

Coordinate assignment uses two linear integer primitives, not a solver: **Brandes & Köpf** for cross-axis coordinates (GD 2001 — **read the erratum, arXiv:2008.01252**), and optimal topological numbering for compaction. On an integer grid with integer gaps and an acyclic constraint graph, non-overlap plus separation *is* longest-path.

### 11.3 Phase 1 — per-submachine ordering

Independent per submachine: the parallel unit and the dirty unit. Layered rank assignment, then intra-rank ordering by **median** (tight 3-approximation) or **global sifting** (5–10% fewer crossings than level-by-level sweeps, eliminates type-2 conflicts). **Not barycenter** — no constant-factor bound, ratio Ω(√n).

Inter-rank edge labels become **label dummy nodes** sized to the label, so rank separation accommodates them by construction (§11.9).

### 11.4 Phase 2 — sizing and sibling packing

Sizes bottom-up; port positions and hints top-down; fixed pass count.

Submachine size composition (Castelló et al., JGAA 6(3), 2002): **width = Σ over layers of (max width in layer); height = max over layers of (Σ heights in layer)**.

Composite state box, from the requesting entity's `BoxSpace` (§8.1):

```
w = max(min_w, packed_subs_w, kind_min_w) + 2*pad
h = max(h_before + packed_subs_h + h_after, kind_min_h) + 2*pad
```

`pad` and the per-`StateKind` `kind_min_w`/`kind_min_h` are profile fields (§11.15), never hardcoded. A state with no space request passes an all-zero `BoxSpace`, so `kind_min_*` is what gives a fork bar its wide-and-thin extent (§7.2) — without that term the formula would size it `2*pad` square.

**Sibling submachines are packed here, not in phase 1** — packing requires the siblings already sized. **LR-rectpacking** (Domrös et al., IVAPP 2021): greedy width approximation → placement → compaction → whitespace elimination, `O(n log n)`. Take the LR variant; plain rectpacking's one-oversized-child special case was deleted by its own authors as unmaintainable and aspect-ratio-blind.

Order-preserving and gap-avoiding are one constraint: restricting placement to four positions relative to the predecessor (directly right; right on the current row level; next subrow; next row) is exactly what makes local whitespace elimination always possible.

Width approximation is `target_w = isqrt(floor_div(total_area * dar_num, dar_den))` with that exact operation order, `isqrt` = floor. `DAR` is an integer pair. On same-height submachines the older `box` packer wins; `trybox` is evaluated once per layout, deterministically. A 1-unit change can flip the packer and reflow siblings; that is a boundary condition for hints (§14), not grounds for remembering the previous choice.

**Bottom-up sizing has no locality** — a leaf growing one unit resizes every ancestor to the root. Inherent, and simply paid: one pass up, one pass down, fixed count. No hysteresis; that would be hidden state (§11.11).

Not top-down layout: its central size-approximation problem is unsolved by its own authors, it introduces per-level scale factors that break port-split segment continuity, and it is mutually exclusive with cross-hierarchy edges in ELK. Cost: bottom-up sizing at depth is a readability problem on fixed media (a depth-9 SCChart lays out to 0.322 pt max font on A4). Acceptable because output is a zoomable canvas.

### 11.5 Phase 3 — routing

Axis-aligned is a **hard constraint**. States, submachine rectangles, and placed boxes are obstacles, so Tier-0 edge-through-box is unrepresentable rather than penalized.

Routing graph is an **orthogonal visibility graph** or a **channel-representative graph** (Hegemann & Wolff, GD 2023, arXiv:2309.01671). At per-submachine scale (n≈20–50) both are cheap: **choose on quality and implementation simplicity, not asymptotics.** Published full-OVG scaling failures (~30 min at 4,330 obstacles) apply only to the degenerate flat chart — one submachine holding 2k states, which is legal input. Keep a sparse graph available for that path.

Bend cost lives **in the shortest-path metric**, via the **separated OVG** (Wybrow et al., Diagrams 2012): split each node into h-plane and v-plane copies joined by an edge whose weight *is* the bend penalty. Length and bends collapse to one uniform edge weight; unmodified Dijkstra/A* optimizes both, so bend-heavy routes are never generated. A* state is `(vertex, entry_direction)`, with the 5-case admissible remaining-bend bound from GD 2009 §4 Fig 2a. Tie-break `(f, g, entry_direction, node_index)`.

**Inter-submachine segments are routed in the parent's frame**, and every separator channel is owned by exactly one submachine (the LCA) and routed there. Without this, "independent per submachine" is false for exactly the edges this project exists to handle.

Congestion is **history-based** (PathFinder, McMurchie & Ebeling, FPGA'95; TritonRoute's marker cost). It is inherently sequential: route nets in `(submachine, transition)` order within a congestion domain, double-buffer the history map so iteration *k* reads only the *k−1* snapshot, merge updates in net order, fixed iteration count from the profile, integer ramp schedule as a lookup table.

**Nudging** retains the combinatorial stage: build the shared-edge graph, assign pseudo-directions by BFS from the lowest-`(channel, edge)`-keyed member (enumerate components by minimum key; on conflict mark a split point and reverse), then insert into each shared edge's order by projecting from the lowest-keyed already-ordered neighbor. Path-consistent components get minimum crossings with no extra bends. Placement assigns **integer offsets `k*gap`** within the channel; segment order is a total integral key `(channel, offset_slot, edge)`, never a partial comparator.

**Router is swappable** behind a POD vtable — internal only. Contract: pure w.r.t. `(input, ud)`, reentrant, no global state, allocates only from a caller-supplied arena, called concurrently from workers, must not unwind. Router name and version are hashed inputs. The C ABI exposes routers **by name** (`scav_router_by_name`, `scav_router_list`); function pointers never cross it.

`RouteInput`: submachine box, obstacle rects, port assignments. `RouteOutput`: integer polylines plus per-edge metrics (bends, length, boundary crossings, crossing span) so routers are A/B'd automatically. Internal POD — no `scav_` prefix, because neither crosses the ABI.

### 11.6 Cost

Lexicographic across tiers. **No multipliers between tiers** — a dominating weight inside a sum is the cost cliff that breaks local search, and it overflows.

```cpp
struct Cost {                 // compared lexicographically, in this order
  int32_t t0_violations;      // 0 for any admissible candidate
  int64_t t1_hints;           // unsatisfied hint count, priority-weighted
  int64_t t2;                 // weighted sum below
};
```

**Tier 0 — forbidden, not priced.** Edge through a state box, submachine box, or placed box; box-box overlap. Structurally impossible via the obstacle set. The predicate survives as a net for three cases: the straight-line **surrogate** during search; **degenerate enclosure** (a state with no free channel — inflate spacing by a fixed integer increment, capped, then diagnostic); and a marked violation with a stable code if retries exhaust. Never a silent overlap.

**Tier 2 —** every weight is an integer with a documented ceiling. The largest term is `w_area * bounding_box_area`; area is at most `(2*kCoordMax)^2 ≈ 2^40`, so with weights capped at `2^10` the sum stays under `2^53` — inside `int64` with room, and the ceiling exists to keep that true rather than to tune anything:

```
w_b    * bends                                      -- highest in tier
w_par  * shared_corridor_overlap
w_x    * crossings
w_len  * Σ (excess_length * (1 + crossings_of(e)))   -- excess over min_len(e), §11.9
w_adj  * nonadjacent_sub_pairs_joined_by_edge     -- §11.8; excludes fork/join fan-out
w_lbl  * label_overlaps
w_ar   * |w_actual*dar_den - h_actual*dar_num|       -- integer aspect deviation
w_area * bounding_box_area                           -- lowest
```

Weights are integers in **named, versioned profiles**. Express each as an exchange rate ("n means an edge would rather turn n times than cross"), not a bare number. A weight change, a profile change, a font change, or a packer change is an **output-format change**: versioned, golden-tested, reviewable.

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
| `scav.geom.portslot` | port ordinal | `{int32 x, y; uint32 side, boundary_depth}` |
| `scav.geom.chart` | chart | root bounding box |
| `scav.geom.gen` | chart | generation counter (§13) — `u32`, **not hashed, not serialized** |

`ElemKind::point` exists so the point and port-slot arrays are real columns rather than side arrays outside the column rules; entity count is the column length. A transition splits into one port per boundary it crosses (§11.1), not two — a depth-16 edge has up to 15, and the structural hash covers all their sides, so one row per transition cannot hold them.

**Hashing is by explicit allowlist, not by enumerating `Chart.columns`** — otherwise app and plugin columns perturb scav's own goldens, and the same corpus hashed through `scav` and through `scavview` would differ. The **structural hash** covers ranks, orders, port sides, and bend sequences *as direction-turn tokens*; the **coordinate hash** covers the rects and every point and port coordinate. Turn tokens rather than points is what makes a pure translation move the coordinate hash and not the structural one, which is the whole point of the split.

### 11.8 Transitions between concurrent submachines

Supported; arbitrary topologies must be. **Drawn as a direct arrow**, not routed up through the parent — the semantics are parent-mediated (exit to just below the owning ancestor state, re-enter the source submachine at its default initial configuration, enter the target along the path from the LCA) but **the geometry does not follow the execution path.**

General principle: **scav draws topology, not execution paths.** Applies equally to history and choice.

Phase 0 still splits at both submachine borders; the middle segment crosses only the separator, routed in the parent's frame by the LCA-owning submachine (§11.5).

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

**Labels routinely dominate transition length: a constraint, not a pathology.** ``min_len(e) = max(geometric_min, Σ PathBox widths along the route)``, a hard sizing input. `w_len` charges **excess only** (§11.6) — charging raw length makes the optimizer fight an unwinnable constraint and cram everything else. Rank separation grows via label dummy nodes (§11.3).

**Placing a `PathBox`** is Kakoulis & Tollis strip matching: slice into strips sized to the tallest box, slide candidates until they touch their route, keep those overlapping nothing. Candidate order `(transition, order, strip, slide_offset)`; components process in ascending minimum key. NP-hard, so heuristic. On failure, bounded rip-up-and-reroute of that one edge (§11.5).

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

That removes `w_st`, `PriorLayout` and its version key, per-session hysteresis, the sticky packer bit, dirty-region tracking, and VPSC (§11.13) — plus a class of defects: layout depending on edit history, and a golden hash needing a "cold-start" qualifier to mean anything.

**Honest limit.** Stability-by-construction is not a guarantee. A one-node change can flip a crossing-minimisation decision and cascade, and no ordering discipline prevents that in general. Those are the boundary conditions **hints** exist for (§14): when the engine makes a defensible choice the author dislikes, the author pins it rather than the engine remembering what it did last time.

**This is a bet on speed**, and it should be measured rather than assumed: full layout must be fast enough at 2k states that no second mode is wanted. If it is not, the answer is to make layout faster — not to reintroduce a mode, which trades a performance problem for a correctness one.

### 11.12 Quality baseline

The likeliest failure is producing layouts that score well on `Cost` and that readers find worse than the PlantUML output they already have. Nothing in a cost vector detects this.

**A side-by-side harness ships at P5**: the same chart through `dot -Tsvg`, elkjs, and scav. Blind scored review of the corpus at the P6 and P7 gates. Exit criterion is **"no worse than the incumbent on the transcribed corpus"** — not "visually reasonable."

### 11.13 Rejected

**Topology-shape-metrics.** Needs planarity — statecharts are non-planar, planarization is NP-complete, and the literature's ceiling is "a few hundred vertices". Compound nesting isn't in the model; the bolt-on rests on c-planarity, open 1995–2022. Three chained NP-hard problems with documented excess bends and area blowup. HOLA replaces it; CoDaFlow rejected it for compound-plus-ports specifically. Keep only compaction by topological numbering (§11.2).

**VPSC.** Coordinates are generated, not adjusted, so separation is longest-path: linear, exact, integral, ~100 lines. VPSC buys only minimum-displacement-from-prior, and §11.11 leaves no prior. If ever revisited, note the GD 2006 **Correction** — the published algorithm can return **infeasible** solutions.

**LP nudging fallback.** "Integral if coefficients are integral" is false — that needs total unimodularity, unestablished here — and simplex pivoting under degeneracy is tolerance-driven float. Deterministic degradation instead: widen the channel by a fixed integer increment and re-seat, capped, then diagnostic.

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

Consequences:

- **Phase 0 (§11.1) suppresses the source-boundary split** for `internal` and `local`. One fewer segment, one fewer port. The derived boundary-crossing count (§7) must reflect this, or `w_len`'s per-crossing multiplier (§11.6) miscounts.
- **Tier 0 (§11.6) carve-out:** an edge may occupy the interior of a state whose border it does not cross, and **only** that state. Every other state and submachine rectangle remains an obstacle.
- **Internal self-loops are the app's, end to end.** The app sums the band it needs into `h_before`/`h_after` and its builder draws the glyphs inside the returned `scav.geom.state_before`/`_after` rect. There is no route, so `PathBox`/`PathClear`/`min_len` do not apply and the router is not involved. Layout sees only two integers.
- **The reference builder distinguishes the three kinds**, pinned in the `drawlist/` golden. scav cannot mandate what a custom builder draws (§2, §3).

### 11.15 The profile

A versioned, hashed artifact (§6), so it needs a field list rather than thirteen scattered references. All integers.

| Group | Fields |
|---|---|
| geometry | `pad` |
| type | `font_size_grid`, `line_height_k_num`/`_k_den` (`k_den >= 1`) |
| pseudostate sizes | per-`StateKind` min extent. `fork`/`join` are wide-and-thin boxes; nothing scales with arity (§7.2) |
| packing | `dar_num`/`dar_den` (each in `[1, 2^10]`), `trybox`, SM tiebreak order |
| cost | the eight Tier-2 weights, each with a ceiling keeping `Σ Tier-2` inside §11.2's budget |
| search | portfolio `K`, sweep count, congestion iterations, rip-up cap, spacing-inflation cap and increment |
| id | `profile_id`, `profile_version` |

Profile load **validates every bound and rejects out of range** — weight ceilings give the Tier-2 sum a proven bound, and bounded `dar_num` keeps `total_area * dar_num` under `2^50` before `isqrt`.

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
  uint32_t clip;                 // -> clips[]; kInvalid = unclipped
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

`points` and the scalars are fixed per kind, so a backend switches once and never guesses: `rect`/`rrect` 2 points (corners, `a` = radius) · `line` 2 · `polyline`/`path` N >= 2 (`path` closes, `polyline` does not) · `text` 1 (baseline origin, `payload` = the string) · `circle` 1 + `a` = radius · `arc` 1 + `a`/`b` = start/sweep in 1/64 degree · `image` 2 + `payload` = registered id. Any other count is invalid, and the `DrawList` validator rejects it.

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

### 12.1 The reference SVG backend

Headless `scav render` is the first user-visible deliverable (P5), so this one ships.

**Emit the body in integer grid units with the entire scale in one integer `viewBox`.** Float-to-decimal conversion is not portable (MSVC UCRT, glibc, musl, and Apple libc disagree on the last digit) and `-ffp-contract=fast` is the default, so `grid * scale` differs by 1 ULP between Debug and Release. **No float is printed, ever.**

Renderer-vs-metrics agreement, in order: one bundled font, named with a fallback · `textLength` with `lengthAdjust="spacing"` from our own advance sum, turning overflow into slightly loose spacing (Graphviz emits none, which is why its SVG overflows under substitution) · `font-kerning: none` per §11.9.1 · explicit padding, never sizing to exactly the text width · `--embed-font` base64ing the bundled TTF whole into `<defs><style>@font-face`, the only exact agreement that keeps text selectable — whole, not subsetted, because a subsetter is the expensive part of the PDF backend and v1 does not have one. **Never convert text to paths** — needs the outline stack we avoid, discards selection and accessibility.

Emit a stable `class` per element, synthesized from `Prim.origin`: `scav-state scav-id-1234`. External CSS can then restyle a static SVG.

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
  include "wifi.scav" as wifi,

  state Off "powered down",
  state Booting,
  state PreConfig choice,

  trans * -> Off,
  trans Off -> Booting "POWER_ON",

  state On {
    @doc = "Enter: publishes EVT_POWERED_ON",
    @libhsm { submachine_handler, legacy = "false" },

    submachine main {
      state Idle { @libhsm:handler = "false" },
      state Ready,
      trans * -> Idle,
      trans internal Ready -> Ready "BUMP_RETRY",
      trans Ready -> wifi/On/Connected "handoff",
    },
    submachine strays "sweeps while main drives" {
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

**One printer, always reconstructing.** Stored source bytes (§7) are **not** a printing shortcut: emitting untouched statements verbatim preserves their formatting, so two semantically identical models from differently-formatted files print differently — breaking the canonicity the format hash and merges rest on. Print reconstructs, gofmt-style; a repo is expected canonical (`scav fmt` pre-commit). Source bytes are for diagnostics and source mapping.

Also required: text normalized on read (§6); extension columns round-trip losslessly including unknown ones (§8).

**Cost.** Lexer ~400 LOC including `"""` handling and comment capture, parser ~500, comment-preserving printer 3,000–5,000. The printer is the expensive half and a simpler grammar barely helps it.

**JSON survives as an output-only projection** (`scav dump --json`) for programmatic consumers. Mechanical over columnar data, and not a format: it has no comments, so §15's trivia cannot round-trip, and it has no canonical form, so two encoders disagree on byte output.

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

**Handles: five, each with a create and a destroy.** `scav_chart` (the model), `scav_load` (a multi-document load session, §16.2), `scav_metrics` (font tables), `scav_images` (the raster registry a backend reads), and `scav_drawlist` — which exists because `DrawList` is five `std::` containers (§12) and §16.1 requires the reference builder and SVG backend to be reachable from a binding. Its arrays are read out with the same span accessors as a column. Destroy is idempotent on `NULL`; a `scav_chart` outlives every `scav_span` handed out from it, and nothing else owns model memory. `scav_metrics_create(const scav_byte* ttf, uint32_t len, scav_metrics** out)` — the bundled font is embedded, so `NULL` selects it. `scav_metrics` is immutable after create, so it is shared across threads without locking; the other three are single-threaded-per-instance, and any number of instances may be used concurrently. There is no library-global state and no init call.

**The profile reaches layout inside `scav_layout_opts`**, as a `scav_profile` POD by value plus the `scav_router_id` — not a handle, not a file path, so its bytes hash into the golden (§6) directly. `scav_profile_named(const char*, scav_profile* out)` fills it from a shipped profile; `scav_profile_validate` is called by `scav_layout_run` regardless (§11.15).

**Column access** needs three calls, not one: `scav_column_find(chart, name, scav_column_id* out)`, `scav_column_data(chart, id, const scav_byte** out, uint32_t* stride)`, `scav_column_count(chart, id, uint32_t* out)`. A builder cannot walk a column without the row count.

**Out-param protocol**, uniform: pass `cap = 0` with a non-null `out_count` to query the required count, then call again with a buffer. `cap` too small returns `SCAV_E_CAPACITY` and writes the required count; it never truncates silently. Allocation is via one injected allocator, set per handle at create.

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

**Machine-readable ABI:** the header is the source of truth; a libclang build-time tool extracts functions, structs, enums, and field offsets to a committed JSON sidecar. Bindings are generated; a golden test asserts extraction matches, so an ABI break is a review diff rather than a downstream segfault. libclang is tooling-only.

Editor commands do not cross the C boundary as objects; that layer's API is opcodes. (Note `virtual Command Inverse()` returning an abstract base by value does not compile — the editor's inverse is a command buffer append.)

### 16.1 Distribution and bindings

**Extending scav means writing an application** (§3), so bindings must cover the whole pipeline — model, format, metrics, space tables, layout, geometry columns, `DrawList`, SVG — not a plugin corner.

**No extension point is a callback** — everything is data in, data out. So a binding is pure marshalling, with no host-language function invoked from a worker thread across an `-fno-exceptions` boundary. That is what makes bindings tractable.

**One redistributable shared library** — `libscav` = core + layout + draw + svg. The static libraries are a build-time decomposition; the distribution unit is one shared object. `libscavimgui` stays out of it: it needs an ImGui context, which only the host application has. The batteries are everything except the interactive viewer, so the reference builder and SVG backend must be reachable through the C ABI rather than being C++-only conveniences.

- **Generated, not hand-written.** libclang → ABI JSON (§16) → generated low-level layer, plus a thin hand-written idiomatic wrapper per language. The generated half never drifts.
- **Prebuilt binaries**: macOS arm64/x86_64, Linux x86_64/aarch64 (manylinux), Windows x64, plus wasm. No compiler required to `pip install`.
- **Self-contained**, because there are no runtime dependencies. The bundled font is **embedded in the library**, not loaded from a path — it is a layout-hash input and must travel with the code.

**One hazard:** Python makes §8.1's integer purity easy to violate (`/` yields float), so setters reject non-integers and range-check, and space-computation helpers live in the shared library. Handle lifecycle was the other, and §16 now specifies it; it remains a **P3** deliverable because a binding cannot be written without it.

### 16.2 No file I/O in core

**Core performs no file I/O.** It parses from a byte span; the application supplies bytes. This is required for the browser and for bindings, and it is good hygiene regardless.

Include resolution is therefore **iterative and data-driven, not a callback** — a load session accumulating documents and reporting what it still needs:

```c
scav_result scav_load_begin(scav_load** out);
scav_result scav_load_add(scav_load*, const scav_byte*, uint32_t len, const char* name);
scav_result scav_load_pending(const scav_load*, const scav_pending** out, uint32_t* n);
scav_result scav_load_finish(scav_load*, scav_chart** out);
void        scav_load_destroy(scav_load*);

typedef uint32_t scav_doc_id;              // the ABI spelling of DocId (§7)
struct scav_pending {                      // 12 bytes, no padding
  scav_span   path;                        // into the load session's own byte pool
  scav_doc_id from;
};
scav_result scav_load_bytes(const scav_load*, scav_span, const scav_byte** out, uint32_t* len);
```

`add` the root, read `pending`, resolve each however you like, `add` each, repeat until empty, `finish`. The app owns fetch policy, caching, and parallelism; cycles and unresolvable paths are core's errors; and `name` makes diagnostics say `wifi.scav:12` rather than `<buffer>:12`.

Works identically over a filesystem, HTTP, a zip, or memory, and preserves §16.1's no-callback property.

**Nothing is hidden.** `scav_parse` on a byte span is always available and never bypassed. `scav_read_file` and `scav_load_file` compose it and are skippable in full — they ship in the CLI's own support code, not in `libscavcore`, which is what makes "core does no file I/O" a linkable fact rather than a convention. Same layering as the reference builder (§8.1.1): the functions that convert text into the columnar model are primitives, and the batteries sit on top.

No stream type: a `.scav` file is kilobytes, so bytes are the simpler composition point. Revisit only if incremental parse becomes an editor-responsiveness requirement.

### 16.3 The path to a browser viewer

Not a v1 deliverable; what matters is that nothing precludes it. Four conditions, all already required for other reasons:

| Condition | Status |
|---|---|
| single-threaded execution produces byte-identical output | **already required** (§6's null shim backend, in the matrix) |
| core does no file I/O | **§16.2** |
| the font is embedded bytes, not a path | **§16.1** |
| the viewer's platform layer is swappable | ImGui's own concern; it ships SDL and GLFW emscripten backends |

The scav-specific part of a viewer is only `DrawList` → draw calls, so a browser viewer is an emscripten build of the *viewer*, not a change below it.

**It may not be the right web front end anyway.** A web app can run core+layout+draw in wasm and render the `DrawList` in JS to SVG DOM or Canvas — beating ImGui-in-canvas on text selection, copy, accessibility, zoom, printing, and bundle size. Two backends is §3's intended shape.

One nuance: a JS emitter is a second implementation the goldens do not cover. So the wasm build exports the `DrawList` **and** the C++ SVG backend — interactive rendering is JS, static SVG comes from the code CI pins.

## 17. Phases

Where a phase states production LOC, multiply by 1.5–2 for the mandated test classes.

**P0 — the language, the lexer, and the parser.** Validate the format before anything depends on it. Recursive-descent parser over §15's grammar, one document, byte span in. Produces the **front-end slice of the model only** — `src_bytes`, `Document`, `Statement`, trivia, and the string pool with two-pass interning — because a statement stream is all a parser owes (§7). No entity arrays, no includes, no resolution. NFC normalization (§6) lands here since it happens at parse. Plus the **in-RAM synthetic document generator** and **2–3 hand-transcribed real charts** — synthetic input has uniform branching and no accidental structure, so validating on it alone is a trap.

**Recursive descent needs an explicit depth cap** with a diagnostic, not a stack overflow: nesting depth is attacker-controlled and 16 is the *design* target, not a limit the grammar enforces.

**Performance is a P0 test class, not a later concern.** Generate documents in RAM — never on disk, which measures the wrong thing — and assert a throughput floor plus peak-memory-to-input ratio for lex and parse separately. The point is catching accidental `O(n²)`: string-pool growth, per-statement rescans, long comment runs, wide sibling lists. Timing is machine-dependent, so these are floors on a named machine and **not** part of §6's matrix or any golden.
*Exit:* every corpus file parses; every diagnostic locates to a `Statement.src` span; fuzz clean on the lexer and parser; a hostile depth-10,000 document is rejected rather than crashing; throughput floors met at 100 MB in RAM.

**P1 — model spine.** Entity arrays, ids as ordinals with tombstones, spans, extension columns and `ColumnDesc`, append-only builder API, structural validation (§10). Lowering from statements to entities. Determinism discipline (§6) is in force from the first commit; it cannot be retrofitted.
*Exit:* build, validate, and walk a depth-16 / 2k-state chart from code with no text involved; then the same chart via P0's parser, structurally identical.

**P2 — the loader.** The iterative load session (§16.2): pending list, app-supplied bytes, alias-state synthesis (§9), cross-document path resolution, cycle detection. Separate system from the parser — no file I/O, no callbacks. `Include.target` and `DocId` are the loader's; the parser fills `path` and `alias` and stops.
*Exit:* one 3-document network resolved **three ways** — from memory, through the CLI over a filesystem, and from Python/ctypes faking a network fetch — yielding the same chart and the same hash.

**P3 — printer and ABI.** Comment-preserving canonical printer and the seven canonical rules (§15), `scav fmt`, JSON dump; **handle lifecycle — create/destroy per handle, allocator injection, thread-safety per call (§16.1 blocks on it)**, ABI JSON extraction and its golden.
*Exit:* round-trip a depth-16 / 2k-state chart byte-identically, including unknown extension columns; `fmt` idempotent on the corpus; ABI driven from Python.

**P4 — metrics, space requests, layout skeleton.** Font metrics helper, the space tables, Phase 0 splitting, derived classification, trivial placement, straight-line routes, geometry columns. Validate the coordinate extent estimate (§11.2).
*Exit:* geometry columns populated for every chart, no overflow at depth 16.

**P5 — `DrawList`, reference builder, SVG backend, baseline harness.** The `DrawList` type, a builder covering standard appearance, the SVG backend with integer body and single `viewBox`, `textLength`, per-element classes, golden harness, and the `dot`/elkjs/scav side-by-side (§11.12).
*Exit:* `scav render` produces a readable diagram; baseline harness runs.

**P6 — real layout.** Layered rank, median or sifting ordering, Brandes & Köpf coordinates, bottom-up sizing (fixed pass count, no hysteresis), LR-rectpacking with `box` fallback.
*Exit:* better than P4 on the Tier-2 vector **and no worse than the incumbent** on blind review.

**P7 — orthogonal routing.** Router behind the vtable, channel-representative graph, separated OVG, A* with bend state, obstacles including submachines and placed boxes, LCA-owned separator channels, combinatorial nudging with integer offsets, `PathBox` strip placement, bench harness over ≥2 routers.
*Exit:* zero edges through boxes; blind review no worse than incumbent.

**P8 — determinism infrastructure.** Thread shim, model-derived sharding, counter-based RNG, index-ordered reduction, tiered matrix, sanitizer configs, scheduling-delay injector, the standard-library-subset include check (§6), and **`scav selftest`** — the command that makes §6's compiler-independence claim checkable by a user rather than only by our CI.
*Exit:* one structural hash and one coordinate hash across the blocking matrix; full grid green nightly. The `wasm32-wasi` row lands with P11 — until then the matrix is the six native triples, and §6's discipline is what makes adding the row a build change rather than a redesign.

**P9 — search and calibration.** Portfolio, local search, surrogate with ranking test, weight calibration, versioned profiles.
*Exit:* full-layout latency at 2k states measured and published (§11.11's bet); on the corpus, a one-state edit produces a visually small diagram change **in the common case** — §11.11's honest limit means a cascade is a hint's job, not a gate failure.

**P10 — `scavview`.** `libscavimgui`, pan, zoom, linear-scan hit test, hover/select, live highlighting (§13), relayout on request, and the Lua host (§8.3) with its sandbox and determinism obligations. Metrics-parity golden against the builder.
*Exit:* navigate 2k states smoothly; drive highlighting from an external process with no relayout.

**P11 — browser.** Core, layout, draw, and SVG to `wasm32-wasi`, single-threaded on the null shim. A JS host reads the `DrawList` and renders it, or calls the wasm SVG backend.
*Exit:* same chart in a browser, hashes identical to native.

**P12 — editor.** In-place mutation, undo/redo. **[OPEN]** arena snapshot vs command buffer with inverse.

**Plugin work is not a phase.** Column registration and the builder API land in P1; space requests need P4; builder contributions need P5. `scav-scxml` should be built incrementally alongside, because it is the acceptance test for the extension boundary (§8.2) — deferring it means discovering the boundary is wrong after everything is built against it.

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

**No display-name-vs-identifier split is required.** libhsm's `state on_idle as "Idle"` exists to dodge C identifier collisions between same-named states in different submachines; scav addresses by path (`On:main/Idle` vs `On:strays/Idle`), so the collision does not arise. A `.puml` state description lands in `State.label` (§7), which is a *description*, not a second identifier — do not add one. Codegen identifier uniqueness is a `libhsm:ident` attribute owned by that backend.

When written, the importer should be Python against `fi.hsm`'s existing lexer rather than a C++ PlantUML parser — throwaway code, runs once per chart, and `fi.hsm` already encodes the accepted grammar subset including the non-obvious rules (column-0-only comments, `note on X : handler`, legacy mode).

## 18. Licensing

scav is MIT or Apache-2.0. Verified from LICENSE bytes; GitHub's detected field is wrong for all four.

| | License | |
|---|---|---|
| ELK | `EPL-2.0 OR GPL-3.0-or-later` | Java only, no native port |
| Adaptagrams (libavoid, libcola, libvpsc, …) | LGPL-2.1-or-later, uniformly | dynamic link only, or buy Monash's commercial license |
| OGDF | GPL-2.0/3.0 | **blocker** — its exception is outbound-only |
| Graphviz ≥14.1.4 | EPL-2.0, no Secondary License | cleanest, but `dot` is weak on compound graphs |
| the bundled TTF | **[OPEN]** — must be OFL-1.1 or public domain | a layout-hash input, so it is redistributed inside the library (§16.1) |

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

**Decisions owed:** §11.8 whether to depict the implicit submachine reset · §9 durable per-element GUIDs for cross-branch rename identity · §17 P9 undo/redo mechanism.

**Unverified claims, flagged not smoothed:**
- No diagram-routing work found doing history-based negotiated congestion with rip-up-and-reroute. Unconfirmed absence, **not** novelty.
- No published TSM runtime at 1000–2000 nodes; §11.13's rejection rests on a survey statement plus absence of data at our scale.
- No published integer or combinatorial reformulation of VPSC.
- `textLength` support is patchy in non-browser SVG consumers (Inkscape, librsvg, resvg). Test before relying on it.
- Total pairwise rectangle overlap area has no published complexity bound; the `O(n log n)` sweep is our derivation. Union area at `O(n log n)` is published and optimal. Moot while Tier 0 forbids overlap.
- The coordinate extent estimate (§11.2) is derived, not measured. P1 validates it.
