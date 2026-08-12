// Lowering: statements to entities (§17 P1). Two passes over one document.
// The first creates states, submachines, includes, and attrs in document
// order, following block structure; the second creates transitions, after
// every authored state exists, so a forward reference is an ordinary
// reference. The walk is an explicit stack for the same reason the parser's
// is: nesting depth is attacker-controlled.

#include "core/model/model_internal.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scav {

namespace {

struct Lowerer {
  Chart *c;
  ParsedDocument const *pd;
  std::vector<Diagnostic> *diags;
  DocId doc;
  uint32_t stmt_base;  // pd row + stmt_base = global StmtId
  bool clean;
};

// Every lowering diagnostic is statement-shaped: the entity it would describe
// was never created, so the payload is the statement's (rebased) span.
void report(Lowerer &lo, DiagCode code, uint32_t row) {
  lo.clean = false;
  lo.diags->push_back(
      { .code = code, .doc = lo.doc, .src = lo.c->stmts[lo.stmt_base + row].src });
}

std::string_view pd_str(Lowerer const &lo, StrRef ref) {
  return string_pool_view(lo.pd->strings, ref);
}

StmtId global_stmt(Lowerer const &lo, uint32_t row) { return { lo.stmt_base + row }; }

// What a block's statements land on. `sub` is where states, transitions, and
// includes go -- for a state block, its implicit submachine, created only
// when something needs it (§15).
struct Ctx {
  ElemRef subject;   // attr statements attach here
  SubmachineId sub;  // INVALID inside a trans block
  StateId state;     // valid only for a state block: explicit submachines' owner
};

struct Frame {
  Span children;  // -> pd.stmt_ids
  uint32_t next;
  Ctx ctx;
};

struct PendingTrans {
  uint32_t row;        // pd statement row
  SubmachineId scope;  // the submachine the statement lexically appears in
};

// True when a state block holds anything that must live in a submachine.
// Deciding before processing is what puts the implicit submachine at ordinal
// 0 even when an explicit sibling is written first (§15).
bool needs_implicit_submachine(ParsedDocument const &pd, uint32_t row) {
  Span const kids{ pd.stmt_children[row] };
  for (uint32_t i = 0; i < kids.len; ++i) {
    switch (pd.stmts[pd.stmt_ids[kids.off + i].v].kind) {
      case StmtKind::State:
      case StmtKind::Trans:
      case StmtKind::Include: return true;
      case StmtKind::Chart:
      case StmtKind::Submachine:
      case StmtKind::Attr: break;
    }
  }
  return false;
}

// One attr statement onto `subject`. A namespaced key interns composed
// (`@ns { a }` interns `ns:a`); a flag stores the literal "true", so which
// spelling to print is the printer's decision, not a stored fact; a list is
// one row per value in insertion order (§15).
void lower_attr(Lowerer &lo, uint32_t row, ElemRef subject) {
  AttrStmt const &stmt{ lo.pd->attrs[lo.pd->stmt_payload[row]] };
  std::string_view const ns{ pd_str(lo, stmt.ns) };
  std::string composed;
  for (uint32_t i = 0; i < stmt.entries.len; ++i) {
    AttrEntry const &entry{ lo.pd->attr_entries[stmt.entries.off + i] };
    std::string_view key{ pd_str(lo, entry.key) };
    if (!ns.empty()) {
      composed.assign(ns);
      composed.push_back(':');
      composed += key;
      key = composed;
    }
    switch (entry.kind) {
      case AttrValueKind::Flag: build_attr(*lo.c, subject, key, "true"); break;
      case AttrValueKind::Scalar:
      case AttrValueKind::List:
        for (uint32_t k = 0; k < entry.values.len; ++k) {
          build_attr(*lo.c,
                     subject,
                     key,
                     pd_str(lo, lo.pd->attr_values[entry.values.off + k]));
        }
        break;
    }
  }
}

// The entity pass. Returns the transitions it met, in document order, each
// with its lexical submachine -- the scope wildcards synthesize into and
// paths resolve from (§9).
std::vector<PendingTrans> lower_entities(Lowerer &lo, uint32_t root_row) {
  std::vector<PendingTrans> pending;
  std::vector<Frame> stack;

  ChartStmt const &chart_stmt{ lo.pd->charts[lo.pd->stmt_payload[root_row]] };
  SubmachineId const root{
    build_chart(*lo.c, pd_str(lo, chart_stmt.name), pd_str(lo, chart_stmt.label))
  };
  lo.c->submachines[root.v].stmt = global_stmt(lo, root_row);
  stack.push_back({ .children = lo.pd->stmt_children[root_row],
                    .next = 0,
                    .ctx = { .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                             .sub = root,
                             .state = { INVALID } } });

  while (!stack.empty()) {
    Frame &f{ stack.back() };
    if (f.next == f.children.len) {
      stack.pop_back();
      continue;
    }
    uint32_t const row{ lo.pd->stmt_ids[f.children.off + f.next].v };
    f.next += 1;
    Ctx const ctx{ f.ctx };  // copied: pushing a frame may move `f`

    switch (lo.pd->stmts[row].kind) {
      case StmtKind::State: {
        if (ctx.sub.v == INVALID) {  // inside a trans block
          report(lo, DiagCode::MisplacedStatement, row);
          break;
        }
        StateStmt const &s{ lo.pd->states[lo.pd->stmt_payload[row]] };
        StateId const id{
          build_state(*lo.c, ctx.sub, pd_str(lo, s.name), s.kind, pd_str(lo, s.label))
        };
        lo.c->states[id.v].stmt = global_stmt(lo, row);
        if (lo.pd->stmt_children[row].len != 0) {
          SubmachineId implicit{ INVALID };
          if (needs_implicit_submachine(*lo.pd, row)) {
            implicit = build_submachine(*lo.c, id, {}, {});
            lo.c->submachines[implicit.v].stmt = global_stmt(lo, row);
          }
          stack.push_back(
              { .children = lo.pd->stmt_children[row],
                .next = 0,
                .ctx = { .subject = { .kind = ElemKind::State, .ordinal = id.v },
                         .sub = implicit,
                         .state = id } });
        }
        break;
      }

      case StmtKind::Submachine: {
        // A submachine's children are states, so a submachine belongs to a
        // state's block and nowhere else (§9). Chart-level concurrency is a
        // composite state's job; a submachine inside a submachine skipped the
        // state between them.
        if (ctx.state.v == INVALID) {
          report(lo, DiagCode::MisplacedStatement, row);
          break;
        }
        SubmachineStmt const &m{ lo.pd->submachines[lo.pd->stmt_payload[row]] };
        SubmachineId const id{
          build_submachine(*lo.c, ctx.state, pd_str(lo, m.name), pd_str(lo, m.label))
        };
        lo.c->submachines[id.v].stmt = global_stmt(lo, row);
        stack.push_back(
            { .children = lo.pd->stmt_children[row],
              .next = 0,
              .ctx = { .subject = { .kind = ElemKind::Submachine, .ordinal = id.v },
                       .sub = id,
                       .state = { INVALID } } });
        break;
      }

      case StmtKind::Include: {
        if (ctx.sub.v == INVALID) {
          report(lo, DiagCode::MisplacedStatement, row);
          break;
        }
        IncludeStmt const &inc{ lo.pd->includes[lo.pd->stmt_payload[row]] };
        InstId const id{ build_include(*lo.c, ctx.sub, pd_str(lo, inc.alias)) };
        lo.c->includes[id.v].stmt = global_stmt(lo, row);
        lo.c->states[lo.c->includes[id.v].host.v].stmt = global_stmt(lo, row);
        break;
      }

      case StmtKind::Trans: {
        if (ctx.sub.v == INVALID) {
          report(lo, DiagCode::MisplacedStatement, row);
          break;
        }
        pending.push_back({ .row = row, .scope = ctx.sub });
        break;
      }

      case StmtKind::Attr: lower_attr(lo, row, ctx.subject); break;

      case StmtKind::Chart:
        // The grammar admits one chart, as the root; anything else is the
        // parser's bug surfacing, and skipping beats corrupting.
        report(lo, DiagCode::MisplacedStatement, row);
        break;
    }
  }
  return pending;
}

// An endpoint that is a path. Wildcards never reach here.
ResolveStatus resolve_endpoint(Lowerer const &lo,
                               SubmachineId scope,
                               Endpoint const &e,
                               StateId &out) {
  std::vector<ResolveSeg> segs;
  segs.reserve(e.segs.len);
  for (uint32_t i = 0; i < e.segs.len; ++i) {
    PathSeg const &ps{ lo.pd->path_segs[e.segs.off + i] };
    segs.push_back({ .name = pd_str(lo, ps.name),
                     .qualifier = pd_str(lo, ps.qualifier),
                     .ordinal = ps.ordinal });
  }
  return resolve_segments(*lo.c,
                          scope,
                          segs.data(),
                          narrow_clamp<uint32_t>(segs.size()),
                          out);
}

DiagCode diag_for(ResolveStatus status) {
  switch (status) {
    case ResolveStatus::BadQualifier: return DiagCode::BadSubmachineQualifier;
    case ResolveStatus::CrossesInclude: return DiagCode::EndpointCrossesInclude;
    case ResolveStatus::Ok:
    case ResolveStatus::NotFound: break;
  }
  return DiagCode::EndpointUnresolved;
}

// The transition pass. Paths first, wildcards second: a failed path skips the
// whole statement, and synthesizing the wildcard first would leave an orphan
// pseudostate behind the skip.
void lower_transitions(Lowerer &lo, std::vector<PendingTrans> const &pending) {
  for (PendingTrans const &pt : pending) {
    TransStmt const &ts{ lo.pd->transitions[lo.pd->stmt_payload[pt.row]] };

    if ((ts.src.wildcard != 0) && (ts.dst.wildcard != 0)) {
      report(lo, DiagCode::WildcardBothEndpoints, pt.row);
      continue;
    }

    StateId src{ INVALID };
    StateId dst{ INVALID };
    bool failed{ false };
    if (ts.src.wildcard == 0) {
      ResolveStatus const status{ resolve_endpoint(lo, pt.scope, ts.src, src) };
      if (status != ResolveStatus::Ok) {
        report(lo, diag_for(status), pt.row);
        failed = true;
      }
    }
    if (!failed && (ts.dst.wildcard == 0)) {
      ResolveStatus const status{ resolve_endpoint(lo, pt.scope, ts.dst, dst) };
      if (status != ResolveStatus::Ok) {
        report(lo, diag_for(status), pt.row);
        failed = true;
      }
    }
    if (failed) { continue; }

    // Each `*` synthesizes its own pseudostate in the statement's lexical
    // submachine (§9) -- one per statement, so two `trans * -> X` are two
    // initial arrows and §10's multiple-initial check has something to see.
    if (ts.src.wildcard != 0) {
      src = build_state(*lo.c, pt.scope, {}, StateKind::Initial, {});
      lo.c->states[src.v].stmt = global_stmt(lo, pt.row);
    }
    if (ts.dst.wildcard != 0) {
      dst = build_state(*lo.c, pt.scope, {}, StateKind::Final, {});
      lo.c->states[dst.v].stmt = global_stmt(lo, pt.row);
    }

    TransId const id{ build_trans(*lo.c, src, dst, ts.kind, pd_str(lo, ts.label)) };
    lo.c->transitions[id.v].stmt = global_stmt(lo, pt.row);

    // A trans block holds attrs and nothing else.
    Span const kids{ lo.pd->stmt_children[pt.row] };
    for (uint32_t i = 0; i < kids.len; ++i) {
      uint32_t const child{ lo.pd->stmt_ids[kids.off + i].v };
      if (lo.pd->stmts[child].kind == StmtKind::Attr) {
        lower_attr(lo, child, ElemRef{ .kind = ElemKind::Transition, .ordinal = id.v });
      } else {
        report(lo, DiagCode::MisplacedStatement, child);
      }
    }
  }
}

}  // namespace

