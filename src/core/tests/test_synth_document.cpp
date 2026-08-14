#include "core/tests/test_synth_document.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scav {

namespace {

// Hand-rolled: std::to_string is locale-free for integers, but this generator
// feeds a hash-adjacent corpus and one call that is not is one too many.
void append_u32(std::string &out, uint32_t v) {
  std::array<char, 10> buf{};
  uint32_t n{ 0 };
  do {
    buf[n++] = static_cast<char>('0' + (v % 10U));
    v /= 10U;
  } while (v != 0);
  while (n != 0) { out.push_back(buf[--n]); }
}

void append_name(std::string &out, char const *prefix, uint32_t id) {
  out += prefix;
  append_u32(out, id);
}

void indent(std::string &out, uint32_t depth) {
  for (uint32_t i = 0; i < (2U * depth); ++i) { out.push_back(' '); }
}

// What remains to be emitted for one open block. Iterative like the parser: a
// test drives depth to 10,000, and a recursive generator would fall over first.
struct Frame {
  uint32_t depth;
  uint32_t submachine;  // next submachine to open, once the block body is done
  uint32_t indent;
  bool in_submachine;  // this frame is a submachine rather than a state
};

struct Gen {
  SynthSpec spec;
  std::string out;
  SynthStats stats;
  uint32_t next_id;
  uint32_t since_comment;
};

void maybe_comment(Gen &g, uint32_t depth) {
  if (g.spec.comment_every == 0) { return; }
  if (++g.since_comment < g.spec.comment_every) { return; }
  g.since_comment = 0;
  indent(g.out, depth);
  g.out += "// note ";
  append_u32(g.out, g.next_id);
  g.out.push_back('\n');
  ++g.stats.comments;
}

void emit_attrs(Gen &g, uint32_t depth, uint32_t id) {
  for (uint32_t i = 0; i < g.spec.attrs_per_state; ++i) {
    indent(g.out, depth);
    if (i == 0) {
      g.out += "@doc = \"state ";
      append_u32(g.out, id);
      g.out += "\",\n";
    } else if (i == 1) {
      // The block spelling and a list value, so the parser's two other attribute
      // shapes are on the hot path too.
      g.out += "@synth { kind = \"generated\", tags = [\"a\", \"b\"], flagged },\n";
    } else {
      g.out += "@synth:n";
      append_u32(g.out, i);
      g.out += " = \"v\",\n";
    }
    ++g.stats.attrs;
    ++g.stats.statements;
  }
}

// Leaf states and the transitions between them, which is the body of every
// submachine.
void emit_leaves(Gen &g, uint32_t depth) {
  uint32_t const first{ g.next_id };
  for (uint32_t i = 0; i < g.spec.states_per_block; ++i) {
    maybe_comment(g, depth);
    indent(g.out, depth);
    g.out += "state ";
    append_name(g.out, "A", g.next_id++);
    if ((i % 3U) == 1U) { g.out += " choice"; }
    if ((i % 4U) == 2U) { g.out += " \"a leaf\""; }
    g.out += ",\n";
    ++g.stats.states;
    ++g.stats.statements;
  }

  if (g.spec.states_per_block == 0) { return; }

  indent(g.out, depth);
  g.out += "trans * -> ";
  append_name(g.out, "A", first);
  g.out += ",\n";
  ++g.stats.transitions;
  ++g.stats.statements;

  for (uint32_t i = 0; i < g.spec.transitions_per_block; ++i) {
    uint32_t const from{ first + (i % g.spec.states_per_block) };
    uint32_t const to{ first + ((i + 1U) % g.spec.states_per_block) };
    indent(g.out, depth);
    g.out += "trans ";
    if ((i % 5U) == 3U) { g.out += "internal "; }
    append_name(g.out, "A", from);
    g.out += " -> ";
    append_name(g.out, "A", to);
    g.out += " \"EV_";
    append_u32(g.out, i);
    g.out += "\",\n";
    ++g.stats.transitions;
    ++g.stats.statements;
  }
}

// One top-level composite state and everything under it. Nesting continues in
// submachine 0 only, so a subtree is linear in `depth`, not exponential.
void emit_subtree(Gen &g) {
  std::vector<Frame> stack;
  uint32_t const root_id{ g.next_id++ };

  indent(g.out, 1);
  g.out += "state ";
  append_name(g.out, "S", root_id);
  g.out += " \"composite\" {\n";
  ++g.stats.states;
  ++g.stats.statements;
  stack.push_back({ .depth = 0, .submachine = 0, .indent = 2, .in_submachine = false });

  while (!stack.empty()) {
    Frame &f{ stack.back() };

    if (f.in_submachine) {
      // A submachine's body is emitted when it opens, so reaching it again
      // means it is finished.
      uint32_t const close{ f.indent - 1 };
      stack.pop_back();
      indent(g.out, close);
      g.out += "},\n";
      continue;
    }

    if (f.submachine == 0) { emit_attrs(g, f.indent, root_id + f.depth); }

    if (f.submachine >= g.spec.submachines_per_state) {
      uint32_t const close{ f.indent - 1 };
      stack.pop_back();
      indent(g.out, close);
      g.out += "},\n";
      continue;
    }

    uint32_t const which{ f.submachine++ };
    uint32_t const body_indent{ f.indent + 1 };
    uint32_t const depth{ f.depth };

    indent(g.out, f.indent);
    g.out += "submachine ";
    append_name(g.out, "m", which);
    if (which == 0) { g.out += " \"primary\""; }
    g.out += " {\n";
    ++g.stats.submachines;
    ++g.stats.statements;

    // Pushed before the body is written, so the close brace is owed even if the
    // nested state below pushes another frame first.
    stack.push_back(
        { .depth = depth, .submachine = 0, .indent = body_indent, .in_submachine = true });
    emit_leaves(g, body_indent);

    if ((which == 0) && (depth + 1 < g.spec.depth)) {
      indent(g.out, body_indent);
      g.out += "state ";
      append_name(g.out, "S", g.next_id++);
      g.out += " {\n";
      ++g.stats.states;
      ++g.stats.statements;
      stack.push_back({ .depth = depth + 1,
                        .submachine = 0,
                        .indent = body_indent + 1,
                        .in_submachine = false });
    }
  }
}

}  // namespace

