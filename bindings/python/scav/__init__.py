"""scav for Python: the whole pipeline, model to SVG.

The half of a binding that would drift is generated (`_abi.py`); this is the
hand-written half, and it stays small on purpose. Extending scav means writing
an application, so this covers the pipeline rather than a plugin corner: load a
document network, measure it, lay it out, build a DrawList, render SVG.

No extension point is a callback, so every one of these is pure marshalling and
no Python function is ever invoked from a scav worker thread.
"""

from __future__ import annotations

import ctypes
from pathlib import Path
from typing import Iterator, Sequence

from . import _abi
from ._abi import (
    SCAV_CLIP_NONE,
    SCAV_E_CAPACITY,
    SCAV_E_LOAD,
    SCAV_OK,
    SCAV_PRIM_CIRCLE,
    SCAV_PRIM_IMAGE,
    SCAV_PRIM_LINE,
    SCAV_PRIM_PATH,
    SCAV_PRIM_POLYLINE,
    SCAV_PRIM_RECT,
    SCAV_PRIM_RRECT,
    SCAV_PRIM_TEXT,
    SCAV_STYLE_COUNT,
    ScavError,
    check,
    scav_box_space,
    scav_diag,
    scav_extent,
    scav_layout_opts,
    scav_path_box,
    scav_path_clear,
    scav_placed,
    scav_point,
    scav_prim,
    scav_profile,
    scav_rect,
    scav_spaces,
    scav_style,
)

__all__ = [
    "Chart", "DrawList", "Images", "Loader", "Metrics", "ScavError", "Spaces",
    "abi_version", "library", "load_network", "profile", "SCAV_PRIM_TEXT",
]

_LIB: ctypes.CDLL | None = None


def library(path: str | Path | None = None) -> ctypes.CDLL:
    """The one redistributable shared object, loaded once per process."""
    global _LIB
    if _LIB is None or path is not None:
        _LIB = _abi.load(path)
    return _LIB


def abi_version() -> int:
    return int(library().scav_abi_version())


def _bytes(text: str | bytes) -> bytes:
    return text if isinstance(text, bytes) else text.encode("utf-8")


def _integer(value: object, name: str) -> int:
    """Every number crossing into a space request, checked.

    `/` yields a float in Python, and a space request computed that way would
    differ under FMA contraction and fail a golden rather than raise. So the
    boundary refuses anything that is not already an integer -- a bool included,
    since `True + 1` is the kind of accident this exists to catch.
    """
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an int, not {type(value).__name__}")
    if not (-(2**31) <= value < 2**31):
        raise ValueError(f"{name} is outside int32: {value}")
    return value


class _Handle:
    """A scav handle. Destroy is idempotent on NULL, so close twice is fine."""

    _destroy = ""

    def __init__(self, pointer: ctypes.c_void_p) -> None:
        self._pointer = pointer

    def __enter__(self):
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def close(self) -> None:
        if self._pointer:
            getattr(library(), self._destroy)(self._pointer)
            self._pointer = None

    def __del__(self) -> None:
        self.close()

    @property
    def pointer(self):
        if not self._pointer:
            raise ValueError(f"{type(self).__name__} is closed")
        return self._pointer


