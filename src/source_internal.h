/* Source storage helpers; callers serialize source mutations with source_mu. */
#ifndef ASPER_SOURCE_INTERNAL_H
#define ASPER_SOURCE_INTERNAL_H
#include "event_index.h"
#define ASPER_OBJECT_BYTES (64u * 1024u * 1024u)
#define OBJECT_PREFIX "sha256:"
#define ASPER_PIN_BYTES (8u * 1024u * 1024u)
#define ASPER_CONTEXT_BYTES (4u * 1024u * 1024u)
#define ASPER_CONTEXT_EVENTS 4096u
#define ASPER_SCOPE_INDEX_BYTES (1024u * 1024u)
#define ASPER_CURATED_BYTES (8u * 1024u * 1024u)
typedef struct {
  char id[37];
  unsigned order;
  int pinned;
} asper_source_pin;
typedef struct {
  asper_source_pin *rows;
  size_t n;
} asper_source_pins;
asper_err asper_source_pins_load(asper_ctx *c, const char *scope, asper_source_pins *pins);
void asper_source_pins_apply(const asper_source_pins *pins, asper_event *event);
typedef struct {
  asper_ctx *ctx;
  char *path;
  asper_event_files files;
  asper_source_pins pins;
} asper_source_view;
/* Capture one append-only prefix and its pin overlay. No user callback runs
 * under source_mu; each exact read owns only one decoded event. */
asper_err asper_source_view_open(asper_ctx *c, const char *scope, asper_source_view *view);
asper_err asper_source_view_read(asper_source_view *view, uint64_t sequence, asper_event *event);
asper_err asper_source_view_head(asper_source_view *view, uint64_t sequence, asper_event *event);
void asper_source_view_close(asper_source_view *view);
size_t asper_source_tokens(const asper_context_request *request, const char *text);
asper_err asper_source_history(asper_ctx *c, const asper_context_request *request,
                              asper_buf *context, size_t *available, size_t *included);
int asper_source_scope_valid(const char *scope);
int asper_source_object_valid(const char *ref);
char *asper_source_dir(asper_ctx *c, const char *leaf);
char *asper_source_scope_path(asper_ctx *c, const char *scope, const char *name);
char *asper_source_object_path(asper_ctx *c, const char *ref);
asper_err asper_source_text_read(const char *path, size_t limit, char **out, size_t *size);
#endif