bool lower_document(Chart &c, ParsedDocument const &pd, std::vector<Diagnostic> &diags) {
  // P1 lowers the root document into an empty chart; appending further
  // documents is the load session's (P2), which owns DocId assignment order.
  if (!c.documents.empty() || !c.submachines.empty()) { return false; }
  uint32_t const root_row{ syntax_root_statement(pd) };
  if (root_row == INVALID) { return false; }

  // The front-end slice, rebased into the chart's shared pools. Statements
  // and trivia keep document order; src offsets shift by the pool base.
  uint32_t const src_base{ narrow_clamp<uint32_t>(c.src_bytes.size()) };
  uint32_t const comment_base{ narrow_clamp<uint32_t>(c.comments.size()) };
  uint32_t const stmt_base{ narrow_clamp<uint32_t>(c.stmts.size()) };
  DocId const doc{ narrow_clamp<uint32_t>(c.documents.size()) };

  c.src_bytes.insert(c.src_bytes.end(), pd.src_bytes.begin(), pd.src_bytes.end());
  for (Trivia const &t : pd.comments) {
    c.comments.push_back(
        { .src = make_span(t.src.off + src_base, t.src.len), .pos = t.pos });
  }
  for (Statement const &s : pd.stmts) {
    c.stmts.push_back(
        { .kind = s.kind,
          .doc = doc,
          .src = make_span(s.src.off + src_base, s.src.len),
          .comments = make_span(s.comments.off + comment_base, s.comments.len) });
  }
  c.documents.push_back(
      { .path = string_pool_add(c.strings, string_pool_view(pd.strings, pd.doc.path)),
        .text = make_span(pd.doc.text.off + src_base, pd.doc.text.len),
        .statements =
            make_span(pd.doc.statements.off + stmt_base, pd.doc.statements.len) });

  Lowerer lo{ .c = &c,
              .pd = &pd,
              .diags = &diags,
              .doc = doc,
              .stmt_base = stmt_base,
              .clean = true };
  std::vector<PendingTrans> const pending{ lower_entities(lo, root_row) };
  lower_transitions(lo, pending);
  return lo.clean;
}

}  // namespace scav
