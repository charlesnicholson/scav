#!/usr/bin/env python3
"""One command from a clean checkout to a green test run.

envy is scav's own CI convenience, not a build prerequisite, so this only ever hands
CMake paths it discovered; the CMake tree itself asks envy for nothing.

./build.sh                          host default preset, build, test
./build.sh --preset linux-gcc-libstdcxx-release
./build.sh --sanitizer asan         host default triple, ASan preset
./build.sh --coverage               host default triple, coverage gate
./build.sh --list                   list the presets this host can run
./build.sh --clean                  delete the build tree first
./build.sh --no-test                configure and build only
./build.sh -- -DSCAV_CLANG_TIDY=ON  pass anything else to `cmake --preset`
"""

import argparse
import json
import os
import platform
import subprocess
from pathlib import Path
from shutil import rmtree, which

REPO_ROOT = Path(__file__).resolve().parent.parent
ENVY = REPO_ROOT / ("bin/envy.bat" if platform.system() == "Windows" else "bin/envy")

# Keyed by what the host can run; the first entry is that host's default.
HOST_TRIPLES: dict[str, list[str]] = {
    "Darwin": ["macos-clang-libcxx"],
    "Linux": ["linux-gcc-libstdcxx", "linux-clang-libstdcxx", "linux-clang-libcxx"],
    "Windows": ["windows-msvc", "windows-clang"],
}


def run(*cmd: str | Path) -> None:
    argv = [str(c) for c in cmd]
    print(f"+ {' '.join(argv)}", flush=True)
    subprocess.run(argv, cwd=REPO_ROOT, check=True)


def envy_cache_root() -> str:
    """Asked of envy rather than derived; `--root` skips the usage scan."""
    return subprocess.run(
        [str(ENVY), "cache", "--root"], cwd=REPO_ROOT, check=False,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    ).stdout.strip() or "unknown"


def envy_product(name: str) -> Path:
    """envy narrates to stderr and prints the path to stdout."""
    out = subprocess.run(
        [str(ENVY), "product", name], cwd=REPO_ROOT, check=True,
        stdout=subprocess.PIPE, text=True,
    ).stdout.strip()
    if not out:
        raise SystemExit(f"envy product {name} printed nothing")
    return Path(out)


def host_triples() -> list[str]:
    if not (triples := HOST_TRIPLES.get(system := platform.system())):
        raise SystemExit(f"no matrix triple for host system {system!r}")
    return triples


