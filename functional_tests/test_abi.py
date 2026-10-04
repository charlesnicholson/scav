#!/usr/bin/env python3
"""Checks the extracted ABI against its golden, and drives model, metrics, space
tables, layout, geometry, DrawList and SVG through the generated bindings.
"""

import ctypes
import os
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import scavtest  # noqa: E402

cfg = scavtest.load_config()
sys.path.insert(0, str(cfg.repo_root / "bindings/python"))

ABI_JSON = cfg.repo_root / "abi/scav_abi.json"
EXTRACT = cfg.repo_root / "tools/abi_extract.py"
GENERATE = cfg.repo_root / "tools/gen_bindings.py"
GENERATED = cfg.repo_root / "bindings/python/scav/_abi.py"


def shared_library() -> Path:
    for name in ("libscav.dylib", "libscav.so", "scav.dll"):
        if (candidate := cfg.build_dir / "lib" / name).is_file():
            return candidate
        if (candidate := cfg.build_dir / "bin" / name).is_file():
            return candidate
    raise FileNotFoundError(f"no shared scav library under {cfg.build_dir}")


class TestAbiGolden(unittest.TestCase):
    work: str

    @classmethod
    def setUpClass(cls) -> None:
        cls._temp = tempfile.TemporaryDirectory()
        cls.work = cls._temp.name

    @classmethod
    def tearDownClass(cls) -> None:
        cls._temp.cleanup()

    def test_extraction_matches_the_committed_golden(self) -> None:
        result = subprocess.run(
            [str(cfg.python), str(EXTRACT), "--check", str(ABI_JSON),
             "--compiler", str(cfg.cxx_compiler)],
            capture_output=True, text=True, check=False)
        if result.returncode != 0:
            fresh = cfg.scratch_dir / "scav_abi.json"
            fresh.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([str(cfg.python), str(EXTRACT), "--out", str(fresh),
                            "--compiler", str(cfg.cxx_compiler)], check=False)
            self.fail(f"{result.stdout}{result.stderr}\nfresh copy at {fresh}")

    def test_the_scraper_fails_closed_on_a_form_it_does_not_model(self) -> None:
        """An unknown declaration form raises `Unrecognized`."""
        sys.path.insert(0, str(EXTRACT.parent))
        import abi_extract

        def header(body: str) -> Path:
            path = Path(self.work) / "scav" / "scav_bogus_c.h"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(
                "#ifndef X_H\n#define X_H\n"
                '#ifdef __cplusplus\nextern "C" {\n#endif\n'
                f"{body}\n"
                "#ifdef __cplusplus\n}\n#endif\n#endif\n", encoding="utf-8")
            return path

        for body, why in (
                ("int (*routes)(int);", "a function pointer"),
                ("union scav_thing { int a; };", "a union"),
                ("#if defined(SOMETHING)\nint x;\n#endif", "a condition"),
                ("#ifdef SOMETHING\nint x;\n#endif", "an unknown ifdef"),
                ("enum { SCAV_NO_VALUE };", "an enumerator without a value"),
                ("int trailing_with_no_semicolon", "a missing semicolon"),
                ("#define SCAV_TWICE(x) ((x) * 2)", "a function-like macro"),
                ("#define SCAV_ODD not_a_number", "a macro that is not a constant")):
            with self.subTest(why=why):
                with self.assertRaises(abi_extract.Unrecognized):
                    abi_extract.scrape(header(body))

    def test_the_scraper_reads_the_header_as_a_c_compiler_does(self) -> None:
        """Scrapes with `__cplusplus` undefined; C++-only sections drop out."""
        sys.path.insert(0, str(EXTRACT.parent))
        import abi_extract
        surface = abi_extract.scrape(cfg.repo_root / "include/scav/scav_types.h")
        names = {s["name"] for s in surface["structs"]}
        self.assertEqual({"scav_span", "scav_point", "scav_extent", "scav_rect"},
                         names)
        # `scav::Coord`, behind `__cplusplus`, is absent from the aliases.
        self.assertNotIn("Coord", {a["name"] for a in surface["aliases"]})

    def test_the_probe_speaks_each_compiler_driver_dialect(self) -> None:
        """`cl` gets MSVC flags and every other compiler GNU flags."""
        sys.path.insert(0, str(EXTRACT.parent))
        import abi_extract
        args = (Path("p.cpp"), Path("p"), Path("w"), [Path("inc")])

        gnu = abi_extract.probe_command("clang++", *args)
        self.assertIn("-std=c++20", gnu)
        self.assertIn("-o", gnu)

        for spelling in ("cl", "cl.exe", "C:/VC/bin/cl.EXE"):
            with self.subTest(compiler=spelling):
                self.assertTrue(abi_extract.is_msvc(spelling))
                msvc = abi_extract.probe_command(spelling, *args)
                self.assertIn("/std:c++20", msvc)
                self.assertNotIn("-std=c++20", msvc)
                self.assertNotIn("-o", msvc)
                # /Fe and /Fo take attached paths.
                self.assertTrue(any(a.startswith("/Fe") for a in msvc))
                self.assertTrue(any(a.startswith("/Fo") for a in msvc))

        # clang-cl takes the GNU branch.
        self.assertFalse(abi_extract.is_msvc("clang-cl"))

    def test_the_shared_library_exports_exactly_the_golden(self) -> None:
        """The shared library's export table holds the golden's functions and
        nothing else."""
        import json
        if os.name == "nt":
            self.skipTest("the export table is read with nm")
        abi = json.loads(ABI_JSON.read_text(encoding="utf-8"))
        functions = {f["name"] for header in abi["headers"]
                     for f in header["functions"]}
        darwin = sys.platform == "darwin"
        result = subprocess.run(
            ["nm", *(["-gU"] if darwin else ["-D", "--defined-only"]),
             str(shared_library())], capture_output=True, text=True, check=True)
        exported = {line.split()[-1] for line in result.stdout.splitlines()
                    if line.strip()}
        if darwin:
            exported = {name.removeprefix("_") for name in exported}
        self.assertEqual(sorted(functions), sorted(exported))

    def test_the_generated_layer_is_the_one_the_golden_describes(self) -> None:
        """Regenerating the bindings reproduces the committed file."""
        fresh = cfg.scratch_dir / "_abi_regenerated.py"
        fresh.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            [str(cfg.python), str(GENERATE), "--abi", str(ABI_JSON),
             "--out", str(fresh)], capture_output=True, text=True, check=False)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(GENERATED.read_text(encoding="utf-8"),
                         fresh.read_text(encoding="utf-8"),
                         "bindings/python/scav/_abi.py is out of date")

    def test_the_golden_covers_the_whole_surface(self) -> None:
        import json
        abi = json.loads(ABI_JSON.read_text(encoding="utf-8"))
        handles = {h["name"] for header in abi["headers"] for h in header["handles"]}
        # Five handles, each with a create and a destroy.
        self.assertEqual(
            {"scav_load", "scav_chart", "scav_metrics", "scav_images",
             "scav_drawlist"}, handles)
        functions = {f["name"] for header in abi["headers"]
                     for f in header["functions"]}
        for handle in handles:
            self.assertIn(f"{handle}_destroy", functions)
        # Object-like macros appear as constants.
        constants = {c["name"] for header in abi["headers"]
                     for c in header.get("constants", [])}
        self.assertIn("SCAV_CLIP_NONE", constants)

        for entry in ("scav_abi_version", "scav_layout_run", "scav_column_data",
                      "scav_measure_text", "scav_measure_chart", "scav_emit_chart",
                      "scav_svg_write", "scav_image_register",
                      "scav_router_by_name", "scav_str"):
            self.assertIn(entry, functions)

        codes = {value["name"] for header in abi["headers"]
                 for enum in header["enums"] for value in enum["values"]}
        self.assertIn("SCAV_E_ABI", codes)

        # Each caller-owned POD has a size parameter and each returned array a stride.
        params = {f["name"]: [p["name"] for p in f["params"]]
                  for header in abi["headers"] for f in header["functions"]}
        for entry, size in (("scav_profile_named", "out_size"),
                            ("scav_profile_validate", "profile_size"),
                            ("scav_layout_run", "spaces_size"),
                            ("scav_layout_run", "opts_size"),
                            ("scav_layout_run", "placed_size"),
                            ("scav_chart_diag", "out_size"),
                            ("scav_measure_text", "out_size"),
                            ("scav_measure_chart", "profile_size"),
                            ("scav_measure_chart", "box_state_row_size"),
                            ("scav_emit_chart", "palette_row_size"),
                            ("scav_emit_chart", "placed_row_size"),
                            ("scav_palette_standard", "row_size"),
                            ("scav_svg_write", "options_size"),
                            ("scav_svg_bounds", "out_size")):
            with self.subTest(entry=entry, size=size):
                self.assertIn(size, params[entry])
        for entry in ("scav_load_pending", "scav_column_data", "scav_drawlist_prims",
                      "scav_drawlist_styles", "scav_drawlist_points",
                      "scav_drawlist_clips"):
            with self.subTest(entry=entry):
                self.assertIn("out_stride", params[entry])
        # The four caller-owned row arrays behind one struct, each declaring the
        # stride it was filled at.
        spaces = next(s for header in abi["headers"] for s in header["structs"]
                      if s["name"] == "scav_spaces")
        self.assertEqual(
            ["box_state_stride", "box_sub_stride", "path_clear_stride",
             "path_box_stride"],
            [f["name"] for f in spaces["fields"] if f["name"].endswith("_stride")])

        # Every struct records its size and padding, and every field its offset.
        for header in abi["headers"]:
            for struct in header["structs"]:
                self.assertIn("padding", struct)
                self.assertIn("size", struct)
                for field in struct["fields"]:
                    self.assertIn("offset", field)


