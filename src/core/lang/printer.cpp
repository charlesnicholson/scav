#include "core/core_internal.h"
#include "scav/scav_core.h"

#include "scav_stable_sort.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scav {

namespace {

constexpr std::string_view INDENT_UNIT{ "  " };

uint32_t count_cp(std::string_view text) {
  uint32_t n{ 0 };
  for (char const c : text) {
    if ((static_cast<scav_byte>(c) & 0xC0U) != 0x80U) { ++n; }  // not a continuation
  }
  return n;
}

std::string_view pool_of(ParsedDocument const &pd, StrRef ref) {
  return string_pool_view(pd.strings, ref);
}

// The authored bytes, "//" included. The lexed span runs to the newline, so any
// trailing blanks come off here.
std::string_view comment_of(ParsedDocument const &pd, uint32_t index) {
  if (index >= pd.comments.size()) { return {}; }
  Span const src{ pd.comments[index].src };
  if ((static_cast<uint64_t>(src.off) + src.len) > pd.src_bytes.size()) { return {}; }
  std::string_view text{ reinterpret_cast<char const *>(pd.src_bytes.data() + src.off),
                         src.len };
  while (!text.empty() && ((text.back() == ' ') || (text.back() == '\t') ||
                           (text.back() == '\r'))) {
    text.remove_suffix(1);
  }
  return text;
}

bool comment_is_trailing(ParsedDocument const &pd, uint32_t index) {
  return (index < pd.comments.size()) &&
         (pd.comments[index].pos == CommentPos::Trailing);
}

bool comment_is_own_line(ParsedDocument const &pd, uint32_t index) {
  return (index < pd.comments.size()) && (pd.comments[index].pos == CommentPos::OwnLine);
}

void append_indent(std::string &out, uint32_t depth) {
  for (uint32_t i = 0; i < depth; ++i) { out += INDENT_UNIT; }
}

// Always the escaped spelling: a `"""` literal decodes to the same text, so one
// of the two has to win.
void append_literal(std::string &out, std::string_view text) {
  constexpr std::string_view HEX{ "0123456789abcdef" };
  out += '"';
  for (char const c : text) {
    scav_byte const b{ static_cast<scav_byte>(c) };
    switch (b) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default:
        if (b < 0x20U) {
          out += "\\u00";
          out += HEX[(b >> 4U) & 0xFU];
          out += HEX[b & 0xFU];
        } else {
          out += c;
        }
        break;
    }
  }
  out += '"';
}

// Canonical attributes ======================================================

// One `k`, `k = "v"` or `k = ["a", "b"]`. `values` survives so a list too wide
// for the budget can still break value by value.
struct EntryOut {
  Span key;      // -> Printer::text
  Span text;     // -> Printer::text, the whole entry flat
  Span values;   // -> Printer::values, each a literal in Printer::text
  uint32_t cps;  // codepoints of `text`
};

// One emitted `@...` line: a single key, or a namespace block once two keys
// share a namespace.
struct ItemOut {
  Span ns;        // -> Printer::text; empty for a bare key
  Span entries;   // -> Printer::entries; >1 makes this the block spelling
  Span comments;  // -> Printer::comment_ids
  uint32_t cps;
  uint32_t blank;  // a blank line above, from the first statement that merged in
};

struct BlockOut {
  Span items;    // -> Printer::items, sorted by key bytes
  Span kids;     // -> Printer::kids, document order, submachine elision applied
  Span orphans;  // -> Printer::comment_ids, homeless comments hoisted to the top
};

struct StmtOut {
  uint32_t block;     // -> Printer::blocks; INVALID when the statement owns none
  Span head;          // -> Printer::text
  uint32_t head_cps;
  uint32_t flat_cps;  // INVALID when the subtree cannot be one line
  Span pre;           // -> Printer::comment_ids, lines above
  Span dang;          // -> Printer::comment_ids, lines at the end of the block
  Span tail;          // -> Printer::comment_ids, lines after the closing brace
  uint32_t open;      // trailing the `{`, or INVALID
  uint32_t post;      // trailing the `,`, or INVALID
  uint32_t blank;     // a blank line above, before any leading comment
};

// A merged (ns, key) before grouping into an item.
struct Merged {
  StrRef ns, key;
  Span values;  // -> Printer::merged_values
  Span composed;  // -> Printer::text, `ns:key` or `key`, the sort key
};

struct Printer {
  ParsedDocument const *pd;
  uint32_t columns;

  std::string text;
  std::vector<Span> values;
  std::vector<EntryOut> entries;
  std::vector<ItemOut> items;
  std::vector<BlockOut> blocks;
  std::vector<uint32_t> kids;
  std::vector<uint32_t> comment_ids;
  std::vector<StmtOut> stmts;
};

Span append_text(Printer &p, std::string_view s) {
  Span const at{ make_span(narrow_clamp<uint32_t>(p.text.size()),
                           narrow_clamp<uint32_t>(s.size())) };
  p.text += s;
  return at;
}

