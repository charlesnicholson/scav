#!/usr/bin/env python3
"""Runs the element suite through the CLI and tools, and checks the unit suites'
arrays list the same charts as the directory."""

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
# Unit suites holding a hand-maintained list of the element charts.
SUITES = (Path("src/layout/gauntlet_tests.cpp"), Path("src/layout/bench_tests.cpp"))
SVG_NS = "{http://www.w3.org/2000/svg}"

# The `GAUNTLET{...}` initializer, then the chart names inside it.
ARRAY = re.compile(r"GAUNTLET\{(.*?)\}", re.S)
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

    def test_the_directory_and_the_suites_name_the_same_charts(self) -> None:
        for suite in SUITES:
            with self.subTest(suite=suite.name):
                source = (self.cfg.repo_root / suite).read_text(encoding="utf-8")
                body = ARRAY.search(source)
                self.assertIsNotNone(body, f"no GAUNTLET array in {suite}")
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

    @scavtest.full_only
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

    @scavtest.full_only
    def test_the_tools_reach_the_suite(self) -> None:
        """`baseline.py --gauntlet` renders every chart; `audit.py --gauntlet` finds
        each one rendered."""
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
