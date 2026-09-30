#!/usr/bin/env python3
"""Generate the bundled font's metrics table from its TTF.

    src/draw/gen_font_table.py            writes bundled_font_table.inc beside itself
    src/draw/gen_font_table.py --check    exit 1 when the committed file differs

Reads what metrics.cpp reads: head.unitsPerEm, maxp.numGlyphs,
hhea.numberOfHMetrics, hmtx advances, and the cmap subtable metrics.cpp picks.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
TTF = REPO_ROOT / "assets/font/JetBrainsMono-Regular.ttf"
OUT = HERE / "bundled_font_table.inc"

MASK = 0xFFFFFFFF
P1, P2, P3, P4, P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393


def rotl(x: int, r: int) -> int:
    return ((x << r) | (x >> (32 - r))) & MASK


def xxhash32(data: bytes, seed: int = 0) -> int:
    n = len(data)
    at = 0
    if n >= 16:
        v = [(seed + P1 + P2) & MASK, (seed + P2) & MASK, seed & MASK, (seed - P1) & MASK]
        while at + 16 <= n:
            for i in range(4):
                lane = struct.unpack_from("<I", data, at + 4 * i)[0]
                v[i] = (rotl((v[i] + lane * P2) & MASK, 13) * P1) & MASK
            at += 16
        h = (rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18)) & MASK
    else:
        h = (seed + P5) & MASK
    h = (h + n) & MASK
    while at + 4 <= n:
        h = (rotl((h + struct.unpack_from("<I", data, at)[0] * P3) & MASK, 17) * P4) & MASK
        at += 4
    while at < n:
        h = (rotl((h + data[at] * P5) & MASK, 11) * P1) & MASK
        at += 1
    h ^= h >> 15
    h = (h * P2) & MASK
    h ^= h >> 13
    h = (h * P3) & MASK
    h ^= h >> 16
    return h


def tables(ttf: bytes) -> dict[str, tuple[int, int]]:
    count = struct.unpack_from(">H", ttf, 4)[0]
    out = {}
    for i in range(count):
        tag, _, off, length = struct.unpack_from(">4sIII", ttf, 12 + 16 * i)
        out[tag.decode("latin-1")] = (off, length)
    return out


def cmap_rank(platform: int, encoding: int, fmt: int) -> int:
    """metrics.cpp's cmap_rank."""
    if fmt == 12:
        if (platform, encoding) == (0, 4):
            return 4
        if (platform, encoding) == (3, 10):
            return 3
    if fmt == 4:
        if (platform, encoding) == (0, 3):
            return 2
        if (platform, encoding) == (3, 1):
            return 1
    return 0


