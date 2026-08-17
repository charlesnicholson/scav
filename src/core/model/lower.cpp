// Statements to entities in four steps: attach a file's front-end slice,
// instantiate, rebuild containment, resolve transitions.

#include "core/model/model.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"
#include "scav_stable_sort.h"

#include <array>
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
  InstId inst;
  uint32_t stmt_base;  // pd row + stmt_base = global StmtId
  bool clean;
};

// Never below double the capacity: reserving the exact size on every round
// replaces the vector's geometric growth with one reallocation per round, which
// is quadratic over a network of many documents.
template <typename T>
void reserve_at_least(std::vector<T> &v, size_t want) {
  if (want <= v.capacity()) { return; }
  size_t const doubled{ v.capacity() * 2 };
  v.reserve((want > doubled) ? want : doubled);
}

// The entity a lowering diagnostic would name was never created, so the payload
// is the statement's span, rebased into the chart's pool.
void report(Lowerer &lo, DiagCode code, uint32_t row) {
  lo.clean = false;
  lo.diags->push_back(
      { .code = code, .doc = lo.doc, .src = lo.c->stmts[lo.stmt_base + row].src });
}

std::string_view pd_str(Lowerer const &lo, StrRef ref) {
  return string_pool_view(lo.pd->strings, ref);
}

StmtId global_stmt(Lowerer const &lo, uint32_t row) { return { lo.stmt_base + row }; }

// What a block's statements land on. `sub` takes states, transitions and
// includes; a state block's is its implicit submachine.
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

// True when a state block holds anything needing a submachine. Answered before
// the walk, which puts the implicit submachine at ordinal 0.
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

// One attr statement onto `subject`. `@ns { a }` interns `ns:a`, a flag stores
// "true", and a list is one row per value in insertion order.
void lower_attr(Lowerer &lo, uint32_t row, ElemRef subject) {
  AttrStmt const &stmt{ lo.pd->attrs[lo.pd->stmt_payload[row]] };
  std::string_view const ns{ pd_str(lo, stmt.ns) };
  std::string composed;
  for (uint32_t i = 0; i < stmt.entries.len; ++i) {
    AttrEntry const &entry{ lo.pd->attr_entries[stmt.entries.off + i] };
    std::string_view const key{ [&] {
      std::string_view const bare{ pd_str(lo, entry.key) };
      if (ns.empty()) { return bare; }
      composed.assign(ns);
      composed.push_back(':');
      composed += bare;
      return std::string_view{ composed };
    }() };
    // Stamped from the returned index before the next append, which would
    // shift it.
    auto const append = [&](std::string_view value) {
      if (uint32_t const at{ build_attr(*lo.c, subject, key, value) }; at != INVALID) {
        lo.c->attrs[at].stmt = global_stmt(lo, row);
      }
    };
    switch (entry.kind) {
      case AttrValueKind::Flag: append("true"); break;
      case AttrValueKind::Scalar:
      case AttrValueKind::List:
        for (uint32_t k = 0; k < entry.values.len; ++k) {
          append(pd_str(lo, lo.pd->attr_values[entry.values.off + k]));
        }
        break;
    }
  }
}

// The instantiation's root submachine. A root document names the chart; any
// other hangs under its alias host state, unnamed so it adds no qualifier.
SubmachineId instantiate_root(Lowerer &lo, InstJob const &job, uint32_t root_row) {
  ChartStmt const &stmt{ lo.pd->charts[lo.pd->stmt_payload[root_row]] };
  if (job.host.v == INVALID) {
    SubmachineId const root{
      build_chart(*lo.c, pd_str(lo, stmt.name), pd_str(lo, stmt.label))
    };
    if (root.v != INVALID) { lo.c->submachines[root.v].stmt = global_stmt(lo, root_row); }
    return root;
  }
  return model_append_submachine_row(
      *lo.c,
      { .owner = job.host,
        .ordinal = 0,  // assigned for real by the containment rebuild
        .name = {},
        .label = string_pool_add(lo.c->strings, pd_str(lo, stmt.label)),
        .children = {},
        .attrs = {},
        .stmt = global_stmt(lo, root_row),
        .inst = job.inst,
        .live = 1 });
}

