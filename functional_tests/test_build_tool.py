#!/usr/bin/env python3
"""Whether a failed build left binaries worth keeping.

Wrong one way leaves a stale executable after a compile error, answering for
code no longer in the tree. Wrong the other deletes the binaries a red test is
diagnosed and a golden regenerated with."""

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))

import build as build_tool  # noqa: E402

STAMP = ("[62/163] functional dump\n"
         "FAILED: [code=1] stamp/func.dump.passed "
         "/abs/out/preset/stamp/func.dump.passed\n"
         "cd /repo && cmake -E rm -f /abs/out/preset/stamp/func.dump.passed\n"
         "ninja: build stopped: subcommand failed.\n")

COMPILE = ("[67/163] Building CXX object src/layout/CMakeFiles/scavlayout.dir/"
           "size.cpp.o\n"
           "FAILED: src/layout/CMakeFiles/scavlayout.dir/size.cpp.o\n"
           "/usr/bin/clang++ -c size.cpp\n"
           "size.cpp:14:1: error: unknown type name 'oops'\n"
           "ninja: build stopped: subcommand failed.\n")

LINK = ("FAILED: bin/scav_layout_tests\n"
        "ld: symbol(s) not found\n"
        "ninja: build stopped: subcommand failed.\n")


class TestBuildIsStale(unittest.TestCase):
    def test_a_failing_test_leaves_current_binaries(self) -> None:
        self.assertFalse(build_tool.build_is_stale(STAMP))

    def test_a_compile_error_leaves_stale_binaries(self) -> None:
        self.assertTrue(build_tool.build_is_stale(COMPILE))

    def test_a_link_error_leaves_stale_binaries(self) -> None:
        self.assertTrue(build_tool.build_is_stale(LINK))

    def test_a_compile_error_beside_a_red_test_is_still_stale(self) -> None:
        """Ninja reports every edge it could not make. One broken object file
        is enough, whatever else also failed."""
        self.assertTrue(build_tool.build_is_stale(STAMP + COMPILE))

    def test_several_red_tests_are_still_not_stale(self) -> None:
        self.assertFalse(build_tool.build_is_stale(STAMP + STAMP))

    def test_a_failure_naming_nothing_assumes_the_worst(self) -> None:
        """A shape this parser does not recognise must not be read as good
        news: the binaries go, and the next build spends seconds relinking."""
        self.assertTrue(build_tool.build_is_stale("ninja: build stopped.\n"))
        self.assertTrue(build_tool.build_is_stale(""))


if __name__ == "__main__":
    unittest.main()
