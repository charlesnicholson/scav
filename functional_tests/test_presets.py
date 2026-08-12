#!/usr/bin/env python3
"""The committed presets are what the generator produces, and cover every matrix
cell. A generator that quietly stopped emitting a triple would look green."""

import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent / "tools")]

import build  # noqa: E402
import gen_presets  # noqa: E402
import scavtest  # noqa: E402

# Transcribed on purpose: a test that derives its expectation from the thing under
# test is worth nothing.
TRIPLES: dict[str, str] = {
    "macos-clang-libcxx": "Darwin",
    "linux-clang-libcxx": "Linux",
    "linux-clang-libstdcxx": "Linux",
    "linux-gcc-libstdcxx": "Linux",
    "windows-msvc": "Windows",
    "windows-clang": "Windows",
}
COMPILERS: dict[str, str] = {
    "macos-clang-libcxx": "clang++",
    "linux-clang-libcxx": "clang++",
    "linux-clang-libstdcxx": "clang++",
    "linux-gcc-libstdcxx": "g++",
    "windows-msvc": "cl",
    "windows-clang": "clang-cl",
}
CONFIGS: tuple[str, ...] = ("debug", "release", "testable")
SANITIZERS: dict[str, set[str]] = {
    # Availability is a toolchain fact, not a preference.
    "asan": set(TRIPLES) - {"windows-clang"},
    "ubsan": set(TRIPLES) - {"windows-msvc"},
    "tsan": {t for t in TRIPLES if not t.startswith("windows")},
    "msan": {"linux-clang-libcxx"},
}


class TestPresets(unittest.TestCase):
    path: Path
    text: str
    doc: dict[str, Any]
    hidden: dict[str, Any]

    @classmethod
    def setUpClass(cls) -> None:
        cls.path = scavtest.load_config().repo_root / "CMakePresets.json"
        cls.text = cls.path.read_text(encoding="utf-8")
        cls.doc = json.loads(cls.text)
        cls.hidden = {p["name"]: p for p in cls.doc["configurePresets"] if p.get("hidden")}

    def names(self, section: str = "configurePresets") -> set[str]:
        return {p["name"] for p in self.doc[section] if not p.get("hidden")}

    def test_committed_json_is_not_stale(self) -> None:
        self.assertEqual(gen_presets.render(), self.text,
                         f"{self.path} is stale; regenerate it with tools/gen_presets.py")

    def test_every_matrix_cell_has_a_configure_preset(self) -> None:
        for triple in TRIPLES:
            for config in CONFIGS:
                with self.subTest(cell=f"{triple}-{config}"):
                    self.assertIn(f"{triple}-{config}", self.names())

    def test_wasm_row_is_absent(self) -> None:
        # Native only for now. A stubbed preset would make the matrix look
        # complete when it is not.
        self.assertEqual([], [n for n in self.names() if "wasm" in n or "wasi" in n])

    def test_each_triple_pins_a_compiler_and_its_host(self) -> None:
        # "clang" meaning whatever is on PATH makes a row unreproducible.
        for triple, host in TRIPLES.items():
            with self.subTest(triple=triple):
                base = self.hidden.get(f"t-{triple}")
                self.assertIsNotNone(base, f"no hidden base t-{triple}")
                self.assertIn("CMAKE_CXX_COMPILER", base["cacheVariables"])
                self.assertEqual({"type": "equals", "lhs": "${hostSystemName}",
                                  "rhs": host}, base["condition"])

    def test_every_preset_resolves_to_exactly_one_compiler(self) -> None:
        # A bare name is only half a pin, so build.py resolves it against PATH and
        # passes the absolute path, which is what actually makes a row reproducible
        # and a toolchain swap visible. If a preset stopped reaching a compiler
        # through its `inherits` chain that would silently stop happening, and the
        # row would go back to meaning whatever is first on PATH.
        for name in sorted(self.names()):
            with self.subTest(preset=name):
                triple = next(t for t in TRIPLES if name.startswith(f"{t}-"))
                self.assertEqual(COMPILERS[triple], build.preset_compiler(name))

    def test_a_tree_reports_the_compiler_it_was_configured_with(self) -> None:
        # Read back from the cache and compared, because CMake answers a changed
        # compiler by resetting the cache -- which drops the -D arguments and then
        # fails reporting a missing doctest, blaming the wrong thing entirely.
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            self.assertIsNone(build.cached_compiler(tree), "no cache is not an answer")
            (tree / "CMakeCache.txt").write_text(
                "// a comment\n"
                "CMAKE_BUILD_TYPE:STRING=Debug\n"
                "CMAKE_CXX_COMPILER:STRING=/usr/bin/clang++\n",
                encoding="utf-8")
            self.assertEqual("/usr/bin/clang++", build.cached_compiler(tree))

    def test_sanitizers_cover_every_platform_that_supports_them(self) -> None:
        for san, triples in SANITIZERS.items():
            with self.subTest(sanitizer=san):
                found = {n.removesuffix(f"-{san}")
                         for n in self.names() if n.endswith(f"-{san}")}
                self.assertEqual(triples, found)

    def test_sanitizer_presets_set_the_enum_not_a_boolean(self) -> None:
        # One enum, not booleans: ASan and TSan cannot coexist.
        for preset in self.doc["configurePresets"]:
            san = preset["name"].rsplit("-", 1)[-1]
            if preset.get("hidden") or san not in SANITIZERS:
                continue
            with self.subTest(preset=preset["name"]):
                cache = preset.get("cacheVariables", {})
                self.assertEqual(san.upper(), cache.get("SCAV_SANITIZER"))
                self.assertEqual([], [k for k in cache if k.startswith("SCAV_ENABLE_")],
                                 "no per-sanitizer boolean alongside the enum")

    def test_the_testable_config_is_release_plus_scav_testing(self) -> None:
        # Only a meaningful row if it really is release with the define added.
        testable = self.hidden["cfg-testable"]["cacheVariables"]
        release = self.hidden["cfg-release"]["cacheVariables"]
        self.assertEqual(release["CMAKE_BUILD_TYPE"], testable["CMAKE_BUILD_TYPE"])
        self.assertEqual("ON", testable["SCAV_TESTING"])

    def test_every_configure_preset_has_a_build_preset(self) -> None:
        self.assertEqual(self.names(), self.names("buildPresets"))

    def test_there_are_no_test_presets(self) -> None:
        """Tests are build steps, so a build preset already runs them.

        A testPresets section would mean a second command that has to be
        remembered, and the failure mode of forgetting it is a green build with
        untested code -- which is the thing this harness exists to rule out."""
        self.assertNotIn("testPresets", self.doc)

    def test_all_build_output_stays_under_out(self) -> None:
        # Everything scav generates lives under out/, so `rm -rf out` is a
        # factory reset. The base must declare both, or nothing below bites.
        self.assertLessEqual({"binaryDir", "installDir"}, set(self.hidden["base"]))
        for preset in self.doc["configurePresets"]:
            for key in ("binaryDir", "installDir"):
                if key in preset:
                    with self.subTest(preset=preset["name"], key=key):
                        self.assertTrue(preset[key].startswith("${sourceDir}/out/"))


if __name__ == "__main__":
    unittest.main()
