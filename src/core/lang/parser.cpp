#include "scav/scav_core.h"

#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace scav {

namespace {

// Whether the current block is between `{`/`,` and an item, or between an item
// and the `,`/`}` that follows it. Two states is the whole of a block's grammar.
enum class Slot : uint32_t { Item, Separator };

// Capacity rather than size: the performance tests assert a peak, and a vector
// that grew and shrank still holds what it grew to.
template <typename T>
uint64_t bytes_of(std::vector<T> const &v) {
  return uint64_t{ v.capacity() } * sizeof(T);
}

struct Frame {
  uint32_t owner;          // StmtId of the statement whose block this is
  uint32_t scratch_begin;  // where this block's children start in `scratch`
  Slot slot;
};

// State threaded through the parse. A plain struct, taken by reference, so
// control flow stays readable from the source.
struct Parser {
  scav_byte const *bytes;
  uint32_t len;
  Token const *tokens;
  uint32_t token_count;
  DocId doc;
  uint32_t max_depth;
  ParsedDocument *pd;
  std::vector<Diagnostic> *diags;
  uint32_t pos;
  uint32_t last_end;  // end offset of the most recently consumed token
  std::vector<StmtId> scratch;
  std::vector<Frame> frames;
  std::vector<scav_byte> strbuf;  // reused by the string decoder
};

Token const &peek(Parser const &p, uint32_t ahead = 0) {
  uint32_t const at{ p.pos + ahead };
  // The stream always ends with one End token, so clamping is enough and no
  // caller needs a bounds check of its own.
  return p.tokens[(at < p.token_count) ? at : (p.token_count - 1)];
}

bool at_kind(Parser const &p, TokKind kind) { return peek(p).kind == kind; }

std::string_view tok_text(Parser const &p, Token const &t) {
  return { reinterpret_cast<char const *>(p.bytes + t.off), t.len };
}

Token const &advance(Parser &p) {
  Token const &t{ peek(p) };
  if (t.kind != TokKind::End) {
    p.last_end = t.off + t.len;
    ++p.pos;
  }
  return t;
}

void error_at(Parser &p, DiagCode code, Token const &t) {
  p.diags->push_back({ .code = code, .doc = p.doc, .src = make_span(t.off, t.len) });
}

bool expect(Parser &p, TokKind kind, DiagCode code) {
  if (!at_kind(p, kind)) {
    error_at(p, code, peek(p));
    return false;
  }
  advance(p);
  return true;
}

// Straight into the document's pool in the order the parser meets them, so a
// repeated name costs its own bytes.
StrRef pool_append(Parser &p, scav_byte const *bytes, size_t len) {
  if (len == 0) { return {}; }
  std::vector<scav_byte> &pool{ p.pd->strings.bytes };
  StrRef const ref{ str_ref(narrow_clamp<uint32_t>(pool.size()),
                            narrow_clamp<uint32_t>(len)) };
  pool.insert(pool.end(), bytes, bytes + len);
  return ref;
}

StrRef pool_append(Parser &p, std::string_view text) {
  return pool_append(p, reinterpret_cast<scav_byte const *>(text.data()), text.size());
}

StrRef pool_token(Parser &p, Token const &t) {
  return pool_append(p, p.bytes + t.off, t.len);
}

// An identifier in a name position. Reserved words are rejected here, not in the
// lexer: `s`, `m` and `t` are keywords only in statement-leading position.
bool take_name(Parser &p, StrRef &out) {
  if (!at_kind(p, TokKind::Ident)) {
    error_at(p, DiagCode::ExpectedIdentifier, peek(p));
    return false;
  }
  Token const &t{ peek(p) };
  if (lex_is_reserved_word(tok_text(p, t))) {
    error_at(p, DiagCode::ReservedWordAsName, t);
    return false;
  }
  out = pool_token(p, t);
  advance(p);
  return true;
}

bool take_string(Parser &p, StrRef &out) {
  if (!at_kind(p, TokKind::String)) {
    error_at(p, DiagCode::ExpectedString, peek(p));
    return false;
  }
  Token const &t{ peek(p) };
  if (!lex_decode_string_literal(p.bytes,
                                 make_span(t.off, t.len),
                                 p.doc,
                                 p.strbuf,
                                 *p.diags)) {
    return false;
  }
  out = pool_append(p, p.strbuf.data(), p.strbuf.size());
  advance(p);
  return true;
}

// The positional string is the label, and every statement that takes one takes
// it optionally.
bool take_optional_label(Parser &p, StrRef &out) {
  out = {};
  if (!at_kind(p, TokKind::String)) { return true; }
  return take_string(p, out);
}

bool take_number(Parser &p, uint32_t &out) {
  Token const &t{ peek(p) };
  uint64_t value{ 0 };
  for (uint32_t i = 0; i < t.len; ++i) {
    value = (value * 10U) + static_cast<uint64_t>(p.bytes[t.off + i] - '0');
    // INVALID is the sentinel, so it is not available as an ordinal.
    if (value >= INVALID) {
      error_at(p, DiagCode::NumberOutOfRange, t);
      return false;
    }
  }
  out = static_cast<uint32_t>(value);
  advance(p);
  return true;
}

uint32_t begin_stmt(Parser &p, StmtKind kind, uint32_t payload) {
  uint32_t const id{ narrow_clamp<uint32_t>(p.pd->stmts.size()) };
  p.pd->stmts.push_back(
      { .kind = kind, .doc = p.doc, .src = make_span(peek(p).off, 0), .comments = {} });
  p.pd->stmt_payload.push_back(payload);
  p.pd->stmt_children.push_back({});
  return id;
}

void end_stmt(Parser &p, uint32_t id) {
  Span &src{ p.pd->stmts[id].src };
  src.len = (p.last_end > src.off) ? (p.last_end - src.off) : 0;
}

// endpoint := '*' | path ; path := seg ('/' seg)* ; seg := ident [':' (ident|digit+)]
bool parse_endpoint(Parser &p, Endpoint &out) {
  out = {};
  if (at_kind(p, TokKind::Star)) {
    advance(p);
    out.wildcard = 1;
    return true;
  }
  if (!at_kind(p, TokKind::Ident)) {
    error_at(p, DiagCode::ExpectedEndpoint, peek(p));
    return false;
  }

  uint32_t const begin{ narrow_clamp<uint32_t>(p.pd->path_segs.size()) };
  while (true) {
    PathSeg seg{ .name = {}, .qualifier = {}, .ordinal = INVALID };
    if (!take_name(p, seg.name)) { return false; }
    if (at_kind(p, TokKind::Colon)) {
      advance(p);
      if (at_kind(p, TokKind::Number)) {
        if (!take_number(p, seg.ordinal)) { return false; }
      } else if (!take_name(p, seg.qualifier)) {
        return false;
      }
    }
    p.pd->path_segs.push_back(seg);
    if (!at_kind(p, TokKind::Slash)) { break; }
    advance(p);
  }
  out.segs = make_span(begin, narrow_clamp<uint32_t>(p.pd->path_segs.size()) - begin);
  return true;
}

// value := string | '[' [string (',' string)* [',']] ']'
bool parse_value(Parser &p, Span &out, AttrValueKind &kind) {
  uint32_t const begin{ narrow_clamp<uint32_t>(p.pd->attr_values.size()) };
  if (at_kind(p, TokKind::String)) {
    StrRef value{};
    if (!take_string(p, value)) { return false; }
    p.pd->attr_values.push_back(value);
    out = make_span(begin, 1);
    kind = AttrValueKind::Scalar;
    return true;
  }
  if (!at_kind(p, TokKind::LBracket)) {
    error_at(p, DiagCode::ExpectedValue, peek(p));
    return false;
  }

  advance(p);
  while (!at_kind(p, TokKind::RBracket)) {
    StrRef value{};
    if (!take_string(p, value)) { return false; }
    p.pd->attr_values.push_back(value);
    if (!at_kind(p, TokKind::Comma)) { break; }
    advance(p);  // trailing comma is legal, so the loop re-tests for ']'
  }
  if (!expect(p, TokKind::RBracket, DiagCode::ExpectedToken)) { return false; }
  out = make_span(begin, narrow_clamp<uint32_t>(p.pd->attr_values.size()) - begin);
  kind = AttrValueKind::List;
  return true;
}

// entry := ident ['=' value]
bool parse_attr_entry(Parser &p) {
  AttrEntry entry{ .key = {}, .values = {}, .kind = AttrValueKind::Flag };
  if (!take_name(p, entry.key)) { return false; }
  if (at_kind(p, TokKind::Equals)) {
    advance(p);
    if (!parse_value(p, entry.values, entry.kind)) { return false; }
  }
  p.pd->attr_entries.push_back(entry);
  return true;
}

// attr := '@' key ['=' value] | '@' ident datablock ; key := ident [':' ident]
bool parse_attr(Parser &p, uint32_t &out_stmt) {
  uint32_t const payload{ narrow_clamp<uint32_t>(p.pd->attrs.size()) };
  out_stmt = begin_stmt(p, StmtKind::Attr, payload);
  advance(p);  // '@'

  StrRef first{};
  if (!take_name(p, first)) { return false; }

  AttrStmt stmt{ .ns = {}, .entries = {} };
  uint32_t const entries_begin{ narrow_clamp<uint32_t>(p.pd->attr_entries.size()) };

  if (at_kind(p, TokKind::LBrace)) {
    // n keys under one namespace. Parsed inline, not through the frame stack:
    // a datablock holds entries and cannot nest.
    stmt.ns = first;
    advance(p);
    while (!at_kind(p, TokKind::RBrace)) {
      if (!parse_attr_entry(p)) { return false; }
      if (!at_kind(p, TokKind::Comma)) { break; }
      advance(p);
    }
    if (!expect(p, TokKind::RBrace, DiagCode::ExpectedToken)) { return false; }
  } else {
    AttrEntry entry{ .key = first, .values = {}, .kind = AttrValueKind::Flag };
    if (at_kind(p, TokKind::Colon)) {
      advance(p);
      stmt.ns = first;
      if (!take_name(p, entry.key)) { return false; }
    }
    if (at_kind(p, TokKind::Equals)) {
      advance(p);
      if (!parse_value(p, entry.values, entry.kind)) { return false; }
    }
    p.pd->attr_entries.push_back(entry);
  }

  stmt.entries =
      make_span(entries_begin,
                narrow_clamp<uint32_t>(p.pd->attr_entries.size()) - entries_begin);
  p.pd->attrs.push_back(stmt);
  return true;
}

// include := 'include' string 'as' ident
bool parse_include(Parser &p, uint32_t &out_stmt) {
  uint32_t const payload{ narrow_clamp<uint32_t>(p.pd->includes.size()) };
  out_stmt = begin_stmt(p, StmtKind::Include, payload);
  advance(p);  // 'include'

  IncludeStmt stmt{ .path = {}, .alias = {} };
  if (!take_string(p, stmt.path)) { return false; }
  // `as` is contextual, so it is matched by bytes and stays available as a name.
  if (!at_kind(p, TokKind::Ident) || (tok_text(p, peek(p)) != "as")) {
    error_at(p, DiagCode::ExpectedToken, peek(p));
    return false;
  }
  advance(p);
  if (!take_name(p, stmt.alias)) { return false; }
  p.pd->includes.push_back(stmt);
  return true;
}

// state := ('state'|'s') ident [state_kind] [string] [block]
bool parse_state(Parser &p, uint32_t &out_stmt, bool &opens_block) {
  uint32_t const payload{ narrow_clamp<uint32_t>(p.pd->states.size()) };
  out_stmt = begin_stmt(p, StmtKind::State, payload);
  advance(p);  // 'state' or 's'

  StateStmt stmt{ .name = {}, .label = {}, .kind = StateKind::Normal, .has_block = 0 };
  if (!take_name(p, stmt.name)) { return false; }
  // The name slot is first and mandatory, so a bare identifier after it is a
  // kind -- or, if reserved, the next statement with its comma missing.
  if (at_kind(p, TokKind::Ident) && !lex_is_reserved_word(tok_text(p, peek(p)))) {
    Token const &t{ peek(p) };
    if (!syntax_state_kind_from_name(tok_text(p, t), stmt.kind)) {
      error_at(p, DiagCode::UnknownStateKind, t);
      return false;
    }
    advance(p);
  }
  if (!take_optional_label(p, stmt.label)) { return false; }

  opens_block = at_kind(p, TokKind::LBrace);
  stmt.has_block = opens_block ? 1U : 0U;
  p.pd->states.push_back(stmt);
  return true;
}

// submachine := ('submachine'|'m') [ident] [string] block
bool parse_submachine(Parser &p, uint32_t &out_stmt, bool &opens_block) {
  uint32_t const payload{ narrow_clamp<uint32_t>(p.pd->submachines.size()) };
  out_stmt = begin_stmt(p, StmtKind::Submachine, payload);
  advance(p);  // 'submachine' or 'm'

  SubmachineStmt stmt{ .name = {}, .label = {} };
  if (at_kind(p, TokKind::Ident) && !take_name(p, stmt.name)) { return false; }
  if (!take_optional_label(p, stmt.label)) { return false; }
  p.pd->submachines.push_back(stmt);

  // Unlike state and trans, the block is mandatory: a submachine with no states
  // says nothing that leaving it out does not.
  if (!at_kind(p, TokKind::LBrace)) {
    error_at(p, DiagCode::ExpectedBlock, peek(p));
    return false;
  }
  opens_block = true;
  return true;
}

// trans := ('trans'|'t') [trans_kind] endpoint '->' endpoint [string] [block]
bool parse_trans(Parser &p, uint32_t &out_stmt, bool &opens_block) {
  uint32_t const payload{ narrow_clamp<uint32_t>(p.pd->transitions.size()) };
  out_stmt = begin_stmt(p, StmtKind::Trans, payload);
  advance(p);  // 'trans' or 't'

  TransStmt stmt{ .src = {},
                  .dst = {},
                  .label = {},
                  .kind = TransKind::External,
                  .has_block = 0 };
  // The three kind words are reserved, so an identifier here is a kind if it is
  // one of them and a state name otherwise -- no lookahead needed.
  if (at_kind(p, TokKind::Ident)) {
    TransKind kind{ TransKind::External };
    if (syntax_trans_kind_from_name(tok_text(p, peek(p)), kind)) {
      stmt.kind = kind;
      advance(p);
    }
  }
  if (!parse_endpoint(p, stmt.src)) { return false; }
  if (!expect(p, TokKind::Arrow, DiagCode::ExpectedArrow)) { return false; }
  if (!parse_endpoint(p, stmt.dst)) { return false; }
  if (!take_optional_label(p, stmt.label)) { return false; }

  opens_block = at_kind(p, TokKind::LBrace);
  stmt.has_block = opens_block ? 1U : 0U;
  p.pd->transitions.push_back(stmt);
  return true;
}

// item := include | state | submachine | trans | attr
bool parse_item(Parser &p, uint32_t &out_stmt, bool &opens_block) {
  opens_block = false;
  if (at_kind(p, TokKind::At)) { return parse_attr(p, out_stmt); }
  if (!at_kind(p, TokKind::Ident)) {
    error_at(p, DiagCode::ExpectedItem, peek(p));
    return false;
  }

  std::string_view const word{ tok_text(p, peek(p)) };
  if (word == "include") { return parse_include(p, out_stmt); }
  // s / m / t are aliases only in statement-leading position, which is exactly
  // here, so `state s` still declares a state named `s`.
  if ((word == "state") || (word == "s")) { return parse_state(p, out_stmt, opens_block); }
  if ((word == "submachine") || (word == "m")) {
    return parse_submachine(p, out_stmt, opens_block);
  }
  if ((word == "trans") || (word == "t")) { return parse_trans(p, out_stmt, opens_block); }

  error_at(p, DiagCode::ExpectedItem, peek(p));
  return false;
}

// chart := 'chart' ident [string] block
bool parse_chart_header(Parser &p) {
  if (!at_kind(p, TokKind::Ident) || (tok_text(p, peek(p)) != "chart")) {
    error_at(p, DiagCode::ExpectedChart, peek(p));
    return false;
  }
  uint32_t const stmt{ begin_stmt(p, StmtKind::Chart, 0) };
  advance(p);

  ChartStmt chart{ .name = {}, .label = {} };
  if (!take_name(p, chart.name)) { return false; }
  if (!take_optional_label(p, chart.label)) { return false; }
  p.pd->charts.push_back(chart);

  if (!at_kind(p, TokKind::LBrace)) {
    error_at(p, DiagCode::ExpectedBlock, peek(p));
    return false;
  }
  advance(p);
  p.frames.push_back({ .owner = stmt,
                       .scratch_begin = narrow_clamp<uint32_t>(p.scratch.size()),
                       .slot = Slot::Item });
  return true;
}

// Pops the innermost frame, moving its children into the shared id array and
// handing the frame's own statement to the enclosing block.
void close_frame(Parser &p) {
  Frame const frame{ p.frames.back() };
  p.frames.pop_back();

  uint32_t const count{ narrow_clamp<uint32_t>(p.scratch.size()) - frame.scratch_begin };
  Span const children{ make_span(narrow_clamp<uint32_t>(p.pd->stmt_ids.size()), count) };
  for (uint32_t i = frame.scratch_begin; i < p.scratch.size(); ++i) {
    p.pd->stmt_ids.push_back(p.scratch[i]);
  }
  p.scratch.resize(frame.scratch_begin);

  p.pd->stmt_children[frame.owner] = children;
  end_stmt(p, frame.owner);

  if (!p.frames.empty()) {
    // Back to exactly where the parent's own children left off, so document
    // order survives the round trip through the scratch stack.
    p.scratch.push_back({ .v = frame.owner });
    p.frames.back().slot = Slot::Separator;
  }
}

bool run(Parser &p) {
  if (!parse_chart_header(p)) { return false; }

  while (!p.frames.empty()) {
    if (p.frames.back().slot == Slot::Separator) {
      if (at_kind(p, TokKind::Comma)) {
        advance(p);
        p.frames.back().slot = Slot::Item;
        continue;
      }
      if (at_kind(p, TokKind::RBrace)) {
        advance(p);
        close_frame(p);
        continue;
      }
      error_at(p, DiagCode::ExpectedSeparator, peek(p));
      return false;
    }

    // Slot::Item. An immediate '}' is either an empty block or a trailing comma.
    if (at_kind(p, TokKind::RBrace)) {
      advance(p);
      close_frame(p);
      continue;
    }

    uint32_t stmt{ 0 };
    bool opens_block{ false };
    if (!parse_item(p, stmt, opens_block)) { return false; }

    if (opens_block) {
      if (p.frames.size() >= p.max_depth) {
        error_at(p, DiagCode::DepthLimitExceeded, peek(p));
        return false;
      }
      advance(p);  // '{'
      p.frames.push_back({ .owner = stmt,
                           .scratch_begin = narrow_clamp<uint32_t>(p.scratch.size()),
                           .slot = Slot::Item });
      continue;
    }

    end_stmt(p, stmt);
    p.scratch.push_back({ .v = stmt });
    p.frames.back().slot = Slot::Separator;
  }

  if (!at_kind(p, TokKind::End)) {
    error_at(p, DiagCode::TrailingContent, peek(p));
    return false;
  }
  return true;
}

CommentPos classify(LexComment const &c) {
  if (c.code_before != 0) { return CommentPos::Trailing; }
  return (c.blank_after != 0) ? CommentPos::OwnLine : CommentPos::Leading;
}

struct WalkFrame {
  uint32_t stmt;
  uint32_t child;       // index into the statement's children span
  uint32_t last_child;  // the sibling a trailing comment attaches to
};

// Positional, and after the tree is built: a block's leading and trailing
// trivia are separated by all of its children's.
void attach_comments(ParsedDocument &pd, std::vector<LexComment> const &lexed) {
  uint32_t const n{ narrow_clamp<uint32_t>(lexed.size()) };
  pd.comments.clear();
  if (n == 0) { return; }

  std::vector<uint32_t> owner(n, 0);
  uint32_t const root{ syntax_root_statement(pd) };
  if (root == INVALID) { return; }

  uint32_t next{ 0 };
  // Anything before the chart keyword: the loop below only sees comments inside
  // a span, and the root has no parent to have claimed these.
  while ((next < n) && (lexed[next].src.off < pd.stmts[root].src.off)) {
    owner[next] = root;
    ++next;
  }

  std::vector<WalkFrame> stack;
  stack.push_back({ .stmt = root, .child = 0, .last_child = INVALID });

  while (!stack.empty() && (next < n)) {
    WalkFrame &f{ stack.back() };
    Span const children{ pd.stmt_children[f.stmt] };

    if (f.child < children.len) {
      uint32_t const child{ pd.stmt_ids[children.off + f.child].v };
      uint32_t const child_begin{ pd.stmts[child].src.off };
      while ((next < n) && (lexed[next].src.off < child_begin)) {
        // A comment sharing its line with code follows the previous sibling; one
        // on its own line introduces the next.
        bool const trailing{ classify(lexed[next]) == CommentPos::Trailing };
        uint32_t const target{ trailing ? f.last_child : child };
        owner[next] = (target == INVALID) ? f.stmt : target;
        ++next;
      }
      f.last_child = child;
      ++f.child;
      stack.push_back({ .stmt = child, .child = 0, .last_child = INVALID });
      continue;
    }

    Span const src{ pd.stmts[f.stmt].src };
    uint32_t const end{ src.off + src.len };
    while ((next < n) && (lexed[next].src.off < end)) {
      bool const trailing{ classify(lexed[next]) == CommentPos::Trailing };
      uint32_t const target{ trailing ? f.last_child : INVALID };
      owner[next] = (target == INVALID) ? f.stmt : target;
      ++next;
    }
    stack.pop_back();
  }

  // Anything past the chart's closing brace belongs to the chart.
  while (next < n) {
    owner[next] = root;
    ++next;
  }

  // Counting sort by owner, so each statement's comments are one contiguous
  // span and source order survives inside it.
  uint32_t const stmt_count{ narrow_clamp<uint32_t>(pd.stmts.size()) };
  std::vector<uint32_t> counts(stmt_count + 1, 0);
  for (uint32_t i = 0; i < n; ++i) { ++counts[owner[i] + 1]; }
  for (uint32_t i = 0; i < stmt_count; ++i) { counts[i + 1] += counts[i]; }
  for (uint32_t i = 0; i < stmt_count; ++i) {
    pd.stmts[i].comments = make_span(counts[i], counts[i + 1] - counts[i]);
  }

  pd.comments.resize(n);
  std::vector<uint32_t> cursor(counts.begin(), counts.end() - 1);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t const slot{ cursor[owner[i]]++ };
    pd.comments[slot] = { .src = lexed[i].src, .pos = classify(lexed[i]) };
  }
}

}  // namespace

