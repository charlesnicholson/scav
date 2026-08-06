// BOM stripping, line-ending folding, UTF-8 validation and the NFC pass over
// bytes, plus the line/column derivation every diagnostic goes through.

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;

scav_byte const *raw(std::string_view s) {
  return reinterpret_cast<scav_byte const *>(s.data());
}

uint32_t size32(std::string_view s) { return static_cast<uint32_t>(s.size()); }

struct Normalized {
  std::string text;
  std::vector<Diagnostic> diags;
  bool ok;
};

Normalized normalize(std::string_view input) {
  std::vector<scav_byte> out;
  Normalized r;
  r.ok = source_text_normalize(raw(input), size32(input), DocId{ 0 }, out, r.diags);
  r.text.assign(reinterpret_cast<char const *>(out.data()), out.size());
  return r;
}

std::string encode(uint32_t cp) {
  std::vector<scav_byte> out;
  source_text_utf8_encode(cp, out);
  return { reinterpret_cast<char const *>(out.data()), out.size() };
}

DiagCode decode_error(std::string_view bytes) {
  uint32_t cp{ 0 };
  uint32_t width{ 0 };
  DiagCode err{ DiagCode::Ok };
  source_text_utf8_decode(raw(bytes), size32(bytes), 0, cp, width, err);
  return err;
}

LineCol pos_of(std::string_view text, uint32_t offset) {
  return diag_line_col(raw(text), size32(text), offset);
}

}  // namespace

TEST_CASE("normalize: an empty input is valid and empty") {
  Normalized const r{ normalize("") };
  CHECK(r.ok);
  CHECK(r.text.empty());
  CHECK(r.diags.empty());
}

TEST_CASE("normalize: a document too large for a span is rejected, not truncated") {
  // The check runs before the pointer is touched, which is why nullptr is safe
  // here and why the failure is a diagnostic rather than a short read.
  if constexpr (sizeof(size_t) > 4) {
    std::vector<scav_byte> out;
    std::vector<Diagnostic> diags;
    CHECK_FALSE(source_text_normalize(nullptr, SIZE_MAX, DocId{ 0 }, out, diags));
    CHECK(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::DocumentTooLarge);
    CHECK(out.empty());
  }
}

TEST_CASE("normalize: ASCII passes through unchanged") {
  Normalized const r{ normalize("chart a { state B, }\n") };
  CHECK(r.ok);
  CHECK(r.text == "chart a { state B, }\n");
}

TEST_CASE("normalize: a UTF-8 BOM is dropped") {
  Normalized const r{ normalize(
      "\xef\xbb\xbf"
      "chart a {}") };
  CHECK(r.ok);
  CHECK(r.text == "chart a {}");
}

TEST_CASE("normalize: a BOM anywhere but the front is content") {
  // U+FEFF mid-document is a zero-width no-break space, not a signature.
  Normalized const r{ normalize(
      "a\xef\xbb\xbf"
      "b") };
  CHECK(r.ok);
  CHECK(r.text ==
        "a\xef\xbb\xbf"
        "b");
}

TEST_CASE("normalize: a two-byte prefix of a BOM is an incomplete sequence") {
  Normalized const r{ normalize("\xef\xbb") };
  CHECK_FALSE(r.ok);
  CHECK(r.diags[0].code == DiagCode::Utf8Truncated);
}

TEST_CASE("normalize: CRLF, lone CR and mixed endings all fold to LF") {
  CHECK(normalize("a\r\nb").text == "a\nb");
  CHECK(normalize("a\rb").text == "a\nb");
  CHECK(normalize("a\n\rb").text == "a\n\nb");
  CHECK(normalize("a\r\n\r\nb").text == "a\n\nb");
  CHECK(normalize("a\r").text == "a\n");
  CHECK(normalize("\r\n").text == "\n");
}

TEST_CASE("normalize: a CR inside a string literal folds too") {
  // Normalization is whole-file and runs before the lexer, which is why the
  // lexer never has to know CR exists.
  CHECK(normalize("\"a\r\nb\"").text == "\"a\nb\"");
}

