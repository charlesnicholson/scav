#!/usr/bin/env python3
"""A Python caller runs layout through the shared library and reads geometry
columns back through the three-call accessor."""

import ctypes
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "bindings/python"))

import scavtest  # noqa: E402
from scav._abi import scav_layout_opts, scav_placed, scav_spaces  # noqa: E402

CHART = Path("test_data/charts/brew.scav")

SCAV_OK = 0
SCAV_E_INVALID_ARG = -1
SCAV_E_ABI = -9
COORD_MAX = (1 << 19) - 1

SPACES_SIZE = ctypes.sizeof(scav_spaces)
PLACED_SIZE = ctypes.sizeof(scav_placed)


class Rect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32),
                ("w", ctypes.c_int32), ("h", ctypes.c_int32)]


class ShortLayoutOpts(ctypes.Structure):
    """scav_layout_opts as a caller one profile knob behind would lay it out.

    Hand-rolled on purpose: this is the shape of the incident. The real
    struct's profile is 48 int32, and a copy of it one knob short was written
    past by scav_profile_named and read past by scav_layout_run, which is how
    `router` came back as garbage on every runner but one.
    """
    _fields_ = [("profile", ctypes.c_int32 * 47),
                ("router", ctypes.c_uint32),
                ("threads", ctypes.c_uint32)]


def bind(lib: ctypes.CDLL) -> None:
    p = ctypes.POINTER
    u32 = ctypes.c_uint32
    byte_p = p(ctypes.c_ubyte)
    lib.scav_load_begin.restype = ctypes.c_int32
    lib.scav_load_begin.argtypes = [p(ctypes.c_void_p)]
    lib.scav_load_add.restype = ctypes.c_int32
    lib.scav_load_add.argtypes = [ctypes.c_void_p, ctypes.c_char_p, u32, ctypes.c_char_p]
    lib.scav_load_finish.restype = ctypes.c_int32
    lib.scav_load_finish.argtypes = [ctypes.c_void_p, p(ctypes.c_void_p)]
    lib.scav_load_destroy.restype = None
    lib.scav_load_destroy.argtypes = [ctypes.c_void_p]
    lib.scav_chart_destroy.restype = None
    lib.scav_chart_destroy.argtypes = [ctypes.c_void_p]
    lib.scav_chart_counts.restype = ctypes.c_int32
    lib.scav_chart_counts.argtypes = [ctypes.c_void_p] + [p(u32)] * 5
    lib.scav_profile_named.restype = ctypes.c_int32
    lib.scav_profile_named.argtypes = [ctypes.c_char_p, ctypes.c_void_p, u32]
    lib.scav_layout_run.restype = ctypes.c_int32
    lib.scav_layout_run.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, u32, ctypes.c_void_p, u32,
        ctypes.c_void_p, u32, u32, p(u32),
    ]
    lib.scav_column_find.restype = ctypes.c_int32
    lib.scav_column_find.argtypes = [ctypes.c_void_p, ctypes.c_char_p, p(u32)]
    lib.scav_column_data.restype = ctypes.c_int32
    lib.scav_column_data.argtypes = [ctypes.c_void_p, u32, p(byte_p), p(u32)]
    lib.scav_column_count.restype = ctypes.c_int32
    lib.scav_column_count.argtypes = [ctypes.c_void_p, u32, p(u32)]