std::string_view text_of(Printer const &p, Span s) {
  if (s.len == 0) { return {}; }
  return { p.text.data() + s.off, s.len };
}

Span text_since(Printer const &p, uint32_t begin) {
  return make_span(begin, narrow_clamp<uint32_t>(p.text.size()) - begin);
}

// Statement heads ===========================================================

void append_endpoint(Printer &p, ParsedDocument const &pd, Endpoint const &ep) {
  if (ep.wildcard != 0) {
    p.text += '*';
    return;
  }
  for (uint32_t i = 0; i < ep.segs.len; ++i) {
    uint32_t const at{ ep.segs.off + i };
    if (at >= pd.path_segs.size()) { break; }
    PathSeg const &seg{ pd.path_segs[at] };
    if (i != 0) { p.text += '/'; }
    p.text += pool_of(pd, seg.name);
    if (seg.qualifier.len != 0) {
      p.text += ':';
      p.text += pool_of(pd, seg.qualifier);
    } else if (seg.ordinal != INVALID) {
      p.text += ':';
      string_append_u32(p.text, seg.ordinal);
    }
  }
}

void append_label(Printer &p, ParsedDocument const &pd, StrRef label) {
  if (label.len == 0) { return; }
  p.text += ' ';
  append_literal(p.text, pool_of(pd, label));
}

// Everything before the block: keyword, names, kind, label.
Span build_head(Printer &p, uint32_t stmt) {
  ParsedDocument const &pd{ *p.pd };
  uint32_t const begin{ narrow_clamp<uint32_t>(p.text.size()) };
  uint32_t const payload{ pd.stmt_payload[stmt] };

  switch (pd.stmts[stmt].kind) {
    case StmtKind::Chart: {
      if (payload >= pd.charts.size()) { break; }
      ChartStmt const &s{ pd.charts[payload] };
      p.text += "chart ";
      p.text += pool_of(pd, s.name);
      append_label(p, pd, s.label);
      break;
    }
    case StmtKind::Include: {
      if (payload >= pd.includes.size()) { break; }
      IncludeStmt const &s{ pd.includes[payload] };
      p.text += "include ";
      append_literal(p.text, pool_of(pd, s.path));
      p.text += " as ";
      p.text += pool_of(pd, s.alias);
      break;
    }
    case StmtKind::State: {
      if (payload >= pd.states.size()) { break; }
      StateStmt const &s{ pd.states[payload] };
      p.text += "state ";
      p.text += pool_of(pd, s.name);
      if (s.kind != StateKind::Normal) {
        p.text += ' ';
        p.text += syntax_state_kind_name(s.kind);
      }
      append_label(p, pd, s.label);
      break;
    }
    case StmtKind::Submachine: {
      if (payload >= pd.submachines.size()) { break; }
      SubmachineStmt const &s{ pd.submachines[payload] };
      p.text += "submachine";
      if (s.name.len != 0) {
        p.text += ' ';
        p.text += pool_of(pd, s.name);
      }
      append_label(p, pd, s.label);
      break;
    }
    case StmtKind::Trans: {
      if (payload >= pd.transitions.size()) { break; }
      TransStmt const &s{ pd.transitions[payload] };
      p.text += "trans ";
      if (s.kind != TransKind::External) {
        p.text += syntax_trans_kind_name(s.kind);
        p.text += ' ';
      }
      append_endpoint(p, pd, s.src);
      p.text += " -> ";
      append_endpoint(p, pd, s.dst);
      append_label(p, pd, s.label);
      break;
    }
    case StmtKind::Attr: break;  // items carry their own spelling
  }
  return text_since(p, begin);
}

// Comment buckets ===========================================================