class Loader(_Handle):
    """The iterative loader: add the root, resolve what it asks for, finish.

    No callbacks, which is the whole reason a binding is tractable. The
    application decides what a document name means -- a path, a URL, a key into
    a dict -- and scav never learns.
    """

    _destroy = "scav_load_destroy"

    def __init__(self) -> None:
        lib = library()
        out = ctypes.POINTER(_abi.scav_load)()
        check(lib.scav_load_begin(ctypes.byref(out)), "scav_load_begin")
        super().__init__(out)

    def add(self, name: str, source: str | bytes) -> None:
        lib = library()
        raw = _bytes(source)
        buffer = (ctypes.c_ubyte * len(raw)).from_buffer_copy(raw)
        check(lib.scav_load_add(self.pointer, buffer, len(raw), _bytes(name)),
              "scav_load_add")

    def pending(self) -> list[str]:
        """The document names the loader still wants, in the order it asked."""
        lib = library()
        rows = ctypes.POINTER(_abi.scav_pending)()
        count = ctypes.c_uint32(0)
        check(lib.scav_load_pending(self.pointer, ctypes.byref(rows),
                                    ctypes.byref(count)), "scav_load_pending")
        wanted = []
        for i in range(count.value):
            data = ctypes.POINTER(ctypes.c_ubyte)()
            length = ctypes.c_uint32(0)
            check(lib.scav_load_path(self.pointer, rows[i].path, ctypes.byref(data),
                                     ctypes.byref(length)), "scav_load_path")
            wanted.append(bytes(data[:length.value]).decode("utf-8"))
        return wanted

    def finish(self) -> "Chart":
        lib = library()
        out = ctypes.POINTER(_abi.scav_chart)()
        code = lib.scav_load_finish(self.pointer, ctypes.byref(out))
        if code == SCAV_E_LOAD:
            raise ScavError(code, f"scav_load_finish: {self.diagnostics()}")
        check(code, "scav_load_finish")
        return Chart(out)

    def diagnostics(self) -> list[str]:
        lib = library()
        count = ctypes.c_uint32(0)
        check(lib.scav_load_diag_count(self.pointer, ctypes.byref(count)),
              "scav_load_diag_count")
        out = []
        for i in range(count.value):
            code = ctypes.c_uint32(0)
            doc = ctypes.c_uint32(0)
            off = ctypes.c_uint32(0)
            length = ctypes.c_uint32(0)
            check(lib.scav_load_diag(self.pointer, i, ctypes.byref(code),
                                     ctypes.byref(doc), ctypes.byref(off),
                                     ctypes.byref(length)), "scav_load_diag")
            out.append(lib.scav_diag_message(code.value).decode("utf-8"))
        return out


class Chart(_Handle):
    """The model. Outlives every span handed out from it, and owns nothing else."""

    _destroy = "scav_chart_destroy"

    def counts(self) -> dict[str, int]:
        lib = library()
        fields = ("documents", "states", "submachines", "transitions", "includes")
        out = [ctypes.c_uint32(0) for _ in fields]
        check(lib.scav_chart_counts(self.pointer, *(ctypes.byref(v) for v in out)),
              "scav_chart_counts")
        return {name: value.value for name, value in zip(fields, out)}

    @property
    def structural_hash(self) -> int:
        out = ctypes.c_uint32(0)
        check(library().scav_chart_structural_hash(self.pointer, ctypes.byref(out)),
              "scav_chart_structural_hash")
        return out.value

    def digest(self) -> bytes:
        lib = library()
        size = ctypes.c_uint32(0)
        check(lib.scav_chart_digest(self.pointer, None, 0, ctypes.byref(size)),
              "scav_chart_digest")
        buffer = (ctypes.c_ubyte * size.value)()
        check(lib.scav_chart_digest(self.pointer, buffer, size.value,
                                    ctypes.byref(size)), "scav_chart_digest")
        return bytes(buffer)

    def diagnostics(self) -> list[str]:
        """The last operation's findings, which the chart owns."""
        lib = library()
        count = ctypes.c_uint32(0)
        check(lib.scav_chart_diag_count(self.pointer, ctypes.byref(count)),
              "scav_chart_diag_count")
        out = []
        for i in range(count.value):
            row = scav_diag()
            check(lib.scav_chart_diag(self.pointer, i, ctypes.byref(row)),
                  "scav_chart_diag")
            out.append(lib.scav_diag_message(row.code).decode("utf-8"))
        return out

    def column(self, name: str) -> memoryview:
        """One column's rows as a memoryview over the chart's own bytes.

        Three calls, because a walk needs the row count: the view is a window
        onto memory the chart owns, so it dies when the chart does.
        """
        lib = library()
        column = ctypes.c_uint32(0)
        check(lib.scav_column_find(self.pointer, _bytes(name), ctypes.byref(column)),
              f"scav_column_find({name})")
        data = ctypes.POINTER(ctypes.c_ubyte)()
        stride = ctypes.c_uint32(0)
        rows = ctypes.c_uint32(0)
        check(lib.scav_column_data(self.pointer, column, ctypes.byref(data),
                                   ctypes.byref(stride)), "scav_column_data")
        check(lib.scav_column_count(self.pointer, column, ctypes.byref(rows)),
              "scav_column_count")
        if rows.value == 0:
            return memoryview(b"")
        return memoryview(
            ctypes.cast(data, ctypes.POINTER(
                ctypes.c_ubyte * (rows.value * stride.value))).contents)

    def rects(self, name: str) -> list[scav_rect]:
        """A geometry column of rects, typed."""
        raw = self.column(name)
        count = len(raw) // ctypes.sizeof(scav_rect)
        return list((scav_rect * count).from_buffer_copy(raw))

    def string(self, span) -> str:
        lib = library()
        data = ctypes.POINTER(ctypes.c_ubyte)()
        length = ctypes.c_uint32(0)
        check(lib.scav_str(self.pointer, span, ctypes.byref(data),
                           ctypes.byref(length)), "scav_str")
        return bytes(data[:length.value]).decode("utf-8") if length.value else ""

    def layout(self, spaces: "Spaces | None" = None,
               options: scav_layout_opts | None = None) -> list[scav_placed]:
        """Run layout and read the placed boxes back.

        Never ignore the result: geometry lands in derived columns, and a failure
        leaves them holding the last successful run.
        """
        lib = library()
        opts = options if options is not None else scav_layout_opts(
            profile=profile("readable"), router=0, threads=0)
        table = spaces.as_c() if spaces is not None else scav_spaces()
        count = ctypes.c_uint32(0)
        code = lib.scav_layout_run(self.pointer, ctypes.byref(table),
                                   ctypes.byref(opts), None, 0, ctypes.byref(count))
        check(code, f"scav_layout_run: {self.diagnostics()}")
        if count.value == 0:
            return []
        placed = (scav_placed * count.value)()
        check(lib.scav_layout_run(self.pointer, ctypes.byref(table),
                                  ctypes.byref(opts), placed, count.value,
                                  ctypes.byref(count)),
              f"scav_layout_run: {self.diagnostics()}")
        return list(placed)


