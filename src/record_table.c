/* Record ownership and UUID lookup table. */
#include "asper_internal.h"
#include <stdlib.h>
#include <string.h>
asper_record *asper_record_new(void) {
  asper_record *r = (asper_record *)calloc(1, sizeof(*r));
  if (!r) return NULL;
  r->emb_row = -1;
  return r;
}

void asper_record_free_one(asper_record *r) {
  if (!r) return;
  free(r->project);
  free(r->content);
  for (size_t i = 0; i < r->tags_n; i++) free(r->tags[i]);
  free(r->tags);
  for (size_t i = 0; i < r->source_refs_n; i++) free(r->source_refs[i]);
  free(r->source_refs);
  free(r);
}

asper_record *asper_record_clone(const asper_record *r) {
  asper_record *n;
  if (!r) return NULL;
  n = asper_record_new();
  if (!n) return NULL;
  memcpy(n->id, r->id, sizeof(n->id));
  n->section = r->section;
  n->source = r->source;
  n->evidence = r->evidence;
  n->knowledge_status = r->knowledge_status;
  n->knowledge_revision = r->knowledge_revision;
  n->created_at = r->created_at;
  n->updated_at = r->updated_at;
  n->last_access = r->last_access;
  n->access_count = r->access_count;
  n->relevance = r->relevance;
  n->locked = r->locked;
  n->deprecated = r->deprecated;
  n->deprecated_at = r->deprecated_at;
  n->score = r->score;
  n->emb_row = -1;
  n->project = asper_strdup(r->project);
  if (r->project && !n->project) goto fail;
  n->content = asper_strdup(r->content);
  if (r->content && !n->content) goto fail;
  if (r->tags_n > 0) {
    n->tags = (char **)calloc(r->tags_n, sizeof(char *));
    if (!n->tags) goto fail;
    n->tags_n = r->tags_n;
    for (size_t i = 0; i < r->tags_n; i++) {
      n->tags[i] = asper_strdup(r->tags[i]);
      if (r->tags[i] && !n->tags[i]) goto fail;
    }
  }
  if (r->source_refs_n > 0) {
    n->source_refs = (char **)calloc(r->source_refs_n, sizeof(char *));
    if (!n->source_refs) goto fail;
    n->source_refs_n = r->source_refs_n;
    for (size_t i = 0; i < r->source_refs_n; i++) {
      n->source_refs[i] = asper_strdup(r->source_refs[i]);
      if (!n->source_refs[i]) goto fail;
    }
  }
  return n;
fail:
  asper_record_free_one(n);
  return NULL;
}

const char *asper_source_name(asper_source s) {
  switch (s) {
    case ASPER_SRC_SEED:   return "seed";
    case ASPER_SRC_MANUAL: return "manual";
    case ASPER_SRC_CURATOR: return "curator";
  }
  return "curator";
}

bool asper_source_parse(const char *s, asper_source *out) {
  if (!s || !out) return false;
  if (strcmp(s, "seed") == 0)    { *out = ASPER_SRC_SEED;    return true; }
  if (strcmp(s, "manual") == 0)  { *out = ASPER_SRC_MANUAL;  return true; }
  if (strcmp(s, "curator") == 0) { *out = ASPER_SRC_CURATOR; return true; }
  return false;
}

const char *asper_section_name(asper_section s) {
  switch (s) {
    case ASPER_SECTION_IDENTITY: return "identity";
    case ASPER_SECTION_CONTEXT:  return "context";
    case ASPER_SECTION_PROJECT:  return "project";
    case ASPER_SECTION_ANY:      return "any";
  }
  return "any";
}

bool asper_section_parse(const char *s, asper_section *out) {
  if (!s || !out) return false;
  if (strcmp(s, "identity") == 0) { *out = ASPER_SECTION_IDENTITY; return true; }
  if (strcmp(s, "context") == 0)  { *out = ASPER_SECTION_CONTEXT;  return true; }
  if (strcmp(s, "project") == 0)  { *out = ASPER_SECTION_PROJECT;  return true; }
  return false;
}

/* ═══════════════════════ id map + table ═══════════════════════ */

/* Open-addressing map from record id to table index. Slots hold index + 1
 * (0 = empty). No tombstones: any remove rebuilds the whole map. */
struct asper_idmap {
  size_t *slots;
  size_t cap;   /* power of two; 0 only in a degraded (map-less) table */
  size_t used;
};

static uint64_t id_hash(const char *id) {
  return asper_fnv1a64(id, strlen(id));
}