// Which line a comment lands on follows from where its offset falls against the
// statement's own span: before it, inside its block, or past its closing brace.
void bucket_comments(Printer &p, uint32_t stmt) {
  ParsedDocument const &pd{ *p.pd };
  Span const span{ pd.stmts[stmt].comments };
  Span const src{ pd.stmts[stmt].src };
  uint32_t const end{ src.off + src.len };
  StmtOut &so{ p.stmts[stmt] };
  so.open = INVALID;
  so.post = INVALID;
  so.blank = pd.stmts[stmt].blank_before;

  uint32_t const pre_begin{ narrow_clamp<uint32_t>(p.comment_ids.size()) };
  for (uint32_t i = 0; i < span.len; ++i) {
    uint32_t const id{ span.off + i };
    if (id >= pd.comments.size()) { continue; }
    if (comment_is_trailing(pd, id)) { continue; }
    if (pd.comments[id].src.off < src.off) { p.comment_ids.push_back(id); }
  }
  so.pre = make_span(pre_begin,
                     narrow_clamp<uint32_t>(p.comment_ids.size()) - pre_begin);

  uint32_t const dang_begin{ narrow_clamp<uint32_t>(p.comment_ids.size()) };
  for (uint32_t i = 0; i < span.len; ++i) {
    uint32_t const id{ span.off + i };
    if (id >= pd.comments.size()) { continue; }
    uint32_t const off{ pd.comments[id].src.off };
    if (comment_is_trailing(pd, id)) {
      // One trailing comment per line, so the first inside the block takes the
      // opening brace; a second is unreachable and falls back to its own line.
      if (off < end) {
        if (so.open == INVALID) {
          so.open = id;
        } else {
          p.comment_ids.push_back(id);
        }
      }
      continue;
    }
    if ((off >= src.off) && (off < end)) { p.comment_ids.push_back(id); }
  }
  so.dang = make_span(dang_begin,
                      narrow_clamp<uint32_t>(p.comment_ids.size()) - dang_begin);

  uint32_t const tail_begin{ narrow_clamp<uint32_t>(p.comment_ids.size()) };
  for (uint32_t i = 0; i < span.len; ++i) {
    uint32_t const id{ span.off + i };
    if (id >= pd.comments.size()) { continue; }
    uint32_t const off{ pd.comments[id].src.off };
    if (off < end) { continue; }
    if (comment_is_trailing(pd, id)) {
      if (so.post == INVALID) {
        so.post = id;
        continue;
      }
    }
    p.comment_ids.push_back(id);
  }
  so.tail = make_span(tail_begin,
                      narrow_clamp<uint32_t>(p.comment_ids.size()) - tail_begin);
}

// Attribute canonicalization ================================================

// `ns:key`, or the bare key: the sort key, and the lookup key that maps a source
// statement back to the item it merged into.
Span build_composed(Printer &p, StrRef ns, StrRef key) {
  ParsedDocument const &pd{ *p.pd };
  uint32_t const begin{ narrow_clamp<uint32_t>(p.text.size()) };
  if (ns.len != 0) {
    p.text += pool_of(pd, ns);
    p.text += ':';
  }
  p.text += pool_of(pd, key);
  return text_since(p, begin);
}

bool values_are_flag(std::vector<std::string_view> const &vals, Span span) {
  return (span.len == 1) && (vals[span.off] == "true");
}

// `k`, `k = "v"`, `k = ["a", "b"]`: the list spelling arrives at two values.
EntryOut build_entry(Printer &p,
                     StrRef key,
                     std::vector<std::string_view> const &vals,
                     Span span) {
  EntryOut out{ .key = {}, .text = {}, .values = {}, .cps = 0 };
  uint32_t const begin{ narrow_clamp<uint32_t>(p.text.size()) };
  out.key = append_text(p, pool_of(*p.pd, key));

  uint32_t const values_begin{ narrow_clamp<uint32_t>(p.values.size()) };
  if (!values_are_flag(vals, span)) {
    p.text += " = ";
    bool const list{ span.len != 1 };
    if (list) { p.text += '['; }
    for (uint32_t i = 0; i < span.len; ++i) {
      if (i != 0) { p.text += ", "; }
      uint32_t const lit{ narrow_clamp<uint32_t>(p.text.size()) };
      append_literal(p.text, vals[span.off + i]);
      p.values.push_back(text_since(p, lit));
    }
    if (list) { p.text += ']'; }
  }
  out.values = make_span(values_begin,
                         narrow_clamp<uint32_t>(p.values.size()) - values_begin);
  out.text = text_since(p, begin);
  out.cps = count_cp(text_of(p, out.text));
  return out;
}