def available_presets() -> list[str]:
    doc = json.loads((REPO_ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
    prefixes = tuple(f"{t}-" for t in host_triples())
    return [p["name"] for p in doc["configurePresets"]
            if not p.get("hidden") and p["name"].startswith(prefixes)]


def resolve_preset(args: argparse.Namespace) -> str:
    if args.preset:
        return args.preset
    suffix = args.sanitizer or ("coverage" if args.coverage else args.config)
    # Not every triple supports every suffix, so fall through rather than naming a
    # preset that was never going to exist.
    available = available_presets()
    return next((f"{t}-{suffix}" for t in host_triples() if f"{t}-{suffix}" in available),
                f"{host_triples()[0]}-{suffix}")


def preset_compiler(preset: str) -> str | None:
    """The compiler a preset names, following `inherits`; earlier entries win."""
    doc = json.loads((REPO_ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
    by_name = {p["name"]: p for p in doc["configurePresets"]}

    def walk(name: str) -> str | None:
        if (node := by_name.get(name)) is None:
            return None
        if found := node.get("cacheVariables", {}).get("CMAKE_CXX_COMPILER"):
            return found
        return next((f for p in node.get("inherits", []) if (f := walk(p))), None)

    return walk(preset)


def cached_compiler(build_dir: Path) -> str | None:
    """What the existing tree was configured with, if there is one."""
    if not (cache := build_dir / "CMakeCache.txt").exists():
        return None
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_CXX_COMPILER:") and "=" in line:
            return line.split("=", 1)[1].strip()
    return None


def link_compile_commands(build_dir: Path) -> None:
    """Point the single compilation database at the tree just configured."""
    if not (source := build_dir / "compile_commands.json").exists():
        return
    link = REPO_ROOT / "out/compile_commands.json"
    link.unlink(missing_ok=True)
    try:
        link.symlink_to(os.path.relpath(source, link.parent))
    except OSError:
        # Windows without developer mode cannot symlink, and a copy is equivalent
        # here.
        source.copy(link)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--preset", help="configure preset name (see --list)")
    parser.add_argument("--config", default="release",
                        choices=["debug", "release", "testable"],
                        help="matrix configuration (default: %(default)s)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--sanitizer", choices=["asan", "ubsan", "tsan", "msan"],
                      help="use the host default triple's sanitizer preset")
    mode.add_argument("--coverage", action="store_true",
                      help="use the coverage preset and run the gate")
    parser.add_argument("--list", action="store_true", help="list this host's presets")
    parser.add_argument("--clean", action="store_true", help="delete the build tree first")
    parser.add_argument("--no-sync", action="store_true", help="skip `envy sync`")
    parser.add_argument("--no-test", action="store_true",
                        help="build without running the tests")
    parser.add_argument("cmake_args", nargs=argparse.REMAINDER,
                        help="everything after `--` is passed to `cmake --preset`")
    args = parser.parse_args()

    if args.list:
        print(*available_presets(), sep="\n")
        return 0

    preset = resolve_preset(args)
    if preset not in available_presets():
        raise SystemExit(f"preset {preset!r} is not runnable on this host. Available:\n  "
                         + "\n  ".join(available_presets()))

    if not args.no_sync:
        run(ENVY, "sync")

    cache = envy_cache_root()
    hint = ("  (./bin/envy cache --shared to share one across worktrees)"
            if Path(cache).is_relative_to(REPO_ROOT) else "")
    print(f"envy cache: {cache}{hint}", flush=True)

    cmake, ninja, doctest, python = [
        envy_product(p) for p in ("cmake", "ninja", "doctest_cpp_dir", "python3")
    ]

    build_dir = REPO_ROOT / "out" / preset

    # The presets name a compiler unqualified, since every matrix host keeps its
    # own somewhere different. Resolving here pins and prints the choice.
    resolved = which(c) if (c := preset_compiler(preset)) else None
    resolved = Path(resolved).as_posix() if resolved else None

    if args.clean and build_dir.exists():
        print(f"+ rm -rf {build_dir}", flush=True)
        rmtree(build_dir)
    elif resolved and (was := cached_compiler(build_dir)) and was != resolved:
        # A changed compiler resets the cache, and the -D arguments below do not
        # survive that re-run: configure then blames a missing doctest.
        print(f"+ rm -rf {build_dir}\n    compiler changed: {was} -> {resolved}",
              flush=True)
        rmtree(build_dir)

    extra = [a for a in args.cmake_args if a != "--"]
    if resolved:
        extra.append(f"-DCMAKE_CXX_COMPILER={resolved}")
    # Always stated, so a --no-test run cannot stick in the cache and turn
    # every later plain build into a silent test skip.
    extra.append(f"-DSCAV_RUN_TESTS={'OFF' if args.no_test else 'ON'}")

    # MSan without an instrumented libc++ reports false positives forever, and one
    # command has to cover that rather than documenting a step.
    if preset.endswith("-msan") and not any("SCAV_MSAN_LIBCXX_DIR" in a for a in extra):
        libcxx = subprocess.run(
            [str(python), str(REPO_ROOT / "tools/msan_libcxx.py")],
            cwd=REPO_ROOT, check=True, stdout=subprocess.PIPE, text=True,
        ).stdout.strip()
        extra.append(f"-DSCAV_MSAN_LIBCXX_DIR={libcxx}")

    run(cmake, "--preset", preset, f"-DCMAKE_MAKE_PROGRAM={ninja}",
        f"-DSCAV_DOCTEST_DIR={doctest}", f"-DPython3_EXECUTABLE={python}", *extra)
    link_compile_commands(build_dir)
    # Tests are build steps, so this one command builds and verifies. A second
    # run is a no-op: every test stamp is newer than its inputs.
    run(cmake, "--build", "--preset", preset)

    if args.coverage:
        run(python, REPO_ROOT / "tools/coverage.py", "--build", build_dir)

    print(f"\nGreen: {preset}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
