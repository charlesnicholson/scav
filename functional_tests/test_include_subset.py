#!/usr/bin/env python3
"""libscavlayout may reach a documented standard-library subset and nothing
else, so "bring your own compiler" cannot quietly mean "bring your own
conforming <algorithm>". Enforced from the day the directory exists -- the
check is green while src/layout/ is empty, and the first file lands under it.

Quoted includes are held to the -Isrc boundary at the same time: layout may
name its own headers, any library's public scav/ headers, and the src/-root
determinism primitives, but never another library's internals.

libscavdraw carries the same -Isrc boundary but not the standard-library
subset: it holds a string pool and a builder, so it needs more than eight
headers. What it does inherit is the float ban, because the metrics helper is
determinism-critical -- an advance computed in double would differ across
libm the way every integer path here is built to avoid."""

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import scavtest  # noqa: E402

# Transcribed on purpose: a test that derives its expectation from
# the thing under test is worth nothing.
ALLOWED_SYSTEM = {
    "cstdint", "bit", "limits", "vector", "array", "utility", "type_traits",
    "cstring",
    # The C spelling of cstdint, for the header that must compile as C.
    "stdint.h",
}

# Own subsystem headers, public vocabulary, and the src/-root primitives that
# exist precisely to be the vetted channel for anything wider.
ALLOWED_QUOTED = re.compile(r"^(layout/|scav/scav_|scav_)[A-Za-z0-9_./]+\.h$")

INCLUDE = re.compile(r'^\s*#\s*include\s*(?:<([^>]+)>|"([^"]+)")')

# Draw's own headers, the public vocabulary, and the src/-root primitives.
ALLOWED_QUOTED_DRAW = re.compile(r"^(draw/|scav/scav_|scav_)[A-Za-z0-9_./]+\.h$")

# libm is the divergence; the floating types are how it gets reached.
BANNED_IN_DRAW = {"cmath", "math.h", "complex", "numbers", "random"}
FLOAT_TOKEN = re.compile(r"\b(float|double|long\s+double)\b")


def strip_comments(line: str, in_block: bool) -> tuple[str, bool]:
    """One line's code, with both comment forms removed. `in_block` carries a
    `/*` that has not been closed yet."""
    out = ""
    i = 0
    while i < len(line):
        if in_block:
            if line.startswith("*/", i):
                in_block = False
                i += 2
            else:
                i += 1
            continue
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            in_block = True
            i += 2
            continue
        out += line[i]
        i += 1
    return out, in_block


class TestLayoutIncludeSubset(unittest.TestCase):
    def test_layout_sources_stay_inside_the_subset(self) -> None:
        cfg = scavtest.load_config()
        layout = cfg.repo_root / "src/layout"
        sources = sorted(layout.rglob("*")) if layout.is_dir() else []
        offences: list[str] = []
        for path in sources:
            if path.suffix not in (".h", ".cpp") or path.stem.endswith("_tests"):
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



class TestDrawIncludeBoundary(unittest.TestCase):
    def sources(self) -> list[Path]:
        draw = scavtest.load_config().repo_root / "src/draw"
        return [p for p in sorted(draw.rglob("*"))
                if p.suffix in (".h", ".cpp") and not p.stem.endswith("_tests")]

    def test_draw_names_no_other_library_internals(self) -> None:
        offences: list[str] = []
        for path in self.sources():
            for number, line in enumerate(
                    path.read_text(encoding="utf-8").splitlines(), start=1):
                if not (m := INCLUDE.match(line)):
                    continue
                if (quoted := m.group(2)) is None:
                    continue
                if not ALLOWED_QUOTED_DRAW.match(quoted):
                    offences.append(f"{path.name}:{number}: \"{quoted}\"")
        self.assertEqual([], offences,
                         "draw reached into another library's internals")

    def test_no_float_reaches_a_measurement(self) -> None:
        """A float in the metrics helper would put libm's disagreements straight
        into the space tables, and from there into every golden."""
        offences: list[str] = []
        for path in self.sources():
            # Generated by scav_embed_bytes, and nothing but hex bytes.
            if path.name.startswith("scav_embed_"):
                continue
            in_block = False
            for number, line in enumerate(
                    path.read_text(encoding="utf-8").splitlines(), start=1):
                # Prose is allowed to say "float"; code is not. Both comment
                # forms come out, block comments across lines included.
                code, in_block = strip_comments(line, in_block)
                if (m := INCLUDE.match(line)) and (m.group(1) in BANNED_IN_DRAW):
                    offences.append(f"{path.name}:{number}: <{m.group(1)}>")
                elif FLOAT_TOKEN.search(code):
                    offences.append(f"{path.name}:{number}: {code.strip()}")
        self.assertEqual([], offences, "a floating-point type reached libscavdraw")


if __name__ == "__main__":
    unittest.main()
