/* Grounding has its own checked journal; conversation/record compaction cannot
 * erase correction history. c->lock serializes this store and its projection. */
#include "knowledge.h"
#include "event_log.h"
#include <stdlib.h>
#include <string.h>

bool knowledge_hash(const char *s) {
  if (!s || strlen(s) != 64) return false;
  for (size_t i = 0; i < 64; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
  return true;
}
void knowledge_content_hash(const char *text, char out[65]) {
  uint8_t hash[32]; asper_sha256(text,strlen(text),hash);
  for (size_t i = 0; i < 32; i++) snprintf(out+2*i,3,"%02x",hash[i]);
}
static size_t lower_bound(asper_knowledge *k, const char *id) {
  size_t lo = 0, hi = k ? k->n : 0;
  while (lo < hi) {
    size_t mid = lo+(hi-lo)/2;
    if (strcmp(k->entries[mid].id,id) < 0) lo = mid+1; else hi = mid;
  }
  return lo;
}
knowledge_entry *knowledge_find(asper_knowledge *k, const char *id) {
  size_t i = lower_bound(k,id);
  return k && i < k->n && !strcmp(k->entries[i].id,id) ? &k->entries[i] : NULL;
}
static asper_err reserve(asper_knowledge *k) {
  if (k->n < k->cap) return ASPER_OK;
  if (k->n >= KNOWLEDGE_MAX_RECORDS) return ASPER_ERR_LIMIT;
  size_t cap = k->cap ? k->cap*2 : 16;
  knowledge_entry *p = realloc(k->entries,cap*sizeof *p);
  if (!p) return ASPER_ERR_NOMEM;
  k->entries = p; k->cap = cap; return ASPER_OK;
}
static void replace(asper_knowledge *k, knowledge_entry *e) {
  knowledge_entry *old = knowledge_find(k,e->id);
  if (old) knowledge_entry_free(old);
  else {
    size_t at = lower_bound(k,e->id);
    memmove(k->entries+at+1,k->entries+at,(k->n-at)*sizeof *k->entries);
    old = &k->entries[at]; k->n++;
  }
  *old = *e; e->grounding = NULL; e->content = NULL;
}
asper_err knowledge_commit(asper_ctx *c, knowledge_entry *entry) {
  asper_knowledge *k = c->knowledge;
  if (k->poisoned) return ASPER_ERR_IO;
  if (!knowledge_find(k,entry->id)) {
    asper_err e = reserve(k); if (e != ASPER_OK) return e;
  }
  char *text = knowledge_encode(entry);
  if (!text) return ASPER_ERR_NOMEM;
  asper_event event = {0}; event.text = text; event.kind = ASPER_EVENT_DIAGNOSTIC;
  event.at = asper_clock_now(&c->clock); asper_uuid_v4(event.id);
  asper_err e = asper_event_log_append(k->path,&event);
  free(text);
  if (e != ASPER_OK) { k->poisoned = true; return e; }
  entry->sources_ok = true; replace(k,entry); return ASPER_OK;
}
asper_err asper_knowledge_open(asper_ctx *c) {
  asper_knowledge *k = calloc(1,sizeof *k);
  if (!k) return ASPER_ERR_NOMEM;
  c->knowledge = k; k->path = os_path_join(c->store.root,"knowledge.events");
  if (!k->path) return ASPER_ERR_NOMEM;
  unsigned long long next = 0;
  for (;;) {
    asper_event *events = NULL; size_t n = 0;
    asper_err e = asper_event_log_page(k->path,"",next,64,&events,&n,&next);
    if (e != ASPER_OK) return e;
    for (size_t i = 0; i < n; i++) {
      knowledge_entry entry = {0};
      e = events[i].kind == ASPER_EVENT_DIAGNOSTIC ? knowledge_decode(events[i].text,&entry) : ASPER_ERR_PARSE;
      if (e == ASPER_OK) e = knowledge_validate(c,NULL,entry.grounding);
      knowledge_entry *old = e == ASPER_OK ? knowledge_find(k,entry.id) : NULL;
      if (e == ASPER_OK && entry.revision != (old ? old->revision+1 : 1)) e = ASPER_ERR_PARSE;
      if (e == ASPER_OK && !old) e = reserve(k);
      if (e == ASPER_OK) {
        entry.sources_ok = knowledge_sources(c,entry.grounding) == ASPER_OK;
        replace(k,&entry);
      }
      knowledge_entry_free(&entry);
      if (e != ASPER_OK) { asper_events_free(events,n); return e; }
    }
    asper_events_free(events,n);
    if (!n) break;
  }
  for (size_t i = 0; i < k->n; i++) {
    knowledge_entry *e = &k->entries[i];
    const asper_record *record = asper_table_get(&c->store.table,e->id);
    char hash[65];
    if (record) {
      knowledge_content_hash(record->content,hash);
      if (!strcmp(hash,e->hash) && knowledge_validate(c,record,e->grounding) != ASPER_OK) e->sources_ok = false;
    }
  }
  asper_knowledge_refresh(c); return ASPER_OK;
}
void asper_knowledge_close(asper_ctx *c) {
  asper_knowledge *k = c->knowledge;
  if (!k) return;
  for (size_t i = 0; i < k->n; i++) knowledge_entry_free(&k->entries[i]);
  free(k->entries); free(k->observations); free(k->path); free(k); c->knowledge = NULL;
}
