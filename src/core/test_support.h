#ifndef SCAV_CORE_TEST_SUPPORT_H_INCLUDED
#define SCAV_CORE_TEST_SUPPORT_H_INCLUDED

// Shared by the core unit tests. Header-only and test-only, so the coverage gate
// does not count it as production. Charts are inline literals: a parser test that
// opens a file is testing the filesystem too.

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace scav::test {

inline scav_byte const *raw(std::string_view text) {
  return reinterpret_cast<scav_byte const *>(text.data());
}

inline uint32_t size32(std::string_view text) {
  return static_cast<uint32_t>(text.size());
}

struct Parsed {
  ParsedDocument pd;
  std::vector<Diagnostic> diags;
  bool ok;
};

inline Parsed parse(std::string_view text, std::string_view name = "test.scav") {
  Parsed r;
  r.ok = parse_document(raw(text),
                        size32(text),
                        name,
                        parse_default_options(),
                        r.pd,
                        r.diags);
  return r;
}

inline Parsed parse_deep(std::string_view text, uint32_t max_depth) {
  Parsed r;
  ParseOptions const opts{ .max_depth = max_depth };
  r.ok = parse_document(raw(text), size32(text), "test.scav", opts, r.pd, r.diags);
  return r;
}

struct Lexed {
  LexResult result;
  std::vector<Diagnostic> diags;
  bool ok;
  std::vector<scav_byte> bytes;
};

// Normalizes first, so a test's offsets mean the same thing they do in the
// parser.
inline Lexed lex_text(std::string_view text) {
  Lexed r;
  std::vector<Diagnostic> norm;
  DocId const doc{ 0 };
  r.ok = source_text_normalize(raw(text), size32(text), doc, r.bytes, norm);
  r.diags = norm;
  if (!r.ok) { return r; }
  r.ok = lex_source(r.bytes.data(),
                    static_cast<uint32_t>(r.bytes.size()),
                    doc,
                    r.result,
                    r.diags);
  return r;
}

inline std::string_view str(ParsedDocument const &pd, StrRef ref) {
  return string_pool_view(pd.strings, ref);
}

inline std::string_view src(ParsedDocument const &pd, Span span) {
  if (span.len == 0) { return {}; }
  return { reinterpret_cast<char const *>(pd.src_bytes.data() + span.off), span.len };
}

// The first diagnostic's code, or Ok when there were none. Every error test
// asserts the code rather than the message, because the message is prose.
inline DiagCode first_code(std::vector<Diagnostic> const &diags) {
  return diags.empty() ? DiagCode::Ok : diags[0].code;
}

inline bool has_code(std::vector<Diagnostic> const &diags, DiagCode code) {
  for (Diagnostic const &d : diags) {
    if (d.code == code) { return true; }
  }
  return false;
}

// Statement rows of one kind, in document order.
inline std::vector<uint32_t> stmts_of(ParsedDocument const &pd, StmtKind kind) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < pd.stmts.size(); ++i) {
    if (pd.stmts[i].kind == kind) { out.push_back(i); }
  }
  return out;
}

inline StateStmt const &state_at(ParsedDocument const &pd, uint32_t stmt) {
  return pd.states[pd.stmt_payload[stmt]];
}

inline TransStmt const &trans_at(ParsedDocument const &pd, uint32_t stmt) {
  return pd.transitions[pd.stmt_payload[stmt]];
}

inline SubmachineStmt const &submachine_at(ParsedDocument const &pd, uint32_t stmt) {
  return pd.submachines[pd.stmt_payload[stmt]];
}

inline AttrStmt const &attr_at(ParsedDocument const &pd, uint32_t stmt) {
  return pd.attrs[pd.stmt_payload[stmt]];
}

inline IncludeStmt const &include_at(ParsedDocument const &pd, uint32_t stmt) {
  return pd.includes[pd.stmt_payload[stmt]];
}

// Model refs, spelled short so an assertion reads like its claim.
inline ElemRef ref(StateId id) { return { .kind = ElemKind::State, .ordinal = id.v }; }
inline ElemRef ref(SubmachineId id) {
  return { .kind = ElemKind::Submachine, .ordinal = id.v };
}
inline ElemRef ref(TransId id) {
  return { .kind = ElemKind::Transition, .ordinal = id.v };
}
inline ElemRef chart_ref() { return { .kind = ElemKind::Chart, .ordinal = 0 }; }

inline std::string path(Chart const &c, StateId id) {
  std::string out;
  chart_path_of(c, id, out);
  return out;
}

// A path spelled back out, so an endpoint assertion reads like the source did.
inline std::string path_text(ParsedDocument const &pd, Endpoint const &e) {
  if (e.wildcard != 0) { return "*"; }
  std::string out;
  for (uint32_t i = 0; i < e.segs.len; ++i) {
    PathSeg const &seg{ pd.path_segs[e.segs.off + i] };
    if (i != 0) { out.push_back('/'); }
    out += str(pd, seg.name);
    if (seg.qualifier.len != 0) {
      out.push_back(':');
      out += str(pd, seg.qualifier);
    } else if (seg.ordinal != INVALID) {
      out.push_back(':');
      out += std::to_string(seg.ordinal);
    }
  }
  return out;
}

}  // namespace scav::test

#endif  // SCAV_CORE_TEST_SUPPORT_H_INCLUDED