// Attribute statements of one block, merged by key, sorted by key bytes, then
// grouped. `orphans` takes the comments of a statement that spells nothing.
Span build_items(Printer &p, Span children, std::vector<uint32_t> &orphans) {
  ParsedDocument const &pd{ *p.pd };

  // Source order, so a repeated key keeps insertion order among its values.
  // Views rather than pool refs: a flag's value is text the pool never held.
  std::vector<Merged> flat;
  std::vector<std::string_view> vals;
  std::vector<uint32_t> attr_stmts;

  for (uint32_t i = 0; i < children.len; ++i) {
    uint32_t const at{ children.off + i };
    if (at >= pd.stmt_ids.size()) { continue; }
    uint32_t const stmt{ pd.stmt_ids[at].v };
    if ((stmt >= pd.stmts.size()) || (pd.stmts[stmt].kind != StmtKind::Attr)) {
      continue;
    }
    attr_stmts.push_back(stmt);
    uint32_t const payload{ pd.stmt_payload[stmt] };
    if (payload >= pd.attrs.size()) { continue; }
    AttrStmt const &as{ pd.attrs[payload] };
    for (uint32_t e = 0; e < as.entries.len; ++e) {
      uint32_t const eat{ as.entries.off + e };
      if (eat >= pd.attr_entries.size()) { continue; }
      AttrEntry const &entry{ pd.attr_entries[eat] };
      uint32_t const vbegin{ narrow_clamp<uint32_t>(vals.size()) };
      if (entry.kind == AttrValueKind::Flag) {
        vals.emplace_back("true");
      } else {
        for (uint32_t v = 0; v < entry.values.len; ++v) {
          uint32_t const vat{ entry.values.off + v };
          if (vat < pd.attr_values.size()) {
            vals.push_back(pool_of(pd, pd.attr_values[vat]));
          }
        }
      }
      flat.push_back({ .ns = as.ns,
                       .key = entry.key,
                       .values = make_span(vbegin,
                                           narrow_clamp<uint32_t>(vals.size()) - vbegin),
                       .composed = build_composed(p, as.ns, entry.key) });
    }
  }

  if (flat.empty() && attr_stmts.empty()) { return {}; }

  std::vector<uint32_t> order(flat.size());
  for (uint32_t i = 0; i < order.size(); ++i) { order[i] = i; }
  scav_stable_sort(order, [&](uint32_t a, uint32_t b) {
    return text_of(p, flat[a].composed) < text_of(p, flat[b].composed);
  });

  // Equal composed keys merge, values concatenated in written order.
  std::vector<std::string_view> merged_vals;
  std::vector<Merged> merged;
  for (uint32_t const slot : order) {
    Merged const &row{ flat[slot] };
    bool const same{ !merged.empty() &&
                     (text_of(p, merged.back().composed) ==
                      text_of(p, row.composed)) };
    if (!same) {
      merged.push_back({ .ns = row.ns,
                         .key = row.key,
                         .values = make_span(narrow_clamp<uint32_t>(merged_vals.size()),
                                             0),
                         .composed = row.composed });
    }
    for (uint32_t v = 0; v < row.values.len; ++v) {
      merged_vals.push_back(vals[row.values.off + v]);
    }
    merged.back().values.len =
        narrow_clamp<uint32_t>(merged_vals.size()) - merged.back().values.off;
  }

  // Two or more keys under a namespace take the block spelling. The sort made
  // them contiguous: no other namespace composes a key beginning `ns:`.
  uint32_t const items_begin{ narrow_clamp<uint32_t>(p.items.size()) };
  std::vector<Span> item_keys;  // composed key of each merged entry, for lookup
  std::vector<uint32_t> item_of;
  for (uint32_t i = 0; i < merged.size();) {
    uint32_t j{ i + 1 };
    if (merged[i].ns.len != 0) {
      while ((j < merged.size()) &&
             (pool_of(pd, merged[j].ns) == pool_of(pd, merged[i].ns))) {
        ++j;
      }
    }
    bool const group{ (merged[i].ns.len != 0) && ((j - i) >= 2) };
    uint32_t const stop{ group ? j : (i + 1) };

    uint32_t const entries_begin{ narrow_clamp<uint32_t>(p.entries.size()) };
    for (uint32_t k = i; k < stop; ++k) {
      p.entries.push_back(build_entry(p, merged[k].key, merged_vals, merged[k].values));
      item_keys.push_back(merged[k].composed);
      item_of.push_back(narrow_clamp<uint32_t>(p.items.size()));
    }
    ItemOut item{ .ns = {},
                  .entries = make_span(entries_begin,
                                       narrow_clamp<uint32_t>(p.entries.size()) -
                                           entries_begin),
                  .comments = {},
                  .cps = 0,
                  .blank = 0 };
    if (merged[i].ns.len != 0) {
      item.ns = append_text(p, pool_of(pd, merged[i].ns));
    }

    item.cps = 1 + count_cp(text_of(p, item.ns));  // `@k`, `@ns:k`, `@ns { a, b }`
    if (group) {
      item.cps += 3;  // ` { ` after the namespace
      for (uint32_t k = 0; k < item.entries.len; ++k) {
        item.cps += p.entries[item.entries.off + k].cps;
        if (k + 1 < item.entries.len) { item.cps += 2; }
      }
      item.cps += 2;  // ` }` closing
    } else {
      if (item.ns.len != 0) { item.cps += 1; }
      item.cps += p.entries[item.entries.off].cps;
    }
    p.items.push_back(item);
    i = stop;
  }
  Span const items{ make_span(items_begin,
                              narrow_clamp<uint32_t>(p.items.size()) - items_begin) };

  // A block spelling's entries all share its namespace, so a source statement
  // reaches exactly one item and its comments cannot duplicate or vanish.
  std::vector<uint32_t> owner(attr_stmts.size(), INVALID);
  for (uint32_t i = 0; i < attr_stmts.size(); ++i) {
    uint32_t const stmt{ attr_stmts[i] };
    uint32_t const payload{ pd.stmt_payload[stmt] };
    if (payload >= pd.attrs.size()) { continue; }
    AttrStmt const &as{ pd.attrs[payload] };
    if (as.entries.len == 0) { continue; }
    uint32_t const eat{ as.entries.off };
    if (eat >= pd.attr_entries.size()) { continue; }
    Span const composed{ build_composed(p, as.ns, pd.attr_entries[eat].key) };
    for (uint32_t k = 0; k < item_keys.size(); ++k) {
      if (text_of(p, item_keys[k]) == text_of(p, composed)) {
        owner[i] = item_of[k];
        break;
      }
    }
  }

  // Counting sort by owning item, so each item's comments are one span. Counted
  // per comment, not per statement: one statement can carry several.
  std::vector<uint32_t> counts(items.len + 1, 0);
  for (uint32_t i = 0; i < attr_stmts.size(); ++i) {
    if (owner[i] == INVALID) { continue; }
    Span const span{ pd.stmts[attr_stmts[i]].comments };
    for (uint32_t c = 0; c < span.len; ++c) {
      if ((span.off + c) < pd.comments.size()) {
        ++counts[(owner[i] - items_begin) + 1];
      }
    }
  }
  for (uint32_t i = 0; i < items.len; ++i) { counts[i + 1] += counts[i]; }
  uint32_t const base{ narrow_clamp<uint32_t>(p.comment_ids.size()) };
  p.comment_ids.resize(base + counts[items.len]);
  std::vector<uint32_t> cursor(counts.begin(), counts.end() - 1);
  for (uint32_t i = 0; i < attr_stmts.size(); ++i) {
    Span const span{ pd.stmts[attr_stmts[i]].comments };
    for (uint32_t c = 0; c < span.len; ++c) {
      uint32_t const id{ span.off + c };
      if (id >= pd.comments.size()) { continue; }
      if (owner[i] == INVALID) {
        orphans.push_back(id);
        continue;
      }
      p.comment_ids[base + cursor[owner[i] - items_begin]++] = id;
    }
  }
  for (uint32_t i = 0; i < items.len; ++i) {
    p.items[items_begin + i].comments =
        make_span(base + counts[i], counts[i + 1] - counts[i]);
  }
  // Source order, so the earliest statement reaching an item decides its blank.
  for (auto i = narrow_clamp<uint32_t>(attr_stmts.size()); i-- > 0;) {
    if (owner[i] != INVALID) {
      p.items[owner[i]].blank = pd.stmts[attr_stmts[i]].blank_before;
    }
  }
  return items;
}

