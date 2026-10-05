#include "layout/candidate_memo.h"

#include "scav_int.h"
#include "scav_vec.h"

#include <algorithm>
#include <cstring>

namespace scav {

namespace {

constexpr uint32_t ID_LIMIT{ 1U << 28 };   // every state and segment number is below it
constexpr uint32_t LOCAL_LIMIT{ 0xFFFF };  // frame-local node numbers, and the none mark
constexpr int32_t TAG_UNSET{ -1 };
constexpr int32_t TAG_NOT_VIABLE{ -2 };
constexpr int32_t TAG_INFLATED{ -3 };

// Groups items `0..count` into `blocks` blocks by `block_of`, any past the last going in
// the last: block b's items, ascending, are `list[off[b]..off[b + 1])`.
template <typename BlockOf>
void group(uint32_t count,
           uint32_t blocks,
           BlockOf const &block_of,
           std::vector<uint32_t> &off,
           std::vector<uint32_t> &list) {
  auto const at = [&](uint32_t i) { return imin(block_of(i), blocks - 1); };
  vec_assign(off, size_t{ blocks } + 1, 0);
  for (uint32_t i = 0; i < count; ++i) { ++off[at(i) + 1]; }
  for (uint32_t b = 0; b < blocks; ++b) { off[b + 1] += off[b]; }
  std::vector<uint32_t> fill(off.begin(), off.end() - 1);
  vec_assign(list, count, 0);
  for (uint32_t i = 0; i < count; ++i) { list[fill[at(i)]++] = i; }
}

}  // namespace

CandidateMemo::CandidateMemo(Chart const &c, SplitGraph const &g)
    : chart(c), graph(g), serial(memo_serial()) {
  usable = (c.states.size() < ID_LIMIT) && (g.segments.size() < ID_LIMIT) &&
           (c.submachines.size() < ID_LIMIT);
  auto const blocks{ static_cast<uint32_t>(c.submachines.size()) + 1 };
  group(
      static_cast<uint32_t>(g.segments.size()),
      blocks,
      [&g](uint32_t seg) { return g.segments[seg].frame.v; },
      seg_off,
      seg_list);
  group(
      static_cast<uint32_t>(c.states.size()),
      blocks,
      [&c](uint32_t st) { return c.states[st].parent.v; },
      state_off,
      state_list);
}

uint32_t CandidateMemo::row_word(Row const &row) {
  ScopedLock const held{ row_lock };
  for (uint32_t i = 0; i < rows.size(); ++i) {
    Row const &r{ rows[i] };
    if ((std::memcmp(&r.knobs, &row.knobs, sizeof(scav_profile)) == 0) &&
        (r.dar == row.dar) && (r.pack == row.pack) && (r.fold == row.fold)) {
      return i;
    }
  }
  vec_push_back(rows, row);
  return static_cast<uint32_t>(rows.size() - 1);
}

bool CandidateMemo::put_frame(SubmachineOrders const &o,
                              uint32_t m,
                              std::vector<uint32_t> &w) const {
  uint32_t node_off{ 0 };
  uint32_t node_len{ 0 };
  if (m < chart.submachines.size()) {
    Span const ns{ o.sub_nodes[m] };
    Span const es{ o.sub_edges[m] };
    Span const gs{ o.sub_gaps[m] };
    uint32_t const ranks{ o.sub_ranks[m] };
    if ((ns.len >= LOCAL_LIMIT) || (es.len >= LOCAL_LIMIT)) { return false; }
    node_off = ns.off;
    node_len = ns.len;
    vec_insert(w,
               w.end(),
               { ranks,
                 uint32_t{ o.sub_down[m] } | (uint32_t{ o.sub_fold[m] } << 8U),
                 o.sub_fold_cut[m],
                 ns.len | (es.len << 16U),
                 gs.len });
    // Nodes run in rank order with `pos` counting up from 0 in each rank; the counts per
    // rank and the nodes in order give back every rank and pos.
    size_t const counts_at{ w.size() };
    vec_resize(w, w.size() + ((size_t{ ranks } + 1) / 2), 0U);
    uint32_t last{ 0 };
    uint32_t pos{ 0 };
    for (uint32_t k = 0; k < ns.len; ++k) {
      OrderNode const &nd{ o.nodes[ns.off + k] };
      uint32_t const rank{ nd.rank };
      if ((rank >= ranks) || (rank < last) || (nd.subject >= ID_LIMIT)) { return false; }
      if (rank != last) {
        last = rank;
        pos = 0;
      }
      if (nd.pos != pos) { return false; }
      ++pos;
      w[counts_at + (rank / 2)] += ((rank % 2) == 0) ? 1U : (1U << 16U);
      vec_push_back(w, (nd.subject << 2U) | static_cast<uint32_t>(nd.kind));
    }
    for (uint32_t k = 0; k < es.len; ++k) {
      OrderEdge const &e{ o.edges[es.off + k] };
      uint32_t const src{ e.src - ns.off };
      uint32_t const dst{ e.dst - ns.off };
      if ((e.src < ns.off) || (e.dst < ns.off) || (src >= ns.len) || (dst >= ns.len) ||
          (e.reversed > 1) || (e.segment >= ID_LIMIT)) {
        return false;
      }
      vec_push_back(w, src | (dst << 16U));
      vec_push_back(w, (e.segment << 1U) | e.reversed);
    }
    for (uint32_t k = 0; k < gs.len; ++k) {
      vec_push_back(w, static_cast<uint32_t>(o.gaps[gs.off + k]));
      vec_push_back(w, static_cast<uint32_t>(o.labels[gs.off + k]));
    }
  }
  for (uint32_t k = seg_off[m]; k < seg_off[m + 1]; ++k) {
    uint32_t const seg{ seg_list[k] };
    uint32_t const node{ o.seg_node[seg] };
    uint32_t local{ LOCAL_LIMIT };
    if (node != INVALID) {
      local = node - node_off;
      if ((node < node_off) || (local >= node_len)) { return false; }
    }
    vec_push_back(w, o.seg_port[seg]);
    vec_push_back(w,
                  (local << 16U) | (uint32_t{ o.seg_cross[seg] } << 8U) |
                      uint32_t{ o.seg_side[seg] });
  }
  uint32_t packed{ 0 };
  uint32_t shift{ 0 };
  for (uint32_t k = state_off[m]; k < state_off[m + 1]; ++k) {
    packed |= uint32_t{ o.state_loop[state_list[k]] } << shift;
    shift += 8;
    if (shift == 32) {
      vec_push_back(w, packed);
      packed = 0;
      shift = 0;
    }
  }
  if (shift != 0) { vec_push_back(w, packed); }
  return true;
}

namespace {

// One thread's encoding of every block, back to back, and their numbers in memo `serial`.
struct BlockWords {
  uint32_t serial{ 0 };
  std::vector<uint32_t> words;
  std::vector<uint32_t> ends;  // block -> one past its last word
  std::vector<uint32_t> ids;
};

}  // namespace

bool CandidateMemo::frame_ids(SubmachineOrders const &o, std::vector<uint32_t> &ids) {
  if (!usable) { return false; }
  // `now` encodes `o`; `last` holds the thread's previous call, whose unchanged blocks
  // keep their numbers without the lock.
  thread_local BlockWords now;
  thread_local BlockWords last;
  thread_local std::vector<uint32_t> fresh;
  now.words.clear();
  now.ends.clear();
  auto const blocks{ static_cast<uint32_t>(chart.submachines.size()) + 1 };
  for (uint32_t m = 0; m < blocks; ++m) {
    if (!put_frame(o, m, now.words)) { return false; }
    vec_push_back(now.ends, static_cast<uint32_t>(now.words.size()));
  }
  bool const known{ (last.serial == serial) && (last.ids.size() == blocks) };
  vec_resize(now.ids, blocks);
  fresh.clear();
  for (uint32_t m = 0; m < blocks; ++m) {
    uint32_t const at{ (m == 0) ? 0U : now.ends[m - 1] };
    uint32_t const len{ now.ends[m] - at };
    uint32_t const was{ (known && (m > 0)) ? last.ends[m - 1] : 0U };
    bool const same{ known && ((last.ends[m] - was) == len) &&
                     ((len == 0) ||
                      (std::memcmp(last.words.data() + was,
                                   now.words.data() + at,
                                   size_t{ len } * sizeof(uint32_t)) == 0)) };
    if (same) {
      now.ids[m] = last.ids[m];
    } else {
      vec_push_back(fresh, m);
    }
  }
  for (uint32_t const m : fresh) {
    uint32_t const at{ (m == 0) ? 0U : now.ends[m - 1] };
    uint32_t const len{ now.ends[m] - at };
    uint32_t const *const key{ now.words.data() + at };
    uint64_t const hash{ memo_hash(key, len) };
    uint32_t const shard{ shard_of(hash) };
    FrameShard &f{ frames[shard] };
    ScopedLock const held{ f.lock };
    uint32_t index{ f.keys.find(key, len, hash) };
    if (index == INVALID) { index = f.keys.insert(key, len, hash); }
    now.ids[m] = (index == INVALID) ? INVALID : number(index, shard);
    if (now.ids[m] == INVALID) { return false; }
  }
  now.serial = serial;
  std::swap(now, last);
  vec_assign(ids, last.ids.begin(), last.ids.end());
  return true;
}

namespace {

// Row, frame numbers, then `seg_sided` four segments to a word.
void facing_key(uint32_t row,
                std::vector<uint32_t> const &ids,
                SubmachineOrders const &o,
                std::vector<uint32_t> &key) {
  key.clear();
  vec_push_back(key, row);
  vec_insert(key, key.end(), ids.begin(), ids.end());
  uint32_t packed{ 0 };
  uint32_t shift{ 0 };
  for (uint8_t const sided : o.seg_sided) {
    packed |= uint32_t{ sided } << shift;
    shift += 8;
    if (shift == 32) {
      vec_push_back(key, packed);
      packed = 0;
      shift = 0;
    }
  }
  if (shift != 0) { vec_push_back(key, packed); }
}

}  // namespace

FacingFound CandidateMemo::find_facing(uint32_t row,
                                       std::vector<uint32_t> const &ids,
                                       SubmachineOrders const &o,
                                       Facing &out) {
  thread_local std::vector<uint32_t> key;
  facing_key(row, ids, o, key);
  uint64_t const hash{ memo_hash(key) };
  FacingShard &f{ facings[shard_of(hash)] };
  ScopedLock const held{ f.lock };
  uint32_t const at{ f.keys.find(key.data(), static_cast<uint32_t>(key.size()), hash) };
  if (at == INVALID) { return FacingFound::Absent; }
  FacingRecord const &r{ f.records[at] };
  if (r.off == INVALID) { return FacingFound::Failed; }
  vec_resize(out.reverses, r.reverses);
  vec_resize(out.sides, r.sides);
  uint32_t w{ r.off };
  for (ReversePin &p : out.reverses) {
    p = { .trans = TransId{ f.turns[w] }, .leg = f.turns[w + 1] };
    w += 2;
  }
  for (EndPin &p : out.sides) {
    p = { .trans = TransId{ f.turns[w] },
          .leg = f.turns[w + 1],
          .end = f.turns[w + 2] / 4,
          .face = f.turns[w + 2] % 4 };
    w += 3;
  }
  return FacingFound::Turned;
}

void CandidateMemo::store_facing(uint32_t row,
                                 std::vector<uint32_t> const &ids,
                                 SubmachineOrders const &o,
                                 Facing const *turned) {
  thread_local std::vector<uint32_t> key;
  facing_key(row, ids, o, key);
  uint64_t const hash{ memo_hash(key) };
  // A side's end and face share one word.
  if (turned != nullptr) {
    for (EndPin const &p : turned->sides) {
      if ((p.end > 1) || (p.face > 3)) { return; }
    }
  }
  FacingShard &f{ facings[shard_of(hash)] };
  ScopedLock const held{ f.lock };
  auto const len{ static_cast<uint32_t>(key.size()) };
  if (f.keys.find(key.data(), len, hash) != INVALID) { return; }
  if (f.keys.insert(key.data(), len, hash) == INVALID) { return; }
  if (turned == nullptr) {
    vec_push_back(f.records, { .off = INVALID, .reverses = 0, .sides = 0 });
    return;
  }
  vec_push_back(f.records,
                { .off = static_cast<uint32_t>(f.turns.size()),
                  .reverses = static_cast<uint32_t>(turned->reverses.size()),
                  .sides = static_cast<uint32_t>(turned->sides.size()) });
  for (ReversePin const &p : turned->reverses) {
    vec_insert(f.turns, f.turns.end(), { p.trans.v, p.leg });
  }
  for (EndPin const &p : turned->sides) {
    vec_insert(f.turns, f.turns.end(), { p.trans.v, p.leg, (p.end * 4) + p.face });
  }
}

CandidateMemo::Recalled CandidateMemo::find_score(uint32_t row,
                                                  std::vector<uint32_t> const &ids,
                                                  SearchPins const *pins,
                                                  bool labelled) {
  thread_local std::vector<uint32_t> key;
  thread_local std::vector<uint32_t> faces;
  // The router's box-end faces, as `route_transitions` reads them: the last pin naming an
  // end decides it; one word per end, sorted.
  faces.clear();
  static std::vector<EndPin> const NONE;
  for (EndPin const &fp : (pins != nullptr) ? pins->ends : NONE) {
    if ((fp.trans.v == INVALID) || (fp.trans.v >= graph.trans_segments.size()) ||
        (fp.end > 1) || (fp.face > 3)) {
      continue;
    }
    Span const segs{ graph.trans_segments[fp.trans.v] };
    if (fp.leg >= segs.len) { continue; }
    uint32_t const seg{ segs.off + fp.leg };
    if (((fp.end == 0) ? graph.segments[seg].src_port : graph.segments[seg].dst_port) !=
        INVALID) {
      continue;
    }
    uint32_t const end{ (seg << 3U) | (fp.end << 2U) };
    auto const had{ std::ranges::find_if(faces,
                                         [end](uint32_t f) { return (f & ~3U) == end; }) };
    if (had != faces.end()) {
      *had = end | fp.face;
    } else {
      vec_push_back(faces, end | fp.face);
    }
  }
  std::ranges::sort(faces);
  key.clear();
  vec_push_back(key, row);
  vec_insert(key, key.end(), ids.begin(), ids.end());
  vec_insert(key, key.end(), faces.begin(), faces.end());
  uint64_t const hash{ memo_hash(key) };
  uint32_t const shard{ shard_of(hash) };
  ScoreShard &sh{ scores[shard] };
  Recalled out;
  ScopedLock const held{ sh.lock };
  auto const len{ static_cast<uint32_t>(key.size()) };
  uint32_t index{ sh.keys.find(key.data(), len, hash) };
  if (index != INVALID) {
    ScoreRecord const &r{ sh.records[index] };
    out.entry = number(index, shard);
    out.labelled = labelled || (r.t0[0] == TAG_UNSET);
    out.found = read(r, out.labelled ? 1U : 0U, out.score);
    return out;
  }
  index = sh.keys.insert(key.data(), len, hash);
  if (index != INVALID) {
    out.entry = number(index, shard);
    vec_push_back(
        sh.records,
        { .t0 = { TAG_UNSET, TAG_UNSET }, .t2_hi = { 0, 0 }, .t2_lo = { 0, 0 } });
  }
  return out;
}

bool CandidateMemo::score(uint32_t e, bool labelled, MemoScore &out) {
  if (e == INVALID) { return false; }
  ScoreShard &sh{ scores[e & (SHARDS - 1)] };
  ScopedLock const held{ sh.lock };
  return read(sh.records[e >> SHARD_BITS], labelled ? 1U : 0U, out);
}

bool CandidateMemo::read(ScoreRecord const &r, uint32_t k, MemoScore &out) {
  int32_t const t0{ r.t0[k] };
  if (t0 == TAG_UNSET) { return false; }
  out = MemoScore{};
  out.viable = t0 != TAG_NOT_VIABLE;
  out.inflated = t0 == TAG_INFLATED;
  if (t0 >= 0) {
    out.cost.t0_violations = t0;
    out.cost.t2 =
        static_cast<int64_t>((uint64_t{ r.t2_hi[k] } << 32U) | uint64_t{ r.t2_lo[k] });
  }
  return true;
}

void CandidateMemo::set_score(uint32_t e, bool labelled, MemoScore const &s) {
  bool const scored{ s.viable && !s.inflated };
  if ((e == INVALID) ||
      (scored && ((s.cost.t0_violations < 0) || (s.cost.t1_hints != 0)))) {
    return;
  }
  int32_t t0{ s.viable ? TAG_INFLATED : TAG_NOT_VIABLE };
  if (scored) { t0 = s.cost.t0_violations; }
  auto const t2{ static_cast<uint64_t>(scored ? s.cost.t2 : 0) };
  uint32_t const k{ labelled ? 1U : 0U };
  ScoreShard &sh{ scores[e & (SHARDS - 1)] };
  ScopedLock const held{ sh.lock };
  ScoreRecord &r{ sh.records[e >> SHARD_BITS] };
  r.t0[k] = t0;
  r.t2_hi[k] = static_cast<uint32_t>(t2 >> 32U);
  r.t2_lo[k] = static_cast<uint32_t>(t2);
}

size_t CandidateMemo::bytes() {
  size_t out{ 0 };
  {
    ScopedLock const held{ row_lock };
    out += rows.capacity() * sizeof(Row);
  }
  for (FrameShard &f : frames) {
    ScopedLock const held{ f.lock };
    out += f.keys.bytes();
  }
  for (FacingShard &f : facings) {
    ScopedLock const held{ f.lock };
    out += f.keys.bytes() + (f.records.capacity() * sizeof(FacingRecord)) +
           (f.turns.capacity() * sizeof(uint32_t));
  }
  for (ScoreShard &sh : scores) {
    ScopedLock const held{ sh.lock };
    out += sh.keys.bytes() + (sh.records.capacity() * sizeof(ScoreRecord));
  }
  return out;
}

}  // namespace scav
