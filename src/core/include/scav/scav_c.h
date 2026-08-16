#ifndef SCAV_C_H_INCLUDED
#define SCAV_C_H_INCLUDED

/* libscavcore's C API, and the ABI a binding is generated against: flat
 * extern "C", opaque handles, POD structs, out-params, error enums.
 *
 * No std:: type crosses. A string comes out as a span into memory the handle
 * owns, not NUL-terminated, and every destroy is idempotent on NULL. */

#include "scav/scav_types.h"

/* NOLINTNEXTLINE(modernize-deprecated-headers) -- this header must compile as C */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C's naming convention, not the model's CamelCase. */
/* NOLINTBEGIN(readability-identifier-naming) */
enum {
  SCAV_OK = 0,
  SCAV_E_INVALID_ARG = -1, /* a null out-param, or an index out of range */
  SCAV_E_STATE = -2,       /* the call does not apply in the handle's state */
  SCAV_E_CAPACITY = -3,    /* buffer too small; the required count was written */
  SCAV_E_LOAD = -4         /* the load reported diagnostics; read them */
};
/* NOLINTEND(readability-identifier-naming) */

/* Bumped whenever anything below changes shape. A binding checks it once. */
uint32_t scav_abi_version(void);

/* NOLINTBEGIN(modernize-use-using) -- `typedef` is the C spelling */
typedef struct scav_load scav_load;
typedef struct scav_chart scav_chart;

/* 16 bytes, no padding. `from` is a DocId: pending is reported before anything
 * is instantiated, and a file included N times is fetched once. `stmt_row`
 * indexes the statements of `from`'s document. */
typedef struct {
  scav_span path;
  uint32_t from;
  uint32_t stmt_row;
} scav_pending;
/* NOLINTEND(modernize-use-using) */

/* Add the root, read pending, resolve each however you like, add each, repeat
 * until empty, finish. */
scav_result scav_load_begin(scav_load **out);
scav_result scav_load_add(scav_load *session,
                          scav_byte const *bytes,
                          uint32_t len,
                          char const *name);

/* The view is invalidated by the next scav_load_add. */
scav_result scav_load_pending(scav_load *session,
                              scav_pending const **out,
                              uint32_t *out_count);

/* A pending path's bytes, from the session's own pool. Not NUL-terminated. */
scav_result scav_load_path(scav_load const *session,
                           scav_span path,
                           scav_byte const **out,
                           uint32_t *out_len);

/* SCAV_E_LOAD when the load reported anything. `out` is still written when a
 * chart was built, and left NULL when the network could not be assembled. */
scav_result scav_load_finish(scav_load *session, scav_chart **out);

void scav_load_destroy(scav_load *session);
void scav_chart_destroy(scav_chart *chart);

/* Diagnostics stay on the session, since a cycle or a missing document leaves
 * no chart to hang them on. Render a code with scav_diag_message; derive a
 * position from (doc, off, len) against the bytes supplied for that doc. */
scav_result scav_load_diag_count(scav_load const *session, uint32_t *out_count);
scav_result scav_load_diag(scav_load const *session,
                           uint32_t index,
                           uint32_t *out_code,
                           uint32_t *out_doc,
                           uint32_t *out_off,
                           uint32_t *out_len);

/* A short, locale-free description. */
char const *scav_diag_message(uint32_t code);

/* The resolved key a document was claimed under. Not NUL-terminated. */
scav_result scav_load_document_name(scav_load const *session,
                                    uint32_t doc,
                                    scav_byte const **out,
                                    uint32_t *out_len);

/* Row counts, so a binding can assert a shape without walking anything. */
scav_result scav_chart_counts(scav_chart const *chart,
                              uint32_t *out_documents,
                              uint32_t *out_states,
                              uint32_t *out_submachines,
                              uint32_t *out_transitions,
                              uint32_t *out_includes);

/* xxh32 over the structural digest. Two transports of one network agree. */
scav_result scav_chart_structural_hash(scav_chart const *chart, uint32_t *out);

/* The digest bytes. Pass cap = 0 with a non-null out_count to query the size,
 * then call again with a buffer. A cap too small returns SCAV_E_CAPACITY and
 * writes the required count; it never truncates silently. */
scav_result scav_chart_digest(scav_chart const *chart,
                              scav_byte *out,
                              uint32_t cap,
                              uint32_t *out_count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCAV_C_H_INCLUDED */