class Metrics(_Handle):
    """Font tables. Immutable after create, so one instance serves every thread."""

    _destroy = "scav_metrics_destroy"

    def __init__(self, ttf: bytes | None = None) -> None:
        lib = library()
        out = ctypes.POINTER(_abi.scav_metrics)()
        if ttf is None:
            check(lib.scav_metrics_create(None, 0, ctypes.byref(out)),
                  "scav_metrics_create")
        else:
            buffer = (ctypes.c_ubyte * len(ttf)).from_buffer_copy(ttf)
            check(lib.scav_metrics_create(buffer, len(ttf), ctypes.byref(out)),
                  "scav_metrics_create")
        super().__init__(out)

    def _u32(self, call: str) -> int:
        out = ctypes.c_uint32(0)
        check(getattr(library(), call)(self.pointer, ctypes.byref(out)), call)
        return out.value

    @property
    def identity(self) -> int:
        return self._u32("scav_metrics_identity")

    @property
    def units_per_em(self) -> int:
        return self._u32("scav_metrics_units_per_em")

    @property
    def glyph_count(self) -> int:
        return self._u32("scav_metrics_glyph_count")

    def measure(self, text: str, font_size_grid: int) -> tuple[int, int]:
        """One line, in grid units. Raises on a newline or a missing glyph."""
        raw = _bytes(text)
        out = scav_extent()
        buffer = (ctypes.c_ubyte * len(raw)).from_buffer_copy(raw) if raw else None
        check(library().scav_measure_text(
            self.pointer, buffer, len(raw),
            _integer(font_size_grid, "font_size_grid"), ctypes.byref(out)),
            "scav_measure_text")
        return out.w, out.h