TEST_CASE("utf8: encodes each sequence length") {
  CHECK(encode(0x0041) == "A");
  CHECK(encode(0x0000) == std::string(1, '\0'));
  CHECK(encode(0x007F) == "\x7f");
  CHECK(encode(0x0080) == "\xc2\x80");
  CHECK(encode(0x07FF) == "\xdf\xbf");
  CHECK(encode(0x0800) == "\xe0\xa0\x80");
  CHECK(encode(0xFFFF) == "\xef\xbf\xbf");
  CHECK(encode(0x10000) == "\xf0\x90\x80\x80");
  CHECK(encode(0x10FFFF) == "\xf4\x8f\xbf\xbf");
}

TEST_CASE("utf8: decode round-trips every non-surrogate codepoint") {
  for (uint32_t cp = 0; cp <= 0x10FFFF; ++cp) {
    if ((cp >= 0xD800) && (cp <= 0xDFFF)) { continue; }
    std::string const bytes{ encode(cp) };
    uint32_t decoded{ 0 };
    uint32_t width{ 0 };
    DiagCode err{ DiagCode::Ok };
    bool const ok{
      source_text_utf8_decode(raw(bytes), size32(bytes), 0, decoded, width, err)
    };
    if (!ok || (decoded != cp) || (width != bytes.size())) {
      FAIL("U+" << cp << " did not round-trip");
    }
  }
}

TEST_CASE("utf8: rejects every ill-formed shape by name") {
  CHECK(decode_error("\x80") == DiagCode::Utf8InvalidByte);  // stray continuation
  CHECK(decode_error("\xbf") == DiagCode::Utf8InvalidByte);
  CHECK(decode_error("\xf8\x88\x80\x80\x80") == DiagCode::Utf8InvalidByte);  // 5-byte
  CHECK(decode_error("\xff") == DiagCode::Utf8InvalidByte);
  CHECK(decode_error("\xc2") == DiagCode::Utf8Truncated);  // ran out
  CHECK(decode_error("\xe0\xa0") == DiagCode::Utf8Truncated);
  CHECK(decode_error("\xf0\x90\x80") == DiagCode::Utf8Truncated);
  CHECK(decode_error("\xc2\x41") == DiagCode::Utf8InvalidByte);  // not a continuation
  CHECK(decode_error("\xc0\x80") == DiagCode::Utf8Overlong);     // NUL the long way
  CHECK(decode_error("\xc1\xbf") == DiagCode::Utf8Overlong);
  CHECK(decode_error("\xe0\x80\x80") == DiagCode::Utf8Overlong);
  CHECK(decode_error("\xf0\x80\x80\x80") == DiagCode::Utf8Overlong);
  CHECK(decode_error("\xed\xa0\x80") == DiagCode::Utf8Surrogate);  // U+D800
  CHECK(decode_error("\xed\xbf\xbf") == DiagCode::Utf8Surrogate);  // U+DFFF
  CHECK(decode_error("\xf4\x90\x80\x80") == DiagCode::Utf8OutOfRange);
  CHECK(decode_error("\xf7\xbf\xbf\xbf") == DiagCode::Utf8OutOfRange);
}

TEST_CASE("utf8: decoding past the end reports truncation rather than reading") {
  uint32_t cp{ 0 };
  uint32_t width{ 0 };
  DiagCode err{ DiagCode::Ok };
  CHECK_FALSE(source_text_utf8_decode(nullptr, 0, 0, cp, width, err));
  CHECK(err == DiagCode::Utf8Truncated);
}

TEST_CASE("utf8: a failed decode still advances by one byte") {
  uint32_t cp{ 0 };
  uint32_t width{ 0 };
  DiagCode err{ DiagCode::Ok };
  std::string_view const bad{ "\x80\x41" };
  CHECK_FALSE(source_text_utf8_decode(raw(bad), size32(bad), 0, cp, width, err));
  CHECK(width == 1);
}

