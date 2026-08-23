#!/usr/bin/env python3
"""A Python caller runs layout through the shared library and reads geometry
columns back through the three-call accessor."""

import ctypes
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

CHART = Path("test_data/charts/brew.scav")

SCAV_OK = 0
SCAV_E_INVALID_ARG = -1
COORD_MAX = (1 << 19) - 1


class Rect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32),
                ("w", ctypes.c_int32), ("h", ctypes.c_int32)]


class LayoutOpts(ctypes.Structure):
    """scav_layout_opts: the profile is 43 int32 fields, flat."""
    _fields_ = [("profile", ctypes.c_int32 * 43),
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
    lib.scav_profile_named.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
    lib.scav_layout_run.restype = ctypes.c_int32
    lib.scav_layout_run.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_void_p, u32, p(u32),
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

    def test_layout_runs_and_geometry_reads_back(self) -> None:
        text = (self.cfg.repo_root / CHART).read_bytes()
        loader = ctypes.c_void_p()
        self.assertEqual(SCAV_OK, self.lib.scav_load_begin(ctypes.byref(loader)))
        self.assertEqual(
            SCAV_OK, self.lib.scav_load_add(loader, text, len(text), b"brew.scav")
        )
        chart = ctypes.c_void_p()
        self.assertEqual(SCAV_OK, self.lib.scav_load_finish(loader, ctypes.byref(chart)))

        counts = [ctypes.c_uint32(0) for _ in range(5)]
        self.assertEqual(
            SCAV_OK,
            self.lib.scav_chart_counts(chart, *[ctypes.byref(c) for c in counts]),
        )
        n_states = counts[1].value
        self.assertGreater(n_states, 0)

        opts = LayoutOpts()
        self.assertEqual(
            SCAV_OK, self.lib.scav_profile_named(b"readable", ctypes.byref(opts))
        )
        placed = ctypes.c_uint32(99)
        self.assertEqual(
            SCAV_OK,
            self.lib.scav_layout_run(chart, None, ctypes.byref(opts), None, 0,
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
