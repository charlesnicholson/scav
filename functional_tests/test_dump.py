#!/usr/bin/env python3
"""`scav dump` loads a chart from a file and prints the model, each element
line carrying the file and line its declaration started on. The output is a
golden: byte-compared, so it stays deterministic across platforms."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

CHART = Path("test_data/charts/brew.scav")
GOLDEN = Path("test_data/golden/dump/brew.txt")


class TestDump(unittest.TestCase):
    cfg: scavtest.Config
    exe: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        name = "scav.exe" if os.name == "nt" else "scav"
        cls.exe = cls.cfg.build_dir / "bin" / name

    def run_dump(self, *args: scavtest.Arg) -> subprocess.CompletedProcess[str]:
        argv = [str(self.exe), "dump", *[str(a) for a in args]]
        print(f"+ {' '.join(argv)}", flush=True)
        # Both streams separately: the model goes to stdout, diagnostics to
        # stderr, and conflating them would let a diagnostic corrupt the golden.
        return subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=self.cfg.repo_root,
        )

    def test_dump_matches_the_golden(self) -> None:
        # Relative path in, relative path out: the parenthetical locations quote
        # the path as given, which is what keeps the golden machine-independent.
        result = self.run_dump(CHART.as_posix())
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        golden = (self.cfg.repo_root / GOLDEN).read_text(encoding="utf-8")
        if result.stdout != golden:
            actual = self.cfg.scratch_dir / "golden" / "dump" / "brew.txt"
            actual.parent.mkdir(parents=True, exist_ok=True)
            actual.write_text(result.stdout, encoding="utf-8")
            self.fail(f"golden mismatch: {self.cfg.repo_root / GOLDEN} vs {actual}")

    def test_every_element_line_carries_a_location(self) -> None:
        result = self.run_dump(CHART.as_posix())
        self.assertEqual(0, result.returncode)
        for line in result.stdout.splitlines():
            head = line.strip().split(" ")[0]
            if head in ("chart", "state", "submachine", "trans", "include"):
                self.assertRegex(
                    line, r" \(test_data/charts/brew\.scav:\d+\)$",
                    f"element line without a location: {line!r}")

    def test_an_unresolved_include_diagnoses_but_still_dumps(self) -> None:
        chart = self.cfg.scratch_dir / "dump" / "networked.scav"
        chart.parent.mkdir(parents=True, exist_ok=True)
        chart.write_text(
            'chart n {\n'
            '  include "other.scav" as other,\n'
            '  state A,\n'
            '  trans A -> other/Deep/Inside,\n'
            '}\n',
            encoding="utf-8")
        result = self.run_dump(chart)
        # Loaded with findings: the model prints, the finding names its line.
        self.assertEqual(1, result.returncode)
        self.assertIn(":4:", result.stderr)
        self.assertIn("unresolved include", result.stderr)
        self.assertIn("include other unresolved", result.stdout)
        self.assertIn("state A", result.stdout)

    def test_a_parse_error_prints_no_model(self) -> None:
        chart = self.cfg.scratch_dir / "dump" / "broken.scav"
        chart.parent.mkdir(parents=True, exist_ok=True)
        chart.write_text("chart broken { state , }\n", encoding="utf-8")
        result = self.run_dump(chart)
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertIn(":1:", result.stderr)

    def test_a_missing_file_is_an_error(self) -> None:
        result = self.run_dump("test_data/charts/no_such_chart.scav")
        self.assertEqual(2, result.returncode)
        self.assertIn("cannot read", result.stderr)


if __name__ == "__main__":
    unittest.main()