TEST_CASE("normalize: an invalid sequence fails the whole document") {
  Normalized const r{ normalize("chart a \xc3 {}") };
  CHECK_FALSE(r.ok);
  CHECK(r.text.empty());
  REQUIRE(r.diags.size() == 1);
  CHECK(r.diags[0].code == DiagCode::Utf8InvalidByte);
  // The span indexes the raw input, because there is no normalized buffer yet.
  CHECK(r.diags[0].src.off == 8);
}

TEST_CASE("normalize: NFC folds a decomposed sequence") {
  // "e" + combining acute becomes the precomposed character.
  Normalized const r{ normalize("state Caf\x65\xcc\x81") };
  CHECK(r.ok);
  CHECK(r.text == "state Caf\xc3\xa9");
}

TEST_CASE("normalize: an already-composed document is left alone") {
  Normalized const r{ normalize("state Caf\xc3\xa9") };
  CHECK(r.ok);
  CHECK(r.text == "state Caf\xc3\xa9");
}

TEST_CASE("normalize: BOM, CRLF and NFC compose in one pass") {
  Normalized const r{ normalize(
      "\xef\xbb\xbf"
      "a\r\ne\xcc\x81\r\n") };
  CHECK(r.ok);
  CHECK(r.text == "a\n\xc3\xa9\n");
}

TEST_CASE("normalize: zero-width joiners and format characters survive intact") {
  // NFC is decomposition plus composition and nothing else, so it never removes
  // a default-ignorable: a ZWJ sequence must come out byte-identical.
  std::string_view const family{
    "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
    "\xe2\x80\x8d\xf0\x9f\x91\xa7"
  };
  Normalized const r{ normalize(family) };
  CHECK(r.ok);
  CHECK(r.text == family);
  CHECK(source_text_is_nfc(raw(family), size32(family)));

  CHECK(normalize("a\xe2\x80\x8d"
                  "b")
            .text ==
        "a\xe2\x80\x8d"
        "b");  // ZWJ  U+200D
  CHECK(normalize("a\xe2\x80\x8c"
                  "b")
            .text ==
        "a\xe2\x80\x8c"
        "b");                                                 // ZWNJ U+200C
  CHECK(normalize("a\xef\xb8\x8f").text == "a\xef\xb8\x8f");  // VS16 U+FE0F
  CHECK(normalize("a\xe2\x80\x8b"
                  "b")
            .text ==
        "a\xe2\x80\x8b"
        "b");  // ZWSP U+200B
}

TEST_CASE("normalize: unassigned, noncharacter and bidi codepoints pass through") {
  // Valid UTF-8 that Unicode will not renormalize. Rejecting any of it would be
  // a policy this layer does not own.
  CHECK(normalize("\xef\xbf\xbe").text == "\xef\xbf\xbe");          // U+FFFE noncharacter
  CHECK(normalize("\xef\xb7\x90").text == "\xef\xb7\x90");          // U+FDD0 noncharacter
  CHECK(normalize("\xf3\xb0\x80\x80").text == "\xf3\xb0\x80\x80");  // U+F0000 private use

  // U+202E, built rather than written: clang-tidy flags a literal one, which is
  // the right reaction and is why a label carrying it deserves a rule later.
  std::string const rtl{ encode(0x202E) };
  CHECK(normalize(rtl).text == rtl);
}

TEST_CASE("normalize: a combining mark still composes across a joiner boundary") {
  // The joiner is a starter, so it blocks composition past itself -- the mark
  // before it composes, the text after it is untouched.
  CHECK(normalize("e\xcc\x81\xe2\x80\x8d"
                  "x")
            .text ==
        "\xc3\xa9\xe2\x80\x8d"
        "x");
}

TEST_CASE("is_nfc: byte-scans ASCII and only decodes what it must") {
  CHECK(source_text_is_nfc(raw("plain ascii"), 11));
  CHECK(source_text_is_nfc(nullptr, 0));
  CHECK(source_text_is_nfc(raw("caf\xc3\xa9"), 5));
  CHECK_FALSE(source_text_is_nfc(raw("cafe\xcc\x81"), 6));
  // Invalid UTF-8 is reported as not-NFC rather than crashing; the caller
  // validated before it got here.
  CHECK_FALSE(source_text_is_nfc(raw("\xc3"), 1));
}

