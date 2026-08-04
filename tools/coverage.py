#!/usr/bin/env python3
"""An untested file fails the build; percentages are printed but never gated on.

Run through `./build.sh --coverage`, which configures, runs the tests, then calls
this."""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

type Summary = dict[str, dict[str, float]]

REPO_ROOT = Path(__file__).resolve().parent.parent

# Executed by definition, so gating on them says nothing.
EXCLUDED_SUFFIXES: tuple[str, ...] = ("_tests.cpp", "doctest_main.cpp")


def find_llvm_tool(name: str) -> str:
    """Apple's clang hides these behind xcrun; other toolchains put them on PATH.
    Both are ordinary lookups, and guessing a version suffix is not."""
    if found := shutil.which(name):
        return found
    if xcrun := shutil.which("xcrun"):
        result = subprocess.run([xcrun, "--find", name], stdout=subprocess.PIPE,
                                text=True, check=False)
        if not result.returncode and result.stdout.strip():
            return result.stdout.strip()
    raise SystemExit(f"{name} not found; it ships beside clang in every LLVM install")


def is_scav_source(path: str) -> bool:
    resolved = Path(path).resolve()
    if not resolved.is_relative_to(REPO_ROOT):
        return False  # standard library, doctest, anything outside the tree
    rel = resolved.relative_to(REPO_ROOT)
    return rel.parts[0] in ("src", "include") and not str(rel).endswith(EXCLUDED_SUFFIXES)


def percent(summary: Summary, key: str) -> float:
    return summary[key]["percent"] if summary[key]["count"] else 100.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path,
                        help="build tree configured with SCAV_COVERAGE=ON")
    build = parser.parse_args().build.resolve()

    if not (raws := sorted((build / "coverage").glob("*.profraw"))):
        raise SystemExit(
            f"no .profraw under {build / 'coverage'}. Configure with "
            "-DSCAV_COVERAGE=ON and run the tests first; an empty profile set "
            "would otherwise report perfect coverage."
        )
    if not (binaries := sorted(p for p in (build / "bin").iterdir() if p.is_file())):
        raise SystemExit(f"no instrumented binaries under {build / 'bin'}")

    # A file no test links is absent from the report entirely, so a report-driven
    # check would silently pass it -- the exact case this gate exists to catch.
    if not (manifest := build / "scav_sources.txt").is_file():
        raise SystemExit(f"{manifest} is missing; it is written at configure time")
    declared = {Path(line).resolve()
                for line in manifest.read_text(encoding="utf-8").splitlines() if line}
    if not declared:
        raise SystemExit(f"{manifest} lists no production sources")

    merged = build / "coverage/scav.profdata"
    subprocess.run([find_llvm_tool("llvm-profdata"), "merge", "-sparse",
                    "-o", str(merged), *(str(r) for r in raws)], check=True)
    # Each binary carries its own coverage mapping.
    export = subprocess.run(
        [find_llvm_tool("llvm-cov"), "export", "-summary-only",
         f"-instr-profile={merged}", str(binaries[0]),
         *(a for b in binaries[1:] for a in ("-object", str(b)))],
        stdout=subprocess.PIPE, text=True, check=True,
    )
    rows: dict[Path, Summary] = {Path(f["filename"]).resolve(): f["summary"]
            for data in json.loads(export.stdout)["data"]
            for f in data["files"] if is_scav_source(f["filename"])}

    print(f"{'file':<48} {'lines':>14} {'branches':>14}")
    untested: list[tuple[Path, str]] = []
    for path in sorted(declared):
        rel = path.relative_to(REPO_ROOT)
        if (summary := rows.get(path)) is None:
            print(f"{str(rel):<48} {'absent':>14} {'absent':>14}")
            untested.append((rel, "no coverage data: no test links it"))
            continue
        print(f"{str(rel):<48} {percent(summary, 'lines'):>13.1f}%"
              f" {percent(summary, 'branches'):>13.1f}%")
        if summary["lines"]["count"] and not summary["lines"]["covered"]:
            untested.append((rel, "zero executed lines"))

    # Headers are worth printing, but the gate is about declared sources.
    for path in sorted(set(rows) - declared):
        rel = path.relative_to(REPO_ROOT)
        print(f"{str(rel):<48} {percent(rows[path], 'lines'):>13.1f}% {'(header)':>14}")

    if untested:
        print("\nAn untested file fails the build.", file=sys.stderr)
        print(*(f"  {rel}: {why}" for rel, why in untested), sep="\n", file=sys.stderr)
        return 1

    print(f"\ncoverage gate: all {len(declared)} declared source file(s) are executed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
