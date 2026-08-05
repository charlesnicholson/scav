#!/usr/bin/env python3
"""Generate CMakePresets.json from the determinism matrix declared below.

More JSON than anyone should hand-edit, and the format permits no comments, so the
matrix lives here and the committed file is generated from it.

Usage:
    $(./bin/envy product python3) tools/gen_presets.py            # write CMakePresets.json
    $(./bin/envy product python3) tools/gen_presets.py --stdout   # print, write nothing
    $(./bin/envy product python3) tools/gen_presets.py --check    # exit non-zero if the file is stale
"""

import argparse
import json
import sys
from pathlib import Path
from typing import Any

type Cache = dict[str, str]
type Preset = dict[str, Any]

PRESETS_PATH = Path(__file__).resolve().parent.parent / "CMakePresets.json"


def clang(lib: str) -> Cache:
    return {"CMAKE_CXX_COMPILER": "clang++",
            "CMAKE_CXX_FLAGS": f"-stdlib={lib}",
            "CMAKE_EXE_LINKER_FLAGS": f"-stdlib={lib}"}


# Realizable triples only, chosen to span the axes that historically diverge: three
# standard libraries, three vendors' codegen, LP64 vs LLP64, x86_64 vs arm64.
TRIPLES: dict[str, tuple[str, str, Cache]] = {
    "macos-clang-libcxx": ("Darwin", "macOS / clang / libc++",
                           {"CMAKE_CXX_COMPILER": "clang++"}),
    "linux-clang-libcxx": ("Linux", "Linux / clang / libc++", clang("libc++")),
    "linux-clang-libstdcxx": ("Linux", "Linux / clang / libstdc++", clang("libstdc++")),
    "linux-gcc-libstdcxx": ("Linux", "Linux / gcc / libstdc++",
                            {"CMAKE_CXX_COMPILER": "g++"}),
    "windows-msvc": ("Windows", "Windows / MSVC / MSVC STL",
                     {"CMAKE_CXX_COMPILER": "cl"}),
    "windows-clang": ("Windows", "Windows / clang / MSVC STL",
                      {"CMAKE_CXX_COMPILER": "clang-cl"}),
}
DESC: dict[str, str] = {t: desc for t, (_, desc, _) in TRIPLES.items()}

CONFIGS: list[str] = ["debug", "release", "testable"]

# Availability is a property of the toolchains, not a preference: MSVC has no
# UBSan, clang-cl's ASan runtime is added by a driver CMake does not use to link,
# neither Windows toolchain has TSan, and only clang/Linux can do MSan.
SANITIZERS: dict[str, list[str]] = {
    "asan": [t for t in TRIPLES if t != "windows-clang"],
    "ubsan": [t for t in TRIPLES if t != "windows-msvc"],
    "tsan": [t for t in TRIPLES if not t.startswith("windows")],
    "msan": ["linux-clang-libcxx"],
}

# The gate reads llvm-cov's per-file summary, so it is clang-only.
COVERAGE: list[str] = ["macos-clang-libcxx", "linux-clang-libcxx"]

# name -> build type, extra cache, description
CONFIG_BASES: dict[str, tuple[str, Cache, str]] = {
    "cfg-debug": ("Debug", {}, "Matrix configuration 1 of 3."),
    "cfg-release": ("Release", {}, "Matrix configuration 2 of 3."),
    "cfg-testable": (
        "Release", {"SCAV_TESTING": "ON"},
        "Matrix configuration 3 of 3. Release plus SCAV_TESTING, so the shipping "
        "libraries compile with SCAV_INTERNAL at external linkage. All layout "
        "arithmetic is integer, so this must produce byte-identical output to "
        "release; divergence means undefined behaviour somewhere.",
    ),
    "cfg-sanitize": (
        "RelWithDebInfo", {},
        "Not a matrix configuration. Sanitizers are their own test class and are "
        "timing-independent, so they run at -O2 -g rather than adding a fourth "
        "configuration to the matrix.",
    ),
}

