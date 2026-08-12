#include "scav/scav_core.h"

#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

namespace {

// One token per six bytes of input is the measured shape of a dense chart. An
// over-estimate costs one reallocation; an under-estimate costs log n of them.
constexpr uint32_t BYTES_PER_TOKEN_ESTIMATE{ 6 };

constexpr uint32_t RAW_DELIM_LEN{ 3 };

bool is_ident_start(scav_byte b) {
  return ((b >= 'A') && (b <= 'Z')) || ((b >= 'a') && (b <= 'z')) || (b == '_');
}

bool is_digit(scav_byte b) { return (b >= '0') && (b <= '9'); }

bool is_ident_continue(scav_byte b) { return is_ident_start(b) || is_digit(b); }

// The lexer never sees a CR: normalization folded every line ending to LF
// before it ran.
bool is_space(scav_byte b) {
  return (b == ' ') || (b == '\t') || (b == '\v') || (b == '\f');
}

bool is_raw_delim_at(scav_byte const *bytes, uint32_t len, uint32_t at) {
  return (at + RAW_DELIM_LEN <= len) && (bytes[at] == '"') && (bytes[at + 1] == '"') &&
         (bytes[at + 2] == '"');
}

// Every operand is uint32_t: a `scav_byte` promotes to int, so mixing in a char
// literal converts back to unsigned and GCC's -Warith-conversion is right to say
// so.
constexpr uint32_t DIGIT_ZERO{ '0' };
constexpr uint32_t DIGIT_NINE{ '9' };
constexpr uint32_t LOWER_A{ 'a' };
constexpr uint32_t LOWER_F{ 'f' };
constexpr uint32_t UPPER_A{ 'A' };
constexpr uint32_t UPPER_F{ 'F' };

uint32_t hex_value(scav_byte b) {
  uint32_t const c{ b };
  if ((c >= DIGIT_ZERO) && (c <= DIGIT_NINE)) { return c - DIGIT_ZERO; }
  if ((c >= LOWER_A) && (c <= LOWER_F)) { return (c - LOWER_A) + 10U; }
  if ((c >= UPPER_A) && (c <= UPPER_F)) { return (c - UPPER_A) + 10U; }
  return INVALID;
}

TokKind punctuation_kind(scav_byte b) {
  switch (b) {
    case '{': return TokKind::LBrace;
    case '}': return TokKind::RBrace;
    case '[': return TokKind::LBracket;
    case ']': return TokKind::RBracket;
    case ',': return TokKind::Comma;
    case '=': return TokKind::Equals;
    case '@': return TokKind::At;
    case ':': return TokKind::Colon;
    case '/': return TokKind::Slash;
    case '*': return TokKind::Star;
    default: return TokKind::End;
  }
}

// Length including both quotes, or 0 with `err` set. Escape *validity* is the
// decoder's; all this knows is that a backslash defers the closing quote.
uint32_t scan_quoted(scav_byte const *bytes, uint32_t len, uint32_t at, DiagCode &err) {
  uint32_t i{ at + 1 };
  while (i < len) {
    scav_byte const b{ bytes[i] };
    if (b == '\n') {
      err = DiagCode::NewlineInString;
      return 0;
    }
    if (b == '\\') {
      if (i + 1 >= len) { break; }
      i += 2;
      continue;
    }
    if (b == '"') { return (i - at) + 1; }
    ++i;
  }
  err = DiagCode::UnterminatedString;
  return 0;
}

uint32_t scan_raw(scav_byte const *bytes, uint32_t len, uint32_t at, DiagCode &err) {
  for (uint32_t i = at + RAW_DELIM_LEN; i + RAW_DELIM_LEN <= len; ++i) {
    if (is_raw_delim_at(bytes, len, i)) { return (i - at) + RAW_DELIM_LEN; }
  }
  err = DiagCode::UnterminatedRawString;
  return 0;
}

// Appends `line` minus `strip` leading whitespace. A whitespace-only line is
// clamped to empty, not rejected: the under-indentation rule is about content.
bool emit_raw_line(scav_byte const *bytes,
                   uint32_t off,
                   uint32_t len,
                   uint32_t strip,
                   DocId doc,
                   std::vector<scav_byte> &out,
                   std::vector<Diagnostic> &diags) {
  uint32_t ws{ 0 };
  while ((ws < len) && is_space(bytes[off + ws])) { ++ws; }
  if (ws >= len) { return true; }  // blank line
  if (ws < strip) {
    diags.push_back({ .code = DiagCode::RawStringUnderIndented,
                      .doc = doc,
                      .src = make_span(off, len) });
    return false;
  }
  out.insert(out.end(), bytes + off + strip, bytes + off + len);
  return true;
}

bool decode_raw(scav_byte const *bytes,
                Span lexeme,
                DocId doc,
                std::vector<scav_byte> &out,
                std::vector<Diagnostic> &diags) {
  uint32_t const begin{ lexeme.off + RAW_DELIM_LEN };
  uint32_t const end{ (lexeme.off + lexeme.len) - RAW_DELIM_LEN };

  uint32_t last_newline{ INVALID };
  for (uint32_t i = begin; i < end; ++i) {
    if (bytes[i] == '\n') { last_newline = i; }
  }
  if (last_newline == INVALID) {  // single line: nothing to strip
    out.insert(out.end(), bytes + begin, bytes + end);
    return true;
  }

  // Stripped to the closing delimiter's column, taken literally: the column is
  // whatever precedes the closing """ on its line.
  uint32_t const strip{ end - (last_newline + 1) };

  // Collect line bounds first, so the two ends can be trimmed by index instead
  // of by a lookahead inside the emit loop.
  std::vector<Span> lines;
  uint32_t line_begin{ begin };
  for (uint32_t i = begin; i <= end; ++i) {
    if ((i == end) || (bytes[i] == '\n')) {
      lines.push_back(make_span(line_begin, i - line_begin));
      line_begin = i + 1;
    }
  }

  uint32_t first{ 0 };
  // A newline right after the opening delimiter says its line carries no
  // content, which is the shape every multi-line literal is written in.
  if (lines[0].len == 0) { first = 1; }

  uint32_t stop{ narrow_clamp<uint32_t>(lines.size()) };
  if (stop > first) {
    Span const tail{ lines[stop - 1] };
    bool all_space{ true };
    for (uint32_t i = 0; i < tail.len; ++i) {
      if (!is_space(bytes[tail.off + i])) { all_space = false; }
    }
    // The closing delimiter's own indentation is not a line of content.
    if (all_space) { --stop; }
  }

  for (uint32_t i = first; i < stop; ++i) {
    if (i > first) { out.push_back('\n'); }
    // Text on the opening delimiter's own line was never indented, so stripping
    // it would eat content.
    uint32_t const strip_here{ ((i == 0) ? 0U : strip) };
    if (!emit_raw_line(bytes, lines[i].off, lines[i].len, strip_here, doc, out, diags)) {
      return false;
    }
  }
  return true;
}

bool decode_quoted(scav_byte const *bytes,
                   Span lexeme,
                   DocId doc,
                   std::vector<scav_byte> &out,
                   std::vector<Diagnostic> &diags) {
  uint32_t const end{ (lexeme.off + lexeme.len) - 1 };  // the closing quote
  uint32_t i{ lexeme.off + 1 };
  while (i < end) {
    if (bytes[i] != '\\') {
      out.push_back(bytes[i]);
      ++i;
      continue;
    }
    if (i + 1 >= end) {
      diags.push_back(
          { .code = DiagCode::TruncatedEscape, .doc = doc, .src = make_span(i, end - i) });
      return false;
    }

    scav_byte const esc{ bytes[i + 1] };
    if (esc == '\\') {
      out.push_back('\\');
    } else if (esc == '"') {
      out.push_back('"');
    } else if (esc == 'n') {
      out.push_back('\n');
    } else if (esc == 't') {
      out.push_back('\t');
    } else if (esc == 'u') {
      if (i + 6 > end) {
        diags.push_back({ .code = DiagCode::InvalidHexEscape,
                          .doc = doc,
                          .src = make_span(i, end - i) });
        return false;
      }
      uint32_t cp{ 0 };
      for (uint32_t d = 0; d < 4; ++d) {
        uint32_t const nibble{ hex_value(bytes[i + 2 + d]) };
        if (nibble == INVALID) {
          diags.push_back(
              { .code = DiagCode::InvalidHexEscape, .doc = doc, .src = make_span(i, 6) });
          return false;
        }
        cp = (cp << 4U) | nibble;
      }
      // No surrogate pairing: \u names a codepoint, and an astral character is
      // written directly. Pairing would make two spellings of one character.
      if ((cp >= 0xD800U) && (cp <= 0xDFFFU)) {
        diags.push_back(
            { .code = DiagCode::EscapedSurrogate, .doc = doc, .src = make_span(i, 6) });
        return false;
      }
      source_text_utf8_encode(cp, out);
      i += 6;
      continue;
    } else {
      diags.push_back(
          { .code = DiagCode::UnknownEscape, .doc = doc, .src = make_span(i, 2) });
      return false;
    }
    i += 2;
  }
  return true;
}

}  // namespace