// The entity pass. Fills `trans` in document order, each carrying its lexical
// submachine, and `incs` with the include rows whose target the caller owes.
void lower_entities(Lowerer &lo,
                    InstJob const &job,
                    uint32_t root_row,
                    std::vector<PendingTrans> &trans,
                    std::vector<PendingInc> &incs) {
  std::vector<Frame> stack;

  SubmachineId const root{ instantiate_root(lo, job, root_row) };
  if (root.v == INVALID) { return; }

  // A sub-document's chart-level attrs land on its root submachine. The one
  // Chart entity belongs to the root document.
  ElemRef const root_subject{
    (job.host.v == INVALID) ? ElemRef{ .kind = ElemKind::Chart, .ordinal = 0 }
                            : ElemRef{ .kind = ElemKind::Submachine, .ordinal = root.v }
  };
  stack.push_back(
      { .children = lo.pd->stmt_children[root_row],
        .next = 0,
        .ctx = { .subject = root_subject, .sub = root, .state = { INVALID } } });

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
              .inst = lo.inst,
              .live = 1 }) };
        if (lo.pd->stmt_children[row].len != 0) {
          SubmachineId const implicit{ [&]() -> SubmachineId {
            if (!needs_implicit_submachine(*lo.pd, row)) { return { INVALID }; }
            return model_append_submachine_row(
                *lo.c,
                { .owner = id,
                  .ordinal = 0,  // assigned for real by the containment rebuild
                  .name = {},
                  .label = {},
                  .children = {},
                  .attrs = {},
                  .stmt = global_stmt(lo, row),
                  .inst = lo.inst,
                  .live = 1 });
          }() };
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
        // A submachine's children are states, so it belongs in a state's
        // block and nowhere else.
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
              .inst = lo.inst,
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
        // An ordinary state append. One intern of the alias serves both the
        // state's name and the Include row.
        StrRef const alias{ string_pool_add(lo.c->strings, pd_str(lo, inc.alias)) };
        StateId const host{ model_append_state_row(*lo.c,
                                                   { .name = alias,
                                                     .label = {},
                                                     .parent = ctx.sub,
                                                     .kind = StateKind::Normal,
                                                     .submachines = {},
                                                     .attrs = {},
                                                     .stmt = global_stmt(lo, row),
                                                     .inst = lo.inst,
                                                     .live = 1 }) };
        InstId const id{ narrow_clamp<uint32_t>(lo.c->includes.size()) };
        lo.c->includes.push_back(
            { .alias = alias,
              .path = string_pool_add(lo.c->strings, pd_str(lo, inc.path)),
              .target = { INVALID },  // the loader's to fill; see PendingInc
              .host = host,
              .stmt = global_stmt(lo, row) });
        incs.push_back({ .row = row, .doc = lo.doc, .inst = id });
        break;
      }

      case StmtKind::Trans: {
        if (ctx.sub.v == INVALID) {
          report(lo, DiagCode::MisplacedStatement, row);
          break;
        }
        trans.push_back({ .row = row,
                          .doc = lo.doc,
                          .inst = lo.inst,
                          .scope = ctx.sub,
                          .stmt_base = lo.stmt_base });
        break;
      }

      case StmtKind::Attr: lower_attr(lo, row, ctx.subject); break;

      case StmtKind::Chart:
        // The grammar admits one chart and only as the root, so reaching here
        // means the parser produced something it should not have.
        report(lo, DiagCode::MisplacedStatement, row);
        break;
    }
  }
}

