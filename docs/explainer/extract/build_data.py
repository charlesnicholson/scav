#!/usr/bin/env python3
"""Regenerate the explainer's data/*.json from a scav binary.

Every file but perf.json is scav's own output for the corpus charts, so it is
rebuilt here; perf.json is a measured snapshot that records its own sources.

  docs/explainer/extract/build_data.py                    every file, newest out/*/bin/scav
  docs/explainer/extract/build_data.py --only charts,faces
  docs/explainer/extract/build_data.py --scav out/macos-clang-libcxx-release/bin/scav
"""

import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import charts
import common
import faces
import rows
import search
import stages

DATA = Path(__file__).resolve().parent.parent / "data"
BUILDERS = {"charts": charts, "rows": rows, "search": search, "stages": stages, "faces": faces}
SNAPSHOTS = ("perf",)


def find_scav() -> Path:
    """The newest `scav` under out/, whichever preset built it."""
    candidates = sorted(
        (c for name in ("scav", "scav.exe")
         for c in (common.REPO_ROOT / "out").glob(f"*/bin/{name}")),
        key=lambda p: p.stat().st_mtime, reverse=True)
    if not candidates:
        raise SystemExit("no scav binary under out/; run ./build.sh first")
    return candidates[0]


def label_of(scav: Path) -> str:
    """The binary as the repository names it, and the checkout it is read against."""
    head = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True,
                          encoding="utf-8", cwd=common.REPO_ROOT, check=False).stdout.strip()
    try:
        name = scav.relative_to(common.REPO_ROOT).as_posix()
    except ValueError:
        name = str(scav)
    return f"{name} @ {head or 'unknown'}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scav", type=Path, help="the binary to run (default: the newest out/*/bin/scav)")
    parser.add_argument("--only", default="", help=f"comma-separated subset of {','.join(BUILDERS)}")
    parser.add_argument("--timeout", type=float, default=240, help="seconds allowed each scav run")
    args = parser.parse_args()

    names = [n for n in args.only.split(",") if n] or list(BUILDERS)
    if snap := [n for n in names if n in SNAPSHOTS]:
        raise SystemExit(f"{','.join(snap)}: a measured snapshot, not rebuilt; see docs/explainer/README.md")
    if bad := [n for n in names if n not in BUILDERS]:
        raise SystemExit(f"no builder for {','.join(bad)}; choose from {','.join(BUILDERS)}")
    binary = (args.scav or find_scav()).resolve()
    if not binary.is_file():
        raise SystemExit(f"no scav at {binary}")

    print(f"scav: {label_of(binary)}")
    with tempfile.TemporaryDirectory(prefix="scav-explainer-") as scratch:
        scav = common.Scav(binary, Path(scratch), label_of(binary), args.timeout)
        for name in names:
            t = time.time()
            print(f"{name}:", flush=True)
            doc = BUILDERS[name].build(scav)
            out = DATA / f"{name}.json"
            out.write_text(json.dumps(doc, separators=(",", ":")), encoding="utf-8")
            print(f"wrote {out.relative_to(common.REPO_ROOT)} ({out.stat().st_size / 1e3:.0f} kB, "
                  f"{time.time() - t:.1f}s)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
