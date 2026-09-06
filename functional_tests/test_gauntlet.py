#!/usr/bin/env python3
"""The element suite through the same verbs the corpus goes through.

`src/layout/gauntlet_tests.cpp` lays these charts out in-process with no space
requests, so nothing there exercises the loader, the printer, or the renderer,
and the array it iterates is hand-maintained. Both gaps are the same gap: a
chart added to the directory and not to the array is a shape nobody asserts
anything about, and one asserted on but never rendered is a shape no reader
ever sees."""

import os
import re
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

CHARTS = Path("test_data/charts/gauntlet")
SUITE = Path("src/layout/gauntlet_tests.cpp")
SVG_NS = "{http://www.w3.org/2000/svg}"

# The array initializer, then the names inside it. Anchored on the declaration
# so a chart name appearing in a comment elsewhere in the file is not one.
ARRAY = re.compile(r"GAUNTLET\[\]\{(.*?)\}", re.S)
NAME = re.compile(r'"([^"]+)"')

PROFILES = ("readable", "compact")


class TestGauntlet(unittest.TestCase):
    cfg: scavtest.Config
    exe: Path
    charts: list[Path]

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        name = "scav.exe" if os.name == "nt" else "scav"
        cls.exe = cls.cfg.build_dir / "bin" / name
        cls.charts = sorted((cls.cfg.repo_root / CHARTS).glob("*.scav"))
        assert cls.charts

    def run_scav(self, *args: scavtest.Arg) -> subprocess.CompletedProcess[str]:
        argv = [str(self.exe), *[str(a) for a in args]]
        print(f"+ {' '.join(argv)}", flush=True)
        return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, cwd=self.cfg.repo_root)

    def test_the_directory_and_the_suite_name_the_same_charts(self) -> None:
        source = (self.cfg.repo_root / SUITE).read_text(encoding="utf-8")
        body = ARRAY.search(source)
        self.assertIsNotNone(body, f"no GAUNTLET array in {SUITE}")
        named = set(NAME.findall(body.group(1)))
        self.assertEqual({c.name for c in self.charts}, named)

    def test_every_chart_is_canonical_and_valid(self) -> None:
        for chart in self.charts:
            with self.subTest(chart=chart.name):
                for verb in (("fmt", "--check"), ("check",)):
                    result = self.run_scav(*verb, (CHARTS / chart.name).as_posix())
                    self.assertEqual("", result.stderr)
                    self.assertEqual("", result.stdout)
                    self.assertEqual(0, result.returncode)

    def test_every_chart_renders_at_both_profiles(self) -> None:
        for chart in self.charts:
            for profile in PROFILES:
                with self.subTest(chart=chart.name, profile=profile):
                    result = self.run_scav("render", "--profile", profile, chart)
                    self.assertEqual("", result.stderr)
                    self.assertEqual(0, result.returncode)
                    root = ElementTree.fromstring(result.stdout)
                    self.assertEqual(f"{SVG_NS}svg", root.tag)
                    # Something was actually drawn.
                    self.assertTrue(list(root))

    def test_the_tools_reach_the_suite(self) -> None:
        """`--gauntlet` on both, since a switch nothing runs is a switch that
        rots. What is asserted is that they see the suite and agree on the file
        names between them, not what the audit counts: those are findings with
        an owner in the PRD, which is why the audit reports rather than fails."""
        out = scavtest.fresh_dir(self.cfg.scratch_dir / "gauntlet")
        rendered = scavtest.run(
            [self.cfg.python, self.cfg.repo_root / "tools/baseline.py", "--gauntlet",
             "--scav", self.exe, "--out", out],
            env={k: v for k, v in os.environ.items() if k != "SCAV_BASELINE"})
        self.assertEqual(0, rendered.returncode, rendered.stdout)
        for chart in self.charts:
            self.assertTrue((out / f"{chart.stem}.scav.svg").is_file(), chart.name)

        audited = scavtest.run(
            [self.cfg.python, self.cfg.repo_root / "tools/audit.py", "--gauntlet",
             "--scav", self.exe, "--in", out])
        self.assertEqual(0, audited.returncode, audited.stdout)
        self.assertNotIn("not rendered", audited.stdout)
        self.assertIn("route segments", audited.stdout)


if __name__ == "__main__":
    unittest.main()
