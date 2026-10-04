#ifndef SCAV_CORE_C_H_INCLUDED
#define SCAV_CORE_C_H_INCLUDED

/* libscavcore's C ABI. Strings are handle-owned spans, not NUL-terminated; destroy
 * accepts NULL. A wrong struct size is SCAV_E_ABI, checked before any other argument. */

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
  SCAV_E_LOAD = -4,        /* the load reported diagnostics; read them */
  SCAV_E_LAYOUT = -5,      /* layout reported diagnostics; read scav_chart_diag */
  SCAV_E_FONT = -6,        /* a font missing a table, tables that disagree, or bytes
                            * to embed that are not the measured font */
  SCAV_E_NO_GLYPH = -7,    /* the font has no glyph for a codepoint measured */
  SCAV_E_DRAWLIST = -8,    /* a primitive contradicts its own kind */
  SCAV_E_ABI = -9          /* a caller-owned struct or row's size disagrees
                            * with this library's */
};
/* NOLINTEND(readability-identifier-naming) */

/* Bumped whenever anything below changes shape. A binding checks it once. */
uint32_t scav_abi_version(void);

/* NOLINTBEGIN(modernize-use-using, readability-identifier-naming) --
 * `typedef` is the C spelling, and a handle's tag is its ABI name. */
typedef struct scav_load scav_load;
typedef struct scav_chart scav_chart;

/* 16 bytes, no padding. `from_doc` is the DocId of the including document;
 * `stmt_row` indexes that document's statements. */
typedef struct {
  scav_span path;
  uint32_t from_doc;
  uint32_t stmt_row;
} scav_pending;
/* NOLINTEND(modernize-use-using, readability-identifier-naming) */

/* Add the root, read pending, resolve each however you like, add each, repeat
 * until empty, finish. */
scav_result scav_load_begin(scav_load **out);
scav_result scav_load_add(scav_load *loader,
                          scav_byte const *bytes,
                          uint32_t len,
                          char const *name);

/* The view is invalidated by the next scav_load_add. `out_stride` is the row
 * size to walk it with, which a caller asserts against its own sizeof. */
scav_result scav_load_pending(scav_load *loader,
                              scav_pending const **out,
                              uint32_t *out_stride,
                              uint32_t *out_count);

/* A pending path's bytes, from the loader's own pool. Not NUL-terminated. */
scav_result scav_load_path(scav_load const *loader,
                           scav_span path,
                           scav_byte const **out,
                           uint32_t *out_len);

/* SCAV_E_LOAD when the load reported anything. `out` is still written when a
 * chart was built, and left NULL when the network could not be assembled. */
scav_result scav_load_finish(scav_load *loader, scav_chart **out);

void scav_load_destroy(scav_load *loader);
void scav_chart_destroy(scav_chart *chart);

/* Load diagnostics, kept on the loader after finish. (off, len) index doc's normalized
 * bytes (raw for UTF-8 errors), or the chart's src_bytes once finish built a chart. */
scav_result scav_load_diag_count(scav_load const *loader, uint32_t *out_count);
scav_result scav_load_diag(scav_load const *loader,
                           uint32_t index,
                           uint32_t *out_code,
                           uint32_t *out_doc,
                           uint32_t *out_off,
                           uint32_t *out_len);

/* A short, locale-free description. */
char const *scav_diag_message(uint32_t code);

/* The resolved key a document was claimed under. Not NUL-terminated. */
scav_result scav_load_document_name(scav_load const *loader,
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

/* Writes the digest bytes and their count. cap = 0 queries the count; a nonzero cap
 * too small, or a NULL `out`, returns SCAV_E_CAPACITY with the count written. */
scav_result scav_chart_digest(scav_chart const *chart,
                              scav_byte *out,
                              uint32_t cap,
                              uint32_t *out_count);

/* A finding from an operation on an existing chart. A producer running before
 * entities exist fills (doc, off, len); one after fills the subject. 24 bytes. */
/* NOLINTNEXTLINE(modernize-use-using) */
typedef struct {
  uint32_t code; /* scav_diag_message renders it */
  uint32_t subject_kind;
  uint32_t subject_ordinal;
  uint32_t doc;
  uint32_t off, len;
} scav_diag;

/* The latest operation's findings, owned by the chart and overwritten at each
 * operation's entry. Load findings stay on the loader. */
scav_result scav_chart_diag_count(scav_chart const *chart, uint32_t *out_count);
scav_result scav_chart_diag(scav_chart const *chart,
                            uint32_t index,
                            scav_diag *out,
                            uint32_t out_size);

/* NOLINTNEXTLINE(modernize-use-using) */
typedef uint32_t scav_column_id;

/* Reading a column takes find, data (stride = element size) and count.
 * Unknown name: SCAV_E_INVALID_ARG. Empty column: NULL, zero. */
scav_result scav_column_find(scav_chart const *chart,
                             char const *name,
                             scav_column_id *out);
scav_result scav_column_data(scav_chart const *chart,
                             scav_column_id id,
                             scav_byte const **out,
                             uint32_t *out_stride);
scav_result scav_column_count(scav_chart const *chart,
                              scav_column_id id,
                              uint32_t *out_count);

/* A span into the chart's string pool, not NUL-terminated. Zero length reads
 * back NULL and zero; a span past the pool is SCAV_E_INVALID_ARG. */
scav_result scav_str(scav_chart const *chart,
                     scav_span ref,
                     scav_byte const **out,
                     uint32_t *out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCAV_CORE_C_H_INCLUDED */