// Blocks ====================================================================

bool submachine_is_implicit(Printer const &p, uint32_t stmt) {
  ParsedDocument const &pd{ *p.pd };
  if (pd.stmts[stmt].kind != StmtKind::Submachine) { return false; }
  uint32_t const payload{ pd.stmt_payload[stmt] };
  if (payload >= pd.submachines.size()) { return false; }
  SubmachineStmt const &s{ pd.submachines[payload] };
  if ((s.name.len != 0) || (s.label.len != 0)) { return false; }
  // Its attributes hang off the submachine row, so hoisting would move them.
  uint32_t const block{ p.stmts[stmt].block };
  return (block == INVALID) || (p.blocks[block].items.len == 0);
}

// Structural children in document order, with a sole unnamed unlabelled
// attribute-free submachine replaced by its own children.
void build_block(Printer &p, uint32_t stmt) {
  ParsedDocument const &pd{ *p.pd };
  Span const children{ pd.stmt_children[stmt] };

  std::vector<uint32_t> structural;
  uint32_t submachines{ 0 };
  for (uint32_t i = 0; i < children.len; ++i) {
    uint32_t const at{ children.off + i };
    if (at >= pd.stmt_ids.size()) { continue; }
    uint32_t const child{ pd.stmt_ids[at].v };
    if ((child >= pd.stmts.size()) || (pd.stmts[child].kind == StmtKind::Attr)) {
      continue;
    }
    if (pd.stmts[child].kind == StmtKind::Submachine) { ++submachines; }
    structural.push_back(child);
  }

  std::vector<uint32_t> orphans;
  Span const items{ build_items(p, children, orphans) };

  uint32_t const kids_begin{ narrow_clamp<uint32_t>(p.kids.size()) };
  for (uint32_t const child : structural) {
    if ((submachines == 1) && submachine_is_implicit(p, child)) {
      StmtOut const &co{ p.stmts[child] };
      uint32_t const cb{ co.block };
      if (cb != INVALID) {
        for (uint32_t k = 0; k < p.blocks[cb].kids.len; ++k) {
          p.kids.push_back(p.kids[p.blocks[cb].kids.off + k]);
        }
        for (uint32_t k = 0; k < p.blocks[cb].orphans.len; ++k) {
          orphans.push_back(p.comment_ids[p.blocks[cb].orphans.off + k]);
        }
      }
      for (Span const bucket : { co.pre, co.dang, co.tail }) {
        for (uint32_t k = 0; k < bucket.len; ++k) {
          orphans.push_back(p.comment_ids[bucket.off + k]);
        }
      }
      if (co.open != INVALID) { orphans.push_back(co.open); }
      if (co.post != INVALID) { orphans.push_back(co.post); }
      continue;
    }
    p.kids.push_back(child);
  }

  uint32_t const orphans_begin{ narrow_clamp<uint32_t>(p.comment_ids.size()) };
  for (uint32_t const id : orphans) { p.comment_ids.push_back(id); }

  p.blocks.push_back(
      { .items = items,
        .kids = make_span(kids_begin,
                          narrow_clamp<uint32_t>(p.kids.size()) - kids_begin),
        .orphans = make_span(orphans_begin,
                             narrow_clamp<uint32_t>(p.comment_ids.size()) -
                                 orphans_begin) });
  p.stmts[stmt].block = narrow_clamp<uint32_t>(p.blocks.size()) - 1;
}