char const *lex_token_kind_name(TokKind kind) {
  switch (kind) {
    case TokKind::End: return "end of input";
    case TokKind::Ident: return "identifier";
    case TokKind::Number: return "number";
    case TokKind::String: return "string";
    case TokKind::LBrace: return "'{'";
    case TokKind::RBrace: return "'}'";
    case TokKind::LBracket: return "'['";
    case TokKind::RBracket: return "']'";
    case TokKind::Comma: return "','";
    case TokKind::Equals: return "'='";
    case TokKind::At: return "'@'";
    case TokKind::Colon: return "':'";
    case TokKind::Slash: return "'/'";
    case TokKind::Star: return "'*'";
    case TokKind::Arrow: return "'->'";
  }
  return "token";
}

bool lex_is_reserved_word(std::string_view text) {
  return (text == "chart") || (text == "include") || (text == "state") ||
         (text == "submachine") || (text == "trans") || (text == "external") ||
         (text == "internal") || (text == "local");
}

bool lex_decode_string_literal(scav_byte const *bytes,
                               Span lexeme,
                               DocId doc,
                               std::vector<scav_byte> &out,
                               std::vector<Diagnostic> &diags) {
  out.clear();
  bool const raw{ (lexeme.len >= (2 * RAW_DELIM_LEN)) &&
                  is_raw_delim_at(bytes, lexeme.off + lexeme.len, lexeme.off) };
  bool const ok{ raw ? decode_raw(bytes, lexeme, doc, out, diags)
                     : decode_quoted(bytes, lexeme, doc, out, diags) };
  if (!ok) {
    out.clear();
    return false;
  }

  // The source was folded on read, but \u decodes after that -- so fold again,
  // or two spellings of one string intern as two.
  if (!source_text_is_nfc(out.data(), narrow_clamp<uint32_t>(out.size()))) {
    std::vector<scav_byte> composed;
    source_text_to_nfc(out.data(), narrow_clamp<uint32_t>(out.size()), composed);
    out.swap(composed);
  }
  return true;
}

