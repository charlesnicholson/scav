#!/usr/bin/env python3
"""`scav dump` loads a document network from a file and prints the model, each
element line carrying the file and line its declaration started on. The output
is a golden: byte-compared, so it stays deterministic across platforms.

This is the load session over a real filesystem -- real paths in diagnostics,
and a cycle reported against files rather than buffers."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

CHART = Path("test_data/charts/brew.scav")
GOLDEN = Path("test_data/golden/dump/brew.txt")
NETWORK = Path("test_data/charts/vac.scav")
NETWORK_GOLDEN = Path("test_data/golden/dump/vac.txt")
MILL = Path("test_data/charts/mill.scav")
MILL_GOLDEN = Path("test_data/golden/dump/mill.txt")


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

    def check_golden(self, chart: Path, golden: Path) -> str:
        # Relative path in, relative path out: the parenthetical locations quote
        # the path as given, which is what keeps the golden machine-independent.
        result = self.run_dump(chart.as_posix())
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        want = (self.cfg.repo_root / golden).read_text(encoding="utf-8")
        if result.stdout != want:
            actual = self.cfg.scratch_dir / "golden" / "dump" / golden.name
            actual.parent.mkdir(parents=True, exist_ok=True)
            actual.write_text(result.stdout, encoding="utf-8")
            self.fail(f"golden mismatch: {self.cfg.repo_root / golden} vs {actual}")
        return result.stdout

    def test_dump_matches_the_golden(self) -> None:
        self.check_golden(CHART, GOLDEN)

    def test_a_three_document_network_matches_the_golden(self) -> None:
        out = self.check_golden(NETWORK, NETWORK_GOLDEN)
        # Resolution links, it does not flatten: one containment tree, and the
        # included content prints under its alias state.
        self.assertIn("state dock", out)
        self.assertIn("(test_data/charts/dock.scav:", out)
        self.assertIn("(test_data/charts/led.scav:", out)
        # A cross-document endpoint resolved.
        self.assertIn("-> dock/On/Seated", out)
        # led.scav is one document and two instantiations, so it appears under
        # both `dock/lamp` and `lamp` with disjoint rows.
        self.assertIn("trans dock/lamp/Off -> dock/lamp/Blinking", out)
        self.assertIn("trans lamp/Off -> lamp/Blinking", out)

    def test_an_attribute_points_at_its_own_statement(self) -> None:
        out = self.check_golden(MILL, MILL_GOLDEN)
        # The `@machine { ... }` block is one statement producing two rows, so
        # both point at that line rather than at the chart's.
        self.assertIn('@machine:axes = "3" (test_data/charts/mill.scav:8)', out)
        self.assertIn('@machine:spindle_kw = "2" (test_data/charts/mill.scav:8)', out)
        # And an attribute inside a state names the attribute's line, not the
        # state's.
        self.assertIn('@doc = "gantry mill with a carousel changer" '
                      '(test_data/charts/mill.scav:7)', out)

    def test_a_repeated_child_is_one_document_and_many_instantiations(self) -> None:
        out = self.check_golden(MILL, MILL_GOLDEN)

        # Four documents. axis.scav is named three times by mill and once by
        # toolchanger; estop.scav is named by all four.
        edges = [ln.strip() for ln in out.splitlines() if ln.startswith("  include ")]
        self.assertEqual(4, sum(1 for e in edges if '"axis.scav"' in e))
        self.assertEqual(6, sum(1 for e in edges if '"estop.scav"' in e))
        self.assertEqual(11, len(edges))
        # All four name one resolved document; the trailing "(file:line)" is
        # where they were written, so it is dropped before comparing.
        targets = {e.split(" -> ")[1].split(" (")[0]
                   for e in edges if '"axis.scav"' in e}
        self.assertEqual({"test_data/charts/axis.scav"}, targets)

        # Three sibling instantiations of one file under one parent, each
        # addressed apart.
        for alias in ("x", "y", "z"):
            self.assertIn(f"trans {alias}/Parked -> {alias}/Homing", out)
            self.assertIn(f"trans {alias}/stop/Clear -> {alias}/stop/Tripped", out)

    def test_a_path_descends_through_three_include_boundaries(self) -> None:
        out = self.check_golden(MILL, MILL_GOLDEN)
        # mill -> toolchanger -> axis -> estop.
        self.assertIn("trans tool/arm/stop/Clear -> tool/arm/stop/Tripped", out)
        # And an endpoint written in the root that reaches two of them.
        self.assertIn("trans Cutting -> tool/arm/Ready", out)
        # A submachine qualifier surviving a cross-document descent.
        self.assertIn("-> tool/arm/Moving:travel/Cruising", out)

    def test_every_instantiation_of_one_file_hashes_into_a_stable_model(self) -> None:
        first = self.run_dump("--hash", MILL.as_posix())
        second = self.run_dump("--hash", MILL.as_posix())
        self.assertEqual(0, first.returncode)
        self.assertEqual(first.stdout, second.stdout)
        self.assertRegex(first.stdout.strip(), r"^[0-9a-f]{8}$")
        # A different network is a different digest.
        other = self.run_dump("--hash", NETWORK.as_posix())
        self.assertNotEqual(first.stdout, other.stdout)

    def test_the_include_edges_name_both_spellings_and_one_document(self) -> None:
        out = self.check_golden(NETWORK, NETWORK_GOLDEN)
        # `./led.scav` and `led.scav` are one key, so one document -- the
        # authored text is kept verbatim and the resolved target is shared.
        self.assertIn('include lamp "./led.scav" -> test_data/charts/led.scav', out)
        self.assertIn('include lamp "led.scav" -> test_data/charts/led.scav', out)

    def test_every_element_line_carries_a_location(self) -> None:
        result = self.run_dump(CHART.as_posix())
        self.assertEqual(0, result.returncode)
        for line in result.stdout.splitlines():
            head = line.strip().split(" ")[0]
            # An attribute is its own authored statement, so its line carries a
            # location too -- and not its subject's.
            if head.startswith("@") or head in (
                    "chart", "state", "submachine", "trans", "include"):
                self.assertRegex(
                    line, r" \(test_data/charts/brew\.scav:\d+\)$",
                    f"element line without a location: {line!r}")

    def write(self, name: str, text: str) -> Path:
        path = self.cfg.scratch_dir / "dump" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def test_an_include_that_does_not_exist_is_an_error(self) -> None:
        # The CLI's fetch is `fopen`, so a missing include is the operating
        # system's answer rather than a finding about the model.
        chart = self.write(
            "networked.scav",
            'chart n {\n'
            '  include "other.scav" as other,\n'
            '  state A,\n'
            '  trans A -> other/Deep/Inside,\n'
            '}\n')
        result = self.run_dump(chart)
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertIn("cannot read", result.stderr)
        self.assertIn("other.scav", result.stderr)

    def test_an_include_cycle_is_reported_against_a_file_and_line(self) -> None:
        self.write("cyc_a.scav",
                   'chart a {\n  include "cyc_b.scav" as b,\n  state A,\n}\n')
        chart = self.write("cyc_b.scav",
                           'chart b {\n  state B,\n  include "cyc_a.scav" as a,\n}\n')
        result = self.run_dump(self.cfg.scratch_dir / "dump" / "cyc_a.scav")
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertIn("include cycle", result.stderr)
        # Named against the document holding the statement that closes it,
        # which is the second file, not the one on the command line.
        self.assertIn(f"{chart.name}:3:", result.stderr.replace("\\", "/"))

    def test_a_document_that_includes_itself_is_a_cycle(self) -> None:
        chart = self.write("selfref.scav",
                           'chart s {\n  include "selfref.scav" as me,\n  state A,\n}\n')
        result = self.run_dump(chart)
        self.assertEqual(2, result.returncode)
        self.assertIn("include cycle", result.stderr)

    def test_a_parse_error_in_an_included_document_names_that_document(self) -> None:
        self.write("broken_leaf.scav", "chart leaf { state , }\n")
        chart = self.write(
            "has_broken_leaf.scav",
            'chart root {\n  include "broken_leaf.scav" as leaf,\n  state A,\n}\n')
        result = self.run_dump(chart)
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        # The included file, not the root: parse_document does not know which
        # document it holds, so the session stamps the DocId.
        self.assertIn("broken_leaf.scav:1:", result.stderr.replace("\\", "/"))

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
