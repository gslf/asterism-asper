/* Progressive curation admission. Disk remains the source of truth on restart. */
#include "source_internal.h"
#include "source_deferred.h"
#include <stdlib.h>
#include <string.h>

#define PENDING_SCOPES 16384u
typedef struct {
  char scope[65];
  uint64_t cursor, end;
  bool refresh;
} pending_scope;
struct asper_source_pending {
  pending_scope *scopes;
  size_t n, cap, next;
  char (*curated)[37];
  size_t curated_n;
  bool frozen, inventory_dirty;
};

void asper_source_pending_close(asper_ctx *c) {
  if (!c->source_pending) return;
  free(c->source_pending->scopes); free(c->source_pending->curated);
  free(c->source_pending); c->source_pending = NULL;
}

static asper_err add_scope(struct asper_source_pending *p, const char *scope) {
  for (size_t i = 0; i < p->n; i++) if (!strcmp(p->scopes[i].scope, scope)) {
    p->scopes[i].refresh = true; return ASPER_OK;
  }
  if (p->n == PENDING_SCOPES) return ASPER_ERR_LIMIT;
  if (p->n == p->cap) {
    size_t cap = p->cap ? p->cap * 2 : 16;
    pending_scope *grown = realloc(p->scopes, cap * sizeof *grown);
    if (!grown) return ASPER_ERR_NOMEM;
    p->scopes = grown; p->cap = cap;
  }
  pending_scope *rows = p->scopes; rows[p->n] = (pending_scope){0};
  strcpy(rows[p->n].scope, scope); rows[p->n++].refresh = true; return ASPER_OK;
}

static asper_err inventory(asper_ctx *c) {
  struct asper_source_pending *p = c->source_pending;
  asper_err e = ASPER_OK;
  char *dir = asper_source_dir(c, "scopes"), *text = NULL;
  char *path = dir ? os_path_join(dir, "index.log") : NULL;
  size_t bytes = 0; free(dir);
  if (e == ASPER_OK && !path) e = ASPER_ERR_NOMEM;
  if (e == ASPER_OK) {
    os_mutex_lock(&c->source_mu);
    e = asper_source_text_read(path, ASPER_SCOPE_INDEX_BYTES, &text, &bytes);
    os_mutex_unlock(&c->source_mu);
    if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  }
  for (size_t at = 0; e == ASPER_OK && at < bytes;) {
    char *nl = memchr(text + at, '\n', bytes - at);
    size_t n = nl ? (size_t)(nl - text - at) : 0;
    char scope[65];
    if (!nl || !n || n >= sizeof scope) { e = ASPER_ERR_PARSE; break; }
    memcpy(scope, text + at, n); scope[n] = 0;
    if (!asper_source_scope_valid(scope)) { e = ASPER_ERR_PARSE; break; }
    e = add_scope(p, scope); at += n + 1;
  }
  free(path); free(text);
  if (e == ASPER_OK) p->inventory_dirty = false;
  return e;
}

static asper_err initialize(asper_ctx *c) {
  if (c->source_pending) return ASPER_OK;
  struct asper_source_pending *p = calloc(1, sizeof *p);
  if (!p) return ASPER_ERR_NOMEM;
  c->source_pending = p;
  asper_err e = asper_source_curated_load(c, &p->curated, &p->curated_n);
  if (e == ASPER_OK) e = inventory(c);
  if (e != ASPER_OK) asper_source_pending_close(c);
  return e;
}

/* replay_mu serializes cursors. Source reads take only short source_mu locks. */
static void publish(asper_ctx *c) {
  struct asper_source_pending *p = c->source_pending;
  bool pending = p && p->inventory_dirty;
  for (size_t i = 0; p && i < p->n; i++)
    if (p->scopes[i].refresh || p->scopes[i].cursor < p->scopes[i].end) pending = true;
  os_mutex_lock(&c->ev_mu); c->source_backlog = pending;
  if (pending) os_cond_signal(&c->ev_cv);
  os_mutex_unlock(&c->ev_mu);
}

static asper_err capture_scope(asper_ctx *c, pending_scope *s) {
  asper_source_view view;
  asper_err e = asper_source_view_open(c, s->scope, &view);
  if (e == ASPER_OK) {
    if (view.files.count < s->cursor) e = ASPER_ERR_IO;
    else { s->end = view.files.count; s->refresh = false; }
    asper_source_view_close(&view);
  }
  return e;
}

