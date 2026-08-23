#!/usr/bin/env python3
"""Every tool the configured tree recorded came from envy, not from whatever the
machine had. Opt-in, because envy is explicitly not a build prerequisite.

The sandbox itself is not opt-in and is checked unconditionally below: the cache
belongs under out/ so that removing that one directory removes every tool,
package and build artifact."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

# The build-config keys naming a tool envy is expected to have provisioned.
PROVISIONED: tuple[str, ...] = (
    "cmake", "make_program", "doctest_include_dir", "python",
)


class TestProvisioning(unittest.TestCase):
    cfg: scavtest.Config
    packages: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        # ENVY_CACHE_ROOT beats the manifest directive; CI sets it so one warm
        # cache is shared across jobs.
        override = os.environ.get("ENVY_CACHE_ROOT")
        root = Path(override or cls.cfg.repo_root / "out/.envy")
        cls.packages = (root / "packages").resolve()

    def setUp(self) -> None:
        if os.environ.get("SCAV_REQUIRE_ENVY") != "1":
            self.skipTest(
                "SCAV_REQUIRE_ENVY is not 1. envy pins the toolchain for scav's "
                "own CI and is deliberately not a build prerequisite, so a build "
                "with your own cmake is supported and this check is CI's."
            )

    def test_every_tool_came_from_an_envy_package(self) -> None:
        for key in PROVISIONED:
            with self.subTest(tool=key):
                self.assertTrue(value := self.cfg[key], f"{key} is empty")
                path = Path(value).resolve()
                self.assertTrue(path.is_relative_to(self.packages),
                                f"{key} resolved to {path}, outside {self.packages}")


class TestSandbox(unittest.TestCase):
    """`rm -rf out` is a factory reset. That is a promise to anyone who builds
    scav once and does not want a toolchain left on their machine, so it is
    checked whether or not envy is required."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()

    def test_the_manifest_defaults_the_cache_under_out(self) -> None:
        # A property of the manifest, not of this invocation, so it is checked as
        # text: an override must not hide a manifest that changed the default.
        manifest = (self.cfg.repo_root / "envy.lua").read_text(encoding="utf-8")
        for directive in ('cache-posix "out/.envy"', 'cache-win "out\\.envy"'):
            self.assertIn(f"-- @envy {directive}", manifest)

    def test_the_local_settings_file_can_never_be_committed(self) -> None:
        """`.env` holds one machine-specific path, so committing it would break
        every other checkout. git decides that, not a convention."""
        probe = self.cfg.repo_root / ".env"
        existed = probe.exists()
        if not existed:
            probe.write_text("SCAV_ENVY_CACHE_ROOT=/tmp/probe\n", encoding="utf-8")
        try:
            result = subprocess.run(
                ["git", "check-ignore", "-q", ".env"],
                cwd=self.cfg.repo_root, check=False)
            self.assertEqual(0, result.returncode, ".env is not gitignored")
        finally:
            if not existed:
                probe.unlink()

    def test_the_entry_points_read_the_override_without_sourcing_it(self) -> None:
        """A `.env` is data. Sourcing it would let a line in it run as the
        developer, which is a shell scav has no reason to be."""
        for name in ("build.sh", "build.bat", "tools/build.py"):
            with self.subTest(entry=name):
                text = (self.cfg.repo_root / name).read_text(encoding="utf-8")
                self.assertIn("SCAV_ENVY_CACHE_ROOT", text)
                for danger in ("source .env", ". .env", "eval "):
                    self.assertNotIn(danger, text)


if __name__ == "__main__":
    unittest.main()