class TestLayoutOverCtypes(unittest.TestCase):
    cfg: scavtest.Config
    lib: ctypes.CDLL

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        if cls.cfg["sanitizer"].upper() not in ("", "NONE"):
            # A sanitized library dlopen'd into a clean host aborts, since its
            # interceptors must be installed before the process starts.
            raise unittest.SkipTest(
                f"ctypes cannot load a {cls.cfg['sanitizer']} build; "
                "c_api_tests covers these entry points under the sanitizer")
        names = ("libscav.dylib", "libscav.so", "scav.dll")
        found = next(
            (c for name in names for c in cls.cfg.build_dir.rglob(name)), None
        )
        assert found is not None, f"no shared scav library under {cls.cfg.build_dir}"
        cls.lib = ctypes.CDLL(str(found))
        bind(cls.lib)

    def column(self, chart: ctypes.c_void_p, name: bytes) -> tuple[bytes, int, int]:
        """One column's bytes, its stride, and its row count."""
        column_id = ctypes.c_uint32(0)
        self.assertEqual(
            SCAV_OK, self.lib.scav_column_find(chart, name, ctypes.byref(column_id))
        )
        data = ctypes.POINTER(ctypes.c_ubyte)()
        stride = ctypes.c_uint32(0)
        rows = ctypes.c_uint32(0)
        self.assertEqual(
            SCAV_OK,
            self.lib.scav_column_data(chart, column_id, ctypes.byref(data),
                                      ctypes.byref(stride)),
        )
        self.assertEqual(
            SCAV_OK,
            self.lib.scav_column_count(chart, column_id, ctypes.byref(rows)),
        )
        size = stride.value * rows.value
        raw = ctypes.string_at(data, size) if size else b""
        return raw, stride.value, rows.value

    def load(self) -> tuple[ctypes.c_void_p, ctypes.c_void_p]:
        """The corpus chart, through the loader, as a binding would."""
        text = (self.cfg.repo_root / CHART).read_bytes()
        loader = ctypes.c_void_p()
        self.assertEqual(SCAV_OK, self.lib.scav_load_begin(ctypes.byref(loader)))
        self.assertEqual(
            SCAV_OK, self.lib.scav_load_add(loader, text, len(text), b"brew.scav")
        )
        chart = ctypes.c_void_p()
        self.assertEqual(SCAV_OK, self.lib.scav_load_finish(loader, ctypes.byref(chart)))
        return loader, chart

    def test_a_struct_one_field_short_is_refused_rather_than_written_past(self) -> None:
        """The incident, as a test. A caller whose scav_layout_opts is one
        profile knob behind now names its own size, so the library refuses both
        calls instead of writing 192 bytes into 184 and reading `router` from
        whatever followed."""
        loader, chart = self.load()

        short = ShortLayoutOpts()
        raw = ctypes.cast(ctypes.byref(short),
                          ctypes.POINTER(ctypes.c_ubyte * ctypes.sizeof(short)))
        raw.contents[:] = [0x5C] * ctypes.sizeof(short)
        before = bytes(raw.contents)

        self.assertEqual(
            SCAV_E_ABI,
            self.lib.scav_profile_named(b"readable", ctypes.byref(short.profile),
                                        ctypes.sizeof(short.profile)),
        )
        count = ctypes.c_uint32(99)
        self.assertEqual(
            SCAV_E_ABI,
            self.lib.scav_layout_run(chart, None, SPACES_SIZE, ctypes.byref(short),
                                     ctypes.sizeof(short), None, 0, PLACED_SIZE,
                                     ctypes.byref(count)),
        )
        self.assertEqual(before, bytes(raw.contents), "the refused calls wrote")
        self.assertEqual(99, count.value, "the refused call moved the count")

        # And the generated struct, whose size is this library's, goes through.
        opts = scav_layout_opts()
        self.assertEqual(
            SCAV_OK,
            self.lib.scav_profile_named(b"readable", ctypes.byref(opts.profile),
                                        ctypes.sizeof(opts.profile)),
        )
        self.assertEqual(
            SCAV_OK,
            self.lib.scav_layout_run(chart, None, SPACES_SIZE, ctypes.byref(opts),
                                     ctypes.sizeof(opts), None, 0, PLACED_SIZE,
                                     ctypes.byref(count)),
        )
        self.assertEqual(0, count.value)

        self.lib.scav_chart_destroy(chart)
        self.lib.scav_load_destroy(loader)

    def test_layout_runs_and_geometry_reads_back(self) -> None:
        loader, chart = self.load()

        counts = [ctypes.c_uint32(0) for _ in range(5)]
        self.assertEqual(
            SCAV_OK,
            self.lib.scav_chart_counts(chart, *[ctypes.byref(c) for c in counts]),
        )
        n_states = counts[1].value
        self.assertGreater(n_states, 0)

        # The generated binding's struct, which test_abi.py holds to the header: a
        # hand-rolled copy was one field short and passed only while the bytes past
        # it happened to read as router 0.
        opts = scav_layout_opts()
        self.assertEqual(
            SCAV_OK,
            self.lib.scav_profile_named(b"readable", ctypes.byref(opts.profile),
                                        ctypes.sizeof(opts.profile)),
        )
        placed = ctypes.c_uint32(99)
        self.assertEqual(
            SCAV_OK,
            self.lib.scav_layout_run(chart, None, SPACES_SIZE, ctypes.byref(opts),
                                     ctypes.sizeof(opts), None, 0, PLACED_SIZE,
                                     ctypes.byref(placed)),
        )
        self.assertEqual(0, placed.value)  # no path boxes were requested

        # Geometry columns through the accessor, typed by the caller.
        raw, stride, rows = self.column(chart, b"scav.geom.state")
        self.assertEqual(ctypes.sizeof(Rect), stride)
        self.assertEqual(n_states, rows)
        rects = (Rect * rows).from_buffer_copy(raw)
        for r in rects:
            self.assertGreaterEqual(r.w, 0)
            self.assertLessEqual(r.x + r.w, COORD_MAX)
            self.assertLessEqual(r.y + r.h, COORD_MAX)
        self.assertTrue(any(r.w > 0 for r in rects), "every state rect is empty")

        chart_raw, _, chart_rows = self.column(chart, b"scav.geom.chart")
        self.assertEqual(1, chart_rows)
        bounds = Rect.from_buffer_copy(chart_raw)
        self.assertGreater(bounds.w, 0)
        for r in rects:  # everything inside the chart box
            self.assertLessEqual(r.x + r.w, bounds.x + bounds.w)
            self.assertLessEqual(r.y + r.h, bounds.y + bounds.h)

        # An unknown column stays an argument error, not a crash.
        missing = ctypes.c_uint32(0)
        self.assertEqual(
            SCAV_E_INVALID_ARG,
            self.lib.scav_column_find(chart, b"scav.geom.nope", ctypes.byref(missing)),
        )

        self.lib.scav_chart_destroy(chart)
        self.lib.scav_load_destroy(loader)


if __name__ == "__main__":
    unittest.main()
