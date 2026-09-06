/* Durable scoped events and pin overlays. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct { asper_event *v; size_t n; } event_scan;

/* The scope index makes restart discovery portable without relying on a
 * platform-specific directory enumerator. Caller holds source_mu. */
static asper_err register_scope_locked(asper_ctx *c, const char *scope) {
  char *dir = asper_source_dir(c, "scopes");
  char *path = NULL, *data = NULL;
  size_t len = 0;
  FILE *f = NULL;
  asper_err e = ASPER_OK;
  int found = 0;
  if (!dir) return ASPER_ERR_IO;
  path = os_path_join(dir, "index.log");
  free(dir);
  if (!path) return ASPER_ERR_NOMEM;
  e = asper_source_text_read(path, ASPER_SCOPE_INDEX_BYTES, &data, &len);
  if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  if (e != ASPER_OK) goto out;
  if (data && len) {
    if (data[len-1] != '\n') { e = ASPER_ERR_PARSE; goto out; }
    for (char *p = data, *end = data + len; p < end;) {
      char *nl = memchr(p, '\n', (size_t)(end - p));
      size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
      if (n == strlen(scope) && memcmp(p, scope, n) == 0) {
        found = 1;
        break;
      }
      p = nl ? nl + 1 : end;
    }
  }
  if (!found) {
    if (strlen(scope) + 1 > ASPER_SCOPE_INDEX_BYTES - len) { e = ASPER_ERR_LIMIT; goto out; }
    f = os_fopen(path, "ab");
    if (!f || fprintf(f, "%s\n", scope) < 0 || fflush(f) != 0 ||
        os_fsync(f) != ASPER_OK)
      e = ASPER_ERR_IO;
  }
out:
  if (f && fclose(f) != 0 && e == ASPER_OK) e = ASPER_ERR_IO;
  free(data);
  free(path);
  return e;
}

void asper_events_free(asper_event *events, size_t n) {
  if (!events) return;
  for (size_t i = 0; i < n; i++) free(events[i].text);
  free(events);
}

static asper_err apply_pin_log(asper_ctx *c, const char *scope, event_scan *scan) {
  asper_source_pins pins;
  asper_err e = asper_source_pins_load(c, scope, &pins);
  if (e == ASPER_OK)
    for (size_t i = 0; i < scan->n; i++) asper_source_pins_apply(&pins, &scan->v[i]);
  free(pins.rows);
  return e;
}

asper_err asper_event_append(asper_ctx *c, const asper_event_input *event,
                             char out_id[37]) {
  char id[37];
  char *path = NULL;
  asper_event stored = {0};
  asper_err e;
  long long at = 0;
  if (!c || !event || !asper_source_scope_valid(event->scope) || !event->text)
    return c ? asper_seterr(c, ASPER_ERR_INVALID,
                            "source: invalid event arguments")
             : ASPER_ERR_INVALID;
  if (event->kind < ASPER_EVENT_USER || event->kind > ASPER_EVENT_ARTIFACT ||
      !asper_utf8_count(event->text, NULL) ||
      (event->object_ref && event->object_ref[0] &&
       !asper_source_object_valid(event->object_ref)))
    return asper_seterr(c, ASPER_ERR_INVALID, "source: invalid event");
  path = asper_source_scope_path(c, event->scope, "events.log");
  if (!path) return asper_seterr(c, ASPER_ERR_IO,
                                 "source: cannot create scope directory");
  os_mutex_lock(&c->source_mu);
  e = register_scope_locked(c, event->scope);
  if (e != ASPER_OK) goto out;
  asper_uuid_v4(id);
  at = (long long)asper_clock_now(&c->clock);
  memcpy(stored.id, id, 37);
  stored.at = at;
  stored.kind = event->kind;
  stored.pinned = event->pinned != 0;
  stored.text = (char *)event->text;
  if (event->object_ref) memcpy(stored.object_ref, event->object_ref,
                                strlen(event->object_ref)+1);
  e = asper_event_log_append(path, &stored);
  if (e == ASPER_OK && out_id) memcpy(out_id, id, 37);
out:
  os_mutex_unlock(&c->source_mu);
  free(path);
  if (e != ASPER_OK)
    return asper_seterr(c, e, "source: event append failed");
  if (event->kind == ASPER_EVENT_USER ||
      event->kind == ASPER_EVENT_ASSISTANT) {
    asper_err qe = asper_enqueue_turn(
        c, event->kind == ASPER_EVENT_ASSISTANT ? ASPER_ROLE_ASSISTANT
                                                : ASPER_ROLE_USER,
        event->text, (asper_time)at, id, event->scope, event->object_ref);
    if (qe != ASPER_OK)
      asper_log(c, ASPER_LOG_WARN, "source",
                "durable event %s awaits later curation: %s", id,
                asper_err_name(qe));
  }
  return ASPER_OK;
}

