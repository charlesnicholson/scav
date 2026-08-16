#!/usr/bin/env python3
"""The load session driven from Python over ctypes, faking a network fetch, and
compared against the same network loaded by the CLI off disk. Same chart, same
hash.

The fake fetch names documents with URLs rather than paths. Nothing in core
interprets them, include paths resolve against them by the same byte-wise rule,
and the structural hash excludes document names -- so a URL-shaped network
produces the same digits as the same files on disk."""

import ctypes
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

ROOT = "vac.scav"
CHART_DIR = Path("test_data/charts")

SCAV_OK = 0
SCAV_E_CAPACITY = -3
SCAV_E_LOAD = -4

# The document network, named as if it were served over HTTP. Only the basename
# is used to find the bytes; the rest exists to prove core does not care.
ORIGIN = "https://charts.example.invalid/machines/"


class Pending(ctypes.Structure):
    """scav_pending: 16 bytes, no padding."""
    _fields_ = [
        ("path_off", ctypes.c_uint32),
        ("path_len", ctypes.c_uint32),
        ("from_doc", ctypes.c_uint32),
        ("stmt_row", ctypes.c_uint32),
    ]


def bind(lib: ctypes.CDLL) -> None:
    """Argument and return types, spelled out. ctypes defaults every return to
    int, which silently truncates a pointer on LP64 -- so a binding that skips
    this appears to work until the first handle lands above 4 GiB."""
    p = ctypes.POINTER
    u32 = ctypes.c_uint32
    byte_p = p(ctypes.c_ubyte)

    lib.scav_abi_version.restype = u32
    lib.scav_abi_version.argtypes = []

    lib.scav_load_begin.restype = ctypes.c_int32
    lib.scav_load_begin.argtypes = [p(ctypes.c_void_p)]

    lib.scav_load_add.restype = ctypes.c_int32
    lib.scav_load_add.argtypes = [ctypes.c_void_p, ctypes.c_char_p, u32, ctypes.c_char_p]

    lib.scav_load_pending.restype = ctypes.c_int32
    lib.scav_load_pending.argtypes = [ctypes.c_void_p, p(p(Pending)), p(u32)]

    lib.scav_load_path.restype = ctypes.c_int32
    lib.scav_load_path.argtypes = [ctypes.c_void_p, ctypes.c_uint64, p(byte_p), p(u32)]

    lib.scav_load_finish.restype = ctypes.c_int32
    lib.scav_load_finish.argtypes = [ctypes.c_void_p, p(ctypes.c_void_p)]

    lib.scav_load_destroy.restype = None
    lib.scav_load_destroy.argtypes = [ctypes.c_void_p]
    lib.scav_chart_destroy.restype = None
    lib.scav_chart_destroy.argtypes = [ctypes.c_void_p]

    lib.scav_load_diag_count.restype = ctypes.c_int32
    lib.scav_load_diag_count.argtypes = [ctypes.c_void_p, p(u32)]
    lib.scav_load_diag.restype = ctypes.c_int32
    lib.scav_load_diag.argtypes = [ctypes.c_void_p, u32, p(u32), p(u32), p(u32), p(u32)]
    lib.scav_diag_message.restype = ctypes.c_char_p
    lib.scav_diag_message.argtypes = [u32]

    lib.scav_chart_counts.restype = ctypes.c_int32
    lib.scav_chart_counts.argtypes = [ctypes.c_void_p] + [p(u32)] * 5
    lib.scav_chart_structural_hash.restype = ctypes.c_int32
    lib.scav_chart_structural_hash.argtypes = [ctypes.c_void_p, p(u32)]
    lib.scav_chart_digest.restype = ctypes.c_int32
    lib.scav_chart_digest.argtypes = [ctypes.c_void_p, byte_p, u32, p(u32)]