static struct asper_idmap *idmap_create(size_t cap) {
  struct asper_idmap *m = (struct asper_idmap *)calloc(1, sizeof(*m));
  if (!m) return NULL;
  m->slots = (size_t *)calloc(cap, sizeof(size_t));
  if (!m->slots) {
    free(m);
    return NULL;
  }
  m->cap = cap;
  return m;
}

static void idmap_destroy(struct asper_idmap *m) {
  if (!m) return;
  free(m->slots);
  free(m);
}

static void idmap_insert(struct asper_idmap *m, const asper_table *t,
                         size_t idx) {
  size_t pos = (size_t)(id_hash(t->recs[idx]->id) & (uint64_t)(m->cap - 1));
  while (m->slots[pos] != 0) pos = (pos + 1) & (m->cap - 1);
  m->slots[pos] = idx + 1;
  m->used++;
}

/* Rebuild the map sized for at least mincap entries. */
static asper_err table_rehash(asper_table *t, size_t mincap) {
  size_t cap = 16;
  struct asper_idmap *m;
  while (cap < mincap * 2) cap <<= 1;
  m = idmap_create(cap);
  if (!m) return ASPER_ERR_NOMEM;
  for (size_t i = 0; i < t->n; i++) idmap_insert(m, t, i);
  idmap_destroy(t->idmap);
  t->idmap = m;
  return ASPER_OK;
}

static size_t table_find(const asper_table *t, const char *id) {
  if (!t || !id || !id[0] || t->n == 0) return (size_t)-1;
  if (t->idmap && t->idmap->cap > 0) {
    size_t mask = t->idmap->cap - 1;
    size_t pos = (size_t)(id_hash(id) & (uint64_t)mask);
    while (t->idmap->slots[pos] != 0) {
      size_t idx = t->idmap->slots[pos] - 1;
      if (strcmp(t->recs[idx]->id, id) == 0) return idx;
      pos = (pos + 1) & mask;
    }
    return (size_t)-1;
  }
  /* degraded path: a rehash failed under memory pressure */
  for (size_t i = 0; i < t->n; i++)
    if (strcmp(t->recs[i]->id, id) == 0) return i;
  return (size_t)-1;
}

void asper_table_init(asper_table *t) {
  if (!t) return;
  memset(t, 0, sizeof(*t));
}

void asper_table_free(asper_table *t) {
  if (!t) return;
  for (size_t i = 0; i < t->n; i++) asper_record_free_one(t->recs[i]);
  free(t->recs);
  idmap_destroy(t->idmap);
  memset(t, 0, sizeof(*t));
}

asper_err asper_table_add(asper_table *t, asper_record *r) {
  if (!t || !r || r->id[0] == '\0') return ASPER_ERR_INVALID;
  if (table_find(t, r->id) != (size_t)-1) return ASPER_ERR_INVALID;
  if (t->n >= ASPER_RECORD_LIMIT) return ASPER_ERR_LIMIT;
  if (t->n == t->cap) {
    size_t ncap = t->cap ? t->cap * 2 : 32;
    asper_record **nr =
        (asper_record **)realloc(t->recs, ncap * sizeof(asper_record *));
    if (!nr) return ASPER_ERR_NOMEM;
    t->recs = nr;
    t->cap = ncap;
  }
  if (!t->idmap || (t->idmap->used + 1) * 10 > t->idmap->cap * 7) {
    asper_err e = table_rehash(t, t->n + 1);
    if (e != ASPER_OK) return e;
  }
  t->recs[t->n] = r;
  idmap_insert(t->idmap, t, t->n);
  t->n++;
  return ASPER_OK;
}

asper_record *asper_table_get(const asper_table *t, const char *id) {
  size_t idx = table_find(t, id);
  return idx == (size_t)-1 ? NULL : t->recs[idx];
}

bool asper_table_remove(asper_table *t, const char *id) {
  size_t idx = table_find(t, id);
  if (idx == (size_t)-1) return false;
  asper_record_free_one(t->recs[idx]);
  memmove(&t->recs[idx], &t->recs[idx + 1],
          (t->n - idx - 1) * sizeof(asper_record *));
  t->n--;
  if (table_rehash(t, t->n ? t->n : 1) != ASPER_OK) {
    /* keep correctness without the map: lookups fall back to linear scan */
    idmap_destroy(t->idmap);
    t->idmap = NULL;
  }
  return true;
}

/* ═══════════════════════ project registry ═══════════════════════ */

/* Lower bound over the sorted slug list. *found set when slug present. */
