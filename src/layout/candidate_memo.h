#ifndef SCAV_LAYOUT_CANDIDATE_MEMO_H_INCLUDED
#define SCAV_LAYOUT_CANDIDATE_MEMO_H_INCLUDED

// Exact memos of a layout's search candidates, shared by its threads: frames and drawings
// interned as numbers, facing turns, and scores by drawing or by laid ordering.

#include "layout/decompose.h"
#include "layout/memo.h"
#include "layout/order.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_thread.h"

#include <array>
#include <atomic>
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
  bool degraded{ false };
};

// What `find_facing` found.
enum class FacingFound : uint32_t { Absent, Failed, Turned };

// One memo serves one layout: one chart, split graph, spaces, router and objective.
class CandidateMemo {
 public:
  // The bytes the tables hold before every table empties.
  static constexpr uint64_t BUDGET{ uint64_t{ 128 } << 20U };

  CandidateMemo(Chart const &c, SplitGraph const &g, uint64_t bytes = BUDGET);
  CandidateMemo(CandidateMemo const &) = delete;
  CandidateMemo &operator=(CandidateMemo const &) = delete;

  // The key word for `row`, one per distinct row.
  uint32_t row_word(Row const &row);

  // The key word for routing under `knobs`: one per distinct profile, `trybox` ignored.
  uint32_t profile_word(scav_profile const &knobs);

  // An ordering or a drawing encoded frame block by frame block, and each block's number.
  struct Blocks {
    uint32_t serial{ 0 };  // the `memo_serial` the numbers belong to
    std::vector<uint32_t> words;
    std::vector<uint32_t> ends;  // block -> one past its last word
    std::vector<uint32_t> ids;
  };

  // Writes each frame's number in `o` to `ids`, interning new ones; false where one does
  // not fit. A frame matching `like`'s takes its number; `keep` gets the encoding.
  bool frame_ids(SubmachineOrders const &o,
                 std::vector<uint32_t> &ids,
                 Blocks const *like = nullptr,
                 Blocks *keep = nullptr);

  // The number of `o`'s arrangement, its frame numbers in order, interning it if new;
  // INVALID when a frame does not fit the encoding. `like` and `keep` as `frame_ids`.
  uint32_t arrangement(SubmachineOrders const &o,
                       Blocks const *like = nullptr,
                       Blocks *keep = nullptr);

  // The interned number of what routing, labels and cost read of `o` sized as `z` under
  // profile word `profile`; INVALID where it does not fit. `like`, `keep` as `frame_ids`.
  uint32_t drawing(SubmachineOrders const &o,
                   SizedLayout const &z,
                   uint32_t profile,
                   Blocks const *like = nullptr,
                   Blocks *keep = nullptr);

  // `drawing` with `o`'s bend nodes per segment, as `segment_bends` writes them.
  uint32_t drawing(SubmachineOrders const &o,
                   SizedLayout const &z,
                   std::vector<std::vector<uint32_t>> const &bends,
                   uint32_t profile,
                   Blocks const *like = nullptr,
                   Blocks *keep = nullptr);

  // The router's box-end faces under `pins`, as `route_transitions` reads them: the last
  // pin naming an end decides it; one word per end, sorted.
  void box_faces(SearchPins const *pins, std::vector<uint32_t> &faces) const;

  // The facing pass's turns for phase-1 ordering `o`, of arrangement `arranged`, in `row`:
  // `Failed` where its first sizing failed, `Turned` with `out` filled.
  FacingFound find_facing(uint32_t row,
                          uint32_t arranged,
                          SubmachineOrders const &o,
                          Facing &out);
  void store_facing(uint32_t row,
                    uint32_t arranged,
                    SubmachineOrders const &o,
                    Facing const *turned);  // null where the first sizing failed

