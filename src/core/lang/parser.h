#ifndef SCAV_CORE_LANG_PARSER_H_INCLUDED
#define SCAV_CORE_LANG_PARSER_H_INCLUDED

// LL(1) descent over PRD 15's grammar, with the descent held in an explicit
// std::vector of frames rather than in the call stack.
//
// That is not a style choice. Nesting depth is attacker-controlled, so a
// call-recursive parser's failure mode for a hostile document is a stack
// overflow -- which is a crash, not a diagnostic, and no depth cap can be
// checked reliably against a stack whose size the standard does not describe.
// With the frames on the heap the cap is an ordinary comparison and the answer
// is DiagCode::DepthLimitExceeded.

#include "core/lang/diagnostic.h"
#include "core/lang/lexer.h"
#include "core/lang/parse_tree.h"
#include "core/model/ids.h"
#include "scav/types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

// Depth 16 is the design target (PRD 11), not a limit the grammar enforces. The
// format spends two block levels per state level plus one for the chart, so a
// legal depth-16 chart nests 33 deep; 256 leaves room for anything an author
// writes and still rejects a hostile document before the heap notices.
constexpr uint32_t DEFAULT_MAX_DEPTH{ 256 };

struct ParseOptions {
  uint32_t max_depth;
};

ParseOptions default_parse_options();

// Bytes must already be normalized. `name` is the document's path, and exists
// so a diagnostic can say `wifi.scav:12` rather than `<buffer>:12`.
bool parse_tokens(scav_byte const *bytes,
                  uint32_t len,
                  LexResult const &lexed,
                  DocId doc,
                  std::string_view name,
                  ParseOptions const &opts,
                  ParsedDocument &out,
                  std::vector<Diagnostic> &diags);

// normalize -> lex -> parse, over bytes from anywhere. There is no overload
// taking a path: acquiring bytes is a different system (PRD 16.2).
bool parse_document(scav_byte const *bytes,
                    uint32_t len,
                    std::string_view name,
                    ParseOptions const &opts,
                    ParsedDocument &out,
                    std::vector<Diagnostic> &diags);

// Bytes the parsed document holds, for the memory-ratio assertion in the
// performance tests.
uint64_t parse_footprint(ParsedDocument const &pd);

}  // namespace scav

#endif  // SCAV_CORE_LANG_PARSER_H_INCLUDED
