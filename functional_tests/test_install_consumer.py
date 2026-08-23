#!/usr/bin/env python3
"""A separate project links an installed scav, which is how the CMake package stays
honest: plenty of projects ship one they never consume."""

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import scavtest  # noqa: E402

# Config key -> the cache variable it feeds the consumer, so the consumer builds for
# the same triple without inheriting anything scav-specific.
FORWARDED: dict[str, str] = {
    "make_program": "CMAKE_MAKE_PROGRAM",
    "cxx_compiler": "CMAKE_CXX_COMPILER",
    "cxx_flags": "CMAKE_CXX_FLAGS",
    "exe_linker_flags": "CMAKE_EXE_LINKER_FLAGS",
    "build_type": "CMAKE_BUILD_TYPE",
}
COMPILER_ONLY: dict[str, str] = {k: FORWARDED[k] for k in ("make_program", "cxx_compiler")}


class TestInstallAndConsume(unittest.TestCase):
    cfg: scavtest.Config
    prefix: Path
    build: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        cls.prefix = cls.cfg.scratch_dir / "consumer/prefix"
        cls.build = cls.cfg.scratch_dir / "consumer/build"
        if cls.cfg.instrumented:
            return
        scavtest.fresh_dir(cls.prefix)
        result = scavtest.run(
            [cls.cfg.cmake, "--install", cls.cfg.build_dir, "--prefix", cls.prefix]
        )
        assert result.returncode == 0, "cmake --install failed"

    def setUp(self) -> None:
        if self.cfg.instrumented:
            self.skipTest(
                "the archive carries sanitizer or coverage instrumentation whose "
                "runtime the exported target deliberately does not name, so a "
                "plain consumer cannot link it. The consumer gate runs on the "
                "uninstrumented rows."
            )

    def configure(self, source: Path, build: Path, keys: dict[str, str] = FORWARDED) -> int:
        cmd: list[scavtest.Arg] = [
            self.cfg.cmake, "-S", source, "-B", build,
            "-G", self.cfg["generator"], f"-DCMAKE_PREFIX_PATH={self.prefix}",
        ]
        cmd += [f"-D{var}={self.cfg[key]}" for key, var in keys.items() if self.cfg[key]]
        return scavtest.run(cmd).returncode

    def test_install_tree_matches_the_config_package(self) -> None:
        for rel in ("include/scav/scav_types.h",
                    "include/scav/scav_core.h",
                    "include/scav/scav_core_c.h",
                    "include/scav/scav_draw.h",
                    "include/scav/scav_draw_c.h",
                    "include/scav/scav_layout.h",
                    "include/scav/scav_layout_c.h",
                    "lib/cmake/scav/scav-config.cmake",
                    "lib/cmake/scav/scav-config-version.cmake",
                    "lib/cmake/scav/scav-targets.cmake"):
            with self.subTest(path=rel):
                self.assertTrue((self.prefix / rel).is_file(), f"{rel} not installed")
        self.assertTrue(list((self.prefix / "lib").glob("*scavcore*")), "no archive")
        self.assertTrue(list((self.prefix / "lib").glob("*scavlayout*")), "no layout archive")
        self.assertTrue(list((self.prefix / "lib").glob("*scavdraw*")), "no draw archive")

    def test_every_installed_header_is_a_public_one(self) -> None:
        """The public/private split is a directory layout, so it is only real if
        the install tree has the same shape the build tree does.

        A public header lives in `src/<lib>/include/scav/` and is named
        `scav_*.h`. Anything else -- a private header, a test helper -- reaching a
        consumer means the -I boundary leaked somewhere."""
        installed = sorted(p.relative_to(self.prefix).as_posix()
                           for p in self.prefix.rglob("*.h"))
        stray = [p for p in installed
                 if not (p.startswith("include/scav/") and
                         Path(p).name.startswith("scav_"))]
        self.assertEqual([], stray, "non-public headers escaped into the install tree")
        self.assertTrue(installed, "no headers installed at all")

    def test_one_public_header_per_library(self) -> None:
        """one public header per library, plus the shared vocabulary.

        A reader should never have to work out which of several headers a symbol
        lives in. Growing this list is a design decision, so it is spelled out
        rather than counted."""
        self.assertEqual(
            ["include/scav/scav_core.h",
             "include/scav/scav_core_c.h",
             "include/scav/scav_draw.h",
             "include/scav/scav_draw_c.h",
             "include/scav/scav_layout.h",
             "include/scav/scav_layout_c.h",
             "include/scav/scav_types.h"],
            sorted(p.relative_to(self.prefix).as_posix()
                   for p in self.prefix.rglob("*.h")))

    def test_install_tree_omits_what_is_not_shipped(self) -> None:
        # An installed internal header would let a consumer define SCAV_TESTING and
        # link against symbols the shipping archive does not export.
        leaked = [p for pattern in ("*_internal.h", "*testable*", "*_tests.*",
                                    "test_*.h", "*hash_map*", "sort.h",
                                    "interner.h", "*synth_document*")
                  for p in self.prefix.rglob(pattern)]
        self.assertEqual([], leaked, "unshipped artifacts escaped into the install tree")

    def test_consumer_configures_builds_and_runs(self) -> None:
        scavtest.fresh_dir(self.build)
        self.assertEqual(0, self.configure(HERE / "consumer", self.build), "configure")
        self.assertEqual(
            0, scavtest.run([self.cfg.cmake, "--build", self.build]).returncode, "build"
        )

        candidates = (self.build / "consumer",
                      self.build / "consumer.exe",
                      self.build / self.cfg["build_type"] / "consumer.exe")
        exe = next((c for c in candidates if c.exists()), None)
        self.assertIsNotNone(exe, f"no consumer executable under {self.build}")

        result = scavtest.run([exe])
        self.assertEqual(0, result.returncode, "the installed scav computed the wrong value")
        self.assertIn("consumer core ok", result.stdout)
        # The model half too: lower, validate, and resolve through the installed
        # public headers, which is what proves none of them needs a private one.
        self.assertIn("consumer model ok", result.stdout)
        self.assertIn("consumer layout ok", result.stdout)
        self.assertIn("consumer layout run ok", result.stdout)

    def test_version_compatibility_is_same_minor(self) -> None:
        # The C ABI is additive within a minor version, so a newer minor must be
        # refused rather than quietly satisfied.
        major, minor, *_ = self.cfg["version"].split(".")
        too_new = f"{major}.{int(minor) + 1}.0"
        probe = scavtest.fresh_dir(self.cfg.scratch_dir / "consumer/version_probe")
        (probe / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.28)\n"
            "project(probe LANGUAGES CXX)\n"
            f"find_package(scav {too_new} REQUIRED)\n",
            encoding="utf-8",
        )
        self.assertNotEqual(
            0,
            self.configure(probe, probe / "build", COMPILER_ONLY),
            f"find_package(scav {too_new}) should have been refused",
        )


if __name__ == "__main__":
    unittest.main()
