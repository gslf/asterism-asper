/* Source storage helpers; callers serialize source mutations with source_mu. */
#ifndef ASPER_SOURCE_INTERNAL_H
#define ASPER_SOURCE_INTERNAL_H
#include "event_log.h"
#include <ctype.h>
#define ASPER_OBJECT_BYTES (64u * 1024u * 1024u)
#define OBJECT_PREFIX "sha256:"
int asper_source_scope_valid(const char *scope);
int asper_source_object_valid(const char *ref);
char *asper_source_dir(asper_ctx *c, const char *leaf);
char *asper_source_scope_path(asper_ctx *c, const char *scope, const char *name);
char *asper_source_object_path(asper_ctx *c, const char *ref);
#endif
