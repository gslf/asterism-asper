/* Offline decisions postpone semantic processing, never source retention. */
#ifndef ASPER_SOURCE_DEFERRED_H
#define ASPER_SOURCE_DEFERRED_H
#include "source_internal.h"
#define ASPER_DEFERRED_MAX 4096u
#define ASPER_DEFERRED_BYTES (1024u * 1024u)
#define ASPER_DEFERRED_FILE "curation.deferred"
typedef struct asper_source_deferral {
  char scope[65], id[37], hash[65];
  uint64_t sequence;
} asper_source_deferral;
asper_err asper_source_deferred_load(asper_ctx *c, asper_source_deferral **out, size_t *n);
const asper_source_deferral *asper_source_deferred_find(const asper_source_deferral *rows,
    size_t n, const char *scope, const char *id);
bool asper_source_deferred_matches(const asper_source_deferral *row, const asper_event *event);
#endif