ParseOptions parse_default_options() { return { .max_depth = DEFAULT_MAX_DEPTH }; }

bool parse_tokens(scav_byte const *bytes,
                  size_t byte_count,
                  LexResult const &lexed,
                  DocId doc,
                  std::string_view name,
                  ParseOptions const &opts,
                  ParsedDocument &out,
                  std::vector<Diagnostic> &diags) {
  out = {};
  out.id = doc;

  // Statement.src is a Span, so the document has to be addressable by one.
  uint32_t len{ 0 };
  if (!narrow(byte_count, len)) {
    diags.push_back({ .code = DiagCode::DocumentTooLarge, .doc = doc, .src = {} });
    return false;
  }

  out.src_bytes.assign(bytes, bytes + len);
  out.doc.text = make_span(0, len);

  // A statement is a keyword plus a couple of tokens, so tokens/4 spares the
  // statement arrays most of their doubling copies.
  uint32_t const stmt_estimate{ narrow_clamp<uint32_t>(lexed.tokens.size() / 4) };
  out.stmts.reserve(stmt_estimate);
  out.stmt_payload.reserve(stmt_estimate);
  out.stmt_children.reserve(stmt_estimate);
  out.stmt_ids.reserve(stmt_estimate);

  // The stream always ends with an End sentinel, which is what lets lookahead
  // skip its bounds check. An empty one is a caller error, not input.
  if (lexed.tokens.empty()) {
    diags.push_back(
        { .code = DiagCode::ExpectedChart, .doc = doc, .src = make_span(0, 0) });
    return false;
  }

  Parser p{ .bytes = out.src_bytes.data(),
            .len = len,
            .tokens = lexed.tokens.data(),
            .token_count = narrow_clamp<uint32_t>(lexed.tokens.size()),
            .doc = doc,
            .max_depth = (opts.max_depth == 0) ? DEFAULT_MAX_DEPTH : opts.max_depth,
            .pd = &out,
            .diags = &diags,
            .pos = 0,
            .last_end = 0,
            .scratch = {},
            .frames = {},
            .strbuf = {} };
  out.doc.path = pool_append(p, name);

  bool const ok{ run(p) };
  out.doc.statements = make_span(0, narrow_clamp<uint32_t>(out.stmts.size()));
  if (ok) { attach_comments(out, lexed.comments); }
  return ok;
}