// An endpoint that is a path. Wildcards never reach here.
// `segs` is the caller's so one buffer serves every endpoint in the pass.
ResolveStatus resolve_endpoint(Chart const &c,
                               ParsedDocument const &pd,
                               SubmachineId scope,
                               Endpoint const &e,
                               std::vector<ResolveSeg> &segs,
                               StateId &out) {
  segs.clear();
  for (uint32_t i = 0; i < e.segs.len; ++i) {
    PathSeg const &ps{ pd.path_segs[e.segs.off + i] };
    segs.push_back({ .name = string_pool_view(pd.strings, ps.name),
                     .qualifier = string_pool_view(pd.strings, ps.qualifier),
                     .ordinal = ps.ordinal });
  }
  return model_resolve_segments(c,
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
  PendingTrans pt;
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

// Splices the synthesized pseudostates into their scopes in one rebuild of
// state_ids, after each scope's authored children, where the ordinals put them.
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

}  // namespace

DocId model_attach_document(Chart &c, ParsedDocument const &pd, uint32_t &stmt_base) {
  // Statements and trivia keep document order, with src offsets shifted by the
  // pool base. Reserves add to what the chart already holds.
  uint32_t const src_base{ narrow_clamp<uint32_t>(c.src_bytes.size()) };
  uint32_t const comment_base{ narrow_clamp<uint32_t>(c.comments.size()) };
  DocId const doc{ narrow_clamp<uint32_t>(c.documents.size()) };
  stmt_base = narrow_clamp<uint32_t>(c.stmts.size());

  c.src_bytes.insert(c.src_bytes.end(), pd.src_bytes.begin(), pd.src_bytes.end());
  reserve_at_least(c.comments, c.comments.size() + pd.comments.size());
  reserve_at_least(c.stmts, c.stmts.size() + pd.stmts.size());
  reserve_at_least(c.strings.bytes, c.strings.bytes.size() + pd.strings.bytes.size());
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
  return doc;
}

bool model_instantiate(Chart &c,
                       ParsedDocument const &pd,
                       InstJob const &job,
                       std::vector<PendingTrans> &trans,
                       std::vector<PendingInc> &incs,
                       std::vector<Diagnostic> &diags) {
  uint32_t const root_row{ syntax_root_statement(pd) };
  if (root_row == INVALID) { return false; }

  // The known floor for this instantiation. Wildcards land past it and grow
  // normally.
  reserve_at_least(c.states, c.states.size() + pd.states.size() + pd.includes.size());
  reserve_at_least(c.submachines, c.submachines.size() + pd.submachines.size() + 1);
  reserve_at_least(c.transitions, c.transitions.size() + pd.transitions.size());
  reserve_at_least(c.attrs, c.attrs.size() + pd.attr_entries.size());

  Lowerer lo{ .c = &c,
              .pd = &pd,
              .diags = &diags,
              .doc = job.doc,
              .inst = job.inst,
              .stmt_base = job.stmt_base,
              .clean = true };
  lower_entities(lo, job, root_row, trans, incs);
  return lo.clean;
}

void model_finalize_containment(Chart &c) {
  // A counting pass: the child index already ascends, so bucketing by parent
  // lands each container's children in creation order. `children.len` is the
  // fill cursor, starting at zero and ending at the count.
  std::vector<uint32_t> start(c.submachines.size() + 1, 0);
  for (State const &s : c.states) {
    // A parentless state reaches here only from a hand-built chart; skipping
    // leaves it a validation finding instead of an out-of-bounds write.
    if (s.parent.v < c.submachines.size()) { start[s.parent.v + 1] += 1; }
  }
  for (size_t i = 1; i < start.size(); ++i) { start[i] += start[i - 1]; }
  c.state_ids.assign(start.back(), StateId{ INVALID });
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    c.submachines[i].children = make_span(start[i], 0);
  }
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    uint32_t const parent{ c.states[i].parent.v };
    if (parent >= c.submachines.size()) { continue; }
    Span &span{ c.submachines[parent].children };
    c.state_ids[span.off + span.len] = { i };
    span.len += 1;
  }

  // The same pass over submachines, where the cursor also numbers the ordinal.
  std::vector<uint32_t> owned(c.states.size() + 1, 0);
  for (Submachine const &m : c.submachines) {
    if (m.owner.v < c.states.size()) { owned[m.owner.v + 1] += 1; }
  }
  for (size_t i = 1; i < owned.size(); ++i) { owned[i] += owned[i - 1]; }
  c.submachine_ids.assign(owned.back(), SubmachineId{ INVALID });
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    c.states[i].submachines = make_span(owned[i], 0);
  }
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    uint32_t const owner{ c.submachines[i].owner.v };
    if (owner >= c.states.size()) { continue; }  // a document root
    Span &span{ c.states[owner].submachines };
    c.submachines[i].ordinal = span.len;
    c.submachine_ids[span.off + span.len] = { i };
    span.len += 1;
  }
}

