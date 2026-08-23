// The space tables: domain validation attributing each failure to its request,
// and the digest that makes a measurement policy a hashed layout input.

#include "layout/wire.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_types.h"
#include "scav_stable_sort.h"
#include "scav_xxhash.h"

#include <cstdint>
#include <vector>

namespace scav {

namespace {

bool in_domain(int32_t v) { return (v >= 0) && (v <= SPACE_MAX); }

void report(std::vector<Diagnostic> &out, DiagCode code, ElemKind kind, uint32_t ordinal) {
  out.push_back({ .code = code,
                  .subject = { .kind = kind, .ordinal = ordinal },
                  .doc = { INVALID },
                  .src = {} });
}

// A parallel table's count either matches its entity array or is zero, and a
// null pointer carries no rows.
bool check_count(std::vector<Diagnostic> &out,
                 void const *rows,
                 uint32_t count,
                 uint32_t entities) {
  if ((count != 0) && ((count != entities) || (rows == nullptr))) {
    report(out, DiagCode::SpaceCountMismatch, ElemKind::Chart, 0);
    return false;
  }
  return true;
}

void check_boxes(std::vector<Diagnostic> &out,
                 scav_box_space const *rows,
                 uint32_t count,
                 ElemKind kind) {
  for (uint32_t i = 0; i < count; ++i) {
    if (!in_domain(rows[i].min_w) || !in_domain(rows[i].h_before) ||
        !in_domain(rows[i].h_after)) {
      report(out, DiagCode::SpaceOutOfRange, kind, i);
    }
  }
}

}  // namespace

bool spaces_validate(Chart const &c,
                     scav_spaces const &s,
                     std::vector<Diagnostic> &diags) {
  std::vector<Diagnostic> found;

  if (check_count(found,
                  s.box_state,
                  s.n_box_state,
                  chart_entity_count(c, ElemKind::State))) {
    check_boxes(found, s.box_state, s.n_box_state, ElemKind::State);
  }
  if (check_count(found,
                  s.box_sub,
                  s.n_box_sub,
                  chart_entity_count(c, ElemKind::Submachine))) {
    check_boxes(found, s.box_sub, s.n_box_sub, ElemKind::Submachine);
  }

  uint32_t const transitions{ chart_entity_count(c, ElemKind::Transition) };
  if (check_count(found, s.path_clear, s.n_path_clear, transitions)) {
    for (uint32_t i = 0; i < s.n_path_clear; ++i) {
      if (!in_domain(s.path_clear[i].src) || !in_domain(s.path_clear[i].dst)) {
        report(found, DiagCode::SpaceOutOfRange, ElemKind::Transition, i);
      }
    }
  }

  if ((s.path_box == nullptr) && (s.n_path_box != 0)) {
    report(found, DiagCode::SpaceCountMismatch, ElemKind::Chart, 0);
  } else {
    // A transition that gets no route -- an internal or local self-loop --
    // has nothing to slide a box along.
    auto const routeless = [&c](uint32_t t) {
      return (c.transitions[t].src == c.transitions[t].dst) &&
             (c.transitions[t].kind != TransKind::External);
    };
    for (uint32_t i = 0; i < s.n_path_box; ++i) {
      scav_path_box const &box{ s.path_box[i] };
      uint32_t subject{ box.subject };
      if ((subject >= transitions) || (c.transitions[subject].live == 0) ||
          routeless(subject)) {
        report(found,
               DiagCode::SpaceSubjectInvalid,
               ElemKind::Transition,
               (subject >= transitions) ? INVALID : subject);
        subject = INVALID;
      }
      if (!in_domain(box.w) || !in_domain(box.h)) {
        report(found, DiagCode::SpaceOutOfRange, ElemKind::Transition, subject);
      }
    }

    // Uniqueness of (subject, order) by sorting indices, so detection order is
    // the data's and not a hash table's, and rows are not copied.
    std::vector<uint32_t> by_key(s.n_path_box);
    for (uint32_t i = 0; i < s.n_path_box; ++i) { by_key[i] = i; }
    scav_stable_sort(by_key, [&s](uint32_t a, uint32_t b) {
      if (s.path_box[a].subject != s.path_box[b].subject) {
        return s.path_box[a].subject < s.path_box[b].subject;
      }
      return s.path_box[a].order < s.path_box[b].order;
    });
    for (uint32_t i = 1; i < by_key.size(); ++i) {
      scav_path_box const &cur{ s.path_box[by_key[i]] };
      scav_path_box const &prev{ s.path_box[by_key[i - 1]] };
      if ((cur.subject == prev.subject) && (cur.order == prev.order)) {
        report(found, DiagCode::SpaceOrderDuplicate, ElemKind::Transition, cur.subject);
      }
    }
  }

  // A total order over the triple; stability keeps equal triples in scan order.
  scav_stable_sort(found, [](Diagnostic const &a, Diagnostic const &b) {
    if (a.code != b.code) {
      return static_cast<uint32_t>(a.code) < static_cast<uint32_t>(b.code);
    }
    if (a.subject.kind != b.subject.kind) {
      return static_cast<uint32_t>(a.subject.kind) < static_cast<uint32_t>(b.subject.kind);
    }
    return a.subject.ordinal < b.subject.ordinal;
  });

  bool const clean{ found.empty() };
  diags.insert(diags.end(), found.begin(), found.end());
  return clean;
}

uint32_t spaces_digest(scav_spaces const &s) {
  // Field by field, never a struct's bytes, with each table's count prefixed
  // so two adjacent tables cannot spell one.
  std::vector<scav_byte> bytes;
  bytes.reserve(16 + (12ULL * (s.n_box_state + s.n_box_sub)) + (8ULL * s.n_path_clear) +
                (16ULL * s.n_path_box));
  append_u32(bytes, s.n_box_state);
  for (uint32_t i = 0; i < s.n_box_state; ++i) {
    append_i32(bytes, s.box_state[i].min_w);
    append_i32(bytes, s.box_state[i].h_before);
    append_i32(bytes, s.box_state[i].h_after);
  }
  append_u32(bytes, s.n_box_sub);
  for (uint32_t i = 0; i < s.n_box_sub; ++i) {
    append_i32(bytes, s.box_sub[i].min_w);
    append_i32(bytes, s.box_sub[i].h_before);
    append_i32(bytes, s.box_sub[i].h_after);
  }
  append_u32(bytes, s.n_path_clear);
  for (uint32_t i = 0; i < s.n_path_clear; ++i) {
    append_i32(bytes, s.path_clear[i].src);
    append_i32(bytes, s.path_clear[i].dst);
  }
  append_u32(bytes, s.n_path_box);
  for (uint32_t i = 0; i < s.n_path_box; ++i) {
    append_u32(bytes, s.path_box[i].subject);
    append_i32(bytes, s.path_box[i].w);
    append_i32(bytes, s.path_box[i].h);
    append_u32(bytes, s.path_box[i].order);
  }
  return xxhash32(bytes.data(), bytes.size(), 0);
}

}  // namespace scav