class TestGeneratedBindings(unittest.TestCase):
    """Drives the shared library through the generated Python bindings."""

    @classmethod
    def setUpClass(cls) -> None:
        if cfg["sanitizer"].upper() not in ("", "NONE"):
            # A sanitized library aborts when dlopen'd into an uninstrumented host.
            raise unittest.SkipTest(
                f"ctypes cannot load a {cfg['sanitizer']} build; the unit tests "
                "cover these entry points under the sanitizer")
        import scav
        cls.scav = scav
        scav.library(shared_library())
        cls.charts = cfg.repo_root / "test_data/charts"

    def network(self) -> dict[str, str]:
        """vac plus what it includes, keyed by the names vac asks for."""
        sources = {}
        for name in ("vac.scav", "dock.scav", "led.scav"):
            sources[name] = (self.charts / name).read_text(encoding="utf-8")
        # vac includes `./led.scav` and dock includes `led.scav`.
        sources["./led.scav"] = sources["led.scav"]
        return sources

    def test_the_whole_pipeline_through_generated_marshalling(self) -> None:
        scav = self.scav
        sources = self.network()
        root = "vac.scav"
        loader = scav.Loader()
        loader.add(root, sources[root])
        rounds = 0
        while wanted := loader.pending():
            rounds += 1
            self.assertLess(rounds, 8, "the loader did not converge")
            for name in wanted:
                key = Path(name).name
                self.assertIn(key, sources, f"loader asked for {name}")
                loader.add(name, sources[key])
        chart = loader.finish()
        loader.close()

        counts = chart.counts()
        self.assertEqual(3, counts["documents"])
        self.assertEqual(3, counts["includes"])
        self.assertGreater(counts["states"], 10)
        self.assertNotEqual(0, chart.structural_hash)
        self.assertTrue(chart.digest())

        with scav.Metrics() as metrics:
            self.assertEqual(1000, metrics.units_per_em)
            self.assertEqual(1743, metrics.glyph_count)
            width, height = metrics.measure("Idle", 160)
            self.assertGreater(width, 0)
            self.assertEqual(160, height)

            prof = scav.profile("readable")
            spaces = scav.Spaces.measure(chart, metrics, prof)
            self.assertEqual(counts["states"], len(spaces.box_state))
            self.assertEqual(counts["transitions"], len(spaces.path_clear))
            self.assertTrue(any(row.min_w > 0 for row in spaces.box_state))

            placed = chart.layout(spaces)
            self.assertEqual(len(spaces.path_box), len(placed))

            # Geometry through the three-call column accessor.
            boxes = chart.rects("scav.geom.state")
            self.assertEqual(counts["states"], len(boxes))
            self.assertTrue(any((b.w > 0) and (b.h > 0) for b in boxes))
            extent = chart.rects("scav.geom.chart")[0]
            self.assertGreater(extent.w, 0)

            drawlist = scav.DrawList.build(chart, metrics, spaces, placed)
            drawlist.validate()
            drawlist.canonicalize()
            drawn = drawlist.counts()
            self.assertGreater(drawn["prims"], 0)
            self.assertGreater(drawn["styles"], 0)
            self.assertNotEqual(0, drawlist.digest(metrics))

            names = {text for _, text in drawlist.texts()}
            self.assertIn("Booting", names)

            doc = drawlist.svg(metrics, margin=prof.pad)
            root_element = ElementTree.fromstring(doc)
            self.assertEqual("{http://www.w3.org/2000/svg}svg", root_element.tag)
            self.assertIn("Booting", doc)
            drawlist.close()

    def test_the_generated_svg_is_the_one_the_cli_produces(self) -> None:
        """The bindings' SVG matches `scav render`, with and without `--embed-font`;
        a truncated font raises SCAV_E_FONT."""
        scav = self.scav
        chart_path = self.charts / "estop.scav"
        name = "scav.exe" if os.name == "nt" else "scav"
        cli = subprocess.run(
            [str(cfg.build_dir / "bin" / name), "render", str(chart_path)],
            capture_output=True, text=True, check=False)
        self.assertEqual(0, cli.returncode, cli.stderr)

        chart = scav.load_network(
            "estop.scav", {"estop.scav": chart_path.read_text(encoding="utf-8")})
        with scav.Metrics() as metrics:
            prof = scav.profile("readable")
            spaces = scav.Spaces.measure(chart, metrics, prof)
            placed = chart.layout(spaces)
            drawlist = scav.DrawList.build(chart, metrics, spaces, placed)
            drawlist.canonicalize()
            self.assertEqual(cli.stdout, drawlist.svg(metrics, margin=prof.pad))

            embedded = subprocess.run(
                [str(cfg.build_dir / "bin" / name), "render", "--embed-font",
                 str(chart_path)], capture_output=True, text=True, check=False)
            self.assertEqual(0, embedded.returncode, embedded.stderr)
            ttf = scav.bundled_font()
            self.assertEqual(embedded.stdout,
                             drawlist.svg(metrics, embed_font=ttf, margin=prof.pad))
            with self.assertRaises(scav.ScavError) as refused:
                drawlist.svg(metrics, embed_font=ttf[:-1], margin=prof.pad)
            self.assertEqual(scav._abi.SCAV_E_FONT, refused.exception.code)
            drawlist.close()
        chart.close()

    def test_a_float_never_reaches_a_space_request(self) -> None:
        """Space requests take only an int within int32; a float or bool raises."""
        scav = self.scav
        chart = scav.load_network(
            "estop.scav",
            {"estop.scav": (self.charts / "estop.scav").read_text(encoding="utf-8")})
        with scav.Metrics() as metrics:
            spaces = scav.Spaces.measure(chart, metrics, scav.profile("readable"))
            with self.assertRaises(TypeError):
                spaces.set_box_state(0, 100 / 2, 0, 0)     # a float
            with self.assertRaises(TypeError):
                spaces.set_box_state(0, True, 0, 0)        # a bool
            with self.assertRaises(ValueError):
                spaces.set_box_state(0, 2**31, 0, 0)       # past int32
            spaces.set_box_state(0, 100 // 2, 0, 0)        # accepted
            self.assertEqual(50, spaces.box_state[0].min_w)
            with self.assertRaises(TypeError):
                metrics.measure("x", 16.0)
        chart.close()

    def test_a_handle_is_its_own_type_and_closes_idempotently(self) -> None:
        scav = self.scav
        chart = scav.load_network(
            "estop.scav",
            {"estop.scav": (self.charts / "estop.scav").read_text(encoding="utf-8")})
        drawlist = scav.DrawList()
        # Distinct opaque types, so one cannot be handed where another belongs.
        with self.assertRaises(ctypes.ArgumentError):
            scav.library().scav_chart_destroy(drawlist.pointer)
        drawlist.close()
        drawlist.close()  # idempotent
        with self.assertRaises(ValueError):
            _ = drawlist.pointer
        chart.close()
        chart.close()

    def test_an_error_carries_its_code_and_its_name(self) -> None:
        scav = self.scav
        with self.assertRaises(scav.ScavError) as caught:
            scav.profile("no-such-profile")
        self.assertEqual(scav._abi.SCAV_E_INVALID_ARG, caught.exception.code)
        self.assertIn("SCAV_E_INVALID_ARG", str(caught.exception))

    def test_a_buffer_too_small_reaches_the_caller_as_a_capacity_error(self) -> None:
        """A cap one byte short returns SCAV_E_CAPACITY and the required size, and
        writes nothing."""
        scav = self.scav
        lib = scav.library()
        chart = scav.load_network(
            "estop.scav",
            {"estop.scav": (self.charts / "estop.scav").read_text(encoding="utf-8")})
        size = ctypes.c_uint32(0)
        scav._abi.check(
            lib.scav_chart_digest(chart.pointer, None, 0, ctypes.byref(size)),
            "scav_chart_digest")
        self.assertGreater(size.value, 1)

        needed = ctypes.c_uint32(0)
        buffer = (ctypes.c_ubyte * (size.value - 1))()
        code = lib.scav_chart_digest(chart.pointer, buffer, size.value - 1,
                                     ctypes.byref(needed))
        self.assertEqual(scav._abi.SCAV_E_CAPACITY, code)
        self.assertEqual(size.value, needed.value)
        self.assertEqual(0, sum(buffer))  # nothing written
        with self.assertRaises(scav.ScavError) as caught:
            scav._abi.check(code, "scav_chart_digest")
        self.assertIn("SCAV_E_CAPACITY", str(caught.exception))
        chart.close()

    def test_a_struct_size_the_library_disagrees_with_is_its_own_code(self) -> None:
        """A wrong struct size returns SCAV_E_ABI, named in the raised error."""
        scav = self.scav
        prof = scav.profile("readable")
        code = scav.library().scav_profile_named(
            b"readable", ctypes.byref(prof), ctypes.sizeof(prof) - 4)
        self.assertEqual(scav._abi.SCAV_E_ABI, code)
        with self.assertRaises(scav.ScavError) as caught:
            scav._abi.check(code, "scav_profile_named")
        self.assertIn("SCAV_E_ABI", str(caught.exception))

    def test_a_drawlist_appends_with_its_indices_rebased(self) -> None:
        scav = self.scav
        chart = scav.load_network(
            "led.scav",
            {"led.scav": (self.charts / "led.scav").read_text(encoding="utf-8")})
        with scav.Metrics() as metrics:
            prof = scav.profile("readable")
            spaces = scav.Spaces.measure(chart, metrics, prof)
            placed = chart.layout(spaces)
            one = scav.DrawList.build(chart, metrics, spaces, placed)
            two = scav.DrawList.build(chart, metrics, spaces, placed, depth=10)
            before = one.counts()["prims"]
            one.append(two)
            one.validate()
            self.assertEqual(before * 2, one.counts()["prims"])
            # Both lists' depths survive the append.
            self.assertEqual({0, 10}, {p.depth for p in one.prims()})
            one.close()
            two.close()
        chart.close()

    def test_an_image_registers_and_reaches_the_document(self) -> None:
        scav = self.scav
        with scav.Images() as images:
            images.register("logo", b"\x89PNG\r", 16, 16, "image/png")
            with self.assertRaises(scav.ScavError):
                images.register("logo", b"\x89PNG\r", 16, 16, "image/png")
            with self.assertRaises(TypeError):
                images.register("other", b"\x89PNG\r", 16.0, 16, "image/png")


if __name__ == "__main__":
    unittest.main()