// Three phases: resolve every path, synthesize and attach the wildcards, append
// the transition rows. A failed path skips its statement before phase two.
bool model_resolve_transitions(Chart &c,
                               ParsedDocument const *const *docs,
                               uint32_t doc_count,
                               std::vector<PendingTrans> const &pending,
                               std::vector<Diagnostic> &diags) {
  bool clean{ true };
  auto const report_at = [&](DiagCode code, PendingTrans const &pt) {
    clean = false;
    diags.push_back(
        { .code = code, .doc = pt.doc, .src = c.stmts[pt.stmt_base + pt.row].src });
  };

  std::vector<PlannedTrans> planned;
  planned.reserve(pending.size());
  std::vector<ResolveSeg> segs;
  for (PendingTrans const &pt : pending) {
    if (pt.doc.v >= doc_count) { continue; }
    ParsedDocument const &pd{ *docs[pt.doc.v] };
    TransStmt const &ts{ pd.transitions[pd.stmt_payload[pt.row]] };
    PlannedTrans plan{ .pt = pt,
                       .src = { INVALID },
                       .dst = { INVALID },
                       .src_wild = ts.src.wildcard != 0,
                       .dst_wild = ts.dst.wildcard != 0,
                       .ok = true };
    if (plan.src_wild && plan.dst_wild) {
      report_at(DiagCode::WildcardBothEndpoints, pt);
      plan.ok = false;
    }
    if (plan.ok && !plan.src_wild) {
      ResolveStatus const status{
        resolve_endpoint(c, pd, pt.scope, ts.src, segs, plan.src)
      };
      if (status != ResolveStatus::Ok) {
        report_at(diag_for(status), pt);
        plan.ok = false;
      }
    }
    if (plan.ok && !plan.dst_wild) {
      ResolveStatus const status{
        resolve_endpoint(c, pd, pt.scope, ts.dst, segs, plan.dst)
      };
      if (status != ResolveStatus::Ok) {
        report_at(diag_for(status), pt);
        plan.ok = false;
      }
    }
    planned.push_back(plan);
  }

  // One pseudostate per `*`, in the statement's lexical submachine. Never
  // merged per submachine: two `trans * -> X` are two initial arrows.
  std::vector<WildChild> wild;
  for (PlannedTrans &plan : planned) {
    if (!plan.ok) { continue; }
    auto const synthesize = [&](StateKind kind) {
      StateId const id{ model_append_state_row(
          c,
          { .name = {},
            .label = {},
            .parent = plan.pt.scope,
            .kind = kind,
            .submachines = {},
            .attrs = {},
            .stmt = { plan.pt.stmt_base + plan.pt.row },
            .inst = plan.pt.inst,
            .live = 1 }) };
      wild.push_back({ .scope = plan.pt.scope,
                       .state = id,
                       .order = narrow_clamp<uint32_t>(wild.size()) });
      return id;
    };
    if (plan.src_wild) { plan.src = synthesize(StateKind::Initial); }
    if (plan.dst_wild) { plan.dst = synthesize(StateKind::Final); }
  }
  attach_wildcards(c, wild);

  for (PlannedTrans const &plan : planned) {
    if (!plan.ok) { continue; }
    ParsedDocument const &pd{ *docs[plan.pt.doc.v] };
    TransStmt const &ts{ pd.transitions[pd.stmt_payload[plan.pt.row]] };
    TransId const id{
      build_trans(c, plan.src, plan.dst, ts.kind, string_pool_view(pd.strings, ts.label))
    };
    c.transitions[id.v].stmt = { plan.pt.stmt_base + plan.pt.row };
    c.transitions[id.v].inst = plan.pt.inst;

    // A trans block holds attrs and nothing else.
    Lowerer lo{ .c = &c,
                .pd = &pd,
                .diags = &diags,
                .doc = plan.pt.doc,
                .inst = plan.pt.inst,
                .stmt_base = plan.pt.stmt_base,
                .clean = true };
    Span const kids{ pd.stmt_children[plan.pt.row] };
    for (uint32_t i = 0; i < kids.len; ++i) {
      uint32_t const child{ pd.stmt_ids[kids.off + i].v };
      if (pd.stmts[child].kind == StmtKind::Attr) {
        lower_attr(lo, child, ElemRef{ .kind = ElemKind::Transition, .ordinal = id.v });
      } else {
        report(lo, DiagCode::MisplacedStatement, child);
      }
    }
    clean = clean && lo.clean;
  }
  return clean;
}

bool lower_document(Chart &c, ParsedDocument const &pd, std::vector<Diagnostic> &diags) {
  // The four steps above with N == 1.
  if (!c.documents.empty() || !c.submachines.empty()) { return false; }
  if (syntax_root_statement(pd) == INVALID) { return false; }

  uint32_t stmt_base{ 0 };
  DocId const doc{ model_attach_document(c, pd, stmt_base) };

  std::vector<PendingTrans> trans;
  std::vector<PendingInc> incs;
  InstJob const job{ .doc = doc,
                     .inst = { INVALID },
                     .host = { INVALID },
                     .stmt_base = stmt_base };
  bool const entities{ model_instantiate(c, pd, job, trans, incs, diags) };
  model_finalize_containment(c);
  std::array<ParsedDocument const *, 1> const one{ &pd };
  bool const resolved{ model_resolve_transitions(c, one.data(), 1, trans, diags) };
  return entities && resolved;
}

}  // namespace scav
