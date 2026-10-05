#ifndef SCAV_LAYOUT_CANDIDATE_MEMO_H_INCLUDED
#define SCAV_LAYOUT_CANDIDATE_MEMO_H_INCLUDED

// Exact memos of a layout's search candidates, shared by its threads: frame orderings as
// numbers, facing turns by phase-1 ordering, scores by laid ordering and faces.

#include "layout/decompose.h"
#include "layout/memo.h"
#include "layout/order.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_thread.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

// The facing pass's output.
struct Facing {
  std::vector<ReversePin> reverses;  // legs whose in-frame edge reverses
  std::vector<EndPin> sides;         // legs whose port moves to a cross border
};

// A candidate's outcome as the memo holds it.
struct MemoScore {
  Cost cost{};
  bool viable{ false };
  bool inflated{ false };
};

// What `find_facing` found.
enum class FacingFound : uint32_t { Absent, Failed, Turned };

class CandidateMemo {
 public:
  CandidateMemo(Chart const &c, SplitGraph const &g);
  CandidateMemo(CandidateMemo const &) = delete;
  CandidateMemo &operator=(CandidateMemo const &) = delete;

  // The key word for `row`, one per distinct row.
  uint32_t row_word(Row const &row);

  // Writes each frame's number in `o` to `ids`, interning new frames. False when a frame
  // does not fit the encoding; the candidate then goes unmemoized.
  bool frame_ids(SubmachineOrders const &o, std::vector<uint32_t> &ids);

  // The facing pass's turns for phase-1 ordering `o` (numbered `ids`) in `row`: `Failed`
  // where its first sizing failed, `Turned` with `out` filled.
  FacingFound find_facing(uint32_t row,
                          std::vector<uint32_t> const &ids,
                          SubmachineOrders const &o,
                          Facing &out);
  void store_facing(uint32_t row,
                    std::vector<uint32_t> const &ids,
                    SubmachineOrders const &o,
                    Facing const *turned);  // null where the first sizing failed

  // A score lookup: the entry, INVALID when the memo cannot hold it, and the score that
  // answered, if any.
  struct Recalled {
    uint32_t entry{ INVALID };
    bool found{ false };
    bool labelled{ false };  // the labelled score answered
    MemoScore score;
  };

  // The entry for laid ordering `ids` in `row` under `pins`' box-end faces, created if
  // new; a bound request reads the unlabelled score if set, any other the labelled one.
  Recalled find_score(uint32_t row,
                      std::vector<uint32_t> const &ids,
                      SearchPins const *pins,
                      bool labelled);

  // Reads entry `e`'s labelled or unlabelled score; false while it is unset.
  bool score(uint32_t e, bool labelled, MemoScore &out);
  void set_score(uint32_t e, bool labelled, MemoScore const &s);

  // Bytes the tables hold.
  [[nodiscard]] size_t bytes();

 private:
  // An entry's unlabelled then labelled score: `t0` the Tier-0 count or a negative tag for
  // unset, not viable or inflated; `t2_hi`, `t2_lo` the Tier-2 sum's high and low words.
  struct ScoreRecord {
    std::array<int32_t, 2> t0;
    std::array<uint32_t, 2> t2_hi;
    std::array<uint32_t, 2> t2_lo;
  };
  static_assert(sizeof(ScoreRecord) == 24);

  // Writes `r`'s score `k` (0 unlabelled, 1 labelled) to `out`; false while it is unset.
  static bool read(ScoreRecord const &r, uint32_t k, MemoScore &out);

  // One phase-1 ordering's turns in `turns`: `reverses` pairs (trans, leg), then `sides`
  // triples (trans, leg, end * 4 + face). `off` is INVALID where the first sizing failed.
  struct FacingRecord {
    uint32_t off, reverses, sides;
  };
  static_assert(sizeof(FacingRecord) == 12);

  // Appends frame block `m`'s ordering to `w`; false where a field does not fit.
  bool put_frame(SubmachineOrders const &o, uint32_t m, std::vector<uint32_t> &w) const;

  // Tables are striped by key hash into `SHARDS` shards, each under its own lock; a number
  // is the shard's number for the key shifted left by `SHARD_BITS`, or'd with the shard.
  static constexpr uint32_t SHARD_BITS{ 4 };
  static constexpr uint32_t SHARDS{ 1U << SHARD_BITS };
  struct FrameShard {
    Mutex lock;
    KeyIndex keys;
  };
  struct FacingShard {
    Mutex lock;
    KeyIndex keys;
    std::vector<FacingRecord> records;  // parallel to `keys`
    std::vector<uint32_t> turns;
  };
  struct ScoreShard {
    Mutex lock;
    KeyIndex keys;
    std::vector<ScoreRecord> records;  // parallel to `keys`
  };

  // The shard holding a key of hash `hash`.
  static uint32_t shard_of(uint64_t hash) {
    return static_cast<uint32_t>(hash >> 28U) & (SHARDS - 1);
  }
  // The number for a shard's key `index`; INVALID past the numbering.
  static uint32_t number(uint32_t index, uint32_t shard) {
    return (index < (INVALID >> SHARD_BITS)) ? ((index << SHARD_BITS) | shard) : INVALID;
  }

  Chart const &chart;
  SplitGraph const &graph;
  uint32_t const serial;  // a `memo_serial` naming this memo in thread caches
  bool usable{ true };    // every state and segment number fits the encoding
  // Frame blocks: one per submachine, then one for segments and states in none.
  std::vector<uint32_t> seg_off, seg_list;      // block -> its segments
  std::vector<uint32_t> state_off, state_list;  // block -> its states

  Mutex row_lock;  // guards `rows`
  std::vector<Row> rows;
  std::array<FrameShard, SHARDS> frames;
  std::array<FacingShard, SHARDS> facings;
  std::array<ScoreShard, SHARDS> scores;
};

}  // namespace scav

#endif  // SCAV_LAYOUT_CANDIDATE_MEMO_H_INCLUDED