asper_err asper_event_list(asper_ctx *c, const char *scope,
                           asper_event **out, size_t *out_n) {
  char *path;
  event_scan scan;
  asper_err e;
  if (!c || !out || !out_n || !asper_source_scope_valid(scope)) return ASPER_ERR_INVALID;
  *out = NULL;
  *out_n = 0;
  path = asper_source_scope_path(c, scope, "events.log");
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  unsigned long long next;
  e = asper_event_log_page(path, "", 0, SIZE_MAX, &scan.v, &scan.n, &next);
  if (e == ASPER_OK) {
    e = apply_pin_log(c, scope, &scan);
    if (e != ASPER_OK) asper_events_free(scan.v, scan.n);
  }
  os_mutex_unlock(&c->source_mu);
  free(path);
  if (e != ASPER_OK) return e;
  *out = scan.v;
  *out_n = scan.n;
  return ASPER_OK;
}

asper_err asper_event_search(asper_ctx *c, const char *scope, const char *query,
                             unsigned long long after, size_t limit,
                             asper_event **out, size_t *out_n,
                             unsigned long long *next) {
  event_scan scan;
  asper_err e;
  char *path;
  if (!c || !asper_source_scope_valid(scope) || !out || !out_n || !next || !query ||
      limit == 0 || limit > 1000) return ASPER_ERR_INVALID;
  *out = NULL; *out_n = 0; *next = after;
  path = asper_source_scope_path(c, scope, "events.log");
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  e = asper_event_log_page(path, query, after, limit, &scan.v, &scan.n, next);
  if (e == ASPER_OK) {
    e = apply_pin_log(c, scope, &scan);
    if (e != ASPER_OK) asper_events_free(scan.v, scan.n);
  }
  os_mutex_unlock(&c->source_mu);
  free(path);
  if (e != ASPER_OK) { *next = after; return e; }
  *out = scan.v; *out_n = scan.n;
  return ASPER_OK;
}

asper_err asper_event_set_pinned(asper_ctx *c, const char *scope,
                                 const char *event_id, int pinned) {
  asper_event *events = NULL;
  size_t n = 0;
  char *path = NULL;
  FILE *f = NULL;
  asper_err e;
  int found = 0;
  if (!c || !asper_source_scope_valid(scope) || !asper_uuid_valid(event_id))
    return ASPER_ERR_INVALID;
  unsigned long long cursor = 0;
  do {
    e = asper_event_search(c, scope, "", cursor, 256, &events, &n, &cursor);
    if (e != ASPER_OK) return e;
    for (size_t i = 0; i < n; i++)
      if (strcmp(events[i].id, event_id) == 0) found = 1;
    asper_events_free(events, n);
  } while (!found && n == 256);
  if (!found) return ASPER_ERR_NOT_FOUND;
  path = asper_source_scope_path(c, scope, "pins.log");
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  uint64_t bytes = 0;
  e = os_file_size(path, &bytes);
  if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  if (e == ASPER_OK && bytes > ASPER_PIN_BYTES - 39) e = ASPER_ERR_LIMIT;
  if (e != ASPER_OK) { os_mutex_unlock(&c->source_mu); free(path); return e; }
  f = os_fopen(path, "ab");
  if (!f || fprintf(f, "%s %d\n", event_id, pinned ? 1 : 0) < 0 ||
      fflush(f) != 0 || os_fsync(f) != ASPER_OK)
    e = ASPER_ERR_IO;
  else
    e = ASPER_OK;
  if (f && fclose(f) != 0 && e == ASPER_OK) e = ASPER_ERR_IO;
  os_mutex_unlock(&c->source_mu);
  free(path);
  return e;
}
