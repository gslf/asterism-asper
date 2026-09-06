/* Current checkpoint projection and durable source event. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

static asper_err atomic_text(asper_ctx *c, const char *path,
                             const char *text) {
  char *tmp;
  size_t n = strlen(path);
  asper_err e;
  tmp = (char *)malloc(n + 5);
  if (!tmp) return ASPER_ERR_NOMEM;
  memcpy(tmp, path, n);
  memcpy(tmp + n, ".tmp", 5);
  e = os_write_file(tmp, text, strlen(text));
  if (e == ASPER_OK) e = os_file_replace(tmp, path);
  if (e != ASPER_OK) (void)os_remove_file(tmp);
  free(tmp);
  if (e != ASPER_OK) return asper_seterr(c, e, "source: atomic write failed");
  return ASPER_OK;
}

asper_err asper_checkpoint_commit(asper_ctx *c, const char *scope,
                                   const char *text_utf8,
                                   char out_event_id[37]) {
  asper_event_input event;
  char object_ref[72];
  char event_id[37];
  char *path;
  asper_err e;
  if (!c || !asper_source_scope_valid(scope) || !text_utf8 ||
      !asper_utf8_count(text_utf8, NULL)) return ASPER_ERR_INVALID;
  e = asper_object_put(c, text_utf8, strlen(text_utf8), object_ref);
  if (e != ASPER_OK) return e;
  memset(&event, 0, sizeof event);
  event.scope = scope;
  event.kind = ASPER_EVENT_CHECKPOINT;
  event.text = text_utf8;
  event.object_ref = object_ref;
  e = asper_event_append(c, &event, event_id);
  if (e != ASPER_OK) return e;
  path = asper_source_scope_path(c, scope, "checkpoint.txt");
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  e = atomic_text(c, path, text_utf8);
  os_mutex_unlock(&c->source_mu);
  free(path);
  if (e == ASPER_OK && out_event_id) memcpy(out_event_id, event_id, 37);
  return e;
}

asper_err asper_checkpoint_load(asper_ctx *c, const char *scope,
                                 char **out_text) {
  char *path;
  asper_err e;
  if (!c || !out_text || !asper_source_scope_valid(scope)) return ASPER_ERR_INVALID;
  *out_text = NULL;
  path = asper_source_scope_path(c, scope, "checkpoint.txt");
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  e = asper_source_text_read(path, ASPER_EVENT_BYTES, out_text, NULL);
  os_mutex_unlock(&c->source_mu);
  free(path);
  if (e == ASPER_ERR_NOT_FOUND) {
    asper_source_view view;
    e = asper_source_view_open(c, scope, &view);
    if (e != ASPER_OK) return e;
    for (uint64_t i = view.files.count; i && !*out_text; i--) {
      asper_event event;
      e = asper_source_view_head(&view, i, &event);
      if (e != ASPER_OK) break;
      if (event.kind == ASPER_EVENT_CHECKPOINT) {
        e = asper_source_view_read(&view, i, &event);
        if (e == ASPER_OK) { *out_text = event.text; event.text = NULL; }
      }
      free(event.text);
      if (e != ASPER_OK) break;
    }
    asper_source_view_close(&view);
    return e == ASPER_OK && !*out_text ? ASPER_ERR_NOT_FOUND : e;
  }
  return e;
}
