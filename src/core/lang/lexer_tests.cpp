// Tokens, trivia and string-literal decoding through the lexer's own entry
// point. Source is a raw literal, so a `.scav` string keeps its quotes.

#include "core/core_internal.h"
#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

std::vector<TokKind> kinds(std::string_view text) {
  Lexed const r{ lex_text(text) };
  std::vector<TokKind> out;
  out.reserve(r.result.tokens.size());
  for (Token const &t : r.result.tokens) { out.push_back(t.kind); }
  return out;
}

// The lexemes, so a test can assert what a token covered rather than only what
// kind it was.
std::vector<std::string> texts(Lexed const &r) {
  std::vector<std::string> out;
  out.reserve(r.result.tokens.size());
  for (Token const &t : r.result.tokens) {
    out.emplace_back(reinterpret_cast<char const *>(r.bytes.data() + t.off), t.len);
  }
  return out;
}

// A scav \u escape, built rather than written: a universal-character-name in
// this file's own source is a question about the C++ compiler, not about scav.
std::string u_escape(uint32_t cp) {
  static constexpr std::string_view DIGITS{ "0123456789abcdef" };
  std::string out{ "\\u" };
  for (int32_t shift = 12; shift >= 0; shift -= 4) {
    out.push_back(DIGITS[(cp >> static_cast<uint32_t>(shift)) & 0xFU]);
  }
  return out;
}

std::string quoted(std::string_view body) {
  std::string out{ '"' };
  out += body;
  out.push_back('"');
  return out;
}

struct Decoded {
  std::string text;
  std::vector<Diagnostic> diags;
  bool ok;
};

// Lexes one literal and decodes it, so the test writes source rather than a
// hand-computed span.
Decoded decode(std::string_view literal) {
  Lexed const r{ lex_text(literal) };
  Decoded d;
  if (r.result.tokens.empty() || (r.result.tokens[0].kind != TokKind::String)) {
    d.ok = false;
    d.diags = r.diags;
    return d;
  }
  Token const &t{ r.result.tokens[0] };
  std::vector<scav_byte> out;
  d.ok = lex_decode_string_literal(r.bytes.data(),
                                   make_span(t.off, t.len),
                                   DocId{ 0 },
                                   out,
                                   d.diags);
  d.text.assign(reinterpret_cast<char const *>(out.data()), out.size());
  return d;
}

}  // namespace

TEST_CASE("lex: an empty input is one End token") {
  CHECK(kinds("") == std::vector<TokKind>{ TokKind::End });
  CHECK(kinds("   \n\t  \n") == std::vector<TokKind>{ TokKind::End });
}

TEST_CASE("lex: a buffer too large for a token span is rejected") {
  // A Token's off/len is uint32. lex_source is a public entry point, so it
  // checks even though its usual caller normalized first.
  if constexpr (sizeof(size_t) > 4) {
    LexResult out;
    std::vector<Diagnostic> diags;
    CHECK_FALSE(lex_source(nullptr, SIZE_MAX, DocId{ 0 }, out, diags));
    CHECK(first_code(diags) == DiagCode::DocumentTooLarge);
    CHECK(out.tokens.empty());
  }
}

TEST_CASE("lex: every punctuation token") {
  CHECK(kinds("{}[],=@: / * ->") == std::vector<TokKind>{ TokKind::LBrace,
                                                          TokKind::RBrace,
                                                          TokKind::LBracket,
                                                          TokKind::RBracket,
                                                          TokKind::Comma,
                                                          TokKind::Equals,
                                                          TokKind::At,
                                                          TokKind::Colon,
                                                          TokKind::Slash,
                                                          TokKind::Star,
                                                          TokKind::Arrow,
                                                          TokKind::End });
}

TEST_CASE("lex: identifiers, numbers and strings") {
  CHECK(kinds(R"(Idle _x a9 09 "s")") == std::vector<TokKind>{ TokKind::Ident,
                                                               TokKind::Ident,
                                                               TokKind::Ident,
                                                               TokKind::Number,
                                                               TokKind::String,
                                                               TokKind::End });
}

TEST_CASE("lex: an identifier may not start with a digit") {
  // `9a` is a number then an identifier, which the parser rejects in context.
  CHECK(kinds("9a") ==
        std::vector<TokKind>{ TokKind::Number, TokKind::Ident, TokKind::End });
}

