#!/usr/bin/env python3
"""One command from a clean checkout to a green test run; hands CMake the tool
paths envy provides.

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
import re
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
    """The envy cache root, or "unknown"; `--root` skips envy's usage scan."""
    return subprocess.run(
        [str(ENVY), "cache", "--root"], cwd=REPO_ROOT, check=False,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    ).stdout.strip() or "unknown"


def envy_product(name: str) -> Path:
    """Runs `envy product` and returns the path it prints to stdout."""
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
    # The first host triple with a preset for this suffix, else the host default.
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


# Ninja's failed edge targets; `stamp/` ones are tests, any other is a broken build.
FAILED_EDGE = re.compile(r"^FAILED: (?:\[[^\]]*\] )?(\S+)", re.MULTILINE)


def build_is_stale(output: str) -> bool:
    """True when the build failed at a non-`stamp/` edge, or named no edge."""
    edges = FAILED_EDGE.findall(output)
    if not edges:
        return True
    return any(not edge.startswith("stamp/") for edge in edges)


def discard_binaries(build_dir: Path) -> None:
    """Deletes every executable file in `build_dir`/bin."""
    binaries = build_dir / "bin"
    if not binaries.is_dir():
        return
    for path in sorted(binaries.iterdir()):
        if path.is_file() and os.access(path, os.X_OK):
            path.unlink(missing_ok=True)


def build_and_relay(cmake: Path, preset: str) -> tuple[str, int]:
    """Echoes each line as it arrives, keeping the transcript for build_is_stale."""
    argv = [str(cmake), "--build", "--preset", preset]
    print(f"+ {' '.join(argv)}", flush=True)
    proc = subprocess.Popen(argv, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    lines = []
    for line in proc.stdout:
        print(line, end="", flush=True)
        lines.append(line)
    return "".join(lines), proc.wait()


def link_compile_commands(build_dir: Path) -> None:
    """Point the single compilation database at the tree just configured."""
    if not (source := build_dir / "compile_commands.json").exists():
        return
    link = REPO_ROOT / "out/compile_commands.json"
    link.unlink(missing_ok=True)
    try:
        link.symlink_to(os.path.relpath(source, link.parent))
    except OSError:
        # Copies where symlinks are unavailable (Windows without developer mode).
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

    # The preset's compiler resolved on PATH; passed as CMAKE_CXX_COMPILER below.
    resolved = which(c) if (c := preset_compiler(preset)) else None
    resolved = Path(resolved).as_posix() if resolved else None

    if args.clean and build_dir.exists():
        print(f"+ rm -rf {build_dir}", flush=True)
        rmtree(build_dir)
    elif resolved and (was := cached_compiler(build_dir)) and was != resolved:
        # A changed compiler deletes the build tree before configuring.
        print(f"+ rm -rf {build_dir}\n    compiler changed: {was} -> {resolved}",
              flush=True)
        rmtree(build_dir)

    extra = [a for a in args.cmake_args if a != "--"]
    if resolved:
        extra.append(f"-DCMAKE_CXX_COMPILER={resolved}")
    # Sets SCAV_RUN_TESTS on every configure, overriding any cached value.
    extra.append(f"-DSCAV_RUN_TESTS={'OFF' if args.no_test else 'ON'}")
    if not any("SCAV_TEST_TIER" in a for a in extra):
        extra.append("-DSCAV_TEST_TIER=fast")

    # MSan presets get an instrumented libc++ from msan_libcxx.py.
    if preset.endswith("-msan") and not any("SCAV_MSAN_LIBCXX_DIR" in a for a in extra):
        libcxx = subprocess.run(
            [str(python), str(REPO_ROOT / "tools/msan_libcxx.py")],
            cwd=REPO_ROOT, check=True, stdout=subprocess.PIPE, text=True,
        ).stdout.strip()
        extra.append(f"-DSCAV_MSAN_LIBCXX_DIR={libcxx}")

    run(cmake, "--preset", preset, f"-DCMAKE_MAKE_PROGRAM={ninja}",
        f"-DSCAV_DOCTEST_DIR={doctest}", f"-DPython3_EXECUTABLE={python}", *extra)
    link_compile_commands(build_dir)
    # Builds and runs the tests, which are build steps.
    transcript, code = build_and_relay(cmake, preset)
    if code != 0:
        if build_is_stale(transcript):
            discard_binaries(build_dir)
            print("\nThe tree did not build; binaries removed rather than left stale.",
                  flush=True)
        raise SystemExit(code)

    if args.coverage and not args.no_test:
        run(python, REPO_ROOT / "tools/coverage.py", "--build", build_dir)

    print(f"\nGreen: {preset}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