BASE_DESCRIPTION: str = (
    "CMake + Ninja, and everything scav generates lands under out/, one gitignored "
    "directory. Build trees are named for the preset that made them, so a factory "
    "reset is `rm -rf out`."
)

TRIPLE_DESCRIPTION_SUFFIX: str = (
    "Realizable triples only, chosen to span the axes that historically diverge: "
    "three standard libraries, three vendors' codegen, LP64 vs LLP64, and x86_64 "
    "vs arm64."
)


def sanitizer_flags(san: str) -> Cache:
    """MSan takes its standard library from the instrumented prefix, so the triple's
    -stdlib= is both redundant and, under -Werror, an unused-argument error."""
    return {"CMAKE_CXX_FLAGS": "", "CMAKE_EXE_LINKER_FLAGS": ""} if san == "msan" else {}


def build_document() -> Preset:
    # Earlier entries in `inherits` win, so a triple's compiler choice takes
    # precedence over anything a configuration base sets.
    configure: list[Preset] = [
        {"name": "base", "hidden": True, "description": BASE_DESCRIPTION,
         "generator": "Ninja",
         "binaryDir": "${sourceDir}/out/${presetName}",
         "installDir": "${sourceDir}/out/${presetName}/install",
         "cacheVariables": {"SCAV_WARNINGS_AS_ERRORS": "ON"}},
        *({"name": name, "hidden": True, "inherits": "base", "description": desc,
           "cacheVariables": {"CMAKE_BUILD_TYPE": build_type, **extra}}
          for name, (build_type, extra, desc) in CONFIG_BASES.items()),
        *({"name": f"t-{t}", "hidden": True,
           "description": f"Platform triple: {desc}. {TRIPLE_DESCRIPTION_SUFFIX}",
           "condition": {"type": "equals", "lhs": "${hostSystemName}", "rhs": host},
           "cacheVariables": dict(sorted(cache.items()))}
          for t, (host, desc, cache) in TRIPLES.items()),
        *({"name": f"{t}-{c}", "displayName": f"{DESC[t]} -- {c}",
           "inherits": [f"t-{t}", f"cfg-{c}"]}
          for t in TRIPLES for c in CONFIGS),
        *({"name": f"{t}-{san}", "displayName": f"{DESC[t]} -- {san.upper()}",
           "inherits": [f"t-{t}", "cfg-sanitize"],
           "cacheVariables": {"SCAV_SANITIZER": san.upper(), **sanitizer_flags(san)}}
          for san, triples in SANITIZERS.items() for t in triples),
        *({"name": f"{t}-coverage", "displayName": f"{DESC[t]} -- branch coverage",
           "inherits": [f"t-{t}", "cfg-debug"],
           "cacheVariables": {"SCAV_COVERAGE": "ON"}}
          for t in COVERAGE),
    ]
    concrete = [p["name"] for p in configure if not p.get("hidden")]

    return {
        "version": 6,
        "cmakeMinimumRequired": {"major": 3, "minor": 28, "patch": 0},
        "configurePresets": configure,
        "buildPresets": [{"name": n, "configurePreset": n, "jobs": 0} for n in concrete],
        # noTestsAction=error: a preset that runs zero tests is a broken preset, and
        # reports success otherwise.
        "testPresets": [
            {"name": n, "configurePreset": n,
             "output": {"outputOnFailure": True, "shortProgress": True},
             "execution": {"noTestsAction": "error", "stopOnFailure": False}}
            for n in concrete
        ],
    }


def render() -> str:
    return json.dumps(build_document(), indent=2) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdout", action="store_true", help="print, write nothing")
    parser.add_argument("--check", action="store_true", help="fail if the file is stale")
    args = parser.parse_args()
    rendered = render()

    if args.stdout:
        sys.stdout.write(rendered)
    elif args.check:
        if PRESETS_PATH.read_text(encoding="utf-8") != rendered:
            print(f"{PRESETS_PATH} is stale; run tools/gen_presets.py", file=sys.stderr)
            return 1
    else:
        PRESETS_PATH.write_text(rendered, encoding="utf-8")
        print(f"wrote {PRESETS_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
