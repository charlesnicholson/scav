# scav

Statechart authoring, layout, rendering. [PRD.md](PRD.md) is the normative design
document; everything below describes what is built.

**Status: P3 — the printer.** `libscavcore` carries the `.scav` lexer and
parser, the model (flat entity arrays linked by ordinal ids with tombstones,
extension columns in lockstep, an append-only builder, structural validation),
the iterative loader that turns one document into a resolved network, and now
the comment-preserving canonical printer.

Canonical form is a property of *running* the printer, not of the format, so
`scav fmt` is what makes it something a repo can rely on. Printing reconstructs
gofmt-style rather than echoing the source: two documents that differ only in
formatting print the same bytes, which is what the format hash and a three-way
merge rest on. The seven rules are PRD §15's — long keyword spellings, a
repeated key as a list, `"true"` as a flag, a shared namespace as a block,
attributes sorted by key bytes while structure keeps document order, a trailing
comma iff the block broke, and line breaking by a column budget. Comments carry
their position (leading, trailing, own-line) and are the expensive half.

Whitespace is otherwise not the model's, with one exception: `blank_before` on a
`Statement` keeps a blank line between two statements, because source order is a
layout hint and grouping is how an author writes that hint down. It is a bit and
not a count, so a run collapses to one, and it is suppressed where it would open
or close a block.

The `scav` executable (`apps/cli`) now has five verbs:

```
scav render [-o F] [--embed-font] [--profile N] <file>   chart -> SVG
scav fmt [--check] <file>...      canonical print, in place; --check gates
scav check <file>                 structural validation, exit 1 on a finding
scav deps [--target N] <file>     the document network as a make/ninja depfile
scav dump [--hash|--json] [--layout] <file>  the model: entity rows, not syntax
```

`libscavlayout` runs four phases: boundary splitting, a layered graph per
submachine, extents composed bottom-up onto those ranks, and routes through
them. `libscavdraw` measures text against the bundled font, builds a `DrawList`
from the geometry columns, and `libscavsvg` writes it out; `scav render` is
those three in a line. Routes are orthogonal: an A* over a per-frame visibility
graph separated into h- and v-planes, which is what a caller with no opinion
gets. No corpus chart routes an edge through a box. What is still wrong is
between routes and around them -- two edges reaching the same lane draw as one
polyline, and a transition label can land on a state name -- and that is P7c
and P7d.

Bindings are generated from `abi/scav_abi.json`, which is extracted from the C
headers and committed as a golden, so an ABI break is a review diff rather than
a downstream segfault. `bindings/python/scav` drives the whole pipeline.

Four properties of the front end are worth knowing before reading it:

- **Lexing and parsing are separate passes over a materialized token vector**, not
  a pull loop. `lex()` returns every token at once; the parser walks the array by
  index. That is what makes the two separately timeable and separately fuzzable,
  which P0's exit gate asks for, and it costs about 2× the input in transient
  memory — measured, not assumed, by `perf: peak memory is a bounded multiple of
  the input`.
- **The descent is an explicit `std::vector` of frames, not the call stack.**
  Nesting depth is attacker-controlled, so a call-recursive parser answers a
  hostile document with a stack overflow. Here the depth cap is an ordinary
  comparison and the answer is a diagnostic. Same reason the trivia-attachment
  walk and the synthetic generator are iterative. The printer's emitter and its
  width pass are stacks for the same reason.
- **No API forces a caller through a filesystem.** Parsing takes bytes;
  acquiring them is a different system. `read_file` and `load_file` ship in
  core as batteries written against the same public primitives, and are
  skippable in full — a browser host, a binding and an editor holding unsaved
  buffers stay first-class. Unit tests carry their charts as inline literals.
- **The printer reads a `ParsedDocument`, not a `Chart`.** Only the statement
  stream distinguishes `@k` from `@k = "true"`, records the `@ns { ... }`
  spelling, and holds the endpoint path an author wrote; a `Chart` has resolved
  those to `StateId`s. Model-to-text arrives with the editor that needs it.

## Build

```
./build.sh          # macOS / Linux
build.bat           # Windows
```

One command from a clean checkout to a green test run — no arguments, no
environment to set up, no steps below this line that you need to read first.

