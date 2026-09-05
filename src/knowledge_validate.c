/* Hosts attach granular evidence; the curator cannot certify its own claims. */
#include "knowledge.h"
#include <stdlib.h>
#include <string.h>

static bool text_ok(const char *s, size_t cap, bool allow_empty) {
  return memchr(s,0,cap) && (allow_empty || s[0]) && asper_utf8_count(s,NULL);
}
static bool scope_ok(const char *scope) {
  if (!strcmp(scope,".") || !strcmp(scope,"..")) return false;
  for (const char *p = scope; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return false;
  return true;
}
static bool boundary(const char *text, size_t i) { return ((unsigned char)text[i]&0xc0) != 0x80; }
static bool range_ok(const char *text, size_t begin, size_t end) {
  return begin < end && end <= strlen(text) && boundary(text,begin) && boundary(text,end);
}
/* Existing support paths are bounded; cycles cannot provide their own proof. */
static bool reaches(asper_knowledge *k, const char *from, const char *to, unsigned depth,
                    unsigned char *visited) {
  if (!strcmp(from,to) || depth >= 32) return true;
  knowledge_entry *e = knowledge_find(k,from);
  if (!e) return false;
  /* Shared ancestors must not multiply work exponentially in a dense DAG. */
  size_t index = (size_t)(e-k->entries);
  if (visited[index] >= depth+1) return false;
  visited[index] = (unsigned char)(depth+1);
  for (size_t i = 0; i < e->grounding->links_n; i++) {
    asper_knowledge_link *l = &e->grounding->links[i];
    if (l->kind == ASPER_REL_SUPPORTS && reaches(k,l->record_id,to,depth+1,visited)) return true;
  }
  return false;
}
asper_err knowledge_validate(asper_ctx *c, const asper_record *r, const asper_grounding *g) {
  if (!g || g->sources_n > KNOWLEDGE_MAX_ITEMS || g->dependencies_n > KNOWLEDGE_MAX_ITEMS ||
      g->links_n > KNOWLEDGE_MAX_ITEMS || (g->sources_n && !g->sources) ||
      (g->dependencies_n && !g->dependencies) || (g->links_n && !g->links) ||
      (g->revoked != 0 && g->revoked != 1) || !text_ok(g->reason,sizeof g->reason,!g->revoked)) return ASPER_ERR_INVALID;
  if (r && strlen(r->content) > 65536) return ASPER_ERR_LIMIT;
  for (size_t i = 0; i < g->sources_n; i++) {
    const asper_source_span *s = &g->sources[i];
    if (!text_ok(s->scope,sizeof s->scope,false) || !scope_ok(s->scope) || !text_ok(s->event_id,sizeof s->event_id,false) ||
        !asper_uuid_valid(s->event_id) || !s->sequence || s->sequence > INT64_MAX ||
        s->source_begin >= s->source_end || s->claim_begin >= s->claim_end ||
        s->source_end > INT64_MAX || s->claim_end > INT64_MAX ||
        (r && !range_ok(r->content,s->claim_begin,s->claim_end))) return ASPER_ERR_INVALID;
  }
  for (size_t i = 0; i < g->dependencies_n; i++) {
    const asper_dependency *d = &g->dependencies[i];
    if (!text_ok(d->resource,sizeof d->resource,false) || !text_ok(d->version,sizeof d->version,false)) return ASPER_ERR_INVALID;
    for (size_t j = 0; j < i; j++) if (!strcmp(d->resource,g->dependencies[j].resource)) return ASPER_ERR_INVALID;
  }
  for (size_t i = 0; i < g->links_n; i++) {
    const asper_knowledge_link *l = &g->links[i];
    if (l->kind < ASPER_REL_SUPPORTS || l->kind > ASPER_REL_SUPERSEDES ||
        !text_ok(l->record_id,sizeof l->record_id,false) || !asper_uuid_valid(l->record_id) ||
        !text_ok(l->content_sha256,sizeof l->content_sha256,false) || !knowledge_hash(l->content_sha256)) return ASPER_ERR_INVALID;
    for (size_t j = 0; j < i; j++) if (!strcmp(l->record_id,g->links[j].record_id)) return ASPER_ERR_INVALID;
    if (r) {
      char hash[65]; const asper_record *other = asper_table_get(&c->store.table,l->record_id);
      if (!other || !strcmp(r->id,l->record_id)) return ASPER_ERR_INVALID;
      knowledge_content_hash(other->content,hash);
      if (strcmp(hash,l->content_sha256)) return ASPER_ERR_BUSY;
      if (l->kind == ASPER_REL_SUPPORTS) {
        unsigned char *visited = calloc(c->knowledge->n+1,1);
        if (!visited) return ASPER_ERR_NOMEM;
        bool cycle = reaches(c->knowledge,l->record_id,r->id,0,visited);
        free(visited);
        if (cycle) return ASPER_ERR_INVALID;
      }
    }
  }
  return ASPER_OK;
}
asper_err knowledge_sources(asper_ctx *c, const asper_grounding *g) {
  for (size_t i = 0; i < g->sources_n; i++) {
    const asper_source_span *s = &g->sources[i];
    asper_event *events = NULL; size_t n = 0; unsigned long long next;
    asper_err e = asper_event_search(c,s->scope,"",s->sequence-1,1,&events,&n,&next);
    if (e == ASPER_OK && (!n || events[0].sequence != s->sequence || strcmp(events[0].id,s->event_id) ||
        !range_ok(events[0].text,s->source_begin,s->source_end))) e = ASPER_ERR_INVALID;
    asper_events_free(events,n);
    if (e != ASPER_OK) return e;
  }
  return ASPER_OK;
}