bool lex_source(scav_byte const *bytes,
                size_t byte_count,
                DocId doc,
                LexResult &out,
                std::vector<Diagnostic> &diags) {
  out.tokens.clear();
  out.comments.clear();

  // A token's off/len is uint32. The usual caller normalized first and already
  // checked this, but lex_source is public and takes bytes from anywhere.
  uint32_t len{ 0 };
  if (!narrow(byte_count, len)) {
    diags.push_back({ .code = DiagCode::DocumentTooLarge, .doc = doc, .src = {} });
    return false;
  }

  out.tokens.reserve((len / BYTES_PER_TOKEN_ESTIMATE) + 1);

  bool ok{ true };
  bool code_on_line{ false };
  // The comment that could still turn out to be followed by a blank line.
  uint32_t open_comment{ INVALID };
  uint32_t newlines_after{ 0 };

  uint32_t at{ 0 };
  while (at < len) {
    scav_byte const b{ bytes[at] };

    if (b == '\n') {
      ++at;
      code_on_line = false;
      if (open_comment != INVALID) {
        ++newlines_after;
        // The first newline ends the comment's line; a second is a blank line,
        // which is what detaches a floating comment from the statement below.
        if (newlines_after >= 2) {
          out.comments[open_comment].blank_after = 1;
          open_comment = INVALID;
        }
      }
      continue;
    }
    if (is_space(b)) {
      ++at;
      continue;
    }

    if ((b == '/') && (at + 1 < len) && (bytes[at + 1] == '/')) {
      uint32_t stop{ at };
      while ((stop < len) && (bytes[stop] != '\n')) { ++stop; }
      open_comment = narrow_clamp<uint32_t>(out.comments.size());
      newlines_after = 0;
      out.comments.push_back({ .src = make_span(at, stop - at),
                               .code_before = code_on_line ? 1U : 0U,
                               .blank_after = 0 });
      at = stop;
      continue;
    }

    // After the line-comment check, so `//*` is still a line comment. Skipping to
    // the close means one diagnostic rather than a cascade from lexing the body
    // as tokens -- recovery only, since `ok` is already false.
    if ((b == '/') && (at + 1 < len) && (bytes[at + 1] == '*')) {
      diags.push_back({ .code = DiagCode::BlockCommentUnsupported,
                        .doc = doc,
                        .src = make_span(at, 2) });
      ok = false;
      uint32_t stop{ at + 2 };
      while ((stop + 1 < len) && ((bytes[stop] != '*') || (bytes[stop + 1] != '/'))) {
        ++stop;
      }
      at = (stop + 1 < len) ? (stop + 2) : len;
      continue;
    }

    // Any real token ends whatever comment run preceded it, which is what makes
    // that run the token's leading trivia rather than a floating block.
    open_comment = INVALID;
    code_on_line = true;

    if (is_ident_start(b)) {
      uint32_t stop{ at };
      while ((stop < len) && is_ident_continue(bytes[stop])) { ++stop; }
      out.tokens.push_back({ .off = at, .len = stop - at, .kind = TokKind::Ident });
      at = stop;
      continue;
    }
    if (is_digit(b)) {
      uint32_t stop{ at };
      while ((stop < len) && is_digit(bytes[stop])) { ++stop; }
      out.tokens.push_back({ .off = at, .len = stop - at, .kind = TokKind::Number });
      at = stop;
      continue;
    }
    if (b == '"') {
      DiagCode err{ DiagCode::Ok };
      uint32_t const width{ is_raw_delim_at(bytes, len, at)
                                ? scan_raw(bytes, len, at, err)
                                : scan_quoted(bytes, len, at, err) };
      if (width == 0) {
        diags.push_back({ .code = err, .doc = doc, .src = make_span(at, 1) });
        // No recovery that is not a guess. Stop, but fall through so the
        // stream still gets its End sentinel.
        ok = false;
        break;
      }
      out.tokens.push_back({ .off = at, .len = width, .kind = TokKind::String });
      at += width;
      continue;
    }
    if (b == '-') {
      if ((at + 1 < len) && (bytes[at + 1] == '>')) {
        out.tokens.push_back({ .off = at, .len = 2, .kind = TokKind::Arrow });
        at += 2;
        continue;
      }
      diags.push_back(
          { .code = DiagCode::ExpectedArrow, .doc = doc, .src = make_span(at, 1) });
      ok = false;
      ++at;
      continue;
    }

    if (TokKind const kind{ punctuation_kind(b) }; kind != TokKind::End) {
      out.tokens.push_back({ .off = at, .len = 1, .kind = kind });
      ++at;
      continue;
    }

    // Recoverable: skipping the byte lets one run report every stray character
    // rather than only the first.
    diags.push_back(
        { .code = DiagCode::UnexpectedCharacter, .doc = doc, .src = make_span(at, 1) });
    ok = false;
    ++at;
  }

  // Nothing follows a comment that ends the input, so it is floating rather
  // than leading.
  if (open_comment != INVALID) { out.comments[open_comment].blank_after = 1; }

  out.tokens.push_back({ .off = len, .len = 0, .kind = TokKind::End });
  return ok;
}

uint64_t lex_footprint(LexResult const &result) {
  return (uint64_t{ result.tokens.capacity() } * sizeof(Token)) +
         (uint64_t{ result.comments.capacity() } * sizeof(LexComment));
}

}  // namespace scav
