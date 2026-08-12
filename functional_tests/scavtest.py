"""Shared plumbing for scav's Python functional tests. Standard library only."""

import json
import os
import shutil
import subprocess
from pathlib import Path

type Arg = str | Path

PATHS: frozenset[str] = frozenset({"repo_root", "build_dir", "scratch_dir"})
TRUTHY: frozenset[str] = frozenset({"ON", "1", "TRUE", "YES"})


class Config(dict[str, str]):
    """The build tree under test, as CMake described it at configure time."""

    def __getattr__(self, name: str) -> str | Path:
        value = self[name]
        return Path(value) if name in PATHS else value

    @property
    def instrumented(self) -> bool:
        """Whether the archive needs a runtime the exported target does not name.

        Correct behaviour, not a defect, so the consumer gate runs on plain rows.
        """
        return self["sanitizer"].upper() not in ("", "NONE") or (
            self["coverage"].upper() in TRUTHY
        )


def load_config() -> Config:
    if not (path := os.environ.get("SCAV_BUILD_CONFIG")):
        raise RuntimeError(
            "SCAV_BUILD_CONFIG is unset. These tests run as build steps, which "
            "set it; run them with `./build.sh`, or one at a time with "
            "`cmake --build <dir> --target run.func.<name>`."
        )
    return Config(json.loads(Path(path).read_text(encoding="utf-8")))


def run(cmd: list[Arg]) -> subprocess.CompletedProcess[str]:
    """Capture both streams so a failure prints them once."""
    argv = [str(c) for c in cmd]
    print(f"+ {' '.join(argv)}", flush=True)
    result = subprocess.run(
        argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    if result.returncode:
        print(result.stdout, flush=True)
    return result


def fresh_dir(path: Path) -> Path:
    """A scratch directory under out/, where everything scav writes belongs."""
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True)
    return path