// Written when the block holds anything, a comment included: one on the opening
// brace has nowhere else to live. `submachine` keeps an empty block; a state does not.
bool emits_block(Printer const &p, uint32_t stmt) {
  StmtKind const kind{ p.pd->stmts[stmt].kind };
  if ((kind == StmtKind::Chart) || (kind == StmtKind::Submachine)) { return true; }
  StmtOut const &so{ p.stmts[stmt] };
  if (so.block == INVALID) { return false; }  // an attribute owns no block
  BlockOut const &b{ p.blocks[so.block] };
  return (b.items.len != 0) || (b.kids.len != 0) || (b.orphans.len != 0) ||
         (so.dang.len != 0) || (so.open != INVALID);
}

// Flat width ================================================================

// A comment ends its line, so one inside a statement's own rendering rules out
// the flat form. Its leading and trailing ones block the parent instead.
void compute_flat(Printer &p, uint32_t stmt) {
  StmtOut &so{ p.stmts[stmt] };
  so.flat_cps = INVALID;
  if ((so.open != INVALID) || (so.dang.len != 0) || (so.tail.len != 0)) { return; }
  if (!emits_block(p, stmt)) {
    so.flat_cps = so.head_cps;
    return;
  }
  BlockOut const &b{ p.blocks[so.block] };
  if (b.orphans.len != 0) { return; }

  uint32_t count{ 0 };
  uint64_t sum{ 0 };
  for (uint32_t i = 0; i < b.items.len; ++i) {
    ItemOut const &item{ p.items[b.items.off + i] };
    // A blank line and a comment both end a line, so neither fits the flat form.
    if ((item.comments.len != 0) || ((i != 0) && (item.blank != 0))) { return; }
    sum += item.cps;
    ++count;
  }
  for (uint32_t i = 0; i < b.kids.len; ++i) {
    StmtOut const &ko{ p.stmts[p.kids[b.kids.off + i]] };
    if ((ko.flat_cps == INVALID) || (ko.pre.len != 0) || (ko.post != INVALID)) {
      return;
    }
    if ((ko.blank != 0) && ((i != 0) || (b.items.len != 0))) { return; }
    sum += ko.flat_cps;
    ++count;
  }

  // `head {}` when empty, else `head { a, b }`: a space and a comma per item,
  // less the comma the last one does not take, plus the braces.
  uint64_t const flat{ (count == 0) ? (uint64_t{ so.head_cps } + 3)
                                    : (uint64_t{ so.head_cps } + sum +
                                       (2ULL * count) + 3ULL) };
  so.flat_cps = (flat >= INVALID) ? INVALID : static_cast<uint32_t>(flat);
}

// Emission ==================================================================

struct Emitter {
  Printer const *p;
  std::string *out;
};

bool fits(Printer const &p, uint32_t depth, uint32_t cps, uint32_t comma) {
  if (cps == INVALID) { return false; }
  uint64_t const width{ (2ULL * depth) + cps + comma };
  return width <= p.columns;
}

void emit_comment_lines(Emitter &e, Span span, uint32_t depth) {
  for (uint32_t i = 0; i < span.len; ++i) {
    uint32_t const id{ e.p->comment_ids[span.off + i] };
    append_indent(*e.out, depth);
    *e.out += comment_of(*e.p->pd, id);
    *e.out += '\n';
    if (comment_is_own_line(*e.p->pd, id)) { *e.out += '\n'; }  // reclassifies as own-line
  }
}

void emit_trailing(Emitter &e, uint32_t id) {
  if (id == INVALID) { return; }
  *e.out += ' ';
  *e.out += comment_of(*e.p->pd, id);
}

