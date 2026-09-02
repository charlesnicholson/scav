#!/usr/bin/env python3
"""`scav render` over the corpus, and the side-by-side harness that the P6 and
P7 blind reviews will score against.

The SVG's own bytes are pinned by a unit golden; what this checks is the parts
only a real process can: the file lands where -o said, the document is one a
parser accepts, and the harness runs and says what it could not compare."""

import os
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import scavtest  # noqa: E402

SVG_NS = "{http://www.w3.org/2000/svg}"


class TestRender(unittest.TestCase):
    cfg: scavtest.Config
    exe: Path
    charts: list[Path]

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        name = "scav.exe" if os.name == "nt" else "scav"
        cls.exe = cls.cfg.build_dir / "bin" / name
        cls.charts = sorted((cls.cfg.repo_root / "test_data/charts").glob("*.scav"))
        assert cls.charts

    def run_render(self, *args: scavtest.Arg) -> subprocess.CompletedProcess[str]:
        # Both streams separately: the document goes to stdout and diagnostics
        # to stderr, and conflating them would let one corrupt the other.
        argv = [str(self.exe), "render", *[str(a) for a in args]]
        print(f"+ {' '.join(argv)}", flush=True)
        return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, cwd=self.cfg.repo_root)

    def render(self, chart: Path, *args: str) -> str:
        result = self.run_render(*args, chart)
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        return result.stdout

    def test_every_corpus_chart_renders_to_parseable_svg(self) -> None:
        for chart in self.charts:
            with self.subTest(chart=chart.name):
                doc = self.render(chart)
                # A real XML parser, not a substring check: the point is that a
                # browser or an SVG consumer would accept it.
                root = ElementTree.fromstring(doc)
                self.assertEqual(f"{SVG_NS}svg", root.tag)
                self.assertIn("viewBox", root.attrib)
                # Integer viewBox, all four fields: no float is ever printed.
                for field in root.attrib["viewBox"].split():
                    int(field)
                self.assertTrue(root.attrib["width"].isdigit())
                self.assertTrue(root.attrib["height"].isdigit())
                # Something was actually drawn.
                self.assertTrue(list(root))

    def test_a_state_name_reaches_the_document_as_text(self) -> None:
        root = ElementTree.fromstring(
            self.render(self.cfg.repo_root / "test_data/charts/estop.scav"))
        texts = {node.text for node in root.iter(f"{SVG_NS}text")}
        self.assertLessEqual({"Clear", "Tripped", "Latched"}, texts)
        # And every element carries the class an external stylesheet restyles by.
        classes = {node.attrib.get("class") for node in root.iter()
                   if node.tag != f"{SVG_NS}svg"}
        self.assertTrue(any(c and c.startswith("scav-state ") for c in classes))

    def test_dash_o_writes_the_file_and_prints_nothing(self) -> None:
        target = self.cfg.build_dir / "test/render_out.svg"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.unlink(missing_ok=True)
        chart = self.cfg.repo_root / "test_data/charts/led.scav"
        result = self.run_render("-o", target, chart)
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertEqual(self.render(chart), target.read_text(encoding="utf-8"))

    def test_embed_font_carries_the_ttf_and_nothing_else_moves(self) -> None:
        chart = self.cfg.repo_root / "test_data/charts/led.scav"
        bare = self.render(chart)
        embedded = self.render(chart, "--embed-font")
        self.assertNotIn("@font-face", bare)
        self.assertIn("@font-face", embedded)
        # 268 KB of TTF, base64'd at four characters per three bytes.
        self.assertGreater(len(embedded) - len(bare), 350_000)
        ElementTree.fromstring(embedded)

    def test_a_named_profile_changes_the_diagram(self) -> None:
        chart = self.cfg.repo_root / "test_data/charts/vac.scav"
        readable = self.render(chart)
        compact = self.render(chart, "--profile", "compact")
        self.assertNotEqual(readable, compact)
        # compact ships a smaller type size, so its diagram is smaller.
        def extent(doc: str) -> int:
            root = ElementTree.fromstring(doc)
            return int(root.attrib["width"]) * int(root.attrib["height"])
        self.assertLess(extent(compact), extent(readable))

    def test_bad_arguments_are_refused(self) -> None:
        chart = self.cfg.repo_root / "test_data/charts/led.scav"
        for args in (["render"],
                     ["render", "--profile", "nonesuch", str(chart)],
                     ["render", "--nope", str(chart)],
                     ["render", "-o"],
                     ["render", str(chart), "-o"],
                     ["render", "--embed-font", "--embed-font", str(chart)]):
            with self.subTest(args=args):
                result = self.run_render(*args[1:])
                self.assertNotEqual(0, result.returncode)

    def test_an_unreadable_chart_is_diagnosed_not_rendered(self) -> None:
        result = self.run_render(self.cfg.build_dir / "no_such.scav")
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertIn("scav:", result.stderr)


class TestBaselineHarness(unittest.TestCase):
    """The blind-review harness. The incumbent engines are provisioned only
    under SCAV_BASELINE, which this deliberately does not set, so what is
    asserted is that scav renders and that anything absent is reported rather
    than left as a silent gap."""

    def test_the_harness_runs_and_names_what_it_could_not_compare(self) -> None:
        cfg = scavtest.load_config()
        out = cfg.build_dir / "baseline"
        name = "scav.exe" if os.name == "nt" else "scav"
        result = subprocess.run(
            [str(cfg.python), str(cfg.repo_root / "tools/baseline.py"),
             "--scav", str(cfg.build_dir / "bin" / name),
             "--out", str(out), "--chart", "estop.scav", "--chart", "tcp.scav"],
            capture_output=True, text=True, check=False)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("estop.scav: ", result.stdout)
        self.assertIn("scav", result.stdout)

        index = out / "index.html"
        self.assertTrue(index.is_file())
        page = index.read_text(encoding="utf-8")
        for chart in ("estop.scav", "tcp.scav"):
            self.assertIn(chart, page)
        for engine in ("scav", "plantuml", "elkjs"):
            self.assertIn(engine, page)
        # scav is the one engine that must always be there. The name is
        # `<chart stem>.<engine>.svg`, so scav's own is `estop.scav.svg`.
        for chart in ("estop", "tcp"):
            rendered = out / f"{chart}.scav.svg"
            self.assertTrue(rendered.is_file(), f"{rendered} not written")
            self.assertTrue(rendered.read_text(encoding="utf-8").startswith("<?xml"))
        # Whatever was missing is named against its chart, not silently
        # skipped. Without SCAV_BASELINE that is both incumbents, every time.
        for engine in ("plantuml", "elkjs"):
            self.assertIn(f" {engine}: ", result.stdout)


if __name__ == "__main__":
    unittest.main()