TEST_CASE("is_ascii: exact at the boundary") {
  CHECK(source_text_is_ascii(raw("\x7f"), 1));
  CHECK_FALSE(source_text_is_ascii(raw("\x80"), 1));
  CHECK(source_text_is_ascii(nullptr, 0));
}

TEST_CASE("nfc_bytes: reports whether it changed anything") {
  std::vector<scav_byte> out;
  CHECK_FALSE(source_text_to_nfc(raw("abc"), 3, out));
  CHECK(std::string(reinterpret_cast<char const *>(out.data()), out.size()) == "abc");
  CHECK(source_text_to_nfc(raw("e\xcc\x81"), 3, out));
  CHECK(std::string(reinterpret_cast<char const *>(out.data()), out.size()) == "\xc3\xa9");
}

TEST_CASE("nfc_bytes: invalid UTF-8 is passed through rather than dropped") {
  // Normalization validates first, so this fires only on a fuzz case calling in
  // directly -- and losing bytes silently is worse than carrying them.
  std::vector<scav_byte> out;
  CHECK_FALSE(source_text_to_nfc(raw("\xc3"), 1, out));
  CHECK(out.size() == 1);
  CHECK(out[0] == 0xC3);
}

TEST_CASE("line_col: one-based, and the column counts characters not bytes") {
  std::string_view const text{ "ab\ncd\n\nxy" };
  CHECK(pos_of(text, 0).line == 1);
  CHECK(pos_of(text, 0).column == 1);
  CHECK(pos_of(text, 1).column == 2);
  CHECK(pos_of(text, 2).line == 1);  // the newline itself is still on line 1
  CHECK(pos_of(text, 3).line == 2);
  CHECK(pos_of(text, 3).column == 1);
  CHECK(pos_of(text, 6).line == 3);
  CHECK(pos_of(text, 7).line == 4);
  CHECK(pos_of(text, 8).column == 2);
}

TEST_CASE("line_col: a multi-byte character advances the column once") {
  // Four bytes, two characters, so the caret lands where a reader expects.
  std::string_view const text{ "\xc3\xa9\xc3\xa9x" };
  CHECK(pos_of(text, 4).column == 3);
  CHECK(pos_of(text, 0).column == 1);
  CHECK(pos_of(text, 2).column == 2);
}

TEST_CASE("line_col: an offset past the end clamps to the last position") {
  std::string_view const text{ "abc" };
  CHECK(pos_of(text, 99).line == 1);
  CHECK(pos_of(text, 99).column == 4);
}

TEST_CASE("diag_text: every code has a distinct description") {
  // A switch that falls through to the default is a code someone forgot.
  std::vector<std::string> seen;
  for (uint32_t i = 0; i <= static_cast<uint32_t>(DiagCode::DepthLimitExceeded); ++i) {
    std::string const text{ diag_message(static_cast<DiagCode>(i)) };
    CHECK(text != "unknown diagnostic");
    for (std::string const &other : seen) { CHECK(text != other); }
    seen.push_back(text);
  }
  CHECK(std::string{ diag_message(static_cast<DiagCode>(9999)) } == "unknown diagnostic");
}

TEST_CASE("has_errors: only a non-Ok code counts") {
  CHECK_FALSE(diag_has_errors({}));
  CHECK_FALSE(diag_has_errors({ { .code = DiagCode::Ok, .doc = DocId{ 0 }, .src = {} } }));
  CHECK(diag_has_errors(
      { { .code = DiagCode::ExpectedChart, .doc = DocId{ 0 }, .src = {} } }));
}

TEST_CASE("text_view: a zero-length span is empty and reads no bytes") {
  std::vector<scav_byte> const bytes{ 'a', 'b', 'c' };
  CHECK(source_text_view(bytes, make_span(0, 0)).empty());
  CHECK(source_text_view(bytes, make_span(1, 2)) == "bc");
}
