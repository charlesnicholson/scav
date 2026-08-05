#!/usr/bin/env python3
"""Run clang-format over scav's C++ sources.

clang-format ships with the compiler, so CI gets a version pinned by its container
image. Its output moves between releases, which is why the gate runs on one row
rather than on every developer's differently-versioned copy.

    $(./bin/envy product python3) tools/format.py            # rewrite files in place
    $(./bin/envy product python3) tools/format.py --check    # exit non-zero on any diff
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Everything scav writes; generated and third-party trees are not ours to touch.
SOURCE_ROOTS: tuple[str, ...] = ("include", "src", "apps", "plugins", "functional_tests")
SOURCE_SUFFIXES: tuple[str, ...] = (".h", ".cpp")


def clang_format() -> str:
    if found := shutil.which("clang-format"):
        return found
    raise SystemExit(
        "clang-format is not on PATH. It ships with the clang toolchain; install "
        "that, or leave formatting to CI, which runs it from a pinned image."
    )


def sources() -> list[Path]:
    found = [p for root in SOURCE_ROOTS for p in (REPO_ROOT / root).rglob("*")
             if p.is_file() and p.suffix in SOURCE_SUFFIXES]
    # Byte-wise: directory iteration order is unspecified, and this output should
    # not vary with the filesystem.
    return sorted(found, key=lambda p: str(p).encode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report diffs, change nothing")
    args = parser.parse_args()

    binary, files = clang_format(), sources()
    if not files:
        raise SystemExit("no sources found; is this the repository root?")

    if not args.check:
        subprocess.run([binary, "-i", *(str(f) for f in files)], check=True)
        print(f"formatted {len(files)} file(s)")
        return 0

    unformatted = [
        f.relative_to(REPO_ROOT)
        for f in files
        if subprocess.run([binary, str(f)], stdout=subprocess.PIPE, check=True).stdout
        != f.read_bytes()
    ]
    if unformatted:
        print("clang-format would change:", file=sys.stderr)
        print(*(f"  {f}" for f in unformatted), sep="\n", file=sys.stderr)
        print("\nRun `$(./bin/envy product python3) tools/format.py`.", file=sys.stderr)
        return 1

    print(f"{len(files)} file(s) already formatted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
