#!/usr/bin/env python3
"""Branch protection names one check, so that check has to depend on every job.

A job added without being wired into the anchor still runs and still reports, but
stops being able to block a merge -- a weakening nothing else would notice."""

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

ANCHOR = "all-checks-pass"
# Two spaces then a name then a colon, under `jobs:`. No YAML parser in the
# standard library, and a functional test that needs `pip install` is one more
# thing between a clean checkout and a green run.
JOB = re.compile(r"^  ([a-z][a-z0-9-]*):$")
NEEDS_ITEM = re.compile(r"^      - ([a-z][a-z0-9-]*)$")


class TestCiAnchor(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cfg = scavtest.load_config()
        cls.path = cfg.repo_root / ".github/workflows/ci.yml"
        text = cls.path.read_text(encoding="utf-8")
        body = text[text.index("\njobs:\n"):]
        cls.lines = body.splitlines()
        cls.jobs = [m[1] for line in cls.lines if (m := JOB.match(line))]

    def needs(self) -> list[str]:
        start = self.lines.index(f"  {ANCHOR}:")
        after = self.lines[start:]
        first = next(i for i, line in enumerate(after) if line.strip() == "needs:")
        found = []
        for line in after[first + 1:]:
            if not (m := NEEDS_ITEM.match(line)):
                break
            found.append(m[1])
        return found

    def test_the_anchor_exists(self) -> None:
        self.assertIn(ANCHOR, self.jobs, f"{self.path} declares no {ANCHOR} job")

    def test_the_anchor_needs_every_other_job(self) -> None:
        missing = sorted(set(self.jobs) - {ANCHOR} - set(self.needs()))
        self.assertEqual([], missing, f"{ANCHOR} does not depend on {missing}")

    def test_the_anchor_runs_even_when_a_job_fails(self) -> None:
        # Without `if: always()` the anchor is skipped rather than failed, and a
        # skipped required check does not block a merge.
        start = self.lines.index(f"  {ANCHOR}:")
        head = "\n".join(self.lines[start:start + 4])
        self.assertIn("if: always()", head)

    def test_the_anchor_fails_on_any_non_success(self) -> None:
        tail = "\n".join(self.lines[self.lines.index(f"  {ANCHOR}:"):])
        for result in ("failure", "cancelled", "skipped"):
            with self.subTest(result=result):
                self.assertIn(f"contains(needs.*.result, '{result}')", tail)


if __name__ == "__main__":
    unittest.main()
