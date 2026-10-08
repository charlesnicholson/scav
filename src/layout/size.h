#ifndef SCAV_LAYOUT_SIZE_H_INCLUDED
#define SCAV_LAYOUT_SIZE_H_INCLUDED

// Phase 2: the box formula composes extents bottom-up, ranks and the cross-axis
// assignment give frame-local positions, one descent makes them root-absolute.

#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/pack.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_pod_vector.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

// Everything the geometry columns need except the routes, all root-absolute.
// Tombstones stay all-zero.
struct SizedLayout {
  SizedLayout() = default;
  SizedLayout(SizedLayout const &) = default;
  SizedLayout &operator=(SizedLayout const &) = default;
  SizedLayout(SizedLayout &&) noexcept;  // moves and the destructor are out of line
  SizedLayout &operator=(SizedLayout &&) noexcept;
  ~SizedLayout();

  PodVector<scav_rect> state, before, after;  // parallel to states
  PodVector<scav_rect> lead, trail;           // parallel to states: the side bands
  PodVector<scav_rect> loop;                  // parallel to states: its inner loops' room
  PodVector<scav_extent> natural;  // parallel to states: the box before seats floor it
  PodVector<uint8_t> loop_place;   // parallel to states: the room's `face * 2 + end`
  PodVector<scav_rect> sub;        // parallel to submachines
  PodVector<scav_point> node;      // parallel to the orders' nodes
  // Parallel to the segments, or empty: 1 where a straight leg seats at the leading end of
  // its ends' overlap, with its label's room on the trailing side.
  PodVector<uint8_t> lean;
  PodVector<uint8_t> folded;  // parallel to submachines: 1 where a frame's run wraps
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
// bottom band's bottom, and the loop room extended to the free interior's boundary its
// loops leave by.
std::array<scav_rect, 5> state_walls(SizedLayout const &z, uint32_t st);

// A loop room's face of the free interior (0 left, 1 right, 2 top, 3 bottom) and its end
// on that face (0 leading, 1 trailing).
struct LoopPlace {
  uint32_t face{ 1 };
  uint32_t end{ 1 };
};
// `z.loop_place[st]`, or the unpinned placement where `z` has no entry.
LoopPlace loop_place(SizedLayout const &z, uint32_t st);
// The unpinned placement: the trailing end of the first anchored face of right, left,
// bottom, top; the right face where none is.
LoopPlace loop_place_default(scav_spaces const &s, uint32_t state);
// Whether legs leaving by `face` end on a drawn edge: no band lines it, or its band is
// ruled.
bool loop_anchored(scav_spaces const &s, uint32_t state, uint32_t face);
// Where the legs of loops leaving by `face` end: the border, or the band's inner edge.
int32_t loop_boundary(SizedLayout const &z, uint32_t st, uint32_t face);
// True when placement `place` (`face * 2 + end`) is on the face `z` draws `st`'s loop room
// by and lays the room out where `z` has it.
bool loop_room_unmoved(Chart const &c, SizedLayout const &z, uint32_t st, uint32_t place);

// Whether a band of `state` lines `face` (0 left, 1 right, 2 top, 3 bottom), which then
// takes no port.
bool face_lined(scav_spaces const &s, uint32_t state, uint32_t face);

// Fills `z.natural` from `z.state`, the space tables, `z.loop_place` and `p`: a live leaf
// `Normal` state's box before seats floor it, every other state's box as drawn.
void size_natural(Chart const &c,
                  scav_spaces const &s,
                  scav_profile const &p,
                  SizedLayout &z);

// Per state, its route ends in its parent frame: segment ends at its box and at ports on
// its border. An inner loop has none.
void size_route_ends(Chart const &c, SplitGraph const &g, PodVector<uint32_t> &ends);

// One inner loop's row in its state's room: rows stack across the exit face in transition
// order. The far leg runs `loop_reach` in from the exit face and spans the label stack
// beyond it; `cross` runs along the exit face, `along` away from it.
struct LoopRow {
  int32_t label_w, label_h, lane, cross, along;  // `lane` between the legs
};
LoopRow loop_row(scav_profile const &p, scav_extent label, bool vertical);

// Per inner loop, the extent its path boxes stack to.
void loop_labels(Chart const &c, scav_spaces const &s, PodVector<scav_extent> &label);

// Per state, the room its inner loops stack into for the placement `place[st]` gives
// (`face * 2 + end`; the right face where `place` is short), and `label` as `loop_labels`.
void loop_rooms(Chart const &c,
                scav_spaces const &s,
                scav_profile const &p,
                PodVector<uint8_t> const &place,
                PodVector<scav_extent> &label,
                PodVector<scav_extent> &room);

// Per transition, its inner loop's row across its state's room, zero for any other; and
// `label` as `loop_labels` gives it.
void loop_rows(Chart const &c,
               SizedLayout const &z,
               scav_spaces const &s,
               scav_profile const &p,
               PodVector<scav_extent> &label,
               PodVector<scav_rect> &row);

// Which row knobs can change a sizing of `c` under some pins: `trybox` and compaction
// where a packing may hold two rects, the owner's hole inside a composite, the fold rule.
struct RowReads {
  bool trybox{ false };
  bool pack{ false };
  bool dar{ false };
  bool fold{ false };
};

// The knobs sizing reads of `c`, from ordering `o`'s frames: their components and edges.
RowReads size_row_reads(Chart const &c, SubmachineOrders const &o);

// `row` with each knob `reads` leaves unread at its value in `base`'s row 0.
Row size_row_canonical(Row const &row, RowReads const &reads, scav_profile const &base);

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