// The one-line spelling: no trailing comma, one space inside each brace.
void emit_flat(Emitter &e, uint32_t root) {
  struct Frame {
    uint32_t stmt;
    uint32_t next;   // index into the block's items, then its kids
    uint32_t total;
  };
  std::vector<Frame> stack;
  Printer const &p{ *e.p };

  auto const open_stmt = [&](uint32_t stmt) {
    *e.out += text_of(p, p.stmts[stmt].head);
    if (!emits_block(p, stmt)) { return; }
    BlockOut const &b{ p.blocks[p.stmts[stmt].block] };
    uint32_t const total{ b.items.len + b.kids.len };
    if (total == 0) {
      *e.out += " {}";
      return;
    }
    *e.out += " {";
    stack.push_back({ .stmt = stmt, .next = 0, .total = total });
  };

  open_stmt(root);
  while (!stack.empty()) {
    Frame &f{ stack.back() };
    BlockOut const &b{ p.blocks[p.stmts[f.stmt].block] };
    if (f.next == f.total) {
      *e.out += " }";
      stack.pop_back();
      continue;
    }
    *e.out += (f.next == 0) ? " " : ", ";
    uint32_t const at{ f.next++ };
    if (at < b.items.len) {
      ItemOut const &item{ p.items[b.items.off + at] };
      *e.out += '@';
      *e.out += text_of(p, item.ns);
      if (item.entries.len >= 2) {
        *e.out += " { ";
        for (uint32_t k = 0; k < item.entries.len; ++k) {
          if (k != 0) { *e.out += ", "; }
          *e.out += text_of(p, p.entries[item.entries.off + k].text);
        }
        *e.out += " }";
      } else {
        if (item.ns.len != 0) { *e.out += ':'; }
        *e.out += text_of(p, p.entries[item.entries.off].text);
      }
      continue;
    }
    // Read before the call: `open_stmt` may push, which invalidates `f` and `b`.
    uint32_t const kid{ p.kids[b.kids.off + (at - b.items.len)] };
    open_stmt(kid);
  }
}

// `@k = [` then one value per line: the only break a single attribute admits.
void emit_entry(Emitter &e, EntryOut const &entry, uint32_t depth, uint32_t comma) {
  Printer const &p{ *e.p };
  if (fits(p, depth, entry.cps, comma) || (entry.values.len < 2)) {
    *e.out += text_of(p, entry.text);
    return;
  }
  *e.out += text_of(p, entry.key);
  *e.out += " = [\n";
  for (uint32_t i = 0; i < entry.values.len; ++i) {
    append_indent(*e.out, depth + 1);
    *e.out += text_of(p, p.values[entry.values.off + i]);
    *e.out += ",\n";
  }
  append_indent(*e.out, depth);
  *e.out += ']';
}

void emit_item(Emitter &e, ItemOut const &item, uint32_t depth, uint32_t blank) {
  Printer const &p{ *e.p };
  bool const group{ item.entries.len >= 2 };
  if (blank != 0) { *e.out += '\n'; }

  // All but a final trailing comment go on lines above: a line takes one, and
  // merging two statements can hand this item two.
  uint32_t const n{ item.comments.len };
  bool const trails{ (n != 0) &&
                     comment_is_trailing(*p.pd,
                                         p.comment_ids[item.comments.off + n - 1]) };
  emit_comment_lines(e,
                     make_span(item.comments.off, trails ? (n - 1) : n),
                     depth);

  append_indent(*e.out, depth);
  if (fits(p, depth, item.cps, 1)) {
    *e.out += '@';
    *e.out += text_of(p, item.ns);
    if (group) {
      *e.out += " { ";
      for (uint32_t k = 0; k < item.entries.len; ++k) {
        if (k != 0) { *e.out += ", "; }
        *e.out += text_of(p, p.entries[item.entries.off + k].text);
      }
      *e.out += " }";
    } else {
      if (item.ns.len != 0) { *e.out += ':'; }
      *e.out += text_of(p, p.entries[item.entries.off].text);
    }
  } else if (group) {
    *e.out += '@';
    *e.out += text_of(p, item.ns);
    *e.out += " {\n";
    for (uint32_t k = 0; k < item.entries.len; ++k) {
      append_indent(*e.out, depth + 1);
      emit_entry(e, p.entries[item.entries.off + k], depth + 1, 1);
      *e.out += ",\n";
    }
    append_indent(*e.out, depth);
    *e.out += '}';
  } else {
    *e.out += '@';
    *e.out += text_of(p, item.ns);
    if (item.ns.len != 0) { *e.out += ':'; }
    emit_entry(e, p.entries[item.entries.off], depth, 1);
  }
  *e.out += ',';
  if (trails) { emit_trailing(e, p.comment_ids[item.comments.off + n - 1]); }
  *e.out += '\n';
}

struct Job {
  uint32_t stmt;
  uint32_t depth;
  uint32_t comma;
  uint32_t closing;  // 1 = write the closing brace, not the head
  uint32_t blank;    // 1 = a blank line above, which the first item never takes
};