class Session:
    """A thin RAII wrapper, so a failing assertion cannot leak a handle."""

    def __init__(self, lib: ctypes.CDLL) -> None:
        self.lib = lib
        self.handle = ctypes.c_void_p()
        assert lib.scav_load_begin(ctypes.byref(self.handle)) == SCAV_OK
        self.chart = ctypes.c_void_p()

    def add(self, body: bytes, name: str) -> int:
        return self.lib.scav_load_add(self.handle, body, len(body), name.encode())

    def pending(self) -> list[tuple[str, int, int]]:
        rows = ctypes.POINTER(Pending)()
        count = ctypes.c_uint32(0)
        assert self.lib.scav_load_pending(
            self.handle, ctypes.byref(rows), ctypes.byref(count)) == SCAV_OK
        out = []
        for i in range(count.value):
            row = rows[i]
            # A span into the session's own pool; not NUL-terminated, so it is
            # read by offset and length like every other string here.
            span = row.path_off | (row.path_len << 32)
            data = ctypes.POINTER(ctypes.c_ubyte)()
            length = ctypes.c_uint32(0)
            assert self.lib.scav_load_path(
                self.handle, span, ctypes.byref(data), ctypes.byref(length)) == SCAV_OK
            text = bytes(bytearray(data[i] for i in range(length.value))).decode()
            out.append((text, row.from_doc, row.stmt_row))
        return out

    def finish(self) -> int:
        return self.lib.scav_load_finish(self.handle, ctypes.byref(self.chart))

    def diagnostics(self) -> list[str]:
        count = ctypes.c_uint32(0)
        assert self.lib.scav_load_diag_count(self.handle, ctypes.byref(count)) == SCAV_OK
        out = []
        for i in range(count.value):
            code, doc, off, length = (ctypes.c_uint32(0) for _ in range(4))
            assert self.lib.scav_load_diag(
                self.handle, i, ctypes.byref(code), ctypes.byref(doc),
                ctypes.byref(off), ctypes.byref(length)) == SCAV_OK
            out.append(self.lib.scav_diag_message(code.value).decode())
        return out

    def counts(self) -> dict[str, int]:
        fields = ["documents", "states", "submachines", "transitions", "includes"]
        values = [ctypes.c_uint32(0) for _ in fields]
        assert self.lib.scav_chart_counts(
            self.chart, *[ctypes.byref(v) for v in values]) == SCAV_OK
        return dict(zip(fields, [v.value for v in values]))

    def structural_hash(self) -> int:
        out = ctypes.c_uint32(0)
        assert self.lib.scav_chart_structural_hash(self.chart, ctypes.byref(out)) == SCAV_OK
        return out.value

    def digest(self) -> bytes:
        needed = ctypes.c_uint32(0)
        assert self.lib.scav_chart_digest(self.chart, None, 0, ctypes.byref(needed)) == SCAV_OK
        buf = (ctypes.c_ubyte * needed.value)()
        got = ctypes.c_uint32(0)
        assert self.lib.scav_chart_digest(
            self.chart,
            ctypes.cast(buf, ctypes.POINTER(ctypes.c_ubyte)),
            needed.value,
            ctypes.byref(got)) == SCAV_OK
        return bytes(bytearray(buf))

    def close(self) -> None:
        self.lib.scav_chart_destroy(self.chart)
        self.lib.scav_load_destroy(self.handle)
        self.chart = ctypes.c_void_p()
        self.handle = ctypes.c_void_p()