bool parse_document(scav_byte const *bytes,
                    size_t len,
                    std::string_view name,
                    ParseOptions const &opts,
                    ParsedDocument &out,
                    std::vector<Diagnostic> &diags) {
  out = {};
  DocId const doc{ 0 };

  std::vector<scav_byte> normalized;
  if (!source_text_normalize(bytes, len, doc, normalized, diags)) { return false; }

  // source_text_normalize already rejected anything a Span cannot address, so
  // this cannot overflow.
  uint32_t const norm_len{ narrow_clamp<uint32_t>(normalized.size()) };
  LexResult lexed;
  if (!lex_source(normalized.data(), norm_len, doc, lexed, diags)) {
    // Parse anyway when the lexer recovered: one run should report the syntax
    // error too rather than making the author fix stray bytes first.
    if (lexed.tokens.empty()) { return false; }
    parse_tokens(normalized.data(), norm_len, lexed, doc, name, opts, out, diags);
    return false;
  }
  return parse_tokens(normalized.data(), norm_len, lexed, doc, name, opts, out, diags);
}

uint64_t parse_footprint(ParsedDocument const &pd) {
  return bytes_of(pd.src_bytes) + bytes_of(pd.stmts) + bytes_of(pd.stmt_payload) +
         bytes_of(pd.stmt_children) + bytes_of(pd.stmt_ids) + bytes_of(pd.comments) +
         bytes_of(pd.charts) + bytes_of(pd.includes) + bytes_of(pd.states) +
         bytes_of(pd.submachines) + bytes_of(pd.transitions) + bytes_of(pd.attrs) +
         bytes_of(pd.attr_entries) + bytes_of(pd.attr_values) + bytes_of(pd.path_segs) +
         bytes_of(pd.strings.bytes);
}

}  // namespace scav
