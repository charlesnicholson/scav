#!/usr/bin/env python3
"""Canonical form belongs to running the printer rather than to the format, so
this verb is what makes it something a repo can hold. It gates the corpus."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

CHARTS = Path("test_data/charts")


class TestFmt(unittest.TestCase):
    cfg: scavtest.Config
    exe: Path
    scratch: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        name = "scav.exe" if os.name == "nt" else "scav"
        cls.exe = cls.cfg.build_dir / "bin" / name
        cls.scratch = scavtest.fresh_dir(cls.cfg.scratch_dir / "fmt")

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
        # Newline off, so a test writing CRLF gets CRLF on every platform.
        path.write_text(text, encoding="utf-8", newline="")
        return path

    def corpus(self) -> list[str]:
        root = self.cfg.repo_root / CHARTS
        return sorted((CHARTS / p.name).as_posix() for p in root.glob("*.scav"))

    # The gate ==============================================================

    def test_the_committed_corpus_is_canonical(self) -> None:
        result = self.run_scav("fmt", "--check", *self.corpus())
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        self.assertEqual("", result.stdout)

    def test_check_never_writes(self) -> None:
        path = self.write("messy.scav", "chart c { s A, s B, }\n")
        before = path.read_bytes()
        result = self.run_scav("fmt", "--check", path)
        self.assertEqual(1, result.returncode)
        self.assertIn("not canonical", result.stderr)
        self.assertEqual(before, path.read_bytes())

    def test_check_names_every_file_that_fails(self) -> None:
        a = self.write("a.scav", "chart a { s A, }\n")
        b = self.write("b.scav", "chart b {\n  state B,\n}\n")
        c = self.write("c.scav", "chart c { m x { s C, }, }\n")
        result = self.run_scav("fmt", "--check", a, b, c)
        self.assertEqual(1, result.returncode)
        self.assertIn("a.scav: not canonical", result.stderr)
        self.assertIn("c.scav: not canonical", result.stderr)
        self.assertNotIn("b.scav: not canonical", result.stderr)

    # Rewriting =============================================================

    def test_fmt_rewrites_in_place_and_is_then_clean(self) -> None:
        path = self.write("rewrite.scav", "chart c { s A, s B, t A -> B, }\n")
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        self.assertEqual(
            "chart c {\n  state A,\n  state B,\n  trans A -> B,\n}\n",
            path.read_text(encoding="utf-8"),
        )
        self.assertEqual(0, self.run_scav("fmt", "--check", path).returncode)

    def test_a_second_run_changes_nothing(self) -> None:
        path = self.write("idempotent.scav", "chart c { s A { s B, }, @z, @a, }\n")
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        once = path.read_bytes()
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        self.assertEqual(once, path.read_bytes())

    def test_every_corpus_chart_is_its_own_fixed_point(self) -> None:
        # Copied out of the tree so a bug here cannot rewrite the corpus.
        for name in self.corpus():
            source = (self.cfg.repo_root / name).read_bytes()
            copy = self.scratch / "corpus" / Path(name).name
            copy.parent.mkdir(parents=True, exist_ok=True)
            copy.write_bytes(source)
            self.assertEqual(0, self.run_scav("fmt", copy).returncode, name)
            self.assertEqual(source, copy.read_bytes(), name)

    def test_a_crlf_file_is_not_canonical_and_is_rewritten_to_lf(self) -> None:
        # Line endings normalize at parse, so a CRLF file is non-canonical
        # however its statements are spelled.
        path = self.write("crlf.scav", "chart c {\r\n  state A,\r\n}\r\n")
        self.assertEqual(1, self.run_scav("fmt", "--check", path).returncode)
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        self.assertEqual(b"chart c {\n  state A,\n}\n", path.read_bytes())

    def test_a_byte_order_mark_is_stripped(self) -> None:
        path = self.write("bom.scav", "﻿chart c {\n  state A,\n}\n")
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        self.assertEqual(b"chart c {\n  state A,\n}\n", path.read_bytes())

    def test_comments_survive_a_rewrite(self) -> None:
        path = self.write(
            "comments.scav",
            "// header\n"
            "chart c { // opens\n"
            "  // about A\n"
            "  s A, // trailing A\n"
            "  // dangling\n"
            "}\n"
            "// afterword\n",
        )
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        text = path.read_text(encoding="utf-8")
        for comment in ("// header", "// opens", "// about A", "// trailing A",
                        "// dangling", "// afterword"):
            self.assertIn(comment, text)
        self.assertIn("state A, // trailing A", text)

    def test_blank_lines_between_statements_survive(self) -> None:
        path = self.write(
            "spaced.scav",
            "chart c {\n"
            "  state A,\n"
            "\n"
            "  state B,\n"
            "\n"
            "  // a heading\n"
            "\n"
            "  trans A -> B,\n"
            "}\n",
        )
        before = path.read_bytes()
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        self.assertEqual(before, path.read_bytes())

    def test_the_corpus_keeps_the_grouping_it_was_written_with(self) -> None:
        # The reason the bit exists: a chart of any size is unreadable once its
        # groups run together.
        for name in self.corpus():
            text = (self.cfg.repo_root / name).read_text(encoding="utf-8")
            if "\n\n" in text:
                return
        self.fail("no corpus chart has a blank line, so this asserts nothing")

    # Failure ===============================================================

    def test_a_parse_error_leaves_the_file_alone(self) -> None:
        # Printing a half-parsed document would write a file saying less than
        # the one on disk.
        path = self.write("broken.scav", "chart c { state , }\n")
        before = path.read_bytes()
        result = self.run_scav("fmt", path)
        self.assertEqual(2, result.returncode)
        self.assertIn(":1:", result.stderr)
        self.assertEqual(before, path.read_bytes())

    def test_one_bad_file_does_not_stop_the_others(self) -> None:
        good = self.write("good.scav", "chart g { s A, }\n")
        bad = self.write("bad.scav", "chart b { state , }\n")
        result = self.run_scav("fmt", good, bad)
        self.assertEqual(2, result.returncode)
        self.assertEqual(
            "chart g {\n  state A,\n}\n", good.read_text(encoding="utf-8")
        )

    def test_a_missing_file_is_an_error(self) -> None:
        result = self.run_scav("fmt", self.scratch / "nope.scav")
        self.assertEqual(2, result.returncode)
        self.assertIn("cannot read", result.stderr)

    def test_fmt_does_not_follow_includes(self) -> None:
        # Canonical form is a property of a document; a network's documents are
        # formatted by naming them.
        leaf = self.write("leaf.scav", "chart leaf { s L, }\n")
        root = self.write("root.scav", 'chart root { include "leaf.scav" as l, s R, }\n')
        self.assertEqual(0, self.run_scav("fmt", root).returncode)
        self.assertEqual("chart leaf { s L, }\n", leaf.read_text(encoding="utf-8"))

    def test_fmt_with_no_paths_is_a_usage_error(self) -> None:
        result = self.run_scav("fmt")
        self.assertEqual(2, result.returncode)
        self.assertIn("usage:", result.stderr)
        result = self.run_scav("fmt", "--check")
        self.assertEqual(2, result.returncode)

    def test_an_unknown_flag_is_a_usage_error(self) -> None:
        path = self.write("flagged.scav", "chart c {\n  state A,\n}\n")
        result = self.run_scav("fmt", "--verify", path)
        self.assertEqual(2, result.returncode)
        self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
