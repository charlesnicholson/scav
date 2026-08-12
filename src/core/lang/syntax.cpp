#include "scav/scav_core.h"

#include <cstdint>
#include <string_view>

namespace scav {

char const *syntax_elem_kind_name(ElemKind kind) {
  switch (kind) {
    case ElemKind::Chart: return "chart";
    case ElemKind::Include: return "include";
    case ElemKind::State: return "state";
    case ElemKind::Submachine: return "submachine";
    case ElemKind::Trans: return "trans";
    case ElemKind::Attr: return "attr";
  }
  return "unknown";
}

char const *syntax_state_kind_name(StateKind kind) {
  switch (kind) {
    case StateKind::Normal: return "normal";
    case StateKind::Initial: return "initial";
    case StateKind::Final: return "final";
    case StateKind::Choice: return "choice";
    case StateKind::Junction: return "junction";
    case StateKind::Fork: return "fork";
    case StateKind::Join: return "join";
    case StateKind::History: return "history";
    case StateKind::DeepHistory: return "deephistory";
  }
  return "unknown";
}

char const *syntax_trans_kind_name(TransKind kind) {
  switch (kind) {
    case TransKind::External: return "external";
    case TransKind::Internal: return "internal";
    case TransKind::Local: return "local";
  }
  return "unknown";
}

bool syntax_state_kind_from_name(std::string_view text, StateKind &out) {
  // `initial` and `final` are absent on purpose: the format reaches them only
  // through `*`, so accepting them here would add a second spelling.
  if (text == "normal") {
    out = StateKind::Normal;
    return true;
  }
  if (text == "choice") {
    out = StateKind::Choice;
    return true;
  }
  if (text == "junction") {
    out = StateKind::Junction;
    return true;
  }
  if (text == "fork") {
    out = StateKind::Fork;
    return true;
  }
  if (text == "join") {
    out = StateKind::Join;
    return true;
  }
  if (text == "history") {
    out = StateKind::History;
    return true;
  }
  if (text == "deephistory") {
    out = StateKind::DeepHistory;
    return true;
  }
  return false;
}

bool syntax_trans_kind_from_name(std::string_view text, TransKind &out) {
  if (text == "external") {
    out = TransKind::External;
    return true;
  }
  if (text == "internal") {
    out = TransKind::Internal;
    return true;
  }
  if (text == "local") {
    out = TransKind::Local;
    return true;
  }
  return false;
}

uint32_t syntax_root_statement(ParsedDocument const &pd) {
  // The chart statement is created first, so it is always row zero.
  return pd.stmts.empty() ? INVALID : 0U;
}

}  // namespace scav
