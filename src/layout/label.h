#ifndef SCAV_LAYOUT_LABEL_H_INCLUDED
#define SCAV_LAYOUT_LABEL_H_INCLUDED

// Path boxes onto the finished routes: a fixed-length leader from a point on
// the label's own polyline to one of the eight points of its rectangle, the
// anchor slid along the route, the feasible candidate least at risk of reading
// as somebody else's (11.9.4).

#include "layout/geom.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

// How a box's candidates are searched. Every one keys and tests every
// candidate; `Pruned` skips the ones that provably cannot win; `Memoized` is
// `Pruned` behind a per-thread table of the boxes this thread has placed.
// Nothing shipping passes anything but `Memoized`: the other two are what a
// test weighs it against.
enum class LabelSearch : uint32_t { Exhaustive, Pruned, Memoized };

// Fills `out` parallel to `s.path_box`; returns the boxes that found no
// feasible candidate and took the centred placement instead.
uint32_t place_labels(Chart const &c,
                      SizedLayout const &z,
                      scav_spaces const &s,
                      std::vector<scav_span> const &route,
                      std::vector<scav_point> const &points,
                      scav_profile const &p,
                      std::vector<scav_rect> &out);

// How a box settled beyond where it went: whether the search found it a
// place, and the leg and the slide along it, in the chart's coordinates, that
// the next box of its transition goes past.
struct LabelSettle {
  uint32_t seg{ 0 };
  int32_t mid{ 0 };
  uint8_t found{ 0 };
};

// A placement over the same chart, sizing, path boxes and profile as the one
// to be made, with the routes it was made over.
struct LabelBase {
  std::vector<scav_span> const *route{ nullptr };
  std::vector<scav_point> const *points{ nullptr };
  std::vector<scav_rect> const *placed{ nullptr };
  std::vector<LabelSettle> const *settled{ nullptr };
};

// `place_labels`, filling `how` parallel to `out` as well. A box whose own
// route is the one it had in `was`, and whose region no leg that moved and no
// box that settled elsewhere reaches, is the problem it was there, so it keeps
// where it went without being searched.
uint32_t place_labels(Chart const &c,
                      SizedLayout const &z,
                      scav_spaces const &s,
                      std::vector<scav_span> const &route,
                      std::vector<scav_point> const &points,
                      scav_profile const &p,
                      std::vector<scav_rect> &out,
                      std::vector<LabelSettle> &how,
                      LabelBase const *was);

}  // namespace scav

#endif  // SCAV_LAYOUT_LABEL_H_INCLUDED
