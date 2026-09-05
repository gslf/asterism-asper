/* Validity is a runtime projection. It never increases confidence from reuse. */
#include "knowledge.h"
#include <string.h>

const char *asper_knowledge_status_name(asper_knowledge_status status) {
  switch (status) {
    case ASPER_KNOWLEDGE_UNVERIFIED: return "unverified";
    case ASPER_KNOWLEDGE_CURRENT: return "current";
    case ASPER_KNOWLEDGE_STALE: return "stale";
    case ASPER_KNOWLEDGE_CONTESTED: return "contested";
    case ASPER_KNOWLEDGE_REVOKED: return "revoked";
    case ASPER_KNOWLEDGE_UNAVAILABLE: return "unavailable";
  }
  return "unavailable";
}
asper_knowledge_status asper_record_knowledge_status(const asper_record *r) {
  return r ? r->knowledge_status : ASPER_KNOWLEDGE_UNAVAILABLE;
}
unsigned long long asper_record_knowledge_revision(const asper_record *r) {
  return r ? r->knowledge_revision : 0;
}
asper_err asper_knowledge_guard(asper_ctx *c) {
  return c->knowledge && c->knowledge->poisoned ? ASPER_ERR_IO : ASPER_OK;
}
static bool same_claim(const asper_record *r, const char *expected) {
  char hash[65];
  if (!r) return false;
  knowledge_content_hash(r->content,hash); return !strcmp(hash,expected);
}
/* Separate spans may cover a whole claim; gaps keep it explicitly unverified. */
static bool covered(const asper_grounding *g, size_t size) {
  size_t end = 0;
  for (size_t pass = 0; pass < g->sources_n; pass++)
    for (size_t i = 0; i < g->sources_n; i++)
      if (g->sources[i].claim_begin <= end && g->sources[i].claim_end > end) end = g->sources[i].claim_end;
  return end >= size;
}
static asper_knowledge_status initial(asper_ctx *c, const asper_record *r) {
  asper_knowledge *k = c->knowledge;
  if (k && k->poisoned) return ASPER_KNOWLEDGE_UNAVAILABLE;
  if (r->deprecated || (r->evidence.expires_at && asper_clock_now(&c->clock) >= r->evidence.expires_at))
    return ASPER_KNOWLEDGE_STALE;
  knowledge_entry *e = knowledge_find(k,r->id);
  if (!e) return ASPER_KNOWLEDGE_UNVERIFIED;
  const asper_grounding *g = e->grounding;
  if (g->revoked) return ASPER_KNOWLEDGE_REVOKED;
  if (!same_claim(r,e->hash)) return ASPER_KNOWLEDGE_STALE;
  if (!e->sources_ok) return ASPER_KNOWLEDGE_UNAVAILABLE;
  for (size_t i = 0; i < g->dependencies_n; i++) {
    const asper_dependency *d = &g->dependencies[i], *observed = NULL;
    for (size_t j = 0; j < k->observed_n; j++)
      if (!strcmp(d->resource,k->observations[j].resource)) { observed = &k->observations[j]; break; }
    if (!observed || !observed->version[0]) return ASPER_KNOWLEDGE_UNAVAILABLE;
    if (strcmp(observed->version,d->version)) return ASPER_KNOWLEDGE_STALE;
  }
  if (covered(g,strlen(r->content))) return ASPER_KNOWLEDGE_CURRENT;
  for (size_t i = 0; i < g->links_n; i++) if (g->links[i].kind == ASPER_REL_SUPPORTS) return ASPER_KNOWLEDGE_CURRENT;
  return ASPER_KNOWLEDGE_UNVERIFIED;
}
static void propagate_support(asper_ctx *c) {
  asper_knowledge *k = c->knowledge;
  for (size_t pass = 0; pass < k->n; pass++) {
    bool changed = false;
    for (size_t i = 0; i < k->n; i++) {
      knowledge_entry *e = &k->entries[i];
      asper_record *r = asper_table_get(&c->store.table,e->id);
      if (!r || r->knowledge_status >= ASPER_KNOWLEDGE_STALE) continue;
      for (size_t j = 0; j < e->grounding->links_n; j++) {
        asper_knowledge_link *l = &e->grounding->links[j];
        if (l->kind != ASPER_REL_SUPPORTS) continue;
        asper_record *parent = asper_table_get(&c->store.table,l->record_id);
        asper_knowledge_status next = r->knowledge_status;
        if (!same_claim(parent,l->content_sha256)) next = ASPER_KNOWLEDGE_STALE;
        else if (parent->knowledge_status >= ASPER_KNOWLEDGE_STALE)
          next = parent->knowledge_status == ASPER_KNOWLEDGE_REVOKED ? ASPER_KNOWLEDGE_STALE : parent->knowledge_status;
        else if (parent->knowledge_status == ASPER_KNOWLEDGE_UNVERIFIED) next = ASPER_KNOWLEDGE_UNVERIFIED;
        if (next != r->knowledge_status) { r->knowledge_status = next; changed = true; }
        if (next >= ASPER_KNOWLEDGE_STALE) break;
      }
    }
    if (!changed) break;
  }
}

/* A correction retires a claim before that claim can contest another one.
 * Splitting the phases keeps results independent from UUID/table order. */
static void apply_relations(asper_ctx *c, asper_relation_kind kind) {
  asper_knowledge *k = c->knowledge;
  for (size_t i = 0; i < k->n; i++) {
    knowledge_entry *e = &k->entries[i];
    asper_record *r = asper_table_get(&c->store.table,e->id);
    for (size_t j = 0; j < e->grounding->links_n; j++) {
      asper_knowledge_link *l = &e->grounding->links[j];
      if (l->kind != kind) continue;
      asper_record *target = asper_table_get(&c->store.table,l->record_id);
      if (!same_claim(target,l->content_sha256)) continue;
      if (kind == ASPER_REL_SUPERSEDES && target->knowledge_status != ASPER_KNOWLEDGE_REVOKED)
        target->knowledge_status = ASPER_KNOWLEDGE_STALE;
      if (kind == ASPER_REL_CONTRADICTS && r && same_claim(r,e->hash) &&
          (r->knowledge_status < ASPER_KNOWLEDGE_STALE || r->knowledge_status == ASPER_KNOWLEDGE_CONTESTED) &&
          (target->knowledge_status < ASPER_KNOWLEDGE_STALE || target->knowledge_status == ASPER_KNOWLEDGE_CONTESTED))
        r->knowledge_status = target->knowledge_status = ASPER_KNOWLEDGE_CONTESTED;
    }
  }
}
void asper_knowledge_refresh(asper_ctx *c) {
  asper_knowledge *k = c->knowledge;
  for (size_t i = 0; i < c->store.table.n; i++) {
    asper_record *r = c->store.table.recs[i]; r->knowledge_status = initial(c,r);
    knowledge_entry *e = knowledge_find(k,r->id); r->knowledge_revision = e ? e->revision : 0;
  }
  if (!k) return;
  apply_relations(c,ASPER_REL_SUPERSEDES);
  propagate_support(c);
  apply_relations(c,ASPER_REL_CONTRADICTS);
  propagate_support(c);
}
