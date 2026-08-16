// Serializes the whole structure, then hashes it once. Field by field, strings
// length-prefixed, and a span contributing its contents rather than its offset.

#include "scav_xxhash.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

namespace {

void put_u32(std::vector<scav_byte> &out, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    out.push_back(static_cast<scav_byte>((value >> (i * 8U)) & 0xFFU));
  }
}

void put_text(std::vector<scav_byte> &out, std::string_view text) {
  put_u32(out, narrow_clamp<uint32_t>(text.size()));
  for (char const ch : text) { out.push_back(static_cast<scav_byte>(ch)); }
}

// Key bytes, never the interned id. An AttrKeyId is first-encounter order, so
// two producers of one model can hold different ids for the same key.
void put_attrs(std::vector<scav_byte> &out, Chart const &c, Span attrs) {
  put_u32(out, attrs.len);
  for (uint32_t i = 0; i < attrs.len; ++i) {
    if ((static_cast<uint64_t>(attrs.off) + i) >= c.attrs.size()) { return; }
    Attr const &a{ c.attrs[attrs.off + i] };
    put_text(out, chart_attr_key(c, a.key));
    put_text(out, chart_string(c, a.value));
  }
}

void put_states(std::vector<scav_byte> &out, Chart const &c, Span span) {
  put_u32(out, span.len);
  for (uint32_t i = 0; i < span.len; ++i) {
    if ((static_cast<uint64_t>(span.off) + i) >= c.state_ids.size()) { return; }
    put_u32(out, c.state_ids[span.off + i].v);
  }
}

void put_submachines(std::vector<scav_byte> &out, Chart const &c, Span span) {
  put_u32(out, span.len);
  for (uint32_t i = 0; i < span.len; ++i) {
    if ((static_cast<uint64_t>(span.off) + i) >= c.submachine_ids.size()) { return; }
    put_u32(out, c.submachine_ids[span.off + i].v);
  }
}

}  // namespace

void chart_digest_bytes(Chart const &c, std::vector<scav_byte> &out) {
  out.clear();

  put_text(out, chart_string(c, c.name));
  put_text(out, chart_string(c, c.label));
  put_u32(out, c.root_submachine.v);
  put_attrs(out, c, c.chart_attrs);

  put_u32(out, narrow_clamp<uint32_t>(c.states.size()));
  for (State const &s : c.states) {
    put_text(out, chart_string(c, s.name));
    put_text(out, chart_string(c, s.label));
    put_u32(out, s.parent.v);
    put_u32(out, static_cast<uint32_t>(s.kind));
    put_u32(out, s.inst.v);
    put_u32(out, s.live);
    put_submachines(out, c, s.submachines);
    put_attrs(out, c, s.attrs);
  }

  put_u32(out, narrow_clamp<uint32_t>(c.submachines.size()));
  for (Submachine const &m : c.submachines) {
    put_u32(out, m.owner.v);
    put_u32(out, m.ordinal);
    put_text(out, chart_string(c, m.name));
    put_text(out, chart_string(c, m.label));
    put_u32(out, m.inst.v);
    put_u32(out, m.live);
    put_states(out, c, m.children);
    put_attrs(out, c, m.attrs);
  }

  put_u32(out, narrow_clamp<uint32_t>(c.transitions.size()));
  for (Transition const &t : c.transitions) {
    put_u32(out, t.src.v);
    put_u32(out, t.dst.v);
    put_u32(out, static_cast<uint32_t>(t.kind));
    put_text(out, chart_string(c, t.label));
    put_u32(out, t.inst.v);
    put_u32(out, t.live);
    put_attrs(out, c, t.attrs);
  }

  // The authored path, which is the same text in every transport, unlike
  // documents[target].path.
  put_u32(out, narrow_clamp<uint32_t>(c.includes.size()));
  for (Include const &inc : c.includes) {
    put_text(out, chart_string(c, inc.alias));
    put_text(out, chart_string(c, inc.path));
    put_u32(out, inc.target.v);
    put_u32(out, inc.host.v);
  }
}

uint32_t chart_structural_hash(Chart const &c) {
  std::vector<scav_byte> digest;
  chart_digest_bytes(c, digest);
  return xxhash32(digest.data(), digest.size(), 0);
}

}  // namespace scav