SynthSpec synth_default_spec() {
  return { .depth = 16,
           .states_per_block = 4,
           .submachines_per_state = 2,
           .transitions_per_block = 3,
           .attrs_per_state = 3,
           .comment_every = 7,
           .min_roots = 2,
           .min_bytes = 0 };
}

std::string synth_document(SynthSpec const &spec, SynthStats &stats) {
  Gen g{ .spec = spec, .out = {}, .stats = {}, .next_id = 0, .since_comment = 0 };
  if (spec.min_bytes != 0) {
    g.out.reserve(static_cast<size_t>(spec.min_bytes) + (1U << 16U));
  }

  g.out += "// generated by synth_document\n";
  g.out += "chart synth \"synthetic corpus\" {\n";
  ++g.stats.comments;
  ++g.stats.statements;  // the chart itself

  g.out += "  include \"other.scav\" as other,\n";
  ++g.stats.statements;

  uint32_t roots{ 0 };
  while ((roots < spec.min_roots) || (g.out.size() < spec.min_bytes)) {
    emit_subtree(g);
    ++roots;
  }

  g.out += "}\n";
  stats = g.stats;
  return g.out;
}

std::string synth_deep_document(uint32_t depth) {
  std::string out{ "chart deep {\n" };
  for (uint32_t i = 0; i < depth; ++i) {
    out += "state D";
    append_u32(out, i);
    out += " {\n";
  }
  out += "state Leaf,\n";
  for (uint32_t i = 0; i < depth; ++i) { out += "},\n"; }
  out += "}\n";
  return out;
}

}  // namespace scav
