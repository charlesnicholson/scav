#!/usr/bin/env python3
"""The model from a file, each element line carrying the source it came from,
byte-compared against a golden. Also the loader over a real filesystem."""

import json
import os
import re
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
JSON_GOLDEN = Path("test_data/golden/dump/vac.json")
LAYOUT_GOLDEN = Path("test_data/golden/dump/vac_layout.txt")
LAYOUT_JSON_GOLDEN = Path("test_data/golden/dump/vac_layout.json")


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
        # Captures stdout (the model) and stderr (diagnostics) separately.
        return subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=self.cfg.repo_root,
        )

    def check_golden(self, chart: Path, golden: Path) -> str:
        # Passes a relative path; locations in the output quote it as given.
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

    def pinned(self, chart: Path) -> list[str]:
        return scavtest.pinned(self.cfg.repo_root, chart.name)

    def test_layout_dump_matches_the_golden(self) -> None:
        result = self.run_dump("--layout", *self.pinned(NETWORK), NETWORK.as_posix())
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        want = (self.cfg.repo_root / LAYOUT_GOLDEN).read_text(encoding="utf-8")
        if result.stdout != want:
            actual = self.cfg.scratch_dir / "golden" / "dump" / LAYOUT_GOLDEN.name
            actual.parent.mkdir(parents=True, exist_ok=True)
            actual.write_text(result.stdout, encoding="utf-8")
            self.fail(f"golden mismatch: {self.cfg.repo_root / LAYOUT_GOLDEN} vs {actual}")

    def test_layout_json_parses_and_matches_the_golden(self) -> None:
        result = self.run_dump("--layout", "--json", *self.pinned(NETWORK),
                               NETWORK.as_posix())
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        doc = json.loads(result.stdout)
        geometry = doc["geometry"]
        self.assertEqual(len(doc["states"]), len(geometry["state"]))
        self.assertEqual(len(doc["transitions"]), len(geometry["route"]))
        self.assertGreater(geometry["chart"][2], 0)
        want = (self.cfg.repo_root / LAYOUT_JSON_GOLDEN).read_text(encoding="utf-8")
        self.assertEqual(want, result.stdout)

    def test_layout_columns_carry_only_the_kinds_this_build_registers(self) -> None:
        # The layout dump's full (entity, kind) set; a model-only dump has no columns.
        doc = json.loads(self.run_dump("--layout", "--json", *self.pinned(NETWORK),
                                       NETWORK.as_posix()).stdout)
        self.assertEqual(
            {("state", "pod"), ("state", "u32"), ("submachine", "pod"), ("transition", "span"),
             ("point", "pod"), ("chart", "pod"), ("chart", "u32")},
            {(c["entity"], c["kind"]) for c in doc["columns"]})
        self.assertEqual([], json.loads(
            self.run_dump("--json", NETWORK.as_posix()).stdout)["columns"])

    def test_the_flags_a_layout_rests_on_lay_it_out_again_unsearched(self) -> None:
        # The printed `rests on` flags plus `--no-search` reproduce the geometry;
        # a non-default profile and `--no-text` lead them.
        for given, lead in (([], "--portfolio-row"),
                            (["--profile", "compact", "--no-text"], "--profile")):
            with self.subTest(given=given):
                shipped = self.run_dump("--layout", *given, CHART.as_posix())
                self.assertEqual(0, shipped.returncode)
                rests = [ln for ln in shipped.stdout.splitlines()
                         if ln.startswith("  rests on ")]
                self.assertEqual(1, len(rests))
                flags = rests[0][len("  rests on "):].split()
                self.assertEqual(lead, flags[0])
                again = self.run_dump("--layout", "--no-search", *flags, CHART.as_posix())
                self.assertEqual(0, again.returncode)
                geometry = [ln for ln in shipped.stdout.splitlines()
                            if ln.startswith("geometry ")]
                self.assertEqual(geometry, [ln for ln in again.stdout.splitlines()
                                            if ln.startswith("geometry ")])

    def test_a_frame_turned_down_is_part_of_what_a_layout_rests_on(self) -> None:
        # A given `--orient F` is reported in `rests on`.
        shipped = self.run_dump("--layout", "--no-search", "--orient", "0", CHART.as_posix())
        self.assertEqual(0, shipped.returncode)
        rests = [ln for ln in shipped.stdout.splitlines() if ln.startswith("  rests on ")]
        self.assertEqual(1, len(rests))
        self.assertIn("--orient 0", rests[0])

    def test_a_frame_fold_is_part_of_what_a_layout_rests_on(self) -> None:
        # `--fold F:M` is a pin like the others: given, it is reported.
        shipped = self.run_dump("--layout", "--no-search", "--fold", "0:2", CHART.as_posix())
        self.assertEqual(0, shipped.returncode)
        rests = [ln for ln in shipped.stdout.splitlines() if ln.startswith("  rests on ")]
        self.assertEqual(1, len(rests))
        self.assertIn("--fold 0:2", rests[0])

    def test_a_fold_cut_layer_is_part_of_what_a_layout_rests_on(self) -> None:
        # `--fold F:M:L` names the one rank the cut falls before, and lays out again from it.
        args = ("--layout", "--no-search", "--fold", "0:1:2", CHART.as_posix())
        shipped = self.run_dump(*args)
        self.assertEqual(0, shipped.returncode)
        rests = [ln for ln in shipped.stdout.splitlines() if ln.startswith("  rests on ")]
        self.assertEqual(1, len(rests))
        self.assertIn("--fold 0:1:2", rests[0])
        again = self.run_dump("--layout", "--no-search", *rests[0].split()[2:],
                              CHART.as_posix())
        self.assertEqual(0, again.returncode)
        self.assertEqual(shipped.stdout, again.stdout)
        other = self.run_dump("--layout", "--no-search", "--fold", "0:1:3", CHART.as_posix())
        self.assertNotEqual(shipped.stdout, other.stdout)

    def test_a_port_side_is_part_of_what_a_layout_rests_on(self) -> None:
        # `--end T:L:E:F` at a port end is reported field for field, and lays out again
        # from it.
        chart = "test_data/charts/toolchanger.scav"
        shipped = self.run_dump("--layout", "--no-search", "--end", "6:1:0:2", chart)
        self.assertEqual(0, shipped.returncode)
        rests = [ln for ln in shipped.stdout.splitlines() if ln.startswith("  rests on ")]
        self.assertEqual(1, len(rests))
        self.assertIn(" --end 6:1:0:2", rests[0])
        again = self.run_dump("--layout", "--no-search", *rests[0].split()[2:], chart)
        self.assertEqual(0, again.returncode)
        self.assertEqual(shipped.stdout, again.stdout)
        other = self.run_dump("--layout", "--no-search", "--end", "6:1:0:3", chart)
        self.assertEqual(0, other.returncode)
        geometry = [ln for ln in shipped.stdout.splitlines() if ln.startswith("geometry ")]
        self.assertNotEqual(geometry, [ln for ln in other.stdout.splitlines()
                                       if ln.startswith("geometry ")])

    def test_toolchanger_draws_extend_and_travels_runs_straight(self) -> None:
        # At real text `extend` and every route inside `travel` is one straight line.
        chart = Path("test_data/charts/toolchanger.scav")
        shipped = self.run_dump("--layout", *self.pinned(chart), chart.as_posix())
        self.assertEqual(0, shipped.returncode)
        into = [ln for ln in shipped.stdout.splitlines()
                if ln.startswith("  route ") and " -> arm/Moving:travel/" in ln]
        self.assertEqual(4, len(into))
        for route in into:
            with self.subTest(route=route.split(" (")[0]):
                points = re.findall(r"\((-?\d+),(-?\d+)\)", route)
                self.assertTrue(len({x for x, _ in points}) == 1
                                or len({y for _, y in points}) == 1)

    def test_an_unknown_profile_is_refused(self) -> None:
        result = self.run_dump("--layout", "--profile", "nonesuch", CHART.as_posix())
        self.assertNotEqual(0, result.returncode)
        self.assertIn("no such profile", result.stderr)

    def test_a_loop_placement_is_part_of_what_a_layout_rests_on(self) -> None:
        # `--loop S:F:E` is reported field for field, and lays out again from it.
        chart = "test_data/charts/gauntlet/room.scav"
        shipped = self.run_dump("--layout", "--no-search", "--loop", "0:3:0", chart)
        self.assertEqual(0, shipped.returncode)
        rests = [ln for ln in shipped.stdout.splitlines() if ln.startswith("  rests on ")]
        self.assertEqual(1, len(rests))
        self.assertIn(" --loop 0:3:0", rests[0])
        again = self.run_dump("--layout", "--no-search", *rests[0].split()[2:], chart)
        self.assertEqual(0, again.returncode)
        self.assertEqual(shipped.stdout, again.stdout)
        other = self.run_dump("--layout", "--no-search", "--loop", "0:2:1", chart)
        self.assertEqual(0, other.returncode)
        geometry = [ln for ln in shipped.stdout.splitlines() if ln.startswith("geometry ")]
        self.assertNotEqual(geometry, [ln for ln in other.stdout.splitlines()
                                       if ln.startswith("geometry ")])

    def test_a_malformed_pin_is_a_usage_error(self) -> None:
        for bad in (["--rank", "1"], ["--cut", "a:b"], ["--end", "1:0:2:0"],
                    ["--portfolio-row", "99"], ["--no-search", "--no-search"],
                    ["--no-text", "--no-text"], ["--profile"], ["--orient", "x"],
                    ["--fold", "0:3"], ["--fold", "0"], ["--fold", "0:3:1"],
                    ["--fold", "0:1:x"], ["--fold", "0:1:"], ["--end"],
                    ["--end", "6:1:0"], ["--end", "6:1:2:0"], ["--end", "6:1:0:4"],
                    ["--end", "6:1:0:x"], ["--end", "6:1:0:"], ["--end", "6:1:0:2:1"],
                    ["--loop", "0:4:0"], ["--loop", "0:0:2"], ["--loop", "0:1"]):
            with self.subTest(bad=bad):
                result = self.run_dump("--layout", *bad, CHART.as_posix())
                self.assertEqual(2, result.returncode)

    def test_hash_refuses_layout_and_json(self) -> None:
        result = self.run_dump("--hash", "--layout", NETWORK.as_posix())
        self.assertEqual(2, result.returncode)

    def test_a_three_document_network_matches_the_golden(self) -> None:
        out = self.check_golden(NETWORK, NETWORK_GOLDEN)
        # Included content prints under its alias state, in one containment tree.
        self.assertIn("state dock", out)
        self.assertIn("(test_data/charts/dock.scav:", out)
        self.assertIn("(test_data/charts/led.scav:", out)
        # A cross-document endpoint resolved.
        self.assertIn("-> dock/On/Seated", out)
        # led.scav is one document and two instantiations, so it appears under
        # both `dock/lamp` and `lamp` with disjoint rows.
        self.assertIn("trans dock/lamp/Off -> dock/lamp/Blinking", out)
        self.assertIn("trans lamp/Off -> lamp/Blinking", out)

    def line_of(self, chart: Path, needle: str) -> int:
        text = (self.cfg.repo_root / chart).read_text(encoding="utf-8")
        for n, line in enumerate(text.split("\n"), 1):
            if needle in line:
                return n
        self.fail(f"{needle} not in {chart}")

    def test_an_attribute_points_at_its_own_statement(self) -> None:
        out = self.check_golden(MILL, MILL_GOLDEN)
        # The `@machine { ... }` block is one statement producing two rows, so
        # both point at that line rather than at the chart's.
        machine = self.line_of(MILL, "@machine {")
        self.assertIn(f'@machine:axes = "3" (test_data/charts/mill.scav:{machine})', out)
        self.assertIn(
            f'@machine:spindle_kw = "2" (test_data/charts/mill.scav:{machine})', out)
        # An attribute inside a state reports the attribute's line.
        doc = self.line_of(MILL, "@doc = ")
        self.assertIn('@doc = "gantry mill with a carousel changer" '
                      f'(test_data/charts/mill.scav:{doc})', out)

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
            # An attribute statement carries its own location.
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
        # A missing include is a failed `fopen` in the CLI, reported as `cannot read`.
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
        # The error names cyc_b.scav:3, the include that closes the cycle.
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
        self.assertIn("broken_leaf.scav:1:", result.stderr.replace("\\", "/"))

    def test_a_parse_error_prints_no_model(self) -> None:
        chart = self.cfg.scratch_dir / "dump" / "broken.scav"
        chart.parent.mkdir(parents=True, exist_ok=True)
        chart.write_text("chart broken { state , }\n", encoding="utf-8")
        result = self.run_dump(chart)
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertIn(":1:", result.stderr)

    # --json ================================================================

    def test_json_matches_the_golden(self) -> None:
        result = self.run_dump("--json", NETWORK.as_posix())
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        want = (self.cfg.repo_root / JSON_GOLDEN).read_text(encoding="utf-8")
        if result.stdout != want:
            actual = self.cfg.scratch_dir / "golden" / "dump" / JSON_GOLDEN.name
            actual.parent.mkdir(parents=True, exist_ok=True)
            actual.write_text(result.stdout, encoding="utf-8")
            self.fail(
                f"golden mismatch: {self.cfg.repo_root / JSON_GOLDEN} vs {actual}"
            )

    def test_json_parses_and_carries_every_entity_array(self) -> None:
        doc = json.loads(self.run_dump("--json", NETWORK.as_posix()).stdout)
        self.assertEqual("vac", doc["chart"]["name"])
        self.assertEqual("robot vacuum", doc["chart"]["label"])
        self.assertEqual(3, len(doc["documents"]))
        self.assertEqual(3, len(doc["includes"]))
        for key in ("states", "submachines", "transitions", "attrs", "columns"):
            self.assertIn(key, doc)
        self.assertGreater(len(doc["states"]), 10)

    def test_json_ids_index_the_arrays_they_name(self) -> None:
        doc = json.loads(self.run_dump("--json", NETWORK.as_posix()).stdout)
        for state in doc["states"]:
            if state["parent"] is not None:
                self.assertLess(state["parent"], len(doc["submachines"]))
            for sub in state["submachines"]:
                self.assertLess(sub, len(doc["submachines"]))
            for attr in state["attrs"]:
                self.assertLess(attr, len(doc["attrs"]))
        for trans in doc["transitions"]:
            self.assertLess(trans["src"], len(doc["states"]))
            self.assertLess(trans["dst"], len(doc["states"]))
        for inc in doc["includes"]:
            self.assertLess(inc["target"], len(doc["documents"]))
            self.assertLess(inc["host"], len(doc["states"]))

    def test_json_spells_an_absent_id_as_null(self) -> None:
        doc = json.loads(self.run_dump("--json", NETWORK.as_posix()).stdout)
        # A root document's entities have no instantiation, and the chart's own
        # root submachine has no owner.
        self.assertTrue(any(s["inst"] is None for s in doc["states"]))
        self.assertTrue(any(m["owner"] is None for m in doc["submachines"]))

    def test_json_agrees_with_the_hash_verb(self) -> None:
        doc = json.loads(self.run_dump("--json", NETWORK.as_posix()).stdout)
        text = self.run_dump("--hash", NETWORK.as_posix()).stdout.strip()
        self.assertEqual(int(text, 16), doc["chart"]["structural_hash"])

    def test_json_escapes_what_json_requires(self) -> None:
        chart = self.write(
            "escapes.scav",
            'chart e {\n  state A "quote \\" back \\\\ tab \\t nl \\n",\n}\n',
        )
        result = self.run_dump("--json", chart)
        self.assertEqual(0, result.returncode)
        doc = json.loads(result.stdout)
        labels = [s["label"] for s in doc["states"] if s["name"] == "A"]
        self.assertEqual(['quote " back \\ tab \t nl \n'], labels)

    def test_hash_and_json_cannot_be_asked_for_together(self) -> None:
        result = self.run_dump("--hash", "--json", NETWORK.as_posix())
        self.assertEqual(2, result.returncode)
        self.assertIn("usage:", result.stderr)

    def test_json_escapes_every_control_character_it_names(self) -> None:
        # \r, \b and \f have their own two-character spellings; anything else
        # below 0x20 takes the \u00xx form.
        chart = self.write(
            "control.scav",
            'chart c {\n  state A "cr\\u000d bs\\u0008 ff\\u000c us\\u001f",\n}\n',
        )
        result = self.run_dump("--json", chart)
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        self.assertIn(r'"label": "cr\r bs\b ff\f us\u001f"', result.stdout)
        doc = json.loads(result.stdout)
        self.assertEqual(
            ["cr\r bs\b ff\f us\x1f"],
            [s["label"] for s in doc["states"] if s["name"] == "A"])

    def test_a_missing_file_is_an_error(self) -> None:
        result = self.run_dump("test_data/charts/no_such_chart.scav")
        self.assertEqual(2, result.returncode)
        self.assertIn("cannot read", result.stderr)

    # --layout failures =====================================================

    def test_layout_refuses_text_the_bundled_font_cannot_measure(self) -> None:
        # U+F0001 is a private-use codepoint JetBrains Mono lacks.
        chart = self.write(
            "unmeasurable.scav",
            'chart g {\n  state A,\n  state B,\n  trans A -> B "\U000f0001",\n}\n')
        result = self.run_dump("--layout", chart)
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertEqual(
            f"scav: cannot measure the chart with the bundled font '{chart}'\n",
            result.stderr)

    def test_layout_reports_geometry_past_the_coordinate_domain(self) -> None:
        # Nested boxes, each level adding a padding ring and a sibling beside
        # the one it nests, until the composed width leaves the domain.
        body = "".join(
            f"state S{i} {{" + "".join(f" state W{i}_{k}{'w' * 80}," for k in range(4))
            for i in range(255))
        chart = self.write(
            "overflow.scav", "chart big {" + body + "state Leaf," + ("}," * 255) + "}\n")
        result = self.run_dump("--layout", chart)
        self.assertEqual(1, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertRegex(
            result.stderr,
            "^" + re.escape(chart.as_posix())
            + r":\d+:\d+: composed geometry exceeds the coordinate domain\n$")

    # Trace =================================================================

    def split_trace(self, out: str) -> tuple[list[dict], dict]:
        """Splits `--trace --json` output into the event array and the model."""
        end = out.index("\n]\n") + 3
        return json.loads(out[:end]), json.loads(out[end:])

    @scavtest.full_only
    def test_a_traced_run_draws_what_an_untraced_one_draws(self) -> None:
        """Tracing leaves each corpus chart's layout geometry unchanged."""
        charts = sorted((self.cfg.repo_root / "test_data/charts").glob("*.scav"))
        self.assertTrue(charts)
        for chart in charts:
            if scavtest.cli_skipped(chart.name):
                continue
            with self.subTest(chart=chart.name):
                plain = self.run_dump("--json", "--layout", chart.as_posix())
                self.assertEqual(0, plain.returncode, plain.stderr)
                traced = self.run_dump("--json", "--layout", "--trace",
                                       chart.as_posix())
                self.assertEqual(0, traced.returncode, traced.stderr)
                events, model = self.split_trace(traced.stdout)
                want = json.loads(plain.stdout)["geometry"]
                got = model["geometry"]
                for key in ("structural_hash", "coordinate_hash", "state", "route",
                            "point" if "point" in want else "chart"):
                    self.assertEqual(want[key], got[key], key)
                self.assertTrue(events)

    @scavtest.full_only
    def test_every_event_names_its_kind_and_its_frame(self) -> None:
        traced = self.run_dump("--json", "--layout", "--trace", NETWORK.as_posix())
        self.assertEqual(0, traced.returncode, traced.stderr)
        events, model = self.split_trace(traced.stdout)
        frames = len(model["submachines"])
        kinds = set()
        for i, e in enumerate(events):
            self.assertEqual(i, e["i"])  # dense and in order
            self.assertNotEqual("none", e["kind"])
            kinds.add(e["kind"])
            if "frame" in e:
                self.assertLess(e["frame"], frames)
        # The decisions a route's shape comes from, all present on a real chart.
        self.assertLessEqual({"rank_assigned", "node_placed", "net_planned",
                              "seat_moved"}, kinds)

    def test_trace_needs_a_layout_to_trace(self) -> None:
        result = self.run_dump("--trace", NETWORK.as_posix())
        self.assertEqual(2, result.returncode)
        self.assertTrue(result.stderr.startswith("usage: scav <verb>"))

    def run_trace(self, *args: scavtest.Arg) -> subprocess.CompletedProcess[str]:
        argv = [str(self.exe), "trace", *[str(a) for a in args]]
        print(f"+ {' '.join(argv)}", flush=True)
        return subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=self.cfg.repo_root,
        )

    def test_a_trace_file_holds_the_trace_that_trace_prints(self) -> None:
        """`--trace-file` keeps the trace out of stdout, and `scav trace` prints it as
        `--trace` does, at every scope."""
        flags = [*self.pinned(NETWORK), NETWORK.as_posix()]
        path = self.cfg.scratch_dir / "dump" / "vac.trace"
        path.parent.mkdir(parents=True, exist_ok=True)
        for scope in ([], ["--trace-search"], ["--trace-outline"]):
            with self.subTest(scope=scope):
                printed = self.run_dump("--json", "--layout", "--trace", *scope, *flags)
                self.assertEqual(0, printed.returncode, printed.stderr)
                end = printed.stdout.index("\n]\n") + 3
                path.unlink(missing_ok=True)
                kept = self.run_dump("--json", "--layout", "--trace", *scope,
                                     "--trace-file", path, *flags)
                self.assertEqual(0, kept.returncode, kept.stderr)
                self.assertEqual(printed.stdout[end:], kept.stdout)
                self.assertFalse(path.with_name(path.name + ".tmp").exists())
                read = self.run_trace(path)
                self.assertEqual(0, read.returncode, read.stderr)
                self.assertEqual("", read.stderr)
                self.assertEqual(printed.stdout[:end], read.stdout)

    def run_capped(self, verb: str, *args: scavtest.Arg, stdout: Path | None = None,
                   cap: int) -> subprocess.CompletedProcess[str]:
        """`scav VERB ARGS` with every file it writes held to `cap` bytes, stdout into the
        file `stdout` when given."""
        import resource
        import signal

        def limit() -> None:
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (cap, cap))

        argv = [str(self.exe), verb, *[str(a) for a in args]]
        print(f"+ {' '.join(argv)}  (files capped at {cap} bytes)", flush=True)
        with open(stdout or os.devnull, "w", encoding="utf-8") as out:
            return subprocess.run(argv, stdout=out, stderr=subprocess.PIPE, text=True,
                                  cwd=self.cfg.repo_root, preexec_fn=limit)

    @unittest.skipIf(os.name == "nt", "file size limits are POSIX")
    def test_a_trace_file_replaces_the_old_one_only_when_written_whole(self) -> None:
        flags = [*self.pinned(NETWORK), NETWORK.as_posix()]
        path = self.cfg.scratch_dir / "dump" / "replaced.trace"
        temp = path.with_name(path.name + ".tmp")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("previous", encoding="utf-8")
        whole = self.run_dump("--layout", "--trace", "--trace-file", path, *flags)
        self.assertEqual(0, whole.returncode, whole.stderr)
        self.assertFalse(temp.exists())
        size = path.stat().st_size
        self.assertEqual(0, self.run_trace(path).returncode)
        path.write_text("previous", encoding="utf-8")
        cut = self.run_capped("dump", "--layout", "--trace", "--trace-file", path, *flags,
                              cap=size // 2)
        self.assertEqual(2, cut.returncode)
        self.assertEqual(f"scav: cannot write the trace '{path}'\n", cut.stderr)
        self.assertEqual("previous", path.read_text(encoding="utf-8"))
        self.assertFalse(temp.exists())

    @unittest.skipIf(os.name == "nt", "file size limits are POSIX")
    def test_a_trace_whose_json_cannot_be_written_is_an_error(self) -> None:
        flags = [*self.pinned(NETWORK), NETWORK.as_posix()]
        path = self.cfg.scratch_dir / "dump" / "unwritten.trace"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.unlink(missing_ok=True)
        self.assertEqual(0, self.run_dump("--layout", "--trace", "--trace-file", path,
                                          *flags).returncode)
        out = self.cfg.scratch_dir / "dump" / "unwritten.json"
        for verb, args in (("dump", ["--layout", "--trace", *flags]), ("trace", [path])):
            with self.subTest(verb=verb):
                result = self.run_capped(verb, *args, stdout=out, cap=4096)
                self.assertEqual(2, result.returncode)
                self.assertEqual("scav: cannot write the trace '-'\n", result.stderr)

    def test_a_trace_file_that_cannot_be_written_is_an_error(self) -> None:
        path = self.cfg.scratch_dir / "dump" / "no_such_dir" / "vac.trace"
        result = self.run_dump("--json", "--layout", "--trace", "--trace-file", path,
                               *self.pinned(NETWORK), NETWORK.as_posix())
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertEqual(f"scav: cannot write the trace '{path}'\n", result.stderr)
        self.assertFalse(path.exists())

    def test_trace_prints_nothing_for_a_cut_trace_file(self) -> None:
        """A real trace cut anywhere, mid-record and just before its end record among
        the cuts, prints nothing to stdout, however much of it decodes first."""
        states = 1000
        chart = self.write("chain.scav", "chart g {\n"
                           + "".join(f"  state S{i},\n" for i in range(states))
                           + "".join(f"  trans S{i} -> S{i + 1},\n"
                                     for i in range(states - 1))
                           + "}\n")
        whole = self.cfg.scratch_dir / "dump" / "chain.trace"
        whole.unlink(missing_ok=True)
        made = self.run_dump("--layout", "--no-text", "--portfolio-row", "0",
                             "--no-search", "--trace", "--trace-file", whole, chart)
        self.assertEqual(0, made.returncode, made.stderr)
        read = self.run_trace(whole)
        self.assertEqual(0, read.returncode, read.stderr)
        self.assertGreater(len(read.stdout), 1 << 19)
        data = whole.read_bytes()

        def varint(at: int) -> tuple[int, int]:
            """The varint at `at` and the offset after it."""
            value, shift = 0, 0
            while data[at] & 0x80:
                value |= (data[at] & 0x7F) << shift
                at, shift = at + 1, shift + 7
            return value | (data[at] << shift), at + 1

        def encoded(value: int) -> bytes:
            out = bytearray()
            while value >= 0x80:
                out.append((value & 0x7F) | 0x80)
                value >>= 7
            return bytes(out + bytes([value]))

        # Magic, version, then the length of the rest of the header.
        _, at = varint(8)
        length, at = varint(at)
        header_end = at + length
        end = bytes([0xFF]) + encoded(len(json.loads(read.stdout)))
        self.assertTrue(data.endswith(end))
        end_at = len(data) - len(end)
        middle = len(data) // 2
        cuts = sorted({0, 4, 8, header_end - 1, header_end, header_end + 1,
                       *range(middle, middle + 4), *range(end_at - 4, end_at),
                       end_at, end_at + 1, len(data) - 1})
        cut = self.cfg.scratch_dir / "dump" / "cut.trace"
        for n in [*cuts, None]:
            with self.subTest(cut=n):
                cut.write_bytes(data[:n] if n is not None else data + b"\0")
                result = self.run_trace(cut)
                self.assertEqual(2, result.returncode)
                self.assertEqual("", result.stdout)
                self.assertEqual(f"scav: not a whole trace file '{cut}'\n", result.stderr)

    def test_trace_names_a_trace_another_build_wrote(self) -> None:
        """A header whose version or schema differs from this build's prints nothing and
        says another build wrote it."""
        path = self.cfg.scratch_dir / "dump" / "other.trace"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.unlink(missing_ok=True)
        made = self.run_dump("--layout", "--trace", "--trace-file", path,
                             *self.pinned(NETWORK), NETWORK.as_posix())
        self.assertEqual(0, made.returncode, made.stderr)
        data = path.read_bytes()
        # The version byte follows the 8-byte magic; the first kind is named `none`.
        named = data.index(b"none")
        for at in (8, named):
            with self.subTest(at=at):
                other = bytearray(data)
                other[at] ^= 0x20 if at == named else 0x01
                path.write_bytes(bytes(other))
                result = self.run_trace(path)
                self.assertEqual(2, result.returncode)
                self.assertEqual("", result.stdout)
                self.assertEqual(f"scav: trace written by a different scav build '{path}'\n",
                                 result.stderr)

    def test_trace_prints_only_a_whole_trace_file(self) -> None:
        chart = self.write("not_a.trace", "chart g {\n  state A,\n}\n")
        result = self.run_trace(chart)
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertEqual(f"scav: not a whole trace file '{chart}'\n", result.stderr)
        for args in ([], ["--json", chart], [chart, chart], ["-"]):
            with self.subTest(args=args):
                result = self.run_trace(*args)
                self.assertEqual(2, result.returncode)
                self.assertEqual("", result.stdout)
                self.assertTrue(result.stderr.startswith("usage: scav <verb>"),
                                result.stderr)

    # Usage =================================================================

    def test_bad_arguments_are_refused(self) -> None:
        chart = NETWORK.as_posix()
        for args in (["dump"],
                     ["dump", "--json"],
                     ["dump", "--hash"],
                     ["dump", "--layout"],
                     ["dump", "--trace", chart],
                     ["dump", "--layout", "--trace", "--trace", chart],
                     ["dump", "--layout", "--trace-file", "x.trace", chart],
                     ["dump", "--layout", "--search-stats", "--trace-file", "x.trace",
                      chart],
                     ["dump", "--layout", "--trace", "--trace-file", "a.trace",
                      "--trace-file", "b.trace", chart],
                     ["dump", "--layout", "--trace", "--trace-file"],
                     ["dump", "--json", "--json", chart],
                     ["dump", "--layout", "--layout", chart],
                     ["dump", "--hash", "--hash", chart],
                     ["dump", "--nope", chart],
                     ["dump", chart, chart]):
            with self.subTest(args=args):
                result = self.run_dump(*args[1:])
                self.assertEqual(2, result.returncode)
                self.assertEqual("", result.stdout)
                self.assertTrue(result.stderr.startswith("usage: scav <verb>"),
                                result.stderr)


if __name__ == "__main__":
    unittest.main()