class Images(_Handle):
    """The raster registry a backend reads. Dimensions come from registration."""

    _destroy = "scav_images_destroy"

    def __init__(self) -> None:
        out = ctypes.POINTER(_abi.scav_images)()
        check(library().scav_images_create(ctypes.byref(out)), "scav_images_create")
        super().__init__(out)

    def register(self, name: str, data: bytes, width: int, height: int,
                 mime: str) -> None:
        buffer = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        check(library().scav_image_register(
            self.pointer, _bytes(name), buffer, len(data),
            _integer(width, "width"), _integer(height, "height"), _bytes(mime)),
            "scav_image_register")


class Spaces:
    """The space tables, as the reference measurement pass filled them.

    The pass itself lives in the shared library rather than here, because a
    request computed in Python arithmetic could differ from the one a golden was
    recorded against. Adjusting a row after the fact goes through
    `set_box_state`, which range-checks.
    """

    def __init__(self, box_state, box_sub, path_clear, path_box) -> None:
        self.box_state = box_state
        self.box_sub = box_sub
        self.path_clear = path_clear
        self.path_box = path_box

    @classmethod
    def measure(cls, chart: Chart, metrics: Metrics,
                prof: scav_profile) -> "Spaces":
        lib = library()
        counts = (ctypes.c_uint32 * 4)()
        check(lib.scav_measure_chart(chart.pointer, metrics.pointer,
                                     ctypes.byref(prof), None, 0, None, 0, None, 0,
                                     None, 0, counts), "scav_measure_chart")
        box_state = (scav_box_space * counts[0])()
        box_sub = (scav_box_space * counts[1])()
        path_clear = (scav_path_clear * counts[2])()
        path_box = (scav_path_box * counts[3])()
        check(lib.scav_measure_chart(
            chart.pointer, metrics.pointer, ctypes.byref(prof),
            box_state, counts[0], box_sub, counts[1],
            path_clear, counts[2], path_box, counts[3], counts),
            "scav_measure_chart")
        return cls(box_state, box_sub, path_clear, path_box)

    def set_box_state(self, row: int, min_w: int, h_before: int,
                      h_after: int) -> None:
        self.box_state[_integer(row, "row")] = scav_box_space(
            min_w=_integer(min_w, "min_w"),
            h_before=_integer(h_before, "h_before"),
            h_after=_integer(h_after, "h_after"))

    def as_c(self) -> scav_spaces:
        def base(array, kind):
            return ctypes.cast(array, ctypes.POINTER(kind)) if len(array) else None
        return scav_spaces(
            box_state=base(self.box_state, scav_box_space),
            n_box_state=len(self.box_state),
            box_sub=base(self.box_sub, scav_box_space),
            n_box_sub=len(self.box_sub),
            path_clear=base(self.path_clear, scav_path_clear),
            n_path_clear=len(self.path_clear),
            path_box=base(self.path_box, scav_path_box),
            n_path_box=len(self.path_box))


