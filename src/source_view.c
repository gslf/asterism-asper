/* An indexed prefix and pin overlay, without retaining the scope's payloads. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

void asper_source_view_close(asper_source_view *view) {
  asper_event_files_close(&view->files);
  free(view->pins.rows); free(view->path); memset(view, 0, sizeof *view);
}

asper_err asper_source_view_open(asper_ctx *c, const char *scope, asper_source_view *view) {
  memset(view, 0, sizeof *view);
  if (!c || !asper_source_scope_valid(scope)) return ASPER_ERR_INVALID;
  view->ctx = c; view->path = asper_source_scope_path(c, scope, "events.log");
  if (!view->path) return ASPER_ERR_NOMEM;
  os_mutex_lock(&c->source_mu);
  asper_err e = asper_source_pins_load(c, scope, &view->pins);
  if (e == ASPER_OK) e = asper_event_files_open(&view->files, view->path);
  if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  os_mutex_unlock(&c->source_mu);
  if (e != ASPER_OK) asper_source_view_close(view);
  return e;
}

static asper_err read_event(asper_source_view *view, uint64_t sequence, asper_event *event, bool payload) {
  os_mutex_lock(&view->ctx->source_mu);
  asper_err e = payload ? asper_event_files_read(&view->files, view->path, sequence, event) :
      asper_event_files_head(&view->files, view->path, sequence, event);
  os_mutex_unlock(&view->ctx->source_mu);
  if (e != ASPER_OK) return e;
  asper_source_pins_apply(&view->pins, event);
  return ASPER_OK;
}

asper_err asper_source_view_read(asper_source_view *view, uint64_t sequence, asper_event *event) {
  return read_event(view, sequence, event, true);
}
asper_err asper_source_view_head(asper_source_view *view, uint64_t sequence, asper_event *event) {
  return read_event(view, sequence, event, false);
}