TEST_CASE("lex: keywords are ordinary identifiers") {
  // Reserved-word rejection is the parser's, because s/m/t are keywords only in
  // statement-leading position.
  Lexed const r{ lex_text("chart state s m t choice as kind") };
  for (uint32_t i = 0; i + 1 < r.result.tokens.size(); ++i) {
    CHECK(r.result.tokens[i].kind == TokKind::Ident);
  }
}

TEST_CASE("lex: token spans locate the lexeme exactly") {
  Lexed const r{ lex_text("state Idle,") };
  REQUIRE(r.result.tokens.size() == 4);
  CHECK(r.result.tokens[0].off == 0);
  CHECK(r.result.tokens[0].len == 5);
  CHECK(r.result.tokens[1].off == 6);
  CHECK(r.result.tokens[1].len == 4);
  CHECK(r.result.tokens[2].off == 10);
  CHECK(r.result.tokens[2].len == 1);
  CHECK(r.result.tokens[3].kind == TokKind::End);
  CHECK(r.result.tokens[3].off == 11);
  CHECK(r.result.tokens[3].len == 0);
  CHECK(texts(r) == std::vector<std::string>{ "state", "Idle", ",", "" });
}

TEST_CASE("lex: newlines carry nothing, so a chart on one line is the same stream") {
  CHECK(kinds("m main { s Idle, }") == kinds("m main {\n  s Idle,\n}\n"));
  CHECK(kinds("a\tb\vc\fd") == kinds("a b c d"));
}

TEST_CASE("lex: an arrow is one token and a bare minus is an error") {
  CHECK(kinds("->") == std::vector<TokKind>{ TokKind::Arrow, TokKind::End });
  Lexed const r{ lex_text("a - b") };
  CHECK_FALSE(r.ok);
  CHECK(first_code(r.diags) == DiagCode::ExpectedArrow);
  // Recovered: the identifier after it is still lexed.
  CHECK(r.result.tokens.size() == 3);
}

TEST_CASE("lex: an unexpected character is reported and skipped") {
  Lexed const r{ lex_text("a ? b ! c") };
  CHECK_FALSE(r.ok);
  CHECK(r.diags.size() == 2);
  CHECK(r.diags[0].code == DiagCode::UnexpectedCharacter);
  CHECK(r.diags[0].src.off == 2);
  CHECK(r.diags[1].src.off == 6);
  // Three identifiers survived, which is the point of recovering.
  CHECK(r.result.tokens.size() == 4);
}

TEST_CASE("lex: comments are trivia and never enter the token stream") {
  CHECK(kinds("// nothing here") == std::vector<TokKind>{ TokKind::End });
  Lexed const r{ lex_text("state A, // why\nstate B,") };
  for (Token const &t : r.result.tokens) { CHECK(t.kind != TokKind::String); }
  REQUIRE(r.result.comments.size() == 1);
  CHECK(r.result.comments[0].src.off == 9);
  CHECK(r.result.comments[0].src.len == 6);  // "// why", excluding the newline
}

TEST_CASE("lex: a comment knows whether code preceded it on its line") {
  Lexed const r{ lex_text("// own line\nstate A, // trailing\n") };
  REQUIRE(r.result.comments.size() == 2);
  CHECK(r.result.comments[0].code_before == 0);
  CHECK(r.result.comments[1].code_before == 1);
}

TEST_CASE("lex: a blank line after a comment detaches it from what follows") {
  Lexed const glued{ lex_text("// leading\nstate A,") };
  REQUIRE(glued.result.comments.size() == 1);
  CHECK(glued.result.comments[0].blank_after == 0);

  Lexed const floating{ lex_text("// floating\n\nstate A,") };
  REQUIRE(floating.result.comments.size() == 1);
  CHECK(floating.result.comments[0].blank_after == 1);
}

TEST_CASE("lex: a run of comments is one block, and the last one may still float") {
  Lexed const r{ lex_text("// one\n// two\n\nstate A,") };
  REQUIRE(r.result.comments.size() == 2);
  CHECK(r.result.comments[0].blank_after == 0);
  CHECK(r.result.comments[1].blank_after == 1);
}

TEST_CASE("lex: a comment ending the input has nothing to lead") {
  Lexed const r{ lex_text("state A,\n// last") };
  REQUIRE(r.result.comments.size() == 1);
  CHECK(r.result.comments[0].blank_after == 1);
}

TEST_CASE("lex: a slash that is not doubled is a path separator") {
  CHECK(kinds("a/b") == std::vector<TokKind>{ TokKind::Ident,
                                              TokKind::Slash,
                                              TokKind::Ident,
                                              TokKind::End });
  CHECK(kinds("a//b") == std::vector<TokKind>{ TokKind::Ident, TokKind::End });
}