class DrawList(_Handle):
    """The render IR. Five flat arrays plus a string pool."""

    _destroy = "scav_drawlist_destroy"

    def __init__(self) -> None:
        out = ctypes.POINTER(_abi.scav_drawlist)()
        check(library().scav_drawlist_create(ctypes.byref(out)),
              "scav_drawlist_create")
        super().__init__(out)

    @classmethod
    def build(cls, chart: Chart, metrics: Metrics, spaces: Spaces | None = None,
              placed: Sequence[scav_placed] = (), depth: int = 0,
              palette: Sequence[scav_style] | None = None) -> "DrawList":
        """The reference builder over a laid-out chart.

        Hand back the same space tables and placed boxes layout was given: a
        label's rect is the one layout placed, not one a builder recomputes.
        """
        out = cls()
        table = spaces.as_c() if spaces is not None else scav_spaces()
        rows = (scav_placed * len(placed))(*placed) if placed else None
        style_rows = None
        style_count = 0
        if palette is not None:
            style_rows = (scav_style * len(palette))(*palette)
            style_count = len(palette)
        check(library().scav_emit_chart(
            out.pointer, chart.pointer, metrics.pointer, style_rows, style_count,
            ctypes.byref(table), rows, len(placed), _integer(depth, "depth")),
            "scav_emit_chart")
        return out

    def validate(self) -> None:
        bad = ctypes.c_uint32(0)
        check(library().scav_drawlist_validate(self.pointer, ctypes.byref(bad)),
              f"scav_drawlist_validate (primitive {bad.value})")

    def canonicalize(self) -> None:
        check(library().scav_drawlist_canonicalize(self.pointer),
              "scav_drawlist_canonicalize")

    def digest(self, metrics: Metrics) -> int:
        out = ctypes.c_uint32(0)
        check(library().scav_drawlist_digest(self.pointer, metrics.pointer,
                                            ctypes.byref(out)),
              "scav_drawlist_digest")
        return out.value

    def append(self, other: "DrawList") -> None:
        check(library().scav_drawlist_append(self.pointer, other.pointer),
              "scav_drawlist_append")

    def counts(self) -> dict[str, int]:
        fields = ("prims", "styles", "points", "clips", "text")
        out = [ctypes.c_uint32(0) for _ in fields]
        check(library().scav_drawlist_counts(
            self.pointer, *(ctypes.byref(v) for v in out)), "scav_drawlist_counts")
        return {name: value.value for name, value in zip(fields, out)}

    def prims(self) -> list[scav_prim]:
        rows = ctypes.POINTER(scav_prim)()
        count = ctypes.c_uint32(0)
        check(library().scav_drawlist_prims(self.pointer, ctypes.byref(rows),
                                           ctypes.byref(count)),
              "scav_drawlist_prims")
        return [rows[i] for i in range(count.value)]

    def points(self) -> list[scav_point]:
        rows = ctypes.POINTER(scav_point)()
        count = ctypes.c_uint32(0)
        check(library().scav_drawlist_points(self.pointer, ctypes.byref(rows),
                                            ctypes.byref(count)),
              "scav_drawlist_points")
        return [rows[i] for i in range(count.value)]

    def payload(self, prim: scav_prim) -> str:
        if prim.payload.len == 0:
            return ""
        data = ctypes.POINTER(ctypes.c_ubyte)()
        length = ctypes.c_uint32(0)
        check(library().scav_drawlist_str(self.pointer, prim.payload,
                                         ctypes.byref(data), ctypes.byref(length)),
              "scav_drawlist_str")
        return bytes(data[:length.value]).decode("utf-8")

    def texts(self) -> Iterator[tuple[scav_prim, str]]:
        for prim in self.prims():
            if prim.kind == SCAV_PRIM_TEXT:
                yield prim, self.payload(prim)

    def svg(self, metrics: Metrics, images: Images | None = None,
            embed_font: bool = False, margin: int = 0) -> str:
        """The reference backend. Count first, then write: the protocol every
        span accessor here follows."""
        lib = library()
        options = _abi.scav_svg_options(embed_font=1 if embed_font else 0,
                                        margin=_integer(margin, "margin"))
        size = ctypes.c_uint32(0)
        check(lib.scav_svg_write(self.pointer, metrics.pointer,
                                 images.pointer if images else None,
                                 ctypes.byref(options), None, 0,
                                 ctypes.byref(size)), "scav_svg_write")
        buffer = (ctypes.c_ubyte * size.value)()
        check(lib.scav_svg_write(self.pointer, metrics.pointer,
                                 images.pointer if images else None,
                                 ctypes.byref(options), buffer, size.value,
                                 ctypes.byref(size)), "scav_svg_write")
        return bytes(buffer).decode("utf-8")


def profile(name: str = "readable") -> scav_profile:
    out = scav_profile()
    check(library().scav_profile_named(_bytes(name), ctypes.byref(out)),
          f"scav_profile_named({name})")
    return out


def load_network(root: str, sources: dict[str, str]) -> Chart:
    """The whole loader loop, for the common case of documents already in hand.

    The application decides what a name means; here it means a key in `sources`,
    and scav never learns that. A name it asks for that is absent is a KeyError
    rather than a diagnostic, because the caller knew what it had.
    """
    loader = Loader()
    loader.add(root, sources[root])
    while wanted := loader.pending():
        for name in wanted:
            loader.add(name, sources[name])
    chart = loader.finish()
    loader.close()
    return chart