  // A score lookup: the entry, INVALID when the memo cannot hold it, and the score that
  // answered, if any.
  struct Recalled {
    uint32_t entry{ INVALID };
    bool found{ false };
    bool labelled{ false };  // the labelled score answered
    MemoScore score;
    int64_t route_bound{ -1 };  // the entry's `cost_bound` Tier 2, -1 while unset
  };

  // Finds or makes drawing `drawn`'s score entry under `faces`: a bound request reads the
  // unlabelled score if set, any other the labelled one; a retried entry reads INVALID.
  Recalled find_score(uint32_t drawn, std::vector<uint32_t> const &faces, bool labelled);

  // Entry `e` read as `find_score` reads it.
  Recalled recall(uint32_t e, bool labelled);

  // An ordering key and the score entry linked to it, INVALID while none is.
  struct Linked {
    uint32_t key{ INVALID };
    uint32_t entry{ INVALID };
  };

  // Finds or makes the key for arrangement `arranged` laid in `row` under `faces`.
  Linked find_ordering(uint32_t row,
                       uint32_t arranged,
                       std::vector<uint32_t> const &faces);

  // Links ordering key `key` to score entry `e`.
  void link(uint32_t key, uint32_t e);

  // Reads entry `e`'s labelled or unlabelled score; false while it is unset.
  bool score(uint32_t e, bool labelled, MemoScore &out);
  void set_score(uint32_t e, bool labelled, MemoScore const &s);

  // Stores the Tier 2 of entry `e`'s `cost_bound`, whose Tier 0 is zero.
  void set_route_bound(uint32_t e, int64_t t2);

  // Marks entry `e` retried: it then never answers and takes no score.
  void set_retried(uint32_t e);

  // The most bytes the tables held at once: keys, entries, slots and records.
  [[nodiscard]] uint64_t peak_bytes() const;

 private:
  // An entry's unlabelled then labelled score, `t0` a Tier-0 count or a negative tag, and
  // its route bound's Tier 2, `bound_hi` negative while unset.
  struct ScoreRecord {
    std::array<int32_t, 2> t0;
    std::array<uint32_t, 2> t2_hi;
    std::array<uint32_t, 2> t2_lo;
    int32_t bound_hi;
    uint32_t bound_lo;
  };
  static_assert(sizeof(ScoreRecord) == 32);

  // Writes `r`'s score `k` (0 unlabelled, 1 labelled) to `out`; false while it is unset.
  static bool read(ScoreRecord const &r, uint32_t k, MemoScore &out);
  // Fills `out` with `r`'s answer to a request, as `find_score` gives it.
  static void answer(ScoreRecord const &r, bool labelled, Recalled &out);

  // One phase-1 ordering's turns in `turns`: `reverses` pairs (trans, leg), then `sides`
  // triples (trans, leg, end * 4 + face). `off` is INVALID where the first sizing failed.
  struct FacingRecord {
    uint32_t off, reverses, sides;
  };
  static_assert(sizeof(FacingRecord) == 12);

  // Appends frame block `m`'s ordering to `w`; false where a field does not fit.
  bool put_frame(SubmachineOrders const &o, uint32_t m, std::vector<uint32_t> &w) const;

  // The number of state `st`'s shape in `z`: its extent, loop placement, and its bands and
  // loop room relative to its corner; INVALID when the table is full.
  uint32_t shape(SizedLayout const &z, uint32_t st);

  // Appends frame block `m`'s geometry, relative to its corner (the origin for the last
  // block), to `w`; `bends` as `segment_bends` gives them. False where it does not fit.
  bool put_geometry(SubmachineOrders const &o,
                    SizedLayout const &z,
                    std::vector<std::vector<uint32_t>> const &bends,
                    uint32_t m,
                    std::vector<uint32_t> &w);

