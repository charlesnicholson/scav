#!/usr/bin/env python3
"""scav_inflate_driver against Python's zlib: raw deflate at every level and strategy,
gzip members, and exact output bounds."""

import gzip
import io
import os
import random
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

FONT = Path("assets/font/JetBrainsMono-Regular.ttf")

STRATEGIES: dict[str, int] = {
    "default": zlib.Z_DEFAULT_STRATEGY,
    "filtered": zlib.Z_FILTERED,
    "huffman": zlib.Z_HUFFMAN_ONLY,
    "rle": zlib.Z_RLE,
    "fixed": zlib.Z_FIXED,
}

# InflateStatus, as scav_inflate.h numbers it.
OK = 0
OUTPUT_FULL = 2


def inputs(repo: Path) -> dict[str, bytes]:
    rng = random.Random(1951)
    noise = rng.randbytes(32500)
    words = [b"state", b"trans", b"chart", b"region", b"entry", b"exit", b"Idle", b"->",
             b"{", b"}", b",", b"\n", b"history", b"guard", b"[ready]", b"/ act()"]
    text = b" ".join(rng.choice(words) for _ in range(40_000))
    return {
        "font": (repo / FONT).read_bytes(),
        "empty": b"",
        "one": b"\x5a",
        "bytes256": bytes(range(256)),
        # Runs past 258 and distance-1 copies that overlap their own output.
        "runs": b"a" * 70_000 + b"ab" * 5_000 + bytes([0]) * 1_000,
        # Incompressible: stored or near-stored blocks, several past 65535 bytes.
        "random": rng.randbytes(150_000),
        # zlib's farthest reach, 32506 back, in 258-byte matches.
        "far": noise + noise[:4_000],
        # Text and noise interleaved, so the encoder cuts many blocks of each kind.
        "mixed": b"".join(text[i : i + 20_000] + rng.randbytes(3_000)
                          for i in range(0, len(text), 20_000)),
    }


def deflate(data: bytes, level: int, strategy: int) -> bytes:
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
    return c.compress(data) + c.flush()


class TestInflate(unittest.TestCase):
    cfg: scavtest.Config
    exe: Path
    scratch: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        name = "scav_inflate_driver.exe" if os.name == "nt" else "scav_inflate_driver"
        cls.exe = cls.cfg.build_dir / "bin" / name
        cls.scratch = scavtest.fresh_dir(cls.cfg.scratch_dir / "inflate")

    def decode(self, cases: list[tuple[str, str, bytes, int]]) -> list[tuple[int, bytes]]:
        """(name, mode, stream, cap) through the driver in one process."""
        lines = []
        for i, (_, mode, stream, cap) in enumerate(cases):
            src = self.scratch / f"{i}.in"
            src.write_bytes(stream)
            lines.append(f"{mode}\t{cap}\t{src}\t{self.scratch / f'{i}.out'}\n")
        manifest = self.scratch / "manifest.txt"
        manifest.write_text("".join(lines), encoding="utf-8")
        result = scavtest.run([self.exe, manifest])
        self.assertEqual(0, result.returncode, result.stdout)
        rows = result.stdout.split("\n")[: len(cases)]
        out = []
        for i, row in enumerate(rows):
            status, count = (int(x) for x in row.split())
            written = (self.scratch / f"{i}.out").read_bytes()
            self.assertEqual(count, len(written))
            out.append((status, written))
        for i in range(len(cases)):  # tens of megabytes otherwise
            (self.scratch / f"{i}.in").unlink()
            (self.scratch / f"{i}.out").unlink(missing_ok=True)
        return out

    def check(self, cases: list[tuple[str, str, bytes, int]], plains: list[bytes]) -> None:
        for (name, _, _, _), (status, written), plain in zip(
                cases, self.decode(cases), plains, strict=True):
            with self.subTest(name):
                self.assertEqual(OK, status)
                self.assertTrue(written == plain, f"{name}: output differs")

    def test_every_level_and_strategy(self) -> None:
        cases = []
        plains = []
        for name, data in inputs(self.cfg.repo_root).items():
            for level in range(10):
                for sname, strategy in STRATEGIES.items():
                    stream = deflate(data, level, strategy)
                    cases.append((f"{name}/{level}/{sname}", "raw", stream, len(data)))
                    plains.append(data)
        self.check(cases, plains)

    def test_gzip_members(self) -> None:
        cases = []
        plains = []
        for name, data in inputs(self.cfg.repo_root).items():
            cases.append((f"{name}/bare", "gzip", gzip.compress(data, 9, mtime=0), len(data)))
            plains.append(data)
            buf = io.BytesIO()
            with gzip.GzipFile(filename="named.bin", mode="wb", fileobj=buf, mtime=0) as f:
                f.write(data)
            cases.append((f"{name}/fname", "gzip", buf.getvalue(), len(data)))
            plains.append(data)
        self.check(cases, plains)

    def test_an_exact_bound_fits_and_one_less_does_not(self) -> None:
        cases = []
        for name, data in inputs(self.cfg.repo_root).items():
            if not data:
                continue
            for sname in ("default", "fixed"):
                stream = deflate(data, 9, STRATEGIES[sname])
                cases.append((f"{name}/{sname}/exact", "raw", stream, len(data)))
                cases.append((f"{name}/{sname}/short", "raw", stream, len(data) - 1))
            stored = deflate(data, 0, zlib.Z_DEFAULT_STRATEGY)
            cases.append((f"{name}/stored/short", "raw", stored, len(data) - 1))
        for (name, _, _, cap), (status, written) in zip(cases, self.decode(cases), strict=True):
            with self.subTest(name):
                want = OK if name.endswith("/exact") else OUTPUT_FULL
                self.assertEqual(want, status)
                self.assertLessEqual(len(written), cap)


if __name__ == "__main__":
    unittest.main()
