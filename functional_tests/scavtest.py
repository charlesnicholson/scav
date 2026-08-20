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


def run(
    cmd: list[Arg], env: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    """Capture both streams so a failure prints them once."""
    argv = [str(c) for c in cmd]
    print(f"+ {' '.join(argv)}", flush=True)
    result = subprocess.run(
        argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )
    if result.returncode:
        print(result.stdout, flush=True)
    return result


SANITIZERS: tuple[str, ...] = (
    "ASAN_OPTIONS", "LSAN_OPTIONS", "MSAN_OPTIONS", "TSAN_OPTIONS", "UBSAN_OPTIONS",
)


def env_without_suppressions() -> dict[str, str]:
    """The environment for a process this test spawns rather than the build.

    A sanitizer's suppressions path is a bare filename, because those option
    strings split on ':' and a Windows drive letter would split with them. It
    resolves against the source directory, which a spawned build tool does not
    run in, and a runtime that cannot read it aborts before main.
    """
    env = dict(os.environ)
    for name in SANITIZERS:
        if value := env.get(name):
            kept = [p for p in value.split(":") if not p.startswith("suppressions=")]
            env[name] = ":".join(kept)
    return env


def fresh_dir(path: Path) -> Path:
    """A scratch directory under out/, where everything scav writes belongs."""
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True)
    return path
