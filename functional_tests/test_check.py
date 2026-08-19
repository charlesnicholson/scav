#!/usr/bin/env python3
"""Structural validation behind an exit code: nothing on stdout, findings on
stderr, and "this chart is wrong" distinct from "this file would not read"."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

CHARTS = Path("test_data/charts")


class TestCheck(unittest.TestCase):
    cfg: scavtest.Config
    exe: Path
    scratch: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        name = "scav.exe" if os.name == "nt" else "scav"
        cls.exe = cls.cfg.build_dir / "bin" / name
        cls.scratch = scavtest.fresh_dir(cls.cfg.scratch_dir / "check")

    def run_scav(self, *args: scavtest.Arg) -> subprocess.CompletedProcess[str]:
        argv = [str(self.exe), *[str(a) for a in args]]
        print(f"+ {' '.join(argv)}", flush=True)
        return subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=self.cfg.repo_root,
        )

    def write(self, name: str, text: str) -> Path:
        path = self.scratch / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def test_every_corpus_chart_is_structurally_valid(self) -> None:
        root = self.cfg.repo_root / CHARTS
        # Only the roots: an included document is checked through the network
        # that includes it, and on its own it is a valid chart too.
        for chart in sorted(root.glob("*.scav")):
            result = self.run_scav("check", (CHARTS / chart.name).as_posix())
            self.assertEqual("", result.stderr, chart.name)
            self.assertEqual(0, result.returncode, chart.name)
            self.assertEqual("", result.stdout, chart.name)

    def test_a_duplicate_name_is_a_finding(self) -> None:
        chart = self.write("dup.scav", "chart d {\n  state A,\n  state A,\n}\n")
        result = self.run_scav("check", chart)
        self.assertEqual(1, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertIn("duplicate", result.stderr.lower())

    def test_two_initials_in_one_submachine_is_a_finding(self) -> None:
        chart = self.write(
            "two_initial.scav",
            "chart t {\n  state A,\n  state B,\n  trans * -> A,\n  trans * -> B,\n}\n",
        )
        result = self.run_scav("check", chart)
        self.assertEqual(1, result.returncode)
        self.assertIn("initial", result.stderr.lower())

    def test_an_alias_colliding_with_a_sibling_is_a_finding(self) -> None:
        # An include synthesizes a state named for its alias, so the ordinary
        # duplicate-name check covers this.
        self.write("leaf.scav", "chart leaf {\n  state L,\n}\n")
        chart = self.write(
            "alias_clash.scav",
            'chart a {\n  include "leaf.scav" as dup,\n  state dup,\n}\n',
        )
        result = self.run_scav("check", chart)
        self.assertEqual(1, result.returncode)
        self.assertIn("duplicate", result.stderr.lower())

    def test_a_finding_names_the_document_that_holds_it(self) -> None:
        self.write("bad_leaf.scav", "chart leaf {\n  state L,\n  state L,\n}\n")
        chart = self.write(
            "hosts_bad_leaf.scav",
            'chart root {\n  include "bad_leaf.scav" as leaf,\n  state A,\n}\n',
        )
        result = self.run_scav("check", chart)
        self.assertEqual(1, result.returncode)
        self.assertIn("bad_leaf.scav:", result.stderr.replace("\\", "/"))

    def test_an_unreadable_file_is_distinct_from_a_finding(self) -> None:
        result = self.run_scav("check", self.scratch / "absent.scav")
        self.assertEqual(2, result.returncode)
        self.assertIn("cannot read", result.stderr)

    def test_a_parse_error_is_unusable_rather_than_a_finding(self) -> None:
        chart = self.write("broken.scav", "chart b { state , }\n")
        result = self.run_scav("check", chart)
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)

    def test_check_takes_exactly_one_path(self) -> None:
        chart = self.write("one.scav", "chart o {\n  state A,\n}\n")
        self.assertEqual(2, self.run_scav("check").returncode)
        self.assertEqual(2, self.run_scav("check", chart, chart).returncode)

    def test_an_unknown_verb_prints_usage(self) -> None:
        result = self.run_scav("validate", "x.scav")
        self.assertEqual(2, result.returncode)
        self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
