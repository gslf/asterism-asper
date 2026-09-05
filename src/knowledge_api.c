/* Host-only grounding updates use optimistic revisions and durable acceptance. */
#include "knowledge.h"
#include <stdlib.h>
#include <string.h>

asper_err asper_memory_ground(asper_ctx *c, const char *id, const char *hash,
    unsigned long long expected, const asper_grounding *g) {
  if (!c || !c->knowledge || !asper_uuid_valid(id) || !knowledge_hash(hash) || expected >= INT64_MAX)
    return ASPER_ERR_INVALID;
  os_rwlock_wrlock(&c->lock);
  asper_err e = asper_knowledge_guard(c);
  knowledge_entry *old = knowledge_find(c->knowledge,id), copy = {0};
  asper_record *r = asper_table_get(&c->store.table,id);
  char current[65];
  if (e == ASPER_OK && (!r || r->deprecated)) e = ASPER_ERR_NOT_FOUND;
  if (e == ASPER_OK && expected != (old ? old->revision : 0)) e = ASPER_ERR_BUSY;
  if (e == ASPER_OK) {
    knowledge_content_hash(r->content,current);
    if (strcmp(current,hash)) e = ASPER_ERR_BUSY;
  }
  if (e == ASPER_OK) e = knowledge_validate(c,r,g);
  if (e == ASPER_OK) e = knowledge_sources(c,g);
  if (e == ASPER_OK) {
    knowledge_entry input = {0};
    strcpy(input.id,id); strcpy(input.hash,hash); input.revision = expected+1;
    input.grounding = (asper_grounding *)g; input.content = r->content;
    char *text = knowledge_encode(&input);
    e = text ? knowledge_decode(text,&copy) : ASPER_ERR_NOMEM;
    free(text);
  }
  if (e == ASPER_OK) e = knowledge_commit(c,&copy);
  knowledge_entry_free(&copy);
  asper_knowledge_refresh(c);
  os_rwlock_wrunlock(&c->lock);
  return e;
}
asper_err asper_memory_grounding(asper_ctx *c, const char *id, asper_grounding **out,
    unsigned long long *revision, asper_knowledge_status *status) {
  if (!c || !asper_uuid_valid(id) || !out || !revision || !status) return ASPER_ERR_INVALID;
  *out = NULL; *revision = 0; *status = ASPER_KNOWLEDGE_UNAVAILABLE;
  os_rwlock_wrlock(&c->lock);
  asper_err e = asper_knowledge_guard(c);
  asper_record *r = asper_table_get(&c->store.table,id);
  if (e == ASPER_OK && !r) e = ASPER_ERR_NOT_FOUND;
  if (e == ASPER_OK) {
    asper_knowledge_refresh(c); *status = r->knowledge_status;
    knowledge_entry *entry = knowledge_find(c->knowledge,id);
    if (entry) {
      knowledge_entry copy = {0}; char *text = knowledge_encode(entry);
      e = text ? knowledge_decode(text,&copy) : ASPER_ERR_NOMEM;
      free(text);
      if (e == ASPER_OK) { *out = copy.grounding; *revision = copy.revision; copy.grounding = NULL; }
      knowledge_entry_free(&copy);
    }
  }
  os_rwlock_wrunlock(&c->lock); return e;
}
asper_err asper_memory_observe_dependency(asper_ctx *c, const char *resource, const char *version) {
  if (!c || !c->knowledge || !resource || !*resource || strlen(resource) >= 256 ||
      !asper_utf8_count(resource,NULL) || (version && (strlen(version) >= 129 || !asper_utf8_count(version,NULL))))
    return ASPER_ERR_INVALID;
  if (!version) version = "";
  os_rwlock_wrlock(&c->lock);
  asper_knowledge *k = c->knowledge;
  asper_err e = asper_knowledge_guard(c);
  asper_dependency *d = NULL;
  for (size_t i = 0; i < k->observed_n; i++)
    if (!strcmp(k->observations[i].resource,resource)) { d = &k->observations[i]; break; }
  if (e == ASPER_OK && !d && k->observed_n == k->observed_cap) {
    size_t cap = k->observed_cap ? k->observed_cap*2 : 16;
    if (cap > KNOWLEDGE_MAX_RECORDS) e = ASPER_ERR_LIMIT;
    else {
      asper_dependency *v = realloc(k->observations,cap*sizeof *v);
      if (!v) e = ASPER_ERR_NOMEM;
      else { k->observations = v; k->observed_cap = cap; }
    }
  }
  if (e == ASPER_OK) {
    if (!d) { d = &k->observations[k->observed_n++]; strcpy(d->resource,resource); }
    strcpy(d->version,version); asper_knowledge_refresh(c);
  }
  os_rwlock_wrunlock(&c->lock); return e;
}
