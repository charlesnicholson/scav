// Lowering: statements to entities (§17 P1). Two passes over one document.
// The first creates states, submachines, includes, and attrs in document
// order, following block structure; the second creates transitions, after
// every authored state exists, so a forward reference is an ordinary
// reference. The walk is an explicit stack for the same reason the parser's
// is: nesting depth is attacker-controlled.

#include "core/model/model.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"
#include "scav_stable_sort.h"

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
        StateId const id{ model_append_state_row(
            *lo.c,
            { .name = string_pool_add(lo.c->strings, pd_str(lo, s.name)),
              .label = string_pool_add(lo.c->strings, pd_str(lo, s.label)),
              .parent = ctx.sub,
              .kind = s.kind,
              .submachines = {},
              .attrs = {},
              .stmt = global_stmt(lo, row),
              .inst = { INVALID },
              .live = 1 }) };
        if (lo.pd->stmt_children[row].len != 0) {
          SubmachineId implicit{ INVALID };
          if (needs_implicit_submachine(*lo.pd, row)) {
            implicit = model_append_submachine_row(
                *lo.c,
                { .owner = id,
                  .ordinal = 0,  // assigned for real by the containment rebuild
                  .name = {},
                  .label = {},
                  .children = {},
                  .attrs = {},
                  .stmt = global_stmt(lo, row),
                  .inst = { INVALID },
                  .live = 1 });
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
        SubmachineId const id{ model_append_submachine_row(
            *lo.c,
            { .owner = ctx.state,
              .ordinal = 0,  // assigned for real by the containment rebuild
              .name = string_pool_add(lo.c->strings, pd_str(lo, m.name)),
              .label = string_pool_add(lo.c->strings, pd_str(lo, m.label)),
              .children = {},
              .attrs = {},
              .stmt = global_stmt(lo, row),
              .inst = { INVALID },
              .live = 1 }) };
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
        // The alias host is an ordinary state (§9); the row keeps the same raw
        // append as every other state in this pass.
        StateId const host{ model_append_state_row(
            *lo.c,
            { .name = string_pool_add(lo.c->strings, pd_str(lo, inc.alias)),
              .label = {},
              .parent = ctx.sub,
              .kind = StateKind::Normal,
              .submachines = {},
              .attrs = {},
              .stmt = global_stmt(lo, row),
              .inst = { INVALID },
              .live = 1 }) };
        lo.c->includes.push_back(
            { .alias = string_pool_add(lo.c->strings, pd_str(lo, inc.alias)),
              .target = { INVALID },  // the loader's to fill (P2)
              .host = host,
              .stmt = global_stmt(lo, row) });
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

// Builds every containment span in one pass over the rows: pass 1 appends
// rows raw, because the builder's per-call span insert pays O(shift) plus a
// fix-up sweep, and a document with two submachines per composite turns that
// quadratic. This is §7.3's rebuild rule applied at scale -- the same shape as
// attach_wildcards, run once between the passes. Creation order is document
// order, so a stable sort by owner groups each container's children in the
// order they were written.
void finalize_containment(Chart &c) {
  struct Edge {
    uint32_t parent;
    uint32_t child;
  };

  std::vector<Edge> state_edges;
  state_edges.reserve(c.states.size());
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    state_edges.push_back({ .parent = c.states[i].parent.v, .child = i });
  }
  scav_stable_sort(state_edges, [](Edge const &a, Edge const &b) {
    if (a.parent != b.parent) { return a.parent < b.parent; }
    return a.child < b.child;
  });
  c.state_ids.clear();
  c.state_ids.reserve(state_edges.size());
  for (Submachine &m : c.submachines) { m.children = {}; }
  for (Edge const &e : state_edges) {
    Span &span{ c.submachines[e.parent].children };
    if (span.len == 0) { span.off = narrow_clamp<uint32_t>(c.state_ids.size()); }
    span.len += 1;
    c.state_ids.push_back({ e.child });
  }

  std::vector<Edge> sub_edges;
  sub_edges.reserve(c.submachines.size());
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    if (c.submachines[i].owner.v == INVALID) { continue; }  // a document root
    sub_edges.push_back({ .parent = c.submachines[i].owner.v, .child = i });
  }
  scav_stable_sort(sub_edges, [](Edge const &a, Edge const &b) {
    if (a.parent != b.parent) { return a.parent < b.parent; }
    return a.child < b.child;
  });
  c.submachine_ids.clear();
  c.submachine_ids.reserve(sub_edges.size());
  for (State &s : c.states) { s.submachines = {}; }
  for (Edge const &e : sub_edges) {
    Span &span{ c.states[e.parent].submachines };
    if (span.len == 0) { span.off = narrow_clamp<uint32_t>(c.submachine_ids.size()); }
    c.submachines[e.child].ordinal = span.len;
    span.len += 1;
    c.submachine_ids.push_back({ e.child });
  }
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
  return model_resolve_segments(*lo.c,
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

// A transition statement after endpoint resolution: what to create, or
// nothing when a diagnostic already said why not.
struct PlannedTrans {
  uint32_t row;
  SubmachineId scope;
  StateId src, dst;  // INVALID where a wildcard will synthesize
  bool src_wild, dst_wild;
  bool ok;
};

// A wildcard's synthesized pseudostate, held until the one span rebuild.
struct WildChild {
  SubmachineId scope;
  StateId state;
  uint32_t order;  // creation order, the stable tail of the rebuild sort
};

// Splices the synthesized pseudostates into their scopes' children spans in
// one O(n) rebuild of state_ids -- §7.3's rebuild rule. Per-statement
// build_state would shift the shared array once per wildcard, which is
// quadratic over a document with one initial arrow per submachine.
void attach_wildcards(Chart &c, std::vector<WildChild> &wild) {
  if (wild.empty()) { return; }
  scav_stable_sort(wild, [](WildChild const &a, WildChild const &b) {
    if (a.scope.v != b.scope.v) { return a.scope.v < b.scope.v; }
    return a.order < b.order;
  });
  std::vector<StateId> rebuilt;
  rebuilt.reserve(c.state_ids.size() + wild.size());
  size_t next{ 0 };
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    Submachine &m{ c.submachines[i] };
    uint32_t const off{ narrow_clamp<uint32_t>(rebuilt.size()) };
    for (uint32_t k = 0; k < m.children.len; ++k) {
      rebuilt.push_back(c.state_ids[m.children.off + k]);
    }
    while ((next < wild.size()) && (wild[next].scope.v == i)) {
      rebuilt.push_back(wild[next].state);
      ++next;
    }
    m.children = make_span(off, narrow_clamp<uint32_t>(rebuilt.size()) - off);
  }
  c.state_ids = std::move(rebuilt);
}

// The transition pass, in phases. Resolve every path first -- a failed path
// skips its whole statement, and synthesizing a wildcard first would leave an
// orphan pseudostate behind the skip. Then synthesize the wildcards in
// statement order, attach them in one rebuild, and only then create the
// transition rows, which are plain tail appends.
void lower_transitions(Lowerer &lo, std::vector<PendingTrans> const &pending) {
  std::vector<PlannedTrans> planned;
  planned.reserve(pending.size());
  for (PendingTrans const &pt : pending) {
    TransStmt const &ts{ lo.pd->transitions[lo.pd->stmt_payload[pt.row]] };
    PlannedTrans plan{ .row = pt.row,
                       .scope = pt.scope,
                       .src = { INVALID },
                       .dst = { INVALID },
                       .src_wild = ts.src.wildcard != 0,
                       .dst_wild = ts.dst.wildcard != 0,
                       .ok = true };
    if (plan.src_wild && plan.dst_wild) {
      report(lo, DiagCode::WildcardBothEndpoints, pt.row);
      plan.ok = false;
    }
    if (plan.ok && !plan.src_wild) {
      ResolveStatus const status{ resolve_endpoint(lo, pt.scope, ts.src, plan.src) };
      if (status != ResolveStatus::Ok) {
        report(lo, diag_for(status), pt.row);
        plan.ok = false;
      }
    }
    if (plan.ok && !plan.dst_wild) {
      ResolveStatus const status{ resolve_endpoint(lo, pt.scope, ts.dst, plan.dst) };
      if (status != ResolveStatus::Ok) {
        report(lo, diag_for(status), pt.row);
        plan.ok = false;
      }
    }
    planned.push_back(plan);
  }

  // Each `*` synthesizes its own pseudostate in the statement's lexical
  // submachine (§9) -- one per statement, so two `trans * -> X` are two
  // initial arrows and §10's multiple-initial check has something to see.
  std::vector<WildChild> wild;
  for (PlannedTrans &plan : planned) {
    if (!plan.ok) { continue; }
    if (plan.src_wild) {
      plan.src = model_append_state_row(*lo.c,
                                        { .name = {},
                                          .label = {},
                                          .parent = plan.scope,
                                          .kind = StateKind::Initial,
                                          .submachines = {},
                                          .attrs = {},
                                          .stmt = global_stmt(lo, plan.row),
                                          .inst = { INVALID },
                                          .live = 1 });
      wild.push_back({ .scope = plan.scope,
                       .state = plan.src,
                       .order = narrow_clamp<uint32_t>(wild.size()) });
    }
    if (plan.dst_wild) {
      plan.dst = model_append_state_row(*lo.c,
                                        { .name = {},
                                          .label = {},
                                          .parent = plan.scope,
                                          .kind = StateKind::Final,
                                          .submachines = {},
                                          .attrs = {},
                                          .stmt = global_stmt(lo, plan.row),
                                          .inst = { INVALID },
                                          .live = 1 });
      wild.push_back({ .scope = plan.scope,
                       .state = plan.dst,
                       .order = narrow_clamp<uint32_t>(wild.size()) });
    }
  }
  attach_wildcards(*lo.c, wild);

  for (PlannedTrans const &plan : planned) {
    if (!plan.ok) { continue; }
    TransStmt const &ts{ lo.pd->transitions[lo.pd->stmt_payload[plan.row]] };
    TransId const id{
      build_trans(*lo.c, plan.src, plan.dst, ts.kind, pd_str(lo, ts.label))
    };
    lo.c->transitions[id.v].stmt = global_stmt(lo, plan.row);

    // A trans block holds attrs and nothing else.
    Span const kids{ lo.pd->stmt_children[plan.row] };
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

  // Exact reserves, so three million rows arrive without a doubling copy. The
  // entity arrays get their known floor -- wildcards land past it and grow
  // normally. Guessing high is the costly direction: the memory floor reads
  // capacity.
  c.src_bytes.insert(c.src_bytes.end(), pd.src_bytes.begin(), pd.src_bytes.end());
  c.comments.reserve(c.comments.size() + pd.comments.size());
  c.stmts.reserve(c.stmts.size() + pd.stmts.size());
  c.strings.bytes.reserve(pd.strings.bytes.size());
  c.states.reserve(pd.states.size() + pd.includes.size());
  c.submachines.reserve(pd.submachines.size() + 1);
  c.transitions.reserve(pd.transitions.size());
  c.attrs.reserve(pd.attr_entries.size());
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
  finalize_containment(c);
  lower_transitions(lo, pending);
  return lo.clean;
}

}  // namespace scav