def cmap(ttf: bytes, off: int) -> dict[int, int]:
    """Codepoint to glyph through the subtable metrics.cpp picks."""
    best, pick = 0, None
    for i in range(struct.unpack_from(">H", ttf, off + 2)[0]):
        platform, encoding, rel = struct.unpack_from(">HHI", ttf, off + 4 + 8 * i)
        fmt = struct.unpack_from(">H", ttf, off + rel)[0]
        if (rank := cmap_rank(platform, encoding, fmt)) > best:
            best, pick = rank, (off + rel, fmt)
    if pick is None:
        raise SystemExit("no usable cmap subtable")
    sub, fmt = pick
    out: dict[int, int] = {}
    if fmt == 12:
        for i in range(struct.unpack_from(">I", ttf, sub + 12)[0]):
            first, last, glyph = struct.unpack_from(">III", ttf, sub + 16 + 12 * i)
            for cp in range(first, last + 1):
                out[cp] = glyph + (cp - first)
        return out
    seg_x2 = struct.unpack_from(">H", ttf, sub + 6)[0]
    ends = sub + 14
    starts = ends + seg_x2 + 2
    deltas = starts + seg_x2
    ranges = deltas + seg_x2
    for i in range(seg_x2 // 2):
        end = struct.unpack_from(">H", ttf, ends + 2 * i)[0]
        start = struct.unpack_from(">H", ttf, starts + 2 * i)[0]
        delta = struct.unpack_from(">h", ttf, deltas + 2 * i)[0]
        rng = struct.unpack_from(">H", ttf, ranges + 2 * i)[0]
        for cp in range(start, end + 1):
            if rng == 0:
                glyph = (cp + delta) & 0xFFFF
            else:
                glyph = struct.unpack_from(">H", ttf, ranges + 2 * i + rng + 2 * (cp - start))[0]
                glyph = ((glyph + delta) & 0xFFFF) if glyph else 0
            if glyph:
                out[cp] = glyph
    return out


def emit(name: str, ctype: str, values: list[str], per_line: int) -> str:
    lines = ["  " + " ".join(v + "," for v in values[i:i + per_line])
             for i in range(0, len(values), per_line)]
    return f"constexpr std::array<{ctype}, {len(values)}> {name}{{\n" + "\n".join(lines) + "\n};\n"


def build(ttf: bytes) -> str:
    t = tables(ttf)
    upem = struct.unpack_from(">H", ttf, t["head"][0] + 18)[0]
    glyphs = struct.unpack_from(">H", ttf, t["maxp"][0] + 4)[0]
    h_metrics = struct.unpack_from(">H", ttf, t["hhea"][0] + 34)[0]
    hmtx = t["hmtx"][0]
    advances = [struct.unpack_from(">H", ttf, hmtx + 4 * g)[0] for g in range(h_metrics)]

    mapped = {cp: g for cp, g in cmap(ttf, t["cmap"][0]).items() if 0 < g < glyphs}
    runs: list[list[int]] = []  # [first codepoint, count, index of its first glyph]
    order = sorted(mapped)
    for cp in order:
        if runs and runs[-1][0] + runs[-1][1] == cp:
            runs[-1][1] += 1
        else:
            runs.append([cp, 1, 0])
    at = 0
    for run in runs:
        run[2] = at
        at += run[1]

    steps = [(g, a) for g, a in enumerate(advances) if g == 0 or advances[g - 1] != a]

    out = [f"// GENERATED by gen_font_table.py from assets/font/{TTF.name}. Do not edit.\n\n"]
    out.append(f"constexpr uint32_t BUNDLED_IDENTITY{{ 0x{xxhash32(ttf):08X}U }};  // xxh32 of the TTF\n")
    out.append(f"constexpr uint32_t BUNDLED_TTF_SIZE{{ {len(ttf)}U }};\n")
    out.append(f"constexpr uint32_t BUNDLED_UNITS_PER_EM{{ {upem}U }};\n")
    out.append(f"constexpr uint32_t BUNDLED_NUM_GLYPHS{{ {glyphs}U }};\n")
    out.append(f"constexpr uint32_t BUNDLED_NUM_H_METRICS{{ {h_metrics}U }};\n\n")
    out.append("// Runs of consecutive mapped codepoints: first, count, and the index of\n"
               "// the first one's glyph in BUNDLED_GLYPHS.\n")
    out.append(emit("BUNDLED_RUN_FIRST", "uint32_t", [f"0x{r[0]:X}" for r in runs], 8))
    out.append(emit("BUNDLED_RUN_COUNT", "uint16_t", [str(r[1]) for r in runs], 12))
    out.append(emit("BUNDLED_RUN_INDEX", "uint16_t", [str(r[2]) for r in runs], 12))
    out.append(emit("BUNDLED_GLYPHS", "uint16_t", [str(mapped[cp]) for cp in order], 12))
    out.append("// hmtx advances as steps: each holds from its glyph up to the next step's.\n")
    out.append(emit("BUNDLED_STEP_GLYPH", "uint16_t", [str(g) for g, _ in steps], 12))
    out.append(emit("BUNDLED_STEP_ADVANCE", "uint16_t", [str(a) for _, a in steps], 12))
    return "".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--out", type=Path, default=OUT)
    args = ap.parse_args()
    text = build(TTF.read_bytes())
    if args.check:
        if args.out.read_text(encoding="utf-8") != text:
            print(f"{args.out} is stale; rerun {Path(__file__).name}", file=sys.stderr)
            return 1
        return 0
    args.out.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {args.out} ({args.out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
