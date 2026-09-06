/* Bounded ownership includes in-flight inputs, so retries retain their slots. */
#include "asper_internal.h"
#include "xcdn.h"
#include <stdlib.h>
#include <string.h>

size_t asper_turn_queue_limit(const asper_ctx *c) {
  return c->cfg.event_queue_max < 1 ? 1 :
      (size_t)c->cfg.event_queue_max > ASPER_QUEUE_EVENTS ? ASPER_QUEUE_EVENTS :
      (size_t)c->cfg.event_queue_max;
}
static bool room_for(asper_ctx *c, size_t bytes) {
  os_mutex_lock(&c->ev_mu);
  bool room = c->turns_n + c->turns_inflight < asper_turn_queue_limit(c) &&
      bytes <= ASPER_QUEUE_BYTES - c->turns_bytes - c->turns_inflight_bytes;
  os_mutex_unlock(&c->ev_mu); return room;
}
bool asper_turn_queue_room(asper_ctx *c) { return room_for(c, 1); }
void asper_turn_queue_release(asper_ctx *c, const asper_turn *turns, size_t n) {
  os_mutex_lock(&c->ev_mu);
  for (size_t i = 0; i < n; i++) if (turns[i].text) {
    c->turns_inflight--; c->turns_inflight_bytes -= strlen(turns[i].text) + 1;
  }
  os_mutex_unlock(&c->ev_mu);
}

asper_err asper_enqueue_turn(asper_ctx *c, asper_role role,
                             const char *text_utf8, asper_time now,
                             const char *source_id, const char *scope, const char *object_ref) {
  char *copy;
  asper_turn meta;
  memset(&meta,0,sizeof meta);
  if (!c) return ASPER_ERR_INVALID;
  if (role != ASPER_ROLE_USER && role != ASPER_ROLE_ASSISTANT)
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid role");
  if (asper_str_blank(text_utf8))
    return asper_seterr(c, ASPER_ERR_INVALID, "empty turn text");
  if (!asper_utf8_count(text_utf8, NULL))
    return asper_seterr(c, ASPER_ERR_INVALID, "turn text is not valid UTF-8");
  if (!asper_uuid_valid(source_id))
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid source event id");

  size_t bytes = strlen(text_utf8) + 1;
  if (bytes > ASPER_QUEUE_BYTES) return ASPER_ERR_LIMIT;
  if (!room_for(c, bytes)) return ASPER_ERR_BUSY;
  snprintf(meta.scope,sizeof meta.scope,"%s",scope ? scope : "");
  meta.evidence.observed_at=now;
  snprintf(meta.evidence.provenance,sizeof meta.evidence.provenance,"scope:%s",meta.scope);
  if (object_ref && object_ref[0]) {
    void *data=NULL;size_t len=0;
    asper_err e = asper_object_read(c, object_ref, 0, 0, &data, &len);
    if (e != ASPER_OK) return e;
    {
      xcdn_error_t xe;memset(&xe,0,sizeof xe);
      xcdn_document_t *doc=xcdn_parse_str(data,len,&xe);
      if (doc && doc->values_len==1 && xcdn_node_has_tag(doc->values[0],"turn")) {
        const xcdn_value_t *obj=doc->values[0]->value;
        if (!obj || obj->type != XCDN_VAL_OBJECT) e = ASPER_ERR_PARSE;
        const char *keys[]={"workspace","commit","project"};
        char *dst[]={meta.evidence.workspace,meta.evidence.commit,meta.project};
        size_t caps[]={sizeof meta.evidence.workspace,sizeof meta.evidence.commit,sizeof meta.project};
        for (size_t i=0; e == ASPER_OK && i<3; i++) {
          const xcdn_node_t *v=xcdn_object_get(obj,keys[i]);
          if (!v) continue;
          if (!v->value || v->value->type != XCDN_VAL_STRING ||
              strlen(v->value->data.string) >= caps[i]) e = ASPER_ERR_PARSE;
          else strcpy(dst[i], v->value->data.string);
        }
      }
      if (doc) xcdn_document_free(doc);
      free(data);
      if (e != ASPER_OK) return e;
    }
  }
  copy = asper_strdup(text_utf8);
  if (!copy) return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");

  os_mutex_lock(&c->ev_mu);
  if (c->turns_n + c->turns_inflight >= asper_turn_queue_limit(c) ||
      bytes > ASPER_QUEUE_BYTES - c->turns_bytes - c->turns_inflight_bytes) {
    os_mutex_unlock(&c->ev_mu); free(copy); return ASPER_ERR_BUSY;
  }
  /* Reserve array slots for the in-flight batch too: restoring it must not
   * allocate after new producers have consumed the available queue capacity. */
  if (c->turns_n + c->turns_inflight == c->turns_cap) {
    size_t ncap = c->turns_cap ? c->turns_cap * 2 : 16;
    if (ncap > ASPER_QUEUE_EVENTS) ncap = ASPER_QUEUE_EVENTS;
    asper_turn *nt = realloc(c->turns, ncap * sizeof *nt);
    if (!nt) {
      os_mutex_unlock(&c->ev_mu);
      free(copy);
      return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
    }
    c->turns = nt;
    c->turns_cap = ncap;
  }
  c->turns[c->turns_n]=meta;
  c->turns[c->turns_n].role = role;
  c->turns[c->turns_n].text = copy;
  c->turns[c->turns_n].at = now;
  memcpy(c->turns[c->turns_n].source_id, source_id, 37);
  c->turns_n++; c->turns_bytes += bytes;
  c->last_turn_at = now;
  /* Wake the worker on EVERY enqueued turn so the idle-flush deadline
   * arms even for a partial batch; turn_batch only decides when a cycle is
   * due, not when the worker wakes (notes "Post-review amendments"). */
  if (!c->no_threads && c->worker_running)
    os_cond_signal(&c->ev_cv);
  os_mutex_unlock(&c->ev_mu);
  return ASPER_OK;
}