**Building is testing.** Every test, unit and functional, is a build step whose
output is a stamp file, so `cmake --build` cannot report success with a failing
test and there is no second command to forget. It is also incremental: a second
build back to back is `ninja: no work to do` in zero seconds, and editing
`ci.yml` re-runs only the test that reads it. That is why there is no CTest here
— ctest re-runs everything every invocation, so "build, then test" can never be
incremental.

The first run takes a few minutes: [envy](https://github.com/envy-package-manager/envy)
bootstraps itself, then provisions cmake, ninja, doctest and python 3.14 into
`out/.envy`. After that it is a cache hit. Every build prints which cache it
used, because the default costs ~540 MB and that should not be silent.

### Where the toolchain lives

Under `out/.envy`, with everything else scav generates, so **`rm -rf out` removes
every tool, package and build artifact**. Nothing is written to `$HOME`. That is
the manifest's choice:

```lua
-- @envy cache-local "out/.envy"
```

envy defaults to a user-wide cache; naming a project tree is what opts out of it.

The cost is one toolchain copy per checkout, ~540 MB. If you keep several
worktrees or run parallel agents, run this once from the repo root and every
worktree shares one copy:

```sh
./bin/envy cache --shared
```

That writes a zero-byte `.envy-cache-shared` marker, which every reader — the
binary and both bootstrap launchers — honours. `./bin/envy cache --local` goes
back and removes the marker; a marker is only ever written when it diverges from
the manifest, so the steady state is no marker at all. `.envy-cache-*` is
gitignored and listed in `.worktreeinclude`, so Conductor carries your choice
into new workspaces. Every build prints the resolved root.

Worth knowing before sharing:

- **Mostly free.** Packages are keyed by content fingerprint, so scav's
  `envy.cmake@r0` is the same `darwin-arm64-blake3-49a9b2620de8c380` build any
  other envy project already fetched. Measured: three of four packages hit
  immediately, a 58 MB python fetch, ~499 MB saved per extra worktree.
- **Concurrent access is safe.** Three simultaneous `envy sync` runs against one
  fresh cache all exit 0 — one installs, the others hit, resulting tree correct.
- **What you give up is the factory reset.** `rm -rf out` no longer removes the
  toolchain, and another checkout can invalidate a shared tree. Hence the default.

`ENVY_CACHE_ROOT` (absolute only) still overrides everything, which is what CI
uses. This cannot live in a CMake preset: envy resolves the cache and hands back
absolute paths to cmake, ninja and python before CMake is invoked at all.

Conductor workspaces run `./bin/envy cache --shared` from
`.conductor/settings.toml`, so an agentic session on a fresh worktree reuses the
user-wide cache instead of downloading its own.

Bumping the pinned envy version means regenerating the tracked launchers under
`bin/` with `./bin/envy deploy --platform all` — `envy sync` deploys only the
host's flavour, and a mixed set is a build step resolving the cache by older
rules than the binary it bootstraps. `func.provisioning` fails on a mixed set.

```
./build.sh --config debug            # matrix config: debug | release | testable
./build.sh --sanitizer asan          # asan | ubsan | tsan | msan
./build.sh --coverage                # coverage, then the untested-file gate
./build.sh --no-test                 # build without running the tests
./build.sh --list                    # the presets this host can run
./build.sh --clean                   # delete the build tree first
./build.sh -- -DSCAV_CLANG_TIDY=ON   # anything after `--` goes to cmake
```

**Everything generated lives under `out/`.** Build trees, the envy package cache,
test scratch. `rm -rf out` is a factory reset, and nothing writes to `$HOME`.

**Nothing comes from the system but the compiler.** cmake, ninja, doctest and
python are envy's — CI's Linux container ships its own copies and scav ignores
them, which `func.provisioning` enforces.

**The lint gates are behind `SCAV_LINT`.** Nothing compiles with clang-format or
clang-tidy, so an ordinary build never downloads them and neither does any CI
row but the one Linux job that runs them, in parallel with the builds. Set the
variable to run a gate yourself — on macOS that compiles clang once, since LLVM
publishes no darwin prebuilt:

```
SCAV_LINT=1 $(./bin/envy product python3) tools/format.py          # rewrite
SCAV_LINT=1 ./build.sh --no-test -- -DSCAV_CLANG_TIDY=ON           # tidy
```

`envy.lua` pins the version both gates run, so a finding reads the same
everywhere; without `SCAV_LINT` they fall back to whatever is on `PATH`.

## Building without envy

envy pins the toolchain for *scav's* CI. It is deliberately not a build
prerequisite: layout output is a function of the C++ abstract machine rather than
of any implementation, so any conforming C++20 toolchain is supported and requiring
envy would contradict that:

```
cmake -S . -B build -DSCAV_DOCTEST_DIR=/path/to/doctest -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

That one command builds and tests, because the tests are build steps. There is no
second command and no ctest.

`-DSCAV_BUILD_TESTS=OFF` drops the doctest requirement entirely;
`-DSCAV_RUN_TESTS=OFF` keeps the test executables but takes them out of `ALL`.

## Layout

| | |
|---|---|
| `envy.lua` | toolchain and package manifest; envy's root marker |
| `bin/` | envy's product launchers, checked in and deployed by `envy sync`. Each one bootstraps envy and its package cache on a machine that has neither. The clang ones appear only under `SCAV_LINT`, so they are not tracked |
| `CMakePresets.json` | the determinism matrix, generated by `tools/gen_presets.py` |
| `cmake/` | warnings, sanitizers, the library/subsystem helpers, the export config |
| `include/scav/` | the cross-library vocabulary: POD spellings, no functions |
| `src/<lib>/include/scav/` | that library's public API — the only thing a consumer can reach |
| `src/<lib>/<subsystem>/` | private headers, sources, and their unit tests, adjacent |
| `test_data/golden/` | committed goldens, layered by stage |
| `functional_tests/` | Python 3.14, standard library only |
| `tools/` | *project-wide* tooling only: build driver, preset generator, coverage gate, MSan libc++ builder. Run them with `$(./bin/envy product python3)`. A script used by one library lives with that library |
| `out/` | gitignored: all build output plus the envy cache |

## Finding things

A library's API is `src/<lib>/include/scav/scav_*.h`, and that directory is its
only `PUBLIC` include path. Everything else under `src/<lib>/` needs `-Isrc`,
which is `PRIVATE` to the library and its own tests — so a consumer cannot name a
private header, in the build tree or an installed one. `func.install_consumer`
compiles every public header against nothing but the install prefix, which is
what catches a public header that quietly includes a private one.

**One public header per library**, so there is never a question of which to
include. There are two in the whole project, and a core user includes one:

| header | is |
|---|---|
| `scav/scav_core.h` | all of `libscavcore` |
| `scav/scav_types.h` | the cross-library POD vocabulary; pulled in by the others |

Inside `scav_core.h`, every function carries the prefix of its section, so a
symbol names its neighbourhood and the sections are the table of contents:

| prefix | section |
|---|---|
| — | ids, spans, `INVALID` |
| `narrow*` | the one checked narrowing helper |
| `diag_` | diagnostic codes, line/column from a span |
| `string_` | reading a string pool |
| `source_text_` | normalization on read: BOM, LF, UTF-8, NFC |
| `lex_` | tokens, trivia, string-literal decoding |
| `syntax_` | the statement rows and their spellings |
| `parse_` | the entry points most callers want |
| `chart_` | reading the model: refs, liveness, attrs, addressing |
| `build_` | the append-only builder |
| `column_` | extension-column registration and access |
| `resolve_` | state paths to ids |
| `lower_` | statements to entities |
| `validate_` | the structural checks |

Private, and deliberately unreachable from outside: `lang/unicode_nfc.h`.

**Test code follows one rule.** `foo_tests.cpp` beside `foo.cpp` (or `foo.h`)
is that file's unit tests, strictly paired — nothing else may use the suffix.
Everything suite-level lives in `src/core/tests/`, named by its class
(`functional_*`, `fuzz_*`, `perf_*`), beside the shared fixtures they drive
(`test_support.h`, `test_charts.h`, `test_synth.{h,cpp}`).

**A private header's functions carry the header's stem** — `model.h` declares
`model_*`, `unicode_nfc.h` declares `unicode_nfc_*`, `scav_stable_sort.h`
declares `scav_stable_sort` — so a call site names its header without a grep.
(Test fixture headers count their stem after the `test_` marker:
`test_synth.h` declares `synth_*`.)

## The Unicode tables

Text is normalized on read — BOM stripped, line endings folded to LF, UTF-8
validated, NFC applied — because `core.autocrlf` on Windows and NFD on macOS
otherwise put different bytes in the string pool from the same commit — and those
bytes are the labels that get measured, laid out, and rendered.

NFC needs a table. `src/core/lang/unicode_nfc_tables.inc` is generated from the
UCD and committed, so a build needs neither the network nor Python. The
conformance vectors beside it are Unicode's own `NormalizationTest.txt`, thinned
where it repeats itself. Regenerate only when the pinned Unicode version moves —
**it is a determinism input**, so bumping it can change the string pool for a
document containing characters assigned after the current version:

```
cmake --build out/<preset> --target scav_core_unicode_tables
```

The generator lives at `src/core/lang/gen_unicode_tables.py`, beside the code it
generates and beside its own output, because nothing outside `libscavcore` has a
use for it. Only project-wide tooling lives in `tools/`. Every path in it is
script-relative, so it also runs directly from anywhere.

Hangul is algorithmic in both directions and has no table entry at all, which is
why the tests walk all 11,172 syllables rather than trusting a lookup.

## What the harness enforces

Each of these is a design rule made mechanical, because a rule that only lives in a
document is a rule that gets discovered late:

- **Warnings are errors**, per-compiler set pinned in `cmake/ScavWarnings.cmake`.
- **Sanitizers are one enum**, `SCAV_SANITIZER=NONE|ASAN|UBSAN|TSAN|MSAN`. ASan and
  TSan cannot coexist, so a boolean pair would invite an unbuildable combination.
  An unsupported combination is a configure error naming the reason.
- **MSan gets an instrumented `libc++`**, built by `tools/msan_libcxx.py`. Without
  one, MSan reports false positives from uninstrumented standard-library code for
  the life of the project, and then gets switched off.
- **Library layering** is checked at configure time: a library may link a strictly
  lower tier and nothing else, so `libscavdraw` linking `libscavlayout` fails to
  configure rather than fails review.
- **A subsystem with no tests fails to configure**, and so does a build that
  declares no test executable at all. A phase is not done until its tests are.
- **Tests run as part of the build**, each behind a stamp file in
  `out/<preset>/stamp/`. A failing test deletes its stamp and fails the build, so
  the next build retries it rather than trusting a stale pass. `rm -rf
  out/<preset>/stamp` re-runs the suite without recompiling anything, and
  `--target run.<name>` runs exactly one.
- **An untested file fails the build**, via `tools/coverage.py`. Not a percentage
  target — numbers are not what this gates on.
- **Performance is measured, but the clock does not gate.** The perf tests always
  run, and the bounds they assert by default are counted rather than timed — peak
  memory as a multiple of the input. The throughput and scaling floors need
  `-DSCAV_PERF_FLOORS=ON`, because CI runs on shared runners where a stalled box
  reports a regression that is not there.
- **Each library builds twice**, shipping and testable. Unit tests link the
  testable variant and reach internal functions through `SCAV_INTERNAL`; mocks and
  interface seams are rejected.
- **Failures are actually reported.** `scav_core_tests_reports_failures` runs
  the deliberately-failing case and expects a non-zero exit. A harness that
  reports nothing looks identical to one where everything passes.
- **An installed scav is consumable.** `func.install_consumer` installs to a
  prefix and configures a separate project against it with nothing but
  `CMAKE_PREFIX_PATH`.
- **envy really provisioned it.** `func.provisioning` fails if the configured tree
  recorded a tool from outside the envy cache. CI's Linux rows go further and run
  in a container with nothing installed but a compiler.

## Formatting and clang-tidy

Both ship with the clang toolchain, so they come from wherever your compiler does.
CI runs them on one Linux row against the version its container image pins —
clang-format's output moves between releases, so a single version has to be the
gate:

```
$(./bin/envy product python3) tools/format.py           # rewrite
$(./bin/envy product python3) tools/format.py --check   # CI's gate
./build.sh -- -DSCAV_CLANG_TIDY=ON
```

## Licence

MIT or Apache-2.0.
