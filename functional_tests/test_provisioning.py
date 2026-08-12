#!/usr/bin/env python3
"""Every tool the configured tree recorded came from envy, not from whatever the
machine had. Opt-in, because envy is explicitly not a build prerequisite."""

import os
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

    def test_the_manifest_defaults_the_cache_under_out(self) -> None:
        # A property of the manifest, not of this invocation, so it is checked as
        # text: CI's override must not hide a manifest that changed the default.
        manifest = (self.cfg.repo_root / "envy.lua").read_text(encoding="utf-8")
        for directive in ('cache-posix "out/.envy"', 'cache-win "out\\.envy"'):
            self.assertIn(f"-- @envy {directive}", manifest)


if __name__ == "__main__":
    unittest.main()