TEST_CASE("lex: a block comment is rejected by name") {
  // `/*` is not a comment form, but it is the single most likely thing someone
  // types expecting one, so it gets a diagnostic rather than two stray tokens.
  Lexed const r{ lex_text("/* not a comment */") };
  CHECK_FALSE(r.ok);
  REQUIRE(r.diags.size() == 1);
  CHECK(r.diags[0].code == DiagCode::BlockCommentUnsupported);
  CHECK(r.diags[0].src == make_span(0, 2));  // the caret sits on the `/*`
  CHECK(r.result.comments.empty());          // and it is not recorded as trivia
}

TEST_CASE("lex: a block comment is skipped, so its body is not lexed as tokens") {
  // One diagnostic beats a cascade of nonsense from the text inside it.
  Lexed const r{ lex_text("state A, /* } , @ ! */ state B,") };
  CHECK_FALSE(r.ok);
  CHECK(r.diags.size() == 1);
  CHECK(kinds("state A, /* } , @ ! */ state B,") == std::vector<TokKind>{ TokKind::Ident,
                                                                          TokKind::Ident,
                                                                          TokKind::Comma,
                                                                          TokKind::Ident,
                                                                          TokKind::Ident,
                                                                          TokKind::Comma,
                                                                          TokKind::End });
}

TEST_CASE("lex: an unterminated block comment reports once and stops") {
  Lexed const r{ lex_text("state A, /* never closed") };
  CHECK_FALSE(r.ok);
  REQUIRE(r.diags.size() == 1);
  CHECK(r.diags[0].code == DiagCode::BlockCommentUnsupported);
  CHECK(r.result.tokens.back().kind == TokKind::End);
}

TEST_CASE("lex: a nested block comment still reports exactly once") {
  // No nesting rule to get wrong, because there is no nesting: the scan stops at
  // the first `*/`, and what follows is ordinary source.
  Lexed const r{ lex_text("/* a /* b */") };
  CHECK_FALSE(r.ok);
  CHECK(r.diags.size() == 1);
}

TEST_CASE("lex: the block-comment check does not shadow the real comment form") {
  // `//*` is a line comment whose text happens to start with `*`.
  Lexed const star{ lex_text("//* still a line comment\nstate A,") };
  CHECK(star.ok);
  CHECK(star.diags.empty());
  CHECK(star.result.comments.size() == 1);

  // `*/` alone is two ordinary tokens; only `/*` is special.
  Lexed const close{ lex_text("*/") };
  CHECK(close.ok);
  CHECK(kinds("*/") ==
        std::vector<TokKind>{ TokKind::Star, TokKind::Slash, TokKind::End });

  // And inside a string it is text, which is what a label needs.
  Lexed const in_string{ lex_text(R"("/* verbatim */")") };
  CHECK(in_string.ok);
  CHECK(in_string.diags.empty());
  CHECK(decode(R"("/* verbatim */")").text == "/* verbatim */");
}

TEST_CASE("lex: a quoted string keeps its delimiters in the token span") {
  Lexed const r{ lex_text(R"("hello")") };
  REQUIRE(r.result.tokens.size() == 2);
  CHECK(r.result.tokens[0].len == 7);
  CHECK(texts(r)[0] == R"("hello")");
}

