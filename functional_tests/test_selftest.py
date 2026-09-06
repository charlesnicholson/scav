#!/usr/bin/env python3
"""`scav selftest`: the corpus laid out on this toolchain at every thread count
in the matrix, diffed against the committed goldens.

The charts and the goldens are embedded in the executable, so the verb takes no
paths and the tests run it from a directory that holds neither."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

GOLDEN = Path("test_data/golden/layout/corpus_hashes.txt")
THREAD_COUNTS = 7
COLUMNS = ("inputs", "structural", "coordinate")


class TestSelftest(unittest.TestCase):
    cfg: scavtest.Config
    exe: Path
    golden: list[list[str]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        name = "scav.exe" if os.name == "nt" else "scav"
        cls.exe = cls.cfg.build_dir / "bin" / name
        text = (cls.cfg.repo_root / GOLDEN).read_text(encoding="utf-8")
        cls.golden = [ln.split() for ln in text.splitlines() if ln.strip()]
        assert cls.golden

    def run_selftest(
        self, *args: scavtest.Arg, cwd: Path | None = None
    ) -> subprocess.CompletedProcess[str]:
        # Both streams separately: the report goes to stdout and a path scav
        # cannot use goes to stderr, and conflating them would hide either.
        argv = [str(self.exe), "selftest", *[str(a) for a in args]]
        print(f"+ {' '.join(argv)}", flush=True)
        # A sanitizer resolves its suppressions path against a directory these
        # runs deliberately leave, and a runtime that cannot read one says so on
        # the stderr under assertion.
        return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, cwd=cwd or self.cfg.repo_root,
                              env=scavtest.env_without_suppressions())

    def scratch(self) -> Path:
        return scavtest.fresh_dir(self.cfg.scratch_dir / "selftest")

    def write_golden(self, name: str, rows: list[list[str]]) -> Path:
        path = self.scratch() / name
        path.write_text("".join(" ".join(r) + "\n" for r in rows), encoding="utf-8")
        return path

    def altered(self, chart: str, column: str, value: str) -> list[list[str]]:
        """The committed golden with one hash replaced."""
        at = 1 + COLUMNS.index(column)
        rows = [list(r) for r in self.golden]
        for row in rows:
            if row[0] == chart:
                self.assertNotEqual(value, row[at])
                row[at] = value
                return rows
        self.fail(f"{chart} is not in {GOLDEN}")

    def report(self, out: str) -> tuple[list[list[str]], str]:
        """The chart lines and the summary, which is always last."""
        lines = out.splitlines()
        self.assertTrue(lines)
        return [ln.split() for ln in lines[:-1]], lines[-1]

    def check_summary(self, summary: str, charts: int, failures: int) -> None:
        self.assertEqual(
            f"selftest: {charts} charts, {THREAD_COUNTS} thread counts, "
            f"{failures} failures", summary)

    # The clean run ==========================================================

    def test_the_corpus_matches_the_committed_golden(self) -> None:
        result = self.run_selftest()
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        rows, summary = self.report(result.stdout)
        # One line per golden line, in the golden's own order, carrying the
        # hashes this toolchain just computed rather than a copy of the file.
        self.assertEqual(len(self.golden), len(rows))
        for row, want in zip(rows, self.golden, strict=True):
            self.assertEqual(["ok", *want], row)
        self.check_summary(summary, len(self.golden), 0)

    def test_it_runs_from_a_directory_holding_neither_charts_nor_goldens(self) -> None:
        here = self.scratch()
        self.assertEqual([], list(here.iterdir()))
        result = self.run_selftest(cwd=here)
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        rows, summary = self.report(result.stdout)
        for row, want in zip(rows, self.golden, strict=True):
            self.assertEqual(["ok", *want], row)
        self.check_summary(summary, len(self.golden), 0)

    def test_the_hashes_are_stable_across_runs(self) -> None:
        self.assertEqual(self.run_selftest().stdout, self.run_selftest().stdout)

    # --against ==============================================================

    def test_a_golden_naming_two_charts_checks_two(self) -> None:
        # A chart embedded but not named is not checked, which is what lets a
        # maintainer diff a partial file.
        path = self.write_golden("two.txt", [list(r) for r in self.golden[:2]])
        result = self.run_selftest("--against", path)
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        rows, summary = self.report(result.stdout)
        self.assertEqual([["ok", *r] for r in self.golden[:2]], rows)
        self.check_summary(summary, 2, 0)

    def check_one_altered_column(self, chart: str, column: str, value: str) -> None:
        rows = self.altered(chart, column, value)
        path = self.write_golden(f"{column}.txt", rows)
        result = self.run_selftest("--against", path)
        self.assertEqual("", result.stderr)
        self.assertEqual(1, result.returncode)
        report, summary = self.report(result.stdout)

        failures = [r for r in report if r[0] == "FAIL"]
        self.assertEqual(1, len(failures))
        self.assertEqual(len(self.golden), len(report))
        self.check_summary(summary, len(self.golden), 1)

        # Every other chart still passes, in place.
        for got, want in zip(report, self.golden, strict=True):
            if want[0] != chart:
                self.assertEqual(["ok", *want], got)

        # All three columns named, got and golden apiece: the altered one reads
        # back as the golden, the other two as what the golden still says.
        truth = next(r for r in self.golden if r[0] == chart)
        expected = ["FAIL", chart]
        for i, name in enumerate(COLUMNS):
            golden = value if name == column else truth[1 + i]
            expected += [name, truth[1 + i], "(golden", f"{golden})"]
        self.assertEqual(expected, failures[0])

    def test_an_altered_structural_hash_fails_that_column(self) -> None:
        self.check_one_altered_column("axis.scav", "structural", "1234abcd")

    def test_an_altered_coordinate_hash_fails_that_column(self) -> None:
        self.check_one_altered_column("mill.scav", "coordinate", "0badcafe")

    def test_an_altered_inputs_digest_fails_that_column(self) -> None:
        self.check_one_altered_column("vac.scav", "inputs", "00000000")

    def test_a_malformed_line_names_its_number(self) -> None:
        rows = [list(r) for r in self.golden]
        rows[2] = rows[2][:2]
        path = self.write_golden("malformed.txt", rows)
        result = self.run_selftest("--against", path)
        self.assertEqual("", result.stderr)
        self.assertEqual(1, result.returncode)
        report, summary = self.report(result.stdout)
        self.assertIn("FAIL golden:3 malformed", "\n".join(" ".join(r) for r in report))
        # The malformed line is not a chart, so the other ten still are.
        self.check_summary(summary, len(self.golden) - 1, 1)

    def test_a_hash_that_is_not_lowercase_hex8_is_malformed(self) -> None:
        for bad in ("DFB3A851", "dfb3a85", "zzzzzzzz"):
            with self.subTest(hash=bad):
                rows = [list(r) for r in self.golden]
                rows[0][2] = bad
                path = self.write_golden("nothex.txt", rows)
                result = self.run_selftest("--against", path)
                self.assertEqual(1, result.returncode)
                self.assertIn("FAIL golden:1 malformed", result.stdout)

    def test_a_golden_naming_a_chart_this_build_lacks_fails_it(self) -> None:
        rows = [list(r) for r in self.golden]
        rows[0][0] = "nowhere.scav"
        path = self.write_golden("unknown.txt", rows)
        result = self.run_selftest("--against", path)
        self.assertEqual(1, result.returncode)
        self.assertIn("FAIL nowhere.scav no such chart in this build", result.stdout)

    def test_a_missing_against_file_is_unusable(self) -> None:
        result = self.run_selftest("--against", self.scratch() / "absent.txt")
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertIn("scav: cannot read", result.stderr)
        self.assertIn("absent.txt", result.stderr)

    def test_an_empty_against_file_checks_nothing(self) -> None:
        path = self.write_golden("empty.txt", [])
        result = self.run_selftest("--against", path)
        self.assertEqual(0, result.returncode)
        self.check_summary(result.stdout.strip(), 0, 0)

    # Usage ==================================================================

    def check_usage(self, *args: scavtest.Arg) -> None:
        result = self.run_selftest(*args)
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertIn("usage:", result.stderr)

    def test_against_without_a_value_is_a_usage_error(self) -> None:
        self.check_usage("--against")

    def test_a_positional_argument_is_a_usage_error(self) -> None:
        self.check_usage("test_data/charts/axis.scav")

    def test_against_twice_is_a_usage_error(self) -> None:
        path = self.write_golden("twice.txt", [list(r) for r in self.golden])
        self.check_usage("--against", path, "--against", path)

    def test_an_unknown_option_is_a_usage_error(self) -> None:
        self.check_usage("--nope")

    def test_the_usage_text_lists_selftest(self) -> None:
        result = subprocess.run([str(self.exe)], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True,
                                cwd=self.cfg.repo_root,
                                env=scavtest.env_without_suppressions())
        self.assertEqual(2, result.returncode)
        self.assertIn("selftest [--against FILE]", result.stderr)
        self.assertIn("diff against the goldens", result.stderr)


if __name__ == "__main__":
    unittest.main()
