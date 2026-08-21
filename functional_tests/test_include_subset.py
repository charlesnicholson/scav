#!/usr/bin/env python3
"""libscavlayout may reach a documented standard-library subset and nothing
else, so "bring your own compiler" cannot quietly mean "bring your own
conforming <algorithm>". Enforced from the day the directory exists -- the
check is green while src/layout/ is empty, and the first file lands under it.

Quoted includes are held to the -Isrc boundary at the same time: layout may
name its own headers, any library's public scav/ headers, and the src/-root
determinism primitives, but never another library's internals."""

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import scavtest  # noqa: E402

# Transcribed from the PRD on purpose: a test that derives its expectation from
# the thing under test is worth nothing.
ALLOWED_SYSTEM = {
    "cstdint", "bit", "limits", "vector", "array", "utility", "type_traits",
    "cstring",
}

# Own subsystem headers, public vocabulary, and the src/-root primitives that
# exist precisely to be the vetted channel for anything wider.
ALLOWED_QUOTED = re.compile(r"^(layout/|scav/scav_|scav_)[A-Za-z0-9_./]+\.h$")

INCLUDE = re.compile(r'^\s*#\s*include\s*(?:<([^>]+)>|"([^"]+)")')


class TestLayoutIncludeSubset(unittest.TestCase):
    def test_layout_sources_stay_inside_the_subset(self) -> None:
        cfg = scavtest.load_config()
        layout = cfg.repo_root / "src/layout"
        offences: list[str] = []
        for path in sorted(layout.rglob("*")) if layout.is_dir() else []:
            if path.suffix not in (".h", ".cpp"):
                continue
            for number, line in enumerate(
                    path.read_text(encoding="utf-8").splitlines(), start=1):
                if not (m := INCLUDE.match(line)):
                    continue
                where = f"{path.relative_to(layout.parent.parent)}:{number}"
                if (system := m.group(1)) is not None:
                    if system not in ALLOWED_SYSTEM:
                        offences.append(f"{where}: <{system}>")
                elif not ALLOWED_QUOTED.match(m.group(2)):
                    offences.append(f'{where}: "{m.group(2)}"')
        self.assertEqual([], offences,
                         "layout reached outside its standard-library subset "
                         "or into another library's internals")


if __name__ == "__main__":
    unittest.main()