class TestLoadOverCtypes(unittest.TestCase):
    cfg: scavtest.Config
    lib: ctypes.CDLL
    corpus: dict[str, bytes]

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        if cls.cfg["sanitizer"].upper() not in ("", "NONE"):
            # A sanitized library dlopen'd into a clean host aborts, since its
            # interceptors must be installed before the process starts.
            raise unittest.SkipTest(
                f"ctypes cannot load a {cls.cfg['sanitizer']} build; "
                "c_api_tests covers these entry points under the sanitizer")
        cls.lib = ctypes.CDLL(str(cls.shared_library()))
        bind(cls.lib)
        cls.corpus = {
            p.name: p.read_bytes()
            for p in (cls.cfg.repo_root / CHART_DIR).glob("*.scav")
        }

    @classmethod
    def shared_library(cls) -> Path:
        names = ("libscav.dylib", "libscav.so", "scav.dll")
        for where in (cls.cfg.build_dir / "bin", cls.cfg.build_dir / "lib",
                      cls.cfg.build_dir / "src" / "abi"):
            for name in names:
                if (where / name).is_file():
                    return where / name
        found = sorted(p for name in names for p in cls.cfg.build_dir.rglob(name))
        if not found:
            raise AssertionError(f"no shared scav library under {cls.cfg.build_dir}")
        return found[0]

    def fetch(self, url: str) -> bytes:
        """The fake network. Nothing here is a filesystem call on core's behalf:
        the application decides what a name means, which is the whole point."""
        name = url.rsplit("/", 1)[-1]
        if name not in self.corpus:
            raise AssertionError(f"nothing serves {url}")
        return self.corpus[name]

    def load_network(self, origin: str, reverse: bool = False) -> Session:
        session = Session(self.lib)
        root = origin + ROOT
        self.assertEqual(SCAV_OK, session.add(self.fetch(root), root))
        for _ in range(32):
            wanted = [p[0] for p in session.pending()]
            if not wanted:
                break
            if reverse:
                wanted.reverse()
            for url in wanted:
                self.assertEqual(SCAV_OK, session.add(self.fetch(url), url))
        return session

    def cli_hash(self, chart: Path) -> str:
        exe = self.cfg.build_dir / "bin" / ("scav.exe" if sys.platform == "win32" else "scav")
        result = subprocess.run(
            [str(exe), "dump", "--hash", chart.as_posix()],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            cwd=self.cfg.repo_root)
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        return result.stdout.strip()

    def test_the_abi_version_is_checkable(self) -> None:
        self.assertNotEqual(0, self.lib.scav_abi_version())

    def test_a_network_loads_over_a_faked_fetch(self) -> None:
        session = self.load_network(ORIGIN)
        try:
            self.assertEqual(SCAV_OK, session.finish())
            counts = session.counts()
            # Three files, parsed once each; four include statements across the
            # network, so four instantiations.
            self.assertEqual(3, counts["documents"])
            self.assertEqual(3, counts["includes"])
            self.assertGreater(counts["states"], 10)
        finally:
            session.close()

    def test_pending_reports_resolved_urls_not_authored_text(self) -> None:
        session = Session(self.lib)
        try:
            root = ORIGIN + ROOT
            self.assertEqual(SCAV_OK, session.add(self.fetch(root), root))
            paths = [p[0] for p in session.pending()]
            # `dock.scav` and `./led.scav` as authored; resolved against the
            # root's origin, and the `./` folded away.
            self.assertIn(ORIGIN + "dock.scav", paths)
            self.assertIn(ORIGIN + "led.scav", paths)
            self.assertTrue(all(p[1] == 0 for p in session.pending()))
        finally:
            session.close()

    def test_all_three_transports_agree_on_the_hash(self) -> None:
        """The filesystem run went through `fopen` in the CLI; the ctypes run
        never touched a filesystem on core's behalf and named every document
        with a URL. Same network, same model, same digits."""
        from_files = self.cli_hash(CHART_DIR / ROOT)

        session = self.load_network(ORIGIN)
        try:
            self.assertEqual(SCAV_OK, session.finish())
            from_network = f"{session.structural_hash():08x}"
        finally:
            session.close()

        self.assertEqual(from_files, from_network)

    def test_arrival_order_does_not_change_the_model(self) -> None:
        """A DocId comes from the include graph, never from arrival order,
        which is what lets a host resolve a pending batch with Promise.all and
        still get a byte-identical chart."""
        forward = self.load_network(ORIGIN, reverse=False)
        backward = self.load_network(ORIGIN, reverse=True)
        try:
            self.assertEqual(SCAV_OK, forward.finish())
            self.assertEqual(SCAV_OK, backward.finish())
            self.assertEqual(forward.digest(), backward.digest())
            self.assertEqual(forward.structural_hash(), backward.structural_hash())
        finally:
            forward.close()
            backward.close()

    def test_the_document_name_is_not_part_of_the_model(self) -> None:
        # The same network under two different origins is the same model.
        first = self.load_network(ORIGIN)
        second = self.load_network("zip://bundle.zip!/deeply/nested/")
        try:
            self.assertEqual(SCAV_OK, first.finish())
            self.assertEqual(SCAV_OK, second.finish())
            self.assertEqual(first.digest(), second.digest())
        finally:
            first.close()
            second.close()

    def test_the_digest_honours_the_out_param_protocol(self) -> None:
        session = self.load_network(ORIGIN)
        try:
            self.assertEqual(SCAV_OK, session.finish())
            needed = ctypes.c_uint32(0)
            self.assertEqual(SCAV_OK, self.lib.scav_chart_digest(
                session.chart, None, 0, ctypes.byref(needed)))
            self.assertGreater(needed.value, 0)
            # Too small returns the required count and writes nothing.
            small = (ctypes.c_ubyte * 4)()
            got = ctypes.c_uint32(0)
            self.assertEqual(SCAV_E_CAPACITY, self.lib.scav_chart_digest(
                session.chart,
                ctypes.cast(small, ctypes.POINTER(ctypes.c_ubyte)),
                4, ctypes.byref(got)))
            self.assertEqual(needed.value, got.value)
            self.assertEqual(needed.value, len(session.digest()))
        finally:
            session.close()

    def test_a_cycle_is_reported_rather_than_followed(self) -> None:
        session = Session(self.lib)
        try:
            self.assertEqual(SCAV_OK, session.add(
                b'chart a { include "b.scav" as b, state A, }', "mem:///a.scav"))
            wanted = [p[0] for p in session.pending()]
            self.assertEqual(["mem:///b.scav"], wanted)
            self.assertEqual(SCAV_OK, session.add(
                b'chart b { state B, include "a.scav" as a, }', "mem:///b.scav"))
            self.assertEqual(SCAV_E_LOAD, session.finish())
            self.assertFalse(session.chart)
            self.assertIn("include cycle", session.diagnostics())
        finally:
            session.close()

    def test_a_document_that_never_arrives_is_reported(self) -> None:
        session = Session(self.lib)
        try:
            self.assertEqual(SCAV_OK, session.add(
                b'chart a { include "gone.scav" as g, state A, }', "mem:///a.scav"))
            self.assertEqual(SCAV_E_LOAD, session.finish())
            self.assertFalse(session.chart)
            self.assertIn("include path was never supplied to the load session",
                          session.diagnostics())
        finally:
            session.close()


if __name__ == "__main__":
    unittest.main()
