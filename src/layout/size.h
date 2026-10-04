#ifndef SCAV_LAYOUT_SIZE_H_INCLUDED
#define SCAV_LAYOUT_SIZE_H_INCLUDED

// Phase 2: extents composed bottom-up by the box formula, frame-local
// positions from the ranks and the cross-axis assignment, then one descent
// that makes every position root-absolute.

#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/pack.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

// Everything the geometry columns need except the routes, all root-absolute.
// Tombstones stay all-zero.
struct SizedLayout {
  std::vector<scav_rect> state, before, after;  // parallel to states
  std::vector<scav_rect> lead, trail;           // parallel to states: the side bands
  std::vector<scav_rect> loop;   // parallel to states: its inner loops' room
  std::vector<scav_rect> sub;    // parallel to submachines
  std::vector<scav_point> node;  // parallel to the orders' nodes
  // Parallel to the segments, or empty: 1 where a straight leg seats at the leading end of
  // its ends' overlap, with its label's room on the trailing side.
  std::vector<uint8_t> lean;
  std::vector<uint8_t> folded;  // parallel to submachines: 1 where a frame's run wraps
  scav_rect chart{};
};

// A desired aspect ratio as a pair, in the profile's own `[1, 1024]` bounds so
// the packer's products stay where it proved them. `num` 0 is no ratio at all.
struct FrameDar {
  int32_t num{ 0 }, den{ 0 };
};

// Which ratio every packing inside a state's interior aims at: the profile's
// one ratio at every depth, or the aspect of the hole the state leaves between
// its two text bands, which is the rect its submachines are packed into
// (11.4, 11.10). The hole is only knowable once the state is sized, so
// `OwnerHole` sizes twice -- once at the profile's ratio to find the holes,
// then again against them.
enum class DarSource : uint32_t { Profile, OwnerHole };

// Whether a frame's rank run may wrap. 11.4 lays a component out twice, once
// unwrapped and once cut at the aspect target, and `Scale` keeps whichever the
// scale measure prefers -- a local ratio that cannot see area. `Always` hands
// `Cost` the folded shape instead, so the choice is scored rather than
// arbitrated (11.10a). `Never` keeps the run unwrapped; a fold pin names one frame's.
enum class Fold : uint32_t { Scale, Always, Never };

// A state's bands as walls, then its loop room: the side bands run from the top band's
// top to the bottom band's bottom, so no seam opens where two bands meet, and the room
// runs on to the border its loops leave by.
std::array<scav_rect, 5> state_walls(SizedLayout const &z, uint32_t st);

// Whether a state's loop room sits at the leading end of its interior and its inner loops
// leave by its leading border: a band lines its trailing face and none its leading one.
bool loop_mirrored(SizedLayout const &z, uint32_t st);

// Whether a band of `state` lines `face`, 0 left, 1 right, 2 top, 3 bottom: no port sits
// there.
bool face_lined(scav_spaces const &s, uint32_t state, uint32_t face);

// One inner loop's row in its state's room: rows stack in transition order, the far leg
// runs `reach` in from the room's edge its loop leaves by, and the label sits `gap` beyond
// that leg.
struct LoopRow {
  int32_t label_w, label_h, h;
};
LoopRow loop_row(scav_profile const &p, scav_extent label);
int32_t loop_reach(scav_profile const &p);
int32_t loop_gap(scav_profile const &p);
int32_t loop_lane(scav_profile const &p);  // between the loop's two legs

// Per transition, the extent its path boxes stack to, and per state, the room its inner
// loops stack into: the widest row by every row's height.
void loop_rooms(Chart const &c,
                scav_spaces const &s,
                scav_profile const &p,
                std::vector<scav_extent> &label,
                std::vector<scav_extent> &room);

// Per transition, its inner loop's row across its state's room, zero for any other; and
// `label` as `loop_rooms` gives it.
void loop_rows(Chart const &c,
               SizedLayout const &z,
               scav_spaces const &s,
               scav_profile const &p,
               std::vector<scav_extent> &label,
               std::vector<scav_rect> &row);

// False on an extent that would leave the coordinate domain, with one
// diagnostic per offending entity and `out` left partly written. `dar` and
// `compaction` default to the row-0 tuple, which is the pipeline as it ran
// before the portfolio existed.
bool size_layout(Chart const &c,
                 SplitGraph const &g,
                 SubmachineOrders const &o,
                 scav_spaces const &s,
                 scav_profile const &p,
                 SizedLayout &out,
                 std::vector<Diagnostic> &diags,
                 DarSource dar = DarSource::Profile,
                 Compaction compaction = Compaction::Off,
                 Fold fold = Fold::Scale);

}  // namespace scav

#endif  // SCAV_LAYOUT_SIZE_H_INCLUDED