TEST_CASE("lex: an escaped quote does not close the string") {
  Lexed const r{ lex_text(R"("a\"b" x)") };
  REQUIRE(r.result.tokens.size() == 3);
  CHECK(r.result.tokens[0].kind == TokKind::String);
  CHECK(texts(r)[0] == R"("a\"b")");
  CHECK(r.result.tokens[1].kind == TokKind::Ident);
}

TEST_CASE("lex: an unterminated string stops the scan but still ends the stream") {
  Lexed const r{ lex_text(R"(state "oops)") };
  CHECK_FALSE(r.ok);
  CHECK(first_code(r.diags) == DiagCode::UnterminatedString);
  // The End sentinel is owed even on the failure path, or lookahead has nothing
  // to stop at.
  REQUIRE_FALSE(r.result.tokens.empty());
  CHECK(r.result.tokens.back().kind == TokKind::End);
}

TEST_CASE("lex: a trailing backslash cannot escape the closing quote away") {
  Lexed const r{ lex_text(R"("abc\)") };
  CHECK_FALSE(r.ok);
  CHECK(first_code(r.diags) == DiagCode::UnterminatedString);
}

TEST_CASE("lex: a newline inside a quoted string is an error naming the fix") {
  Lexed const r{ lex_text("\"line one\nline two\"") };
  CHECK_FALSE(r.ok);
  CHECK(first_code(r.diags) == DiagCode::NewlineInString);
}

TEST_CASE("lex: a raw string spans to its closing triple quote") {
  Lexed const r{ lex_text("\"\"\"a\nb\"\"\"") };
  REQUIRE(r.result.tokens.size() == 2);
  CHECK(r.result.tokens[0].kind == TokKind::String);
  CHECK(r.result.tokens[0].len == 9);
}

TEST_CASE("lex: an empty raw string is six quotes") {
  Lexed const r{ lex_text(R"("""""")") };
  REQUIRE(r.result.tokens.size() == 2);
  CHECK(r.result.tokens[0].len == 6);
}

TEST_CASE("lex: an unterminated raw string is its own diagnostic") {
  Lexed const r{ lex_text(R"("""never closed)") };
  CHECK_FALSE(r.ok);
  CHECK(first_code(r.diags) == DiagCode::UnterminatedRawString);
}

TEST_CASE("decode: a plain string is its own bytes") {
  Decoded const d{ decode(R"("hello")") };
  CHECK(d.ok);
  CHECK(d.text == "hello");
  CHECK(decode(R"("")").text.empty());
}

TEST_CASE("decode: every escape the format defines") {
  CHECK(decode(R"("a\\b")").text == R"(a\b)");
  CHECK(decode(R"("a\"b")").text == R"(a"b)");
  CHECK(decode(R"("a\nb")").text == "a\nb");
  CHECK(decode(R"("a\tb")").text == "a\tb");
  CHECK(decode(quoted(u_escape(0x0041))).text == "A");
  CHECK(decode(quoted(u_escape(0x00E9))).text == "\xc3\xa9");
  CHECK(decode(quoted(u_escape(0xFFFD))).text == "\xef\xbf\xbd");
  CHECK(decode(quoted(u_escape(0x0000))).text == std::string(1, '\0'));
}

TEST_CASE("decode: an unknown escape is rejected rather than passed through") {
  Decoded const d{ decode(R"("a\qb")") };
  CHECK_FALSE(d.ok);
  CHECK(first_code(d.diags) == DiagCode::UnknownEscape);
  CHECK(d.text.empty());
  // \r is deliberately absent: normalization folds line endings, so a literal
  // carriage return would survive the fold the rest of the file went through.
  CHECK(first_code(decode(R"("a\rb")").diags) == DiagCode::UnknownEscape);
}

TEST_CASE("decode: a span ending mid-escape is rejected rather than read past") {
  // The lexer cannot produce this, but the decoder takes an arbitrary span, so
  // the guard is what keeps a hand-built one from reading past its end.
  std::string_view const bytes{ R"("ab\")" };
  std::vector<scav_byte> out;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(lex_decode_string_literal(raw(bytes),
                                        make_span(0, size32(bytes)),
                                        DocId{ 0 },
                                        out,
                                        diags));
  CHECK(first_code(diags) == DiagCode::TruncatedEscape);
  CHECK(out.empty());
}

TEST_CASE("decode: a malformed backslash-u is rejected") {
  CHECK(first_code(decode(quoted(R"(\u12)")).diags) == DiagCode::InvalidHexEscape);
  CHECK(first_code(decode(quoted(R"(\u12g4)")).diags) == DiagCode::InvalidHexEscape);
  CHECK(first_code(decode(quoted(R"(\u)")).diags) == DiagCode::InvalidHexEscape);
}

TEST_CASE("decode: a surrogate escape names the fix instead of encoding garbage") {
  CHECK(first_code(decode(quoted(u_escape(0xD800))).diags) == DiagCode::EscapedSurrogate);
  CHECK(first_code(decode(quoted(u_escape(0xDFFF))).diags) == DiagCode::EscapedSurrogate);
  // No pairing either: an astral character is written directly, so there is one
  // spelling of it rather than two.
  CHECK(first_code(decode(quoted(u_escape(0xD83D) + u_escape(0xDE00))).diags) ==
        DiagCode::EscapedSurrogate);
}

TEST_CASE("decode: an escape decoding to a decomposed sequence is NFC-folded") {
  // The source bytes were folded on read, but \u runs after that. Without this
  // the pool would hold two spellings of one string.
  Decoded const d{ decode(quoted("e" + u_escape(0x0301))) };
  CHECK(d.ok);
  CHECK(d.text == "\xc3\xa9");
}

TEST_CASE("decode: a raw string takes no escapes") {
  CHECK(decode(R"("""a\nb""")").text == R"(a\nb)");
  CHECK(decode(R"("""a\""")").text == R"(a\)");
}

TEST_CASE("decode: a single-line raw string is verbatim") {
  CHECK(decode(R"("""  spaced  """)").text == "  spaced  ");
  CHECK(decode(R"("""""")").text.empty());
}

TEST_CASE("decode: a raw string strips indentation to the closing delimiter") {
  Decoded const d{ decode("\"\"\"\n    one\n    two\n    \"\"\"") };
  CHECK(d.ok);
  CHECK(d.text == "one\ntwo");
}

TEST_CASE("decode: extra indentation beyond the closing column is kept") {
  Decoded const d{ decode("\"\"\"\n    one\n      two\n    \"\"\"") };
  CHECK(d.ok);
  CHECK(d.text == "one\n  two");
}

TEST_CASE("decode: a blank line in a raw string is clamped, not rejected") {
  Decoded const d{ decode("\"\"\"\n    one\n\n    two\n    \"\"\"") };
  CHECK(d.ok);
  CHECK(d.text == "one\n\ntwo");
}

TEST_CASE("decode: a line indented less than the closing delimiter is an error") {
  // Silently clamping would change the text the author wrote, which is exactly
  // what a raw string exists to prevent.
  Decoded const d{ decode("\"\"\"\n    one\n  two\n    \"\"\"") };
  CHECK_FALSE(d.ok);
  CHECK(first_code(d.diags) == DiagCode::RawStringUnderIndented);
}

TEST_CASE("decode: a raw string at column zero strips nothing") {
  Decoded const d{ decode("\"\"\"\none\ntwo\n\"\"\"") };
  CHECK(d.ok);
  CHECK(d.text == "one\ntwo");
}

TEST_CASE("decode: text on the opening line is kept as written") {
  Decoded const d{ decode("\"\"\"first\n  second\n  \"\"\"") };
  CHECK(d.ok);
  CHECK(d.text == "first\nsecond");
}

TEST_CASE("decode: a raw string keeps a trailing blank content line") {
  // Only the closing delimiter's own line is dropped; a blank line before it is
  // content the author asked for.
  Decoded const d{ decode("\"\"\"\n  one\n\n  \"\"\"") };
  CHECK(d.ok);
  CHECK(d.text == "one\n");
}

TEST_CASE("is_reserved_word: exactly the eight words the design lists") {
  for (char const *word : { "chart",
                            "include",
                            "state",
                            "submachine",
                            "trans",
                            "external",
                            "internal",
                            "local" }) {
    CHECK(lex_is_reserved_word(word));
  }
  for (char const *word : { "choice",
                            "history",
                            "deephistory",
                            "as",
                            "kind",
                            "s",
                            "m",
                            "t",
                            "normal",
                            "fork",
                            "join",
                            "junction",
                            "initial",
                            "final",
                            "Chart",
                            "STATE",
                            "" }) {
    CHECK_FALSE(lex_is_reserved_word(word));
  }
}

TEST_CASE("tok_kind_name: every kind is named") {
  for (uint32_t i = 0; i <= static_cast<uint32_t>(TokKind::Arrow); ++i) {
    CHECK(std::string{ lex_token_kind_name(static_cast<TokKind>(i)) } != "token");
  }
  CHECK(std::string{ lex_token_kind_name(static_cast<TokKind>(999)) } == "token");
}

TEST_CASE("lex_footprint: grows with the token count and never with nothing") {
  Lexed const empty{ lex_text("") };
  Lexed const big{ lex_text("chart c { state A, state B, state C, state D, }") };
  CHECK(lex_footprint(big.result) >= lex_footprint(empty.result));
  CHECK(lex_footprint(big.result) >= big.result.tokens.size() * sizeof(Token));
}

TEST_CASE("lex: the whole stream is materialized, not pulled") {
  // Asserted rather than assumed: every token exists before the parser sees any,
  // which is what lets the two be timed and fuzzed apart.
  Lexed const r{ lex_text("chart c { state A, }") };
  CHECK(r.result.tokens.size() == 8);
  CHECK(sizeof(Token) == 12);
}
