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

    def write(self, source: str, name: str = "") -> Path:
        """A chart on disk under out/, since `fmt` rewrites a file in place."""
        stem = name or self.id().rsplit(".", 1)[-1]
        path = self.scratch / f"{stem}.scav"
        path.parent.mkdir(parents=True, exist_ok=True)
        # Newline off, so a test writing CRLF gets CRLF on every platform.
        path.write_text(source, encoding="utf-8", newline="")
        return path

    def fmt(self, source: str, expected: str, name: str = "") -> Path:
        """Format `source` in place and compare the file against `expected`.

        Then the two properties every rule owes: the result is its own fixed
        point, and `--check` agrees that it is canonical.
        """
        path = self.write(source, name)
        result = self.run_scav("fmt", path)
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        self.assertEqual(expected, path.read_text(encoding="utf-8"))

        self.assertEqual(0, self.run_scav("fmt", "--check", path).returncode)
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        self.assertEqual(expected, path.read_text(encoding="utf-8"))
        return path

    def corpus(self) -> list[str]:
        root = self.cfg.repo_root / CHARTS
        return sorted((CHARTS / p.name).as_posix() for p in root.glob("*.scav"))

    # Rule 1 -- keyword spelling ============================================

    def test_rule_one_letter_aliases_grow_to_their_long_spelling(self) -> None:
        self.fmt(
            "chart c { s Idle, m x { s In, t * -> In, }, t Idle -> Idle, }\n",
            "chart c {\n"
            "  state Idle,\n"
            "  submachine x { state In, trans * -> In },\n"
            "  trans Idle -> Idle,\n"
            "}\n",
        )

    def test_rule_an_alias_is_still_a_legal_name(self) -> None:
        self.fmt(
            "chart c { state s, state m, state t, }\n",
            "chart c {\n  state s,\n  state m,\n  state t,\n}\n",
        )

    # Rule 2 -- repeated key becomes a list =================================

    def test_rule_a_key_written_twice_becomes_one_list(self) -> None:
        self.fmt(
            'chart c { @k = "b", @k = "a", }\n',
            'chart c {\n  @k = ["b", "a"],\n}\n',
        )

    def test_rule_a_one_element_list_collapses_to_a_scalar(self) -> None:
        self.fmt(
            'chart c { @k = ["only"], }\n',
            'chart c {\n  @k = "only",\n}\n',
        )

    def test_rule_an_empty_list_survives_since_no_scalar_spells_it(self) -> None:
        self.fmt(
            "chart c { @k = [], }\n",
            "chart c {\n  @k = [],\n}\n",
        )

    def test_rule_a_list_and_a_scalar_under_one_key_concatenate(self) -> None:
        self.fmt(
            'chart c { @k = ["a", "b"], @k = "c", }\n',
            'chart c {\n  @k = ["a", "b", "c"],\n}\n',
        )

    # Rule 3 -- flag form ===================================================

    def test_rule_a_true_value_becomes_a_flag(self) -> None:
        self.fmt(
            'chart c { @a = "true", @b, }\n',
            "chart c {\n  @a,\n  @b,\n}\n",
        )

    def test_rule_only_the_exact_text_true_is_a_flag(self) -> None:
        self.fmt(
            'chart c { @a = "True", @b = "true ", @d = "false", }\n',
            'chart c {\n  @a = "True",\n  @b = "true ",\n  @d = "false",\n}\n',
        )

    def test_rule_two_flags_under_one_key_are_a_list_not_a_flag(self) -> None:
        # Rule two runs first, so the pair never reaches the flag test.
        self.fmt(
            "chart c { @k, @k, }\n",
            'chart c {\n  @k = ["true", "true"],\n}\n',
        )

    # Rule 4 -- namespace block form ========================================

    def test_rule_two_keys_sharing_a_namespace_take_the_block_spelling(self) -> None:
        self.fmt(
            'chart c { @ns:b = "2", @ns:a = "1", }\n',
            'chart c {\n  @ns { a = "1", b = "2" },\n}\n',
        )

    def test_rule_a_sole_key_in_a_namespace_unblocks(self) -> None:
        self.fmt(
            'chart c { @ns { only = "1" }, }\n',
            'chart c {\n  @ns:only = "1",\n}\n',
        )

    def test_rule_a_bare_key_never_joins_the_namespace_it_matches(self) -> None:
        self.fmt(
            'chart c { @ns = "bare", @ns:a = "1", @ns:b = "2", }\n',
            'chart c {\n  @ns = "bare",\n  @ns { a = "1", b = "2" },\n}\n',
        )

    def test_rule_a_block_and_a_colon_key_merge_into_one_block(self) -> None:
        self.fmt(
            'chart c { @ns { a }, @ns:b = "2", }\n',
            'chart c {\n  @ns { a, b = "2" },\n}\n',
        )

    def test_rule_a_namespace_with_no_keys_at_all_disappears(self) -> None:
        self.fmt(
            "chart c { @ns {}, state A, }\n",
            "chart c {\n  state A,\n}\n",
        )

    # Rule 5 -- trailing comma ==============================================

    def test_rule_a_trailing_comma_appears_iff_the_block_broke(self) -> None:
        self.fmt(
            "chart c { state A { state B, state C, }, }\n",
            # The chart breaks and takes commas; the state fits and takes none.
            "chart c {\n  state A { state B, state C },\n}\n",
        )

    # Rule 6 -- attribute order =============================================

    def test_rule_attributes_sort_by_key_bytes(self) -> None:
        self.fmt(
            "chart c { @zeta, @alpha, @Mid, }\n",
            "chart c {\n  @Mid,\n  @alpha,\n  @zeta,\n}\n",
        )

    def test_rule_attributes_rise_above_structure_which_keeps_its_order(self) -> None:
        self.fmt(
            "chart c { state Z, @b, state A, @a, state M, }\n",
            "chart c {\n"
            "  @a,\n"
            "  @b,\n"
            "  state Z,\n"
            "  state A,\n"
            "  state M,\n"
            "}\n",
        )

    def test_rule_structure_is_never_reordered(self) -> None:
        self.fmt(
            'chart c { state Z, trans Z -> A, state A, include "x" as ex, state M, }\n',
            "chart c {\n"
            "  state Z,\n"
            "  trans Z -> A,\n"
            "  state A,\n"
            '  include "x" as ex,\n'
            "  state M,\n"
            "}\n",
        )

    def test_rule_sorting_is_by_bytes_so_a_namespace_follows_its_bare_key(self) -> None:
        # ':' is 0x3A, below every letter, so "ns" < "ns:a" < "nsx".
        self.fmt(
            "chart c { @ns:a, @nsx, @ns, }\n",
            "chart c {\n  @ns,\n  @ns:a,\n  @nsx,\n}\n",
        )

    def test_rule_an_attribute_belongs_to_the_block_it_was_written_in(self) -> None:
        self.fmt(
            "chart c { @top, state A { @inner, state B, }, }\n",
            "chart c {\n"
            "  @top,\n"
            "  state A { @inner, state B },\n"
            "}\n",
        )

    # Rule 7 -- line breaking ===============================================

    def test_rule_a_block_within_the_budget_stays_on_one_line(self) -> None:
        self.fmt(
            "chart c {\n  state A {\n    state B,\n  },\n}\n",
            "chart c {\n  state A { state B },\n}\n",
        )

    def test_rule_a_block_over_the_budget_breaks(self) -> None:
        wide = ", ".join(f"state Aaaaaaaaaa{i}" for i in range(6))
        self.fmt(
            "chart c { state Outer { " + wide + ", }, }\n",
            "chart c {\n"
            "  state Outer {\n"
            + "".join(f"    state Aaaaaaaaaa{i},\n" for i in range(6))
            + "  },\n"
            "}\n",
        )

    def test_rule_the_chart_block_always_breaks(self) -> None:
        self.fmt("chart c { state A, }\n", "chart c {\n  state A,\n}\n")
        self.fmt("chart empty {}\n", "chart empty {\n}\n", name="empty")

    def test_rule_a_list_too_wide_breaks_one_value_per_line(self) -> None:
        value = "v" * 40
        self.fmt(
            f'chart c {{ @ns:k = ["{value}", "{value}"], }}\n',
            "chart c {\n"
            "  @ns:k = [\n"
            f'    "{value}",\n'
            f'    "{value}",\n'
            "  ],\n"
            "}\n",
        )

    def test_rule_a_namespace_block_too_wide_breaks_one_entry_per_line(self) -> None:
        value = "v" * 40
        self.fmt(
            f'chart c {{ @ns {{ a = "{value}", b = "{value}" }}, }}\n',
            "chart c {\n"
            "  @ns {\n"
            f'    a = "{value}",\n'
            f'    b = "{value}",\n'
            "  },\n"
            "}\n",
        )

    def test_rule_the_budget_counts_the_at_sign_and_the_namespace(self) -> None:
        # The entry alone fits; `@ns:` in front of it does not, and measuring the
        # entry on its own emitted a line past the budget.
        value = "v" * 70
        self.fmt(
            f'chart c {{ @nsx:k = ["{value}", "bbb"], }}\n',
            "chart c {\n"
            "  @nsx:k = [\n"
            f'    "{value}",\n'
            '    "bbb",\n'
            "  ],\n"
            "}\n",
        )

    def test_rule_the_budget_counts_codepoints_rather_than_bytes(self) -> None:
        # Two bytes and one column each. The inner block is 79 codepoints and
        # 129 bytes, so it stays flat only if the budget counts what it should.
        label = "\u00e9" * 50
        self.fmt(
            f'chart c {{ state Outer {{ state A "{label}", }}, }}\n',
            f'chart c {{\n  state Outer {{ state A "{label}" }},\n}}\n',
        )

    # One spelling per thing ================================================

    def test_a_raw_string_comes_back_escaped(self) -> None:
        self.fmt(
            'chart c { state A """a"b\\c""", }\n',
            'chart c {\n  state A "a\\"b\\\\c",\n}\n',
        )

    def test_every_escape_takes_its_shortest_form(self) -> None:
        self.fmt(
            'chart c { @k = "a\\nb\\tc\\"d\\\\e\\u0001f", }\n',
            'chart c {\n  @k = "a\\nb\\tc\\"d\\\\e\\u0001f",\n}\n',
        )

    def test_an_empty_state_block_disappears_and_a_submachine_keeps_its_own(
        self,
    ) -> None:
        self.fmt(
            "chart c { state A {}, trans A -> A {}, state B { submachine m {},"
            " submachine n {}, }, }\n",
            "chart c {\n"
            "  state A,\n"
            "  trans A -> A,\n"
            "  state B { submachine m {}, submachine n {} },\n"
            "}\n",
        )

    def test_a_sole_anonymous_submachine_is_the_implicit_one_written_out(self) -> None:
        self.fmt(
            "chart c { state A { submachine { state B, trans * -> B, }, }, }\n",
            "chart c {\n  state A { state B, trans * -> B },\n}\n",
        )

    def test_a_named_labelled_or_second_submachine_stays_explicit(self) -> None:
        self.fmt(
            "chart c {\n"
            "  state A { submachine m { state B, }, },\n"
            '  state B { submachine "why" { state C, }, },\n'
            "  state C { submachine { state D, }, submachine n { state E, }, },\n"
            "}\n",
            "chart c {\n"
            "  state A { submachine m { state B } },\n"
            '  state B { submachine "why" { state C } },\n'
            "  state C { submachine { state D }, submachine n { state E } },\n"
            "}\n",
        )

    def test_a_kind_is_written_only_when_it_is_not_the_default(self) -> None:
        self.fmt(
            "chart c {\n"
            "  state A normal,\n"
            "  state B choice,\n"
            "  state C deephistory,\n"
            "  trans external A -> B,\n"
            "  trans internal A -> B,\n"
            "  trans local A -> B,\n"
            "}\n",
            "chart c {\n"
            "  state A,\n"
            "  state B choice,\n"
            "  state C deephistory,\n"
            "  trans A -> B,\n"
            "  trans internal A -> B,\n"
            "  trans local A -> B,\n"
            "}\n",
        )

    def test_every_endpoint_spelling_survives_verbatim(self) -> None:
        self.fmt(
            "chart c {\n"
            "  trans * -> A,\n"
            "  trans A -> *,\n"
            "  trans On:main/Idle -> On:1/Ready,\n"
            "  trans dock/On/Seated -> A,\n"
            "}\n",
            "chart c {\n"
            "  trans * -> A,\n"
            "  trans A -> *,\n"
            "  trans On:main/Idle -> On:1/Ready,\n"
            "  trans dock/On/Seated -> A,\n"
            "}\n",
        )

    # Normalization =========================================================

    def test_crlf_becomes_lf(self) -> None:
        path = self.write("chart c {\r\n  state A,\r\n}\r\n")
        self.assertEqual(1, self.run_scav("fmt", "--check", path).returncode)
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        self.assertEqual(b"chart c {\n  state A,\n}\n", path.read_bytes())

    def test_a_byte_order_mark_is_stripped(self) -> None:
        path = self.write("﻿chart c {\n  state A,\n}\n")
        self.assertEqual(1, self.run_scav("fmt", "--check", path).returncode)
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        self.assertEqual(b"chart c {\n  state A,\n}\n", path.read_bytes())

    def test_decomposed_text_is_composed(self) -> None:
        # Spelled in escapes: a literal here would already be composed, and
        # then the case would assert nothing.
        decomposed = "cafe\u0301"
        composed = "caf\u00e9"
        self.assertNotEqual(decomposed, composed)
        self.fmt(
            f'chart c {{ state A "{decomposed}", }}\n',
            f'chart c {{\n  state A "{composed}",\n}}\n',
        )

    # Comments ==============================================================

    def test_every_comment_position_keeps_its_place(self) -> None:
        text = (
            "// before the chart\n"
            "chart c { // on the opening brace\n"
            "  // leading A\n"
            "  state A, // trailing A\n"
            "  state B,\n"
            "  // dangling at the end of the block\n"
            "}\n"
            "// after the closing brace\n"
        )
        self.fmt(text, text)

    def test_a_comment_keeps_a_subtree_broken(self) -> None:
        self.fmt(
            "chart c { state A { state B, // note\n state C, }, }\n",
            "chart c {\n"
            "  state A {\n"
            "    state B, // note\n"
            "    state C,\n"
            "  },\n"
            "}\n",
        )

    def test_a_comment_on_an_attribute_travels_with_it_when_it_sorts(self) -> None:
        self.fmt(
            "chart c {\n"
            "  // about zeta\n"
            "  @zeta, // trailing zeta\n"
            "  @alpha,\n"
            "}\n",
            "chart c {\n"
            "  @alpha,\n"
            "  // about zeta\n"
            "  @zeta, // trailing zeta\n"
            "}\n",
        )

    def test_merging_two_statements_keeps_both_their_comments(self) -> None:
        self.fmt(
            "chart c {\n"
            "  // first\n"
            '  @k = "a", // one\n'
            "  // second\n"
            '  @k = "b",\n'
            "}\n",
            "chart c {\n"
            "  // first\n"
            "  // one\n"
            "  // second\n"
            '  @k = ["a", "b"],\n'
            "}\n",
        )

    def test_trailing_blanks_inside_a_comment_do_not_reach_the_output(self) -> None:
        self.fmt(
            "chart c {\n  // padded   \n  state A,\n}\n",
            "chart c {\n  // padded\n  state A,\n}\n",
        )

    def test_a_comment_inside_an_otherwise_empty_block_keeps_the_block(self) -> None:
        self.fmt(
            "chart c { state A { // only a note\n }, }\n",
            "chart c {\n  state A { // only a note\n  },\n}\n",
        )

    def test_the_comments_of_a_dropped_attribute_rise_to_the_top(self) -> None:
        self.fmt(
            "chart c {\n  state A,\n  // orphaned\n  @ns {},\n}\n",
            "chart c {\n  // orphaned\n  state A,\n}\n",
        )

    # Blank lines ===========================================================

    def test_a_blank_line_between_statements_survives(self) -> None:
        text = "chart c {\n  state A,\n\n  state B,\n}\n"
        self.fmt(text, text)

    def test_a_run_of_blank_lines_collapses_to_one(self) -> None:
        self.fmt(
            "chart c {\n  state A,\n\n\n\n  state B,\n}\n",
            "chart c {\n  state A,\n\n  state B,\n}\n",
        )

    def test_a_blank_opening_or_closing_a_block_is_dropped(self) -> None:
        self.fmt(
            "chart c {\n\n  state A,\n\n  state B,\n\n}\n",
            "chart c {\n  state A,\n\n  state B,\n}\n",
        )

    def test_a_blank_stays_above_a_heading_comment_and_below_it(self) -> None:
        # Two ways to write a gap around a heading, each keeping its own shape.
        text = (
            "chart c {\n"
            "  state A, // trailing\n"
            "\n"
            "  // a heading\n"
            "\n"
            "  state B,\n"
            "}\n"
        )
        self.fmt(text, text)

    # The gate ==============================================================

    def test_the_committed_corpus_is_canonical(self) -> None:
        result = self.run_scav("fmt", "--check", *self.corpus())
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        self.assertEqual("", result.stdout)

    def test_formatting_does_not_move_the_structural_hash(self) -> None:
        """Canonical form is a spelling, not a model change.

        The attribute sort reorders rows the digest walks, so the digest sorts
        them too; without that, running the gate would change what `dump --hash`
        reports and two copies of one chart would stop comparing.
        """
        # The whole directory, so an included document still resolves beside its
        # root, and the network is three documents rather than one.
        network = scavtest.fresh_dir(self.scratch / "network")
        for chart in (self.cfg.repo_root / CHARTS).glob("*.scav"):
            (network / chart.name).write_bytes(chart.read_bytes())

        # Undo the one thing the sort moved, so the comparison is not a no-op.
        root = network / "vac.scav"
        canonical = root.read_text(encoding="utf-8")
        authored = canonical.replace(
            '@nav { follow_walls = "false", uses_lidar }',
            '@nav { uses_lidar, follow_walls = "false" }',
        )
        self.assertNotEqual(canonical, authored, "corpus no longer has the case")
        root.write_text(authored, encoding="utf-8", newline="")

        before = self.run_scav("dump", "--hash", root)
        self.assertEqual(0, before.returncode, before.stderr)
        self.assertEqual(0, self.run_scav("fmt", root).returncode)
        self.assertEqual(canonical, root.read_text(encoding="utf-8"))

        after = self.run_scav("dump", "--hash", root)
        self.assertEqual(0, after.returncode, after.stderr)
        self.assertEqual(before.stdout, after.stdout)

    def test_the_hash_survives_an_attribute_reordering(self) -> None:
        # The same property on one file, where the sort is the only difference.
        source = 'chart c {\n  @zeta = "1",\n  @alpha = "2",\n  state A,\n}\n'
        path = self.write(source)
        before = self.run_scav("dump", "--hash", path)
        self.assertEqual(0, before.returncode)
        self.assertEqual(0, self.run_scav("fmt", path).returncode)
        self.assertIn("@alpha", path.read_text(encoding="utf-8").split("\n")[1])
        self.assertEqual(before.stdout, self.run_scav("dump", "--hash", path).stdout)

    def test_every_corpus_chart_is_its_own_fixed_point(self) -> None:
        # Copied out of the tree so a bug here cannot rewrite the corpus.
        for name in self.corpus():
            source = (self.cfg.repo_root / name).read_bytes()
            copy = self.scratch / "corpus" / Path(name).name
            copy.parent.mkdir(parents=True, exist_ok=True)
            copy.write_bytes(source)
            self.assertEqual(0, self.run_scav("fmt", copy).returncode, name)
            self.assertEqual(source, copy.read_bytes(), name)

    def test_the_corpus_keeps_the_grouping_it_was_written_with(self) -> None:
        for name in self.corpus():
            if "\n\n" in (self.cfg.repo_root / name).read_text(encoding="utf-8"):
                return
        self.fail("no corpus chart has a blank line, so this asserts nothing")

    def test_check_never_writes(self) -> None:
        path = self.write("chart c { s A, s B, }\n")
        before = path.read_bytes()
        result = self.run_scav("fmt", "--check", path)
        self.assertEqual(1, result.returncode)
        self.assertIn("not canonical", result.stderr)
        self.assertEqual(before, path.read_bytes())

    def test_check_names_every_file_that_fails(self) -> None:
        a = self.write("chart a { s A, }\n", name="loose_a")
        b = self.write("chart b {\n  state B,\n}\n", name="canonical_b")
        c = self.write("chart c { m x { s C, }, }\n", name="loose_c")
        result = self.run_scav("fmt", "--check", a, b, c)
        self.assertEqual(1, result.returncode)
        self.assertIn("loose_a.scav: not canonical", result.stderr)
        self.assertIn("loose_c.scav: not canonical", result.stderr)
        self.assertNotIn("canonical_b.scav: not canonical", result.stderr)

    # Failure ===============================================================

    def test_a_parse_error_leaves_the_file_alone(self) -> None:
        # Printing a half-parsed document would write a file saying less than
        # the one on disk.
        path = self.write("chart c { state , }\n")
        before = path.read_bytes()
        result = self.run_scav("fmt", path)
        self.assertEqual(2, result.returncode)
        self.assertIn(":1:", result.stderr)
        self.assertEqual(before, path.read_bytes())

    def test_one_bad_file_does_not_stop_the_others(self) -> None:
        good = self.write("chart g { s A, }\n", name="good")
        bad = self.write("chart b { state , }\n", name="bad")
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
        leaf = self.write("chart leaf { s L, }\n", name="leaf")
        root = self.write(
            'chart root { include "leaf.scav" as l, s R, }\n', name="root"
        )
        self.assertEqual(0, self.run_scav("fmt", root).returncode)
        self.assertEqual("chart leaf { s L, }\n", leaf.read_text(encoding="utf-8"))

    def test_fmt_with_no_paths_is_a_usage_error(self) -> None:
        self.assertEqual(2, self.run_scav("fmt").returncode)
        self.assertEqual(2, self.run_scav("fmt", "--check").returncode)

    def test_an_unknown_flag_is_a_usage_error(self) -> None:
        path = self.write("chart c {\n  state A,\n}\n")
        result = self.run_scav("fmt", "--verify", path)
        self.assertEqual(2, result.returncode)
        self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