static asper_err fill_scope(asper_ctx *c, pending_scope *s) {
  struct asper_source_pending *p = c->source_pending;
  asper_source_view view;
  asper_err e = asper_source_view_open(c, s->scope, &view);
  if (e != ASPER_OK) return e;
  if (view.files.count < s->end) e = ASPER_ERR_IO;
  while (e == ASPER_OK && s->cursor < s->end && asper_turn_queue_room(c)) {
    asper_event event = {0};
    e = asper_source_view_head(&view, s->cursor + 1, &event);
    if (e == ASPER_OK && (event.kind == ASPER_EVENT_USER || event.kind == ASPER_EVENT_ASSISTANT) &&
        !asper_source_curated_has(p->curated, p->curated_n, event.id)) {
      e = asper_source_view_read(&view, s->cursor + 1, &event);
      const asper_source_deferral *deferred = asper_source_deferred_find(
          c->source_deferrals,c->source_deferred_n,s->scope,event.id);
      if (e == ASPER_OK && deferred) {
        if (!asper_source_deferred_matches(deferred,&event)) e = asper_seterr(c,
            ASPER_ERR_PARSE,"deferred source %s changed; inspect its offline decision",event.id);
      } else if (e == ASPER_OK) {
        e = asper_enqueue_turn(c,
          event.kind == ASPER_EVENT_USER ? ASPER_ROLE_USER : ASPER_ROLE_ASSISTANT,
          event.text, (asper_time)event.at, event.id, s->scope, event.object_ref);
        if (e == ASPER_ERR_BUSY) { free(event.text); e = ASPER_OK; break; }
      }
    }
    free(event.text);
    if (e == ASPER_OK) s->cursor++;
  }
  asper_source_view_close(&view);
  /* Byte pressure can leave space for more event handles. Keep this cursor
   * unchanged until a completed cycle releases the retained payloads. */
  return e;
}

static asper_err refill(asper_ctx *c) {
  struct asper_source_pending *p = c->source_pending;
  asper_err e = p->inventory_dirty && !p->frozen ? inventory(c) : ASPER_OK;
  for (size_t checked = 0; checked < p->n && e == ASPER_OK && asper_turn_queue_room(c); checked++) {
    pending_scope *s = &p->scopes[p->next];
    if (s->refresh && !p->frozen) e = capture_scope(c, s);
    if (e == ASPER_OK && s->cursor < s->end) e = fill_scope(c, s);
    p->next = (p->next + 1) % p->n;
  }
  publish(c); return e;
}

static bool suspended(asper_ctx *c) {
  os_rwlock_rdlock(&c->lock);
  bool guard = c->store.curation_receipt != NULL;
  os_rwlock_rdunlock(&c->lock); return guard;
}

asper_err asper_source_replay_pending(asper_ctx *c) {
  if (!c) return ASPER_ERR_INVALID;
  os_mutex_lock(&c->replay_mu);
  asper_err e = ASPER_OK;
  if (!suspended(c)) {
    e = initialize(c);
    if (e == ASPER_OK) e = refill(c);
  }
  os_mutex_unlock(&c->replay_mu); return e;
}

asper_err asper_source_pending_note(asper_ctx *c, const char *scope) {
  os_mutex_lock(&c->replay_mu);
  asper_err e = initialize(c);
  if (e == ASPER_OK) e = add_scope(c->source_pending, scope);
  if (e != ASPER_OK && c->source_pending) c->source_pending->inventory_dirty = true;
  if (e == ASPER_OK && !c->source_pending->frozen && !suspended(c)) e = refill(c);
  if (c->source_pending) publish(c);
  os_mutex_unlock(&c->replay_mu); return e;
}

/* Full flush freezes admission endpoints, so concurrent writers cannot extend
 * the requested drain indefinitely. Diagnostic events only overestimate work. */
asper_err asper_source_pending_capture(asper_ctx *c, size_t *budget) {
  *budget = 0; os_mutex_lock(&c->replay_mu);
  asper_err e = initialize(c);
  struct asper_source_pending *p = c->source_pending;
  if (e == ASPER_OK && p->inventory_dirty) e = inventory(c);
  for (size_t i = 0; e == ASPER_OK && i < p->n; i++)
    if (p->scopes[i].refresh) e = capture_scope(c, &p->scopes[i]);
  if (e == ASPER_OK) {
    os_mutex_lock(&c->ev_mu); *budget = c->turns_n; os_mutex_unlock(&c->ev_mu);
    for (size_t i = 0; i < p->n; i++) {
      uint64_t left = p->scopes[i].end - p->scopes[i].cursor;
      if (left > SIZE_MAX - *budget) { e = ASPER_ERR_LIMIT; break; }
      *budget += (size_t)left;
    }
    if (e == ASPER_OK) p->frozen = true;
  }
  os_mutex_unlock(&c->replay_mu); return e;
}

void asper_source_pending_release(asper_ctx *c) {
  os_mutex_lock(&c->replay_mu);
  if (c->source_pending) c->source_pending->frozen = false;
  publish(c); os_mutex_unlock(&c->replay_mu);
}
