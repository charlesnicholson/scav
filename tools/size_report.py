#!/usr/bin/env python3
"""The shared library's and the CLI's sizes in one build tree: bytes as built,
bytes as `cmake --install --strip` leaves them, and the code and constant
sections.

    tools/size_report.py out/macos-clang-libcxx-release
"""

import argparse
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile

ARTIFACTS = (("lib", ("libscav.dylib", "libscav.so")), ("bin", ("scav",)))


def stripped_size(path: pathlib.Path, shared: bool) -> int:
    """The size after the strip CMake's install runs: `-x` for a Mach-O dylib."""
    with tempfile.TemporaryDirectory() as work:
        copy = pathlib.Path(work) / path.name
        shutil.copyfile(path, copy)
        flags = ["-x"] if shared and platform.system() == "Darwin" else []
        subprocess.run(["strip", *flags, str(copy)], check=True)
        return copy.stat().st_size


def sections(path: pathlib.Path) -> tuple[int, int]:
    """(code, constants): __TEXT's __text and __const, or ELF's .text and .rodata."""
    if platform.system() == "Darwin":
        out = subprocess.run(["size", "-m", str(path)], capture_output=True,
                             text=True, check=True).stdout
        text = out.split("Segment __TEXT:")[1].split("Segment ")[0]
        found = dict(re.findall(r"Section (\S+): (\d+)", text))
        return int(found.get("__text", 0)), int(found.get("__const", 0))
    out = subprocess.run(["size", "-A", str(path)], capture_output=True, text=True,
                         check=True).stdout
    found = dict(re.findall(r"^(\.\S+)\s+(\d+)", out, re.MULTILINE))
    return int(found.get(".text", 0)), int(found.get(".rodata", 0))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build_dir", type=pathlib.Path)
    args = parser.parse_args()

    rows = []
    for folder, names in ARTIFACTS:
        path = next((args.build_dir / folder / n for n in names
                     if (args.build_dir / folder / n).is_file()), None)
        if path is None:
            print(f"no {' or '.join(names)} under {args.build_dir / folder}",
                  file=sys.stderr)
            return 1
        code, const = sections(path)
        rows.append((f"{folder}/{path.name}", path.stat().st_size,
                     stripped_size(path, folder == "lib"), code, const))

    print(f"{'':20}{'built':>12}{'stripped':>12}{'text':>12}{'const':>12}")
    for name, *sizes in rows:
        print(f"{name:20}" + "".join(f"{s:>12,}" for s in sizes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
