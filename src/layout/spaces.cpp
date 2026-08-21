// The space tables: domain validation attributing each failure to the request
// that caused it, and the digest that makes a measurement policy a hashed
// layout input.

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

void find(std::vector<Diagnostic> &out, DiagCode code, ElemKind kind, uint32_t ordinal) {
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
    find(out, DiagCode::SpaceCountMismatch, ElemKind::Chart, 0);
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
      find(out, DiagCode::SpaceOutOfRange, kind, i);
    }
  }
}

}  // namespace

bool spaces_validate(Chart const &c, Spaces const &s, std::vector<Diagnostic> &diags) {
  std::vector<Diagnostic> found;

  if (check_count(found, s.box_state, s.n_box_state,
                  chart_entity_count(c, ElemKind::State))) {
    check_boxes(found, s.box_state, s.n_box_state, ElemKind::State);
  }
  if (check_count(found, s.box_sub, s.n_box_sub,
                  chart_entity_count(c, ElemKind::Submachine))) {
    check_boxes(found, s.box_sub, s.n_box_sub, ElemKind::Submachine);
  }

  uint32_t const transitions{ chart_entity_count(c, ElemKind::Transition) };
  if (check_count(found, s.path_clear, s.n_path_clear, transitions)) {
    for (uint32_t i = 0; i < s.n_path_clear; ++i) {
      if (!in_domain(s.path_clear[i].src) || !in_domain(s.path_clear[i].dst)) {
        find(found, DiagCode::SpaceOutOfRange, ElemKind::Transition, i);
      }
    }
  }

  if ((s.path_box == nullptr) && (s.n_path_box != 0)) {
    find(found, DiagCode::SpaceCountMismatch, ElemKind::Chart, 0);
  } else {
    for (uint32_t i = 0; i < s.n_path_box; ++i) {
      scav_path_box const &box{ s.path_box[i] };
      uint32_t subject{ box.subject };
      if ((subject >= transitions) || !c.transitions[subject].live) {
        find(found, DiagCode::SpaceSubjectInvalid, ElemKind::Transition,
             (subject >= transitions) ? INVALID : subject);
        subject = INVALID;
      }
      if (!in_domain(box.w) || !in_domain(box.h)) {
        find(found, DiagCode::SpaceOutOfRange, ElemKind::Transition, subject);
      }
    }

    // Uniqueness of (subject, order) by sort-and-scan, so detection order is
    // the data's and not a hash table's.
    std::vector<scav_path_box> boxes(s.path_box, s.path_box + s.n_path_box);
    scav_stable_sort(boxes, [](scav_path_box const &a, scav_path_box const &b) {
      if (a.subject != b.subject) { return a.subject < b.subject; }
      return a.order < b.order;
    });
    for (uint32_t i = 1; i < boxes.size(); ++i) {
      if ((boxes[i].subject == boxes[i - 1].subject) &&
          (boxes[i].order == boxes[i - 1].order)) {
        find(found, DiagCode::SpaceOrderDuplicate, ElemKind::Transition,
             boxes[i].subject);
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

namespace {

void append_u32(std::vector<scav_byte> &out, uint32_t v) {
  out.push_back(static_cast<scav_byte>(v & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 8U) & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 16U) & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 24U) & 0xFFU));
}

void append_i32(std::vector<scav_byte> &out, int32_t v) {
  append_u32(out, static_cast<uint32_t>(v));
}

}  // namespace

uint32_t spaces_digest(Spaces const &s) {
  // Field by field, never a struct's bytes, with each table's count prefixed
  // so two adjacent tables cannot spell one.
  std::vector<scav_byte> bytes;
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