// An explicit stack rather than the call stack: nesting depth is the document's
// to choose, and the parse cap is a diagnostic rather than a small number.
void emit_document(Emitter &e, uint32_t root) {
  Printer const &p{ *e.p };
  std::vector<Job> stack;
  stack.push_back(
      { .stmt = root, .depth = 0, .comma = 0, .closing = 0, .blank = 0 });

  while (!stack.empty()) {
    Job const job{ stack.back() };
    stack.pop_back();
    StmtOut const &so{ p.stmts[job.stmt] };

    if (job.closing != 0) {
      emit_comment_lines(e, so.dang, job.depth + 1);
      append_indent(*e.out, job.depth);
      *e.out += '}';
      if (job.comma != 0) { *e.out += ','; }
      emit_trailing(e, so.post);
      *e.out += '\n';
      emit_comment_lines(e, so.tail, job.depth);
      continue;
    }

    if (job.blank != 0) { *e.out += '\n'; }
    emit_comment_lines(e, so.pre, job.depth);

    // The root always breaks: a one-line file is legal and makes every edit a
    // whole-file diff.
    bool const broken{ (job.stmt == root) ||
                       !fits(p, job.depth, so.flat_cps, job.comma) };
    if (!broken) {
      append_indent(*e.out, job.depth);
      emit_flat(e, job.stmt);
      if (job.comma != 0) { *e.out += ','; }
      emit_trailing(e, so.post);
      *e.out += '\n';
      continue;
    }

    append_indent(*e.out, job.depth);
    *e.out += text_of(p, so.head);
    if (!emits_block(p, job.stmt)) {
      if (job.comma != 0) { *e.out += ','; }
      emit_trailing(e, so.post);
      *e.out += '\n';
      emit_comment_lines(e, so.tail, job.depth);
      continue;
    }

    *e.out += " {";
    emit_trailing(e, so.open);
    *e.out += '\n';

    BlockOut const &b{ p.blocks[so.block] };
    emit_comment_lines(e, b.orphans, job.depth + 1);
    // A blank line opening a block would sit under the brace that opened it, so
    // whatever comes first in the body never takes one.
    uint32_t written{ b.orphans.len };
    for (uint32_t i = 0; i < b.items.len; ++i) {
      ItemOut const &item{ p.items[b.items.off + i] };
      emit_item(e, item, job.depth + 1, (written++ == 0) ? 0U : item.blank);
    }

    stack.push_back({ .stmt = job.stmt,
                      .depth = job.depth,
                      .comma = job.comma,
                      .closing = 1,
                      .blank = 0 });
    for (uint32_t i = b.kids.len; i-- > 0;) {
      uint32_t const kid{ p.kids[b.kids.off + i] };
      stack.push_back({ .stmt = kid,
                        .depth = job.depth + 1,
                        .comma = 1,
                        .closing = 0,
                        .blank = ((written + i) == 0) ? 0U : p.stmts[kid].blank });
    }
  }
}

}  // namespace

PrintOptions print_default_options() { return { .columns = DEFAULT_PRINT_COLUMNS }; }

bool print_options_validate(PrintOptions const &opts) {
  return (opts.columns >= PRINT_COLUMNS_MIN) && (opts.columns <= PRINT_COLUMNS_MAX);
}

bool print_document(ParsedDocument const &pd,
                    PrintOptions const &opts,
                    std::string &out) {
  if (!print_options_validate(opts)) { return false; }
  uint32_t const root{ syntax_root_statement(pd) };
  if (root == INVALID) { return true; }
  if ((pd.stmt_payload.size() != pd.stmts.size()) ||
      (pd.stmt_children.size() != pd.stmts.size())) {
    return true;
  }

  Printer p{ .pd = &pd, .columns = opts.columns, .text = {}, .values = {},
             .entries = {}, .items = {}, .blocks = {}, .kids = {},
             .comment_ids = {}, .stmts = {} };
  p.stmts.resize(pd.stmts.size(),
                 { .block = INVALID, .head = {}, .head_cps = 0, .flat_cps = INVALID,
                   .pre = {}, .dang = {}, .tail = {}, .open = INVALID,
                   .post = INVALID, .blank = 0 });

  for (uint32_t i = 0; i < pd.stmts.size(); ++i) { bucket_comments(p, i); }

  // Children are parsed after their parent, so a reverse walk sees a block's
  // contents first: the elision and width tests both need them.
  for (auto i = narrow_clamp<uint32_t>(pd.stmts.size()); i-- > 0;) {
    if (pd.stmts[i].kind == StmtKind::Attr) { continue; }
    p.stmts[i].head = build_head(p, i);
    p.stmts[i].head_cps = count_cp(text_of(p, p.stmts[i].head));
    // A block row even when empty, so `emits_block` is the only test any reader
    // needs and no caller has to check the id first.
    if (pd.stmt_children[i].len != 0) {
      build_block(p, i);
    } else {
      p.blocks.push_back({ .items = {}, .kids = {}, .orphans = {} });
      p.stmts[i].block = narrow_clamp<uint32_t>(p.blocks.size()) - 1;
    }
    compute_flat(p, i);
  }

  Emitter e{ .p = &p, .out = &out };
  emit_document(e, root);

  // A comment ending the input classifies as own-line, whose blank line would
  // land at the end of the file.
  while ((out.size() >= 2) && (out[out.size() - 1] == '\n') &&
         (out[out.size() - 2] == '\n')) {
    out.pop_back();
  }
  return true;
}

}  // namespace scav
