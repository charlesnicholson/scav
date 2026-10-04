#ifndef SCAV_LAYOUT_SIZE_H_INCLUDED
#define SCAV_LAYOUT_SIZE_H_INCLUDED

// Phase 2: the box formula composes extents bottom-up, ranks and the cross-axis
// assignment give frame-local positions, one descent makes them root-absolute.

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

// A desired aspect ratio as a pair in the profile's `[1, 1024]` bounds; `num` 0 means
// no ratio.
struct FrameDar {
  int32_t num{ 0 }, den{ 0 };
};

// The ratio packings in a state's interior aim at: the profile's, or the aspect of the
// hole inside its bands. `OwnerHole` sizes twice, first at the profile's ratio.
enum class DarSource : uint32_t { Profile, OwnerHole };

// Whether a frame's run may wrap; a fold pin overrides it per frame. `Scale` takes the
// scale measure's pick, `Always` any fold that wraps, `Never` keeps the run unwrapped.
enum class Fold : uint32_t { Scale, Always, Never };

// One row of the search's table: the profile and the phase-2 tuple it lays out with.
struct Row {
  scav_profile knobs{};
  DarSource dar{ DarSource::Profile };
  Compaction pack{ Compaction::Off };
  Fold fold{ Fold::Scale };
};

// A state's walls: top, bottom, the side bands stretched from the top band's top to the
// bottom band's bottom, and the loop room extended to the border its loops leave by.
std::array<scav_rect, 5> state_walls(SizedLayout const &z, uint32_t st);

// Whether a state's loop room sits at the leading end of its interior and its inner loops
// leave by its leading border: a band lines its trailing face and none its leading one.
bool loop_mirrored(SizedLayout const &z, uint32_t st);

// Whether a band of `state` lines `face` (0 left, 1 right, 2 top, 3 bottom), which then
// takes no port.
bool face_lined(scav_spaces const &s, uint32_t state, uint32_t face);

// One inner loop's row in its state's room; rows stack in transition order. The far leg
// runs `loop_reach` in from the room's exit edge and spans the label stack beside it.
struct LoopRow {
  int32_t label_w, label_h, lane, h;  // `lane` between the legs, `h` the row's
};
LoopRow loop_row(scav_profile const &p, scav_extent label);
int32_t loop_reach(scav_profile const &p);
int32_t loop_gap(scav_profile const &p);   // label to far leg, at most `label_leader`
int32_t loop_lane(scav_profile const &p);  // the least lane between the loop's two legs

// Per inner loop, the extent its path boxes stack to; per state, the room its inner loops
// stack into: the widest row by the rows' summed height.
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

// False when an extent would leave the coordinate domain: one diagnostic per entity and
// `out` partly written. The defaults are row 0's tuple.
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
