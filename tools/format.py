#!/usr/bin/env python3
"""Run the pinned clang-format over scav's C++ sources, refusing to fall back to
whatever is on PATH: its output changes between releases.

    python3 tools/format.py            # rewrite files in place
    python3 tools/format.py --check    # exit non-zero on any diff
"""

import argparse
import os
import platform
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Everything scav writes; generated and third-party trees are not ours to touch.
SOURCE_ROOTS: tuple[str, ...] = ("include", "src", "apps", "plugins", "functional_tests")
SOURCE_SUFFIXES: tuple[str, ...] = (".h", ".cpp")


def clang_format() -> str:
    script = REPO_ROOT / ("bin/envy.bat" if platform.system() == "Windows" else "bin/envy")
    # The package is gated, so the query has to unlock it or be told the package
    # does not exist.
    result = subprocess.run(
        [str(script), "product", "clang-format"],
        cwd=REPO_ROOT,
        env=dict(os.environ, SCAV_CLANG_TOOLS="1"),
        stdout=subprocess.PIPE,
        text=True,
        check=False,
    )
    path = result.stdout.strip()
    if result.returncode or not Path(path or ".").is_file():
        raise SystemExit(
            "clang-format is not provisioned. It is gated because getting it means "
            "downloading LLVM's whole release archive:\n\n"
            "    SCAV_CLANG_TOOLS=1 ./bin/envy sync\n"
        )
    return path


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
        print("\nRun `python3 tools/format.py`.", file=sys.stderr)
        return 1

    print(f"{len(files)} file(s) already formatted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