  // Tables are striped by key hash into shards under their own locks; a number is the
  // shard's `base` plus the key's index, shifted by `SHARD_BITS`, or'd with the shard.
  static constexpr uint32_t SHARD_BITS{ 8 };
  static constexpr uint32_t SHARDS{ 1U << SHARD_BITS };
  struct IndexShard {
    Mutex lock;
    KeyIndex keys;
    uint32_t base{ 0 };  // numbers issued before the shard last emptied
  };
  struct FacingShard {
    Mutex lock;
    KeyIndex keys;
    uint32_t base{ 0 };                 // numbers issued before the shard last emptied
    std::vector<FacingRecord> records;  // parallel to `keys`
    std::vector<uint32_t> turns;
  };
  struct ScoreShard {
    Mutex lock;
    KeyIndex keys;
    uint32_t base{ 0 };                // numbers issued before the shard last emptied
    std::vector<ScoreRecord> records;  // parallel to `keys`
  };
  struct LinkShard {
    Mutex lock;
    KeyIndex keys;
    uint32_t base{ 0 };           // numbers issued before the shard last emptied
    std::vector<uint32_t> links;  // parallel to `keys`: a score number or INVALID
  };

  // The shard holding a key of hash `hash`.
  static uint32_t shard_of(uint64_t hash) {
    return static_cast<uint32_t>(hash >> 20U) & (SHARDS - 1);
  }
  // The number for index `index` of shard `shard` at base `base`; INVALID past the
  // numbering.
  static uint32_t number(uint32_t base, uint32_t index, uint32_t shard) {
    uint64_t const n{ uint64_t{ base } + index };
    return (n < (INVALID >> SHARD_BITS))
               ? ((static_cast<uint32_t>(n) << SHARD_BITS) | shard)
               : INVALID;
  }
  // The index of number `e` in a shard at base `base` holding `size` keys; INVALID where
  // `e` was emptied away.
  static uint32_t index_of(uint32_t e, uint32_t base, size_t size) {
    uint32_t const n{ e >> SHARD_BITS };
    return ((e == INVALID) || (n < base) || ((n - base) >= size)) ? INVALID : (n - base);
  }
  // The number of `key[0..len)` in `table`, interning it if new; INVALID when it is full.
  uint32_t intern(std::array<IndexShard, SHARDS> &table,
                  uint32_t const *key,
                  uint32_t len);

  // Adds `n` bytes to `charged`, emptying every table past `budget`.
  void charge(uint64_t n);
  void empty();

  Chart const &chart;
  SplitGraph const &graph;
  uint32_t const serial;  // a `memo_serial` naming this memo in thread caches
  uint64_t const budget;
  bool usable{ true };                 // every state and segment number fits the encoding
  std::atomic<uint64_t> charged{ 0 };  // bytes the tables hold since they last emptied
  std::atomic<uint64_t> peak{ 0 };     // the most `charged` reached
  std::atomic<bool> emptying{ false };
  // Frame blocks: one per submachine, then one for segments and states in none.
  std::vector<uint32_t> seg_off, seg_list;      // block -> its segments
  std::vector<uint32_t> state_off, state_list;  // block -> its states
  // Block -> the submachines its states own, whose corners its geometry holds; those
  // owned by no state go in the last block.
  std::vector<uint32_t> owned_off, owned_list;

  Mutex row_lock;  // guards `rows` and `profiles`
  std::vector<Row> rows;
  std::vector<scav_profile> profiles;
  std::array<IndexShard, SHARDS> frames;
  std::array<IndexShard, SHARDS> arrangements;  // keys: every block's frame number
  std::array<IndexShard, SHARDS> shapes;
  std::array<IndexShard, SHARDS> geometries;
  std::array<IndexShard, SHARDS> drawings;  // keys: profile, chart, every block's geometry
  std::array<FacingShard, SHARDS> facings;
  std::array<ScoreShard, SHARDS> scores;    // keys: drawing, faces
  std::array<LinkShard, SHARDS> orderings;  // keys: row, arrangement, faces
};

}  // namespace scav

#endif  // SCAV_LAYOUT_CANDIDATE_MEMO_H_INCLUDED
