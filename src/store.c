/*
 * store.c — record model, id-indexed in-memory table, section files,
 * project registry, store open/close, op application, compaction.
 *
 * Owns: asper_record_* lifecycle, asper_source/section names,
 * asper_table_* (open-addressing idmap over id strings),
 * asper_store_open/close/apply/compact/project_known/project_add.
 *
 * xCDN mapping (record/op serialize + parse) lives in journal.c.
 */

#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"
#include "xcdn.h"

/* ═══════════════════════ records ═══════════════════════ */

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
  n->created_at = r->created_at;
  n->updated_at = r->updated_at;
  n->last_access = r->last_access;
  n->access_count = r->access_count;
  n->relevance = r->relevance;
  n->locked = r->locked;
  n->deprecated = r->deprecated;
  n->deprecated_at = r->deprecated_at;
  memcpy(n->supersedes, r->supersedes, sizeof(n->supersedes));
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
static size_t project_lower_bound(const asper_store *st, const char *slug,
                                  bool *found) {
  size_t lo = 0, hi = st->projects_n;
  *found = false;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(st->projects[mid], slug);
    if (cmp == 0) {
      *found = true;
      return mid;
    }
    if (cmp < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

bool asper_store_project_known(const asper_ctx *c, const char *slug) {
  bool found;
  if (!c || !slug) return false;
  (void)project_lower_bound(&c->store, slug, &found);
  return found;
}

asper_err asper_store_project_add(asper_ctx *c, const char *slug) {
  asper_store *st = &c->store;
  bool found;
  size_t pos;
  char *dup;
  if (!slug || !asper_slug_valid(slug))
    return asper_seterr(c, ASPER_ERR_INVALID, "project: invalid slug '%s'",
                        slug ? slug : "(null)");
  pos = project_lower_bound(st, slug, &found);
  if (found) return ASPER_OK;
  if (st->projects_n == st->projects_cap) {
    size_t ncap = st->projects_cap ? st->projects_cap * 2 : 8;
    char **np = (char **)realloc(st->projects, ncap * sizeof(char *));
    if (!np)
      return asper_seterr(c, ASPER_ERR_NOMEM, "project: out of memory");
    st->projects = np;
    st->projects_cap = ncap;
  }
  dup = asper_strdup(slug);
  if (!dup) return asper_seterr(c, ASPER_ERR_NOMEM, "project: out of memory");
  memmove(&st->projects[pos + 1], &st->projects[pos],
          (st->projects_n - pos) * sizeof(char *));
  st->projects[pos] = dup;
  st->projects_n++;
  return ASPER_OK;
}

/* ═══════════════════════ section file loading ═══════════════════════ */

/* Serialize a single BORROWED node pretty via a temporary document that
 * never takes ownership (values_len is zeroed before the free, so
 * xcdn_document_free leaves the node alone). malloc'd string or NULL. */
static char *store_node_pretty(xcdn_node_t *node) {
  xcdn_document_t *doc = xcdn_document_new();
  char *s = NULL;
  if (!doc) return NULL;
  doc->values = (xcdn_node_t **)malloc(sizeof(*doc->values));
  if (doc->values) {
    doc->values[0] = node;
    doc->values_len = 1;
    doc->values_cap = 1;
    s = xcdn_to_string_pretty(doc);
    doc->values_len = 0; /* borrowed: do not free node */
  }
  xcdn_document_free(doc);
  return s;
}

/* Append the offending node to <root>/quarantine.xcdn (created on demand,
 * blank-line separated) so the next compaction cannot silently destroy
 * hand-edited data (Post-review amendments). Best-effort: a quarantine
 * failure is WARNed but never fails the open. */
static void store_quarantine_node(asper_ctx *c, const char *src_path,
                                  xcdn_node_t *node) {
  char *qpath = os_path_join(c->store.root, "quarantine.xcdn");
  char *text = qpath ? store_node_pretty(node) : NULL;
  FILE *f = text ? os_fopen(qpath, "ab") : NULL;
  bool ok = false;
  if (f) {
    size_t tlen = strlen(text);
    ok = fwrite(text, 1, tlen, f) == tlen && fwrite("\n\n", 1, 2, f) == 2;
    if (fclose(f) != 0) ok = false;
  }
  if (ok)
    asper_log(c, ASPER_LOG_WARN, "store", "%s: offending value quarantined "
              "to %s", src_path, qpath);
  else
    asper_log(c, ASPER_LOG_WARN, "store",
              "%s: quarantine to %s failed; value dropped", src_path,
              qpath ? qpath : "quarantine.xcdn");
  free(text);
  free(qpath);
}

/* Parse one xCDN stream of #memory values into the table. want is the
 * section the file must contain; slug is the project slug (PROJECT only).
 * Bad records are logged WARN, quarantined and skipped; the store must
 * open. */
static asper_err store_load_section_file(asper_ctx *c, const char *path,
                                         asper_section want,
                                         const char *slug) {
  char *text = NULL;
  size_t len = 0;
  xcdn_error_t xe;
  xcdn_document_t *doc;
  asper_err e;
  size_t kept = 0, skipped = 0;

  if (!os_file_exists(path)) return ASPER_OK;
  e = os_read_file(path, &text, &len);
  if (e != ASPER_OK)
    return asper_seterr(c, e, "store: cannot read %s", path);
  doc = xcdn_parse_str(text, len, &xe);
  free(text);
  if (!doc)
    return asper_seterr(c, ASPER_ERR_PARSE, "store: %s: %s (line %zu)", path,
                        xe.message, xe.span.line);

  for (size_t i = 0; i < doc->values_len; i++) {
    xcdn_node_t *node = doc->values[i];
    asper_record *rec = NULL;
    if (!xcdn_node_has_tag(node, "memory")) {
      asper_log(c, ASPER_LOG_WARN, "store",
                "%s: value %zu is not tagged #memory; skipped", path, i + 1);
      store_quarantine_node(c, path, node);
      skipped++;
      continue;
    }
    e = asper_record_from_node(c, node, &rec);
    if (e == ASPER_ERR_NOMEM) {
      xcdn_document_free(doc);
      return e;
    }
    if (e != ASPER_OK) {
      asper_log(c, ASPER_LOG_WARN, "store", "%s: bad record skipped: %s",
                path, asper_last_error(c));
      store_quarantine_node(c, path, node);
      skipped++;
      continue;
    }
    if (rec->section != want ||
        (want == ASPER_SECTION_PROJECT &&
         (rec->project == NULL || strcmp(rec->project, slug) != 0))) {
      asper_log(c, ASPER_LOG_WARN, "store",
                "%s: record %s belongs to another section/project; skipped",
                path, rec->id);
      store_quarantine_node(c, path, node);
      asper_record_free_one(rec);
      skipped++;
      continue;
    }
    e = asper_table_add(&c->store.table, rec);
    if (e == ASPER_ERR_NOMEM) {
      asper_record_free_one(rec);
      xcdn_document_free(doc);
      return asper_seterr(c, e, "store: out of memory loading %s", path);
    }
    if (e != ASPER_OK) {
      asper_log(c, ASPER_LOG_WARN, "store",
                "%s: duplicate record id %s; skipped", path, rec->id);
      store_quarantine_node(c, path, node);
      asper_record_free_one(rec);
      skipped++;
      continue;
    }
    kept++;
  }
  xcdn_document_free(doc);
  if (skipped > 0)
    asper_log(c, ASPER_LOG_WARN, "store", "%s: %zu record(s) skipped", path,
              skipped);
  asper_log(c, ASPER_LOG_DEBUG, "store", "%s: %zu record(s) loaded", path,
            kept);
  return ASPER_OK;
}

static int name_cmp(const void *a, const void *b) {
  const char *const *sa = (const char *const *)a;
  const char *const *sb = (const char *const *)b;
  return strcmp(*sa, *sb);
}

static void names_free(char **names, size_t n) {
  for (size_t i = 0; i < n; i++) free(names[i]);
  free(names);
}

static asper_err store_load_projects(asper_ctx *c) {
  asper_store *st = &c->store;
  char **names = NULL;
  size_t names_n = 0;
  asper_err e = os_list_dir(st->projects_dir, &names, &names_n);
  if (e != ASPER_OK)
    return asper_seterr(c, e, "store: cannot list %s", st->projects_dir);
  if (names_n > 1) qsort(names, names_n, sizeof(char *), name_cmp);

  for (size_t i = 0; i < names_n; i++) {
    const char *name = names[i];
    size_t nlen = strlen(name);
    char *slug, *path;
    if (nlen <= 5 || strcmp(name + nlen - 5, ".xcdn") != 0) continue;
    slug = asper_strndup(name, nlen - 5);
    if (!slug) {
      names_free(names, names_n);
      return asper_seterr(c, ASPER_ERR_NOMEM, "store: out of memory");
    }
    if (!asper_slug_valid(slug)) {
      asper_log(c, ASPER_LOG_WARN, "store",
                "projects/%s: invalid project slug; file ignored", name);
      free(slug);
      continue;
    }
    path = os_path_join(st->projects_dir, name);
    if (!path) {
      free(slug);
      names_free(names, names_n);
      return asper_seterr(c, ASPER_ERR_NOMEM, "store: out of memory");
    }
    e = store_load_section_file(c, path, ASPER_SECTION_PROJECT, slug);
    free(path);
    if (e == ASPER_OK) e = asper_store_project_add(c, slug);
    free(slug);
    if (e != ASPER_OK) {
      names_free(names, names_n);
      return e;
    }
  }
  names_free(names, names_n);
  return ASPER_OK;
}

/* ═══════════════════════ open / close ═══════════════════════ */

#define COMPACT_MARKER "compact.pending"
#define COMPACT_BACKUP_SUFFIX ".compact.bak"

static asper_err store_write_atomic(asper_ctx *c, const char *path,
                                    const char *data, size_t len);

static char *compact_backup_path(const char *path) {
  size_t n = strlen(path) + sizeof COMPACT_BACKUP_SUFFIX;
  char *out = malloc(n);
  if (out) snprintf(out, n, "%s%s", path, COMPACT_BACKUP_SUFFIX);
  return out;
}

static bool compact_rel_valid(const char *rel) {
  size_t n;
  if (!rel) return false;
  if (strcmp(rel, "identity.xcdn") == 0 ||
      strcmp(rel, "context.xcdn") == 0 || strcmp(rel, "journal.xcdn") == 0)
    return true;
  if (strncmp(rel, "projects/", 9) != 0) return false;
  n = strlen(rel + 9);
  if (n <= 5 || strcmp(rel + strlen(rel) - 5, ".xcdn") != 0) return false;
  {
    char *slug = asper_strndup(rel + 9, n - 5);
    bool valid = slug && asper_slug_valid(slug);
    free(slug);
    return valid;
  }
}

static asper_err compact_copy_atomic(asper_ctx *c, const char *src,
                                     const char *dst) {
  char *data = NULL;
  size_t len = 0;
  asper_err e = os_read_file(src, &data, &len);
  if (e == ASPER_OK) e = store_write_atomic(c, dst, data, len);
  free(data);
  return e;
}

/* A marker means the previous compaction never committed. Backups are copied
 * (not renamed) during recovery, making recovery itself restartable. */
static asper_err compact_recover(asper_ctx *c) {
  char *marker = os_path_join(c->store.root, COMPACT_MARKER);
  char *text = NULL;
  char **backups = NULL;
  size_t backups_n = 0, backups_cap = 0;
  asper_err e = ASPER_OK;
  if (!marker) return ASPER_ERR_NOMEM;
  if (!os_file_exists(marker)) {
    free(marker);
    return ASPER_OK;
  }
  e = os_read_file(marker, &text, NULL);
  if (e != ASPER_OK) goto out;
  for (char *line = text; line && *line;) {
    char *nl = strchr(line, '\n');
    /* Parsed out of the condition and pre-initialized: assigning inside a
     * short-circuit chain is correct but defeats MSVC's definite-assignment
     * analysis (C4701/C4703, fatal under /WX). A malformed line leaves kind
     * at '\0', which the check below rejects exactly as before. */
    char kind = '\0';
    const char *rel = NULL;
    char *target = NULL, *backup = NULL;
    if (nl) *nl = '\0';
    if (strlen(line) >= 3 && line[1] == ' ') {
      kind = line[0];
      rel = line + 2;
    }
    if ((kind != 'E' && kind != 'M') || !compact_rel_valid(rel)) {
      e = asper_seterr(c, ASPER_ERR_PARSE,
                       "compact recovery: invalid marker entry");
      goto out;
    }
    target = os_path_join(c->store.root, rel);
    backup = target ? compact_backup_path(target) : NULL;
    if (!target || !backup) {
      free(target); free(backup);
      e = ASPER_ERR_NOMEM;
      goto out;
    }
    if (kind == 'E') {
      if (!os_file_exists(backup))
        e = asper_seterr(c, ASPER_ERR_IO,
                         "compact recovery: missing backup for %s", rel);
      else
        e = compact_copy_atomic(c, backup, target);
    } else {
      e = os_remove_file(target);
      if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
    }
    free(target);
    if (e != ASPER_OK) {
      free(backup);
      goto out;
    }
    if (backups_n == backups_cap) {
      size_t cap = backups_cap ? backups_cap * 2 : 8;
      char **grown = realloc(backups, cap * sizeof *grown);
      if (!grown) {
        free(backup);
        e = ASPER_ERR_NOMEM;
        goto out;
      }
      backups = grown;
      backups_cap = cap;
    }
    backups[backups_n++] = backup;
    line = nl ? nl + 1 : NULL;
  }
  e = os_remove_file(marker);
  if (e == ASPER_OK) {
    for (size_t i = 0; i < backups_n; i++)
      (void)os_remove_file(backups[i]);
    asper_log(c, ASPER_LOG_WARN, "store",
              "recovered an interrupted compaction");
  }
out:
  for (size_t i = 0; i < backups_n; i++) free(backups[i]);
  free(backups);
  free(text);
  free(marker);
  return e;
}

void asper_store_close(asper_ctx *c) {
  asper_store *st;
  if (!c) return;
  st = &c->store;
  if (st->journal_fp) {
    fclose(st->journal_fp);
    st->journal_fp = NULL;
  }
  if (st->audit_fp) {
    fclose(st->audit_fp);
    st->audit_fp = NULL;
  }
  asper_table_free(&st->table);
  for (size_t i = 0; i < st->projects_n; i++) free(st->projects[i]);
  free(st->projects);
  st->projects = NULL;
  st->projects_n = st->projects_cap = 0;
  free(st->root);           st->root = NULL;
  free(st->identity_path);  st->identity_path = NULL;
  free(st->context_path);   st->context_path = NULL;
  free(st->projects_dir);   st->projects_dir = NULL;
  free(st->journal_path);   st->journal_path = NULL;
  free(st->manifest_path);  st->manifest_path = NULL;
  free(st->cache_dir);      st->cache_dir = NULL;
  free(st->cache_path);     st->cache_path = NULL;
  free(st->audit_path);     st->audit_path = NULL;
  free(st->log_dir);        st->log_dir = NULL;
  st->journal_ops = 0;
}

asper_err asper_store_open(asper_ctx *c) {
  asper_store *st = &c->store;
  asper_err e;
  size_t replayed = 0;

  if (!st->root) {
    e = asper_seterr(c, ASPER_ERR_INVALID, "store: no root directory set");
    goto fail;
  }
  st->identity_path = os_path_join(st->root, "identity.xcdn");
  st->context_path = os_path_join(st->root, "context.xcdn");
  st->projects_dir = os_path_join(st->root, "projects");
  st->journal_path = os_path_join(st->root, "journal.xcdn");
  st->manifest_path = os_path_join(st->root, "manifest.xcdn");
  st->cache_dir = os_path_join(st->root, "cache");
  st->audit_path = os_path_join(st->root, "audit.xcdn");
  st->log_dir = asper_strdup(st->root);
  st->cache_path = st->cache_dir ? os_path_join(st->cache_dir, "embeddings.bin")
                                 : NULL;
  if (!st->identity_path || !st->context_path || !st->projects_dir ||
      !st->journal_path || !st->manifest_path || !st->cache_dir ||
      !st->audit_path || !st->log_dir || !st->cache_path) {
    e = asper_seterr(c, ASPER_ERR_NOMEM, "store: out of memory");
    goto fail;
  }

  if ((e = os_mkdir_p(st->root)) != ASPER_OK ||
      (e = os_mkdir_p(st->projects_dir)) != ASPER_OK ||
      (e = os_mkdir_p(st->cache_dir)) != ASPER_OK) {
    e = asper_seterr(c, e, "store: cannot create directories under %s",
                     st->root);
    goto fail;
  }

  if ((e = compact_recover(c)) != ASPER_OK) goto fail;

  if ((e = asper_manifest_load(c, &st->manifest)) != ASPER_OK) goto fail;

  if ((e = store_load_section_file(c, st->identity_path,
                                   ASPER_SECTION_IDENTITY, NULL)) != ASPER_OK)
    goto fail;
  if ((e = store_load_section_file(c, st->context_path, ASPER_SECTION_CONTEXT,
                                   NULL)) != ASPER_OK)
    goto fail;
  if ((e = store_load_projects(c)) != ASPER_OK) goto fail;

  if ((e = asper_journal_replay(c, st->journal_path, &replayed)) != ASPER_OK)
    goto fail;

  st->journal_fp = os_fopen(st->journal_path, "ab");
  if (!st->journal_fp) {
    e = asper_seterr(c, ASPER_ERR_IO, "store: cannot open journal %s",
                     st->journal_path);
    goto fail;
  }
  if (c->cfg.audit_log) {
    st->audit_fp = os_fopen(st->audit_path, "ab");
    if (!st->audit_fp)
      asper_log(c, ASPER_LOG_WARN, "store", "cannot open audit log %s",
                st->audit_path);
  }

  asper_log(c, ASPER_LOG_INFO, "store",
            "opened %s: %zu record(s), %zu project(s), %zu journal op(s) "
            "replayed",
            st->root, st->table.n, st->projects_n, replayed);
  return ASPER_OK;

fail:
  asper_store_close(c);
  return e;
}

/* ═══════════════════════ op application ═══════════════════════ */

static double boosted(double relevance) {
  double r = relevance + 0.05;
  return r > 1.0 ? 1.0 : r;
}

asper_err asper_store_apply(asper_ctx *c, asper_op *op) {
  asper_table *t = &c->store.table;
  asper_record *rec;
  asper_err e;

  if (!op) return asper_seterr(c, ASPER_ERR_INVALID, "apply: null op");

  switch (op->kind) {
    case ASPER_OP_INSERT:
      if (!op->record)
        return asper_seterr(c, ASPER_ERR_INVALID, "insert: missing record");
      op->record->emb_row = -1;
      e = asper_table_add(t, op->record);
      if (e == ASPER_ERR_NOMEM)
        return asper_seterr(c, e, "insert: out of memory");
      if (e != ASPER_OK)
        return asper_seterr(c, ASPER_ERR_INVALID, "insert: duplicate id %s",
                            op->record->id);
      op->record = NULL; /* table owns it now */
      return ASPER_OK;

    case ASPER_OP_UPDATE: {
      char *nc;
      if (!op->content)
        return asper_seterr(c, ASPER_ERR_INVALID, "update: missing content");
      rec = asper_table_get(t, op->id);
      if (!rec)
        return asper_seterr(c, ASPER_ERR_NOT_FOUND, "update: unknown id %s",
                            op->id);
      if (rec->deprecated)
        return asper_seterr(c, ASPER_ERR_INVALID,
                            "update: record %s is deprecated", op->id);
      nc = asper_strdup(op->content);
      if (!nc) return asper_seterr(c, ASPER_ERR_NOMEM, "update: out of memory");
      free(rec->content);
      rec->content = nc;
      rec->updated_at = op->at;
      return ASPER_OK;
    }

    case ASPER_OP_DEPRECATE:
      rec = asper_table_get(t, op->id);
      if (!rec)
        return asper_seterr(c, ASPER_ERR_NOT_FOUND,
                            "deprecate: unknown id %s", op->id);
      if (rec->deprecated) return ASPER_OK; /* idempotent */
      rec->deprecated = true;
      rec->deprecated_at = op->at;
      return ASPER_OK;

    case ASPER_OP_KEEP:
      rec = asper_table_get(t, op->id);
      if (!rec)
        return asper_seterr(c, ASPER_ERR_NOT_FOUND, "keep: unknown id %s",
                            op->id);
      if (rec->deprecated) {
        rec->deprecated = false;
        rec->deprecated_at = 0;
      }
      rec->last_access = op->at;
      rec->relevance = boosted(rec->relevance);
      return ASPER_OK;

    case ASPER_OP_SET_LOCKED:
      rec = asper_table_get(t, op->id);
      if (!rec)
        return asper_seterr(c, ASPER_ERR_NOT_FOUND,
                            "set_locked: unknown id %s", op->id);
      rec->locked = op->value;
      return ASPER_OK;

    case ASPER_OP_ACCESS:
      for (size_t i = 0; i < op->ids_n; i++) {
        rec = asper_table_get(t, op->ids[i]);
        if (!rec) continue; /* purged since: skip silently */
        rec->access_count++;
        rec->last_access = op->at;
        rec->relevance = boosted(rec->relevance);
      }
      return ASPER_OK;

    case ASPER_OP_PROJECT_CREATE:
      if (!op->project)
        return asper_seterr(c, ASPER_ERR_INVALID,
                            "project_create: missing slug");
      return asper_store_project_add(c, op->project);
  }
  return asper_seterr(c, ASPER_ERR_INVALID, "apply: unknown op kind");
}

/* ═══════════════════════ compaction ═══════════════════════ */

/* write <path>.tmp, fsync, atomically replace path. */
static asper_err store_write_atomic(asper_ctx *c, const char *path,
                                    const char *data, size_t len) {
  asper_err e = ASPER_OK;
  size_t plen = strlen(path);
  FILE *f;
  char *tmp = (char *)malloc(plen + 5);
  if (!tmp) return asper_seterr(c, ASPER_ERR_NOMEM, "store: out of memory");
  memcpy(tmp, path, plen);
  memcpy(tmp + plen, ".tmp", 5);

  f = os_fopen(tmp, "wb");
  if (!f) {
    free(tmp);
    return asper_seterr(c, ASPER_ERR_IO, "store: cannot write %s", path);
  }
  if (len > 0 && fwrite(data, 1, len, f) != len) e = ASPER_ERR_IO;
  if (e == ASPER_OK && fflush(f) != 0) e = ASPER_ERR_IO;
  if (e == ASPER_OK) e = os_fsync(f);
  if (fclose(f) != 0 && e == ASPER_OK) e = ASPER_ERR_IO;
  if (e == ASPER_OK) e = os_file_replace(tmp, path);
  if (e != ASPER_OK) {
    (void)os_remove_file(tmp);
    asper_seterr(c, e, "store: atomic replace of %s failed", path);
  }
  free(tmp);
  return e;
}

static asper_err compact_tx_begin(asper_ctx *c, char *const *rels, size_t n) {
  asper_buf marker;
  char *marker_path = NULL;
  asper_err e = ASPER_OK;
  asper_buf_init(&marker);
  for (size_t i = 0; e == ASPER_OK && i < n; i++) {
    char *target = os_path_join(c->store.root, rels[i]);
    char *backup = target ? compact_backup_path(target) : NULL;
    bool existed = target && os_file_exists(target);
    if (!target || !backup) {
      e = ASPER_ERR_NOMEM;
    } else if (existed) {
      e = compact_copy_atomic(c, target, backup);
    }
    if (e == ASPER_OK)
      e = asper_buf_printf(&marker, "%c %s\n", existed ? 'E' : 'M', rels[i]);
    free(target);
    free(backup);
  }
  marker_path = os_path_join(c->store.root, COMPACT_MARKER);
  if (e == ASPER_OK && !marker_path) e = ASPER_ERR_NOMEM;
  if (e == ASPER_OK)
    e = store_write_atomic(c, marker_path, marker.data, marker.len);
  if (e != ASPER_OK) {
    for (size_t i = 0; i < n; i++) {
      char *target = os_path_join(c->store.root, rels[i]);
      char *backup = target ? compact_backup_path(target) : NULL;
      if (backup) (void)os_remove_file(backup);
      free(target);
      free(backup);
    }
  }
  free(marker_path);
  asper_buf_free(&marker);
  return e;
}

static asper_err compact_tx_commit(asper_ctx *c, char *const *rels, size_t n) {
  char *marker = os_path_join(c->store.root, COMPACT_MARKER);
  asper_err e;
  if (!marker) return ASPER_ERR_NOMEM;
  e = os_remove_file(marker);
  free(marker);
  if (e != ASPER_OK) return e;
  for (size_t i = 0; i < n; i++) {
    char *target = os_path_join(c->store.root, rels[i]);
    char *backup = target ? compact_backup_path(target) : NULL;
    if (backup) (void)os_remove_file(backup);
    free(target);
    free(backup);
  }
  return ASPER_OK;
}

static bool purge_due(const asper_ctx *c, const asper_record *r,
                      asper_time now) {
  return r->deprecated && now - r->deprecated_at >= c->cfg.purge_after_s;
}

asper_err asper_store_compact(asper_ctx *c) {
  asper_store *st = &c->store;
  asper_err e = ASPER_OK;
  asper_time now;
  asper_buf ident, ctxb;
  asper_buf *pbufs = NULL;
  size_t np = 0;
  size_t written = 0, purged = 0;
  char **tx_rels = NULL;
  size_t tx_n = 0;
  size_t journal_ops_before;
  bool tx_active = false;

  asper_buf_init(&ident);
  asper_buf_init(&ctxb);

  os_rwlock_wrlock(&c->lock);
  os_mutex_lock(&c->journal_mu);
  now = asper_clock_now(&c->clock);
  journal_ops_before = st->journal_ops;

  /* Register any project slug reachable only through records so every
   * project gets its file (slugs persist across compaction). */
  for (size_t i = 0; e == ASPER_OK && i < st->table.n; i++) {
    asper_record *r = st->table.recs[i];
    if (r->section == ASPER_SECTION_PROJECT && r->project &&
        !asper_store_project_known(c, r->project))
      e = asper_store_project_add(c, r->project);
  }

  np = st->projects_n;

  /* Back up every file participating in the multi-file commit. The marker
   * is written last, so an interrupted setup is ignored; once present it is
   * sufficient for deterministic rollback on the next open. */
  if (e == ASPER_OK) {
    tx_n = np + 3;
    tx_rels = calloc(tx_n, sizeof *tx_rels);
    if (!tx_rels) {
      e = ASPER_ERR_NOMEM;
    } else {
      tx_rels[0] = asper_strdup("identity.xcdn");
      tx_rels[1] = asper_strdup("context.xcdn");
      tx_rels[2] = asper_strdup("journal.xcdn");
      for (size_t i = 0; i < np; i++) {
        asper_buf rel;
        asper_buf_init(&rel);
        if (asper_buf_printf(&rel, "projects/%s.xcdn", st->projects[i]) ==
            ASPER_OK)
          tx_rels[i + 3] = asper_buf_detach(&rel);
        asper_buf_free(&rel);
      }
      for (size_t i = 0; i < tx_n; i++)
        if (!tx_rels[i]) e = ASPER_ERR_NOMEM;
    }
  }
  if (e == ASPER_OK) {
    e = compact_tx_begin(c, tx_rels, tx_n);
    tx_active = (e == ASPER_OK);
  }

  if (e == ASPER_OK && np > 0) {
    pbufs = (asper_buf *)calloc(np, sizeof(asper_buf));
    if (!pbufs)
      e = asper_seterr(c, ASPER_ERR_NOMEM, "compact: out of memory");
    else
      for (size_t i = 0; i < np; i++) asper_buf_init(&pbufs[i]);
  }

  /* Serialize the table by destination file, skipping purge-due records. */
  for (size_t i = 0; e == ASPER_OK && i < st->table.n; i++) {
    asper_record *r = st->table.recs[i];
    asper_buf *b;
    if (purge_due(c, r, now)) continue;
    if (r->section == ASPER_SECTION_IDENTITY) {
      b = &ident;
    } else if (r->section == ASPER_SECTION_CONTEXT) {
      b = &ctxb;
    } else {
      bool found = false;
      size_t pi =
          r->project ? project_lower_bound(st, r->project, &found) : 0;
      if (!found) {
        e = asper_seterr(c, ASPER_ERR_INVALID,
                         "compact: record %s references unknown project",
                         r->id);
        break;
      }
      b = &pbufs[pi];
    }
    if (b->len > 0) e = asper_buf_appendc(b, '\n'); /* blank-line separator */
    if (e == ASPER_OK) e = asper_record_serialize(r, b);
    if (e == ASPER_OK) e = asper_buf_appendc(b, '\n');
    if (e == ASPER_OK) written++;
  }

  /* Atomic per-file replacement. */
  if (e == ASPER_OK)
    e = store_write_atomic(c, st->identity_path, ident.data ? ident.data : "",
                           ident.len);
  if (e == ASPER_OK)
    e = store_write_atomic(c, st->context_path, ctxb.data ? ctxb.data : "",
                           ctxb.len);
  for (size_t i = 0; e == ASPER_OK && i < np; i++) {
    asper_buf fname;
    char *ppath;
    asper_buf_init(&fname);
    e = asper_buf_printf(&fname, "%s.xcdn", st->projects[i]);
    if (e != ASPER_OK) {
      asper_buf_free(&fname);
      asper_seterr(c, e, "compact: out of memory");
      break;
    }
    ppath = os_path_join(st->projects_dir, fname.data);
    asper_buf_free(&fname);
    if (!ppath) {
      e = asper_seterr(c, ASPER_ERR_NOMEM, "compact: out of memory");
      break;
    }
    e = store_write_atomic(c, ppath, pbufs[i].data ? pbufs[i].data : "",
                           pbufs[i].len);
    free(ppath);
  }

  /* Reset the journal: the active transaction rolls all section files and
   * this journal back together if the process stops before commit. */
  if (e == ASPER_OK) {
    if (st->journal_fp) {
      fclose(st->journal_fp);
      st->journal_fp = NULL;
    }
    e = store_write_atomic(c, st->journal_path, "", 0);
    if (e == ASPER_OK) {
      st->journal_fp = os_fopen(st->journal_path, "ab");
      if (!st->journal_fp)
        e = asper_seterr(c, ASPER_ERR_IO, "compact: cannot reopen journal %s",
                         st->journal_path);
      else
        st->journal_ops = 0;
    } else if (!st->journal_fp) {
      /* keep the store appendable even when the reset failed */
      st->journal_fp = os_fopen(st->journal_path, "ab");
    }
  }

  if (e == ASPER_OK && tx_active) {
    e = compact_tx_commit(c, tx_rels, tx_n);
    if (e == ASPER_OK) tx_active = false;
  }
  if (e != ASPER_OK && tx_active) {
    asper_err recovery;
    if (st->journal_fp) {
      fclose(st->journal_fp);
      st->journal_fp = NULL;
    }
    recovery = compact_recover(c);
    st->journal_fp = os_fopen(st->journal_path, "ab");
    st->journal_ops = journal_ops_before;
    if (recovery != ASPER_OK)
      asper_log(c, ASPER_LOG_ERROR, "store",
                "compaction rollback failed: %s", asper_err_name(recovery));
  }

  /* Purge: drop purge-due records from table and index. */
  if (e == ASPER_OK) {
    for (size_t i = st->table.n; i-- > 0;) {
      asper_record *r = st->table.recs[i];
      if (!purge_due(c, r, now)) continue;
      if (r->emb_row >= 0) asper_index_remove(&c->index, r);
      (void)asper_table_remove(&st->table, r->id);
      purged++;
    }
  }

  if (e == ASPER_OK) {
    st->manifest.last_compaction = now;
    e = asper_manifest_save(c, &st->manifest);
    if (e != ASPER_OK)
      asper_log(c, ASPER_LOG_WARN, "store",
                "compaction: manifest save failed: %s", asper_last_error(c));
    else
      c->stats.last_compaction_at = (long long)now;
  }

  if (e == ASPER_OK)
    asper_log(c, ASPER_LOG_INFO, "store",
              "compaction: %zu record(s) written, %zu purged, journal reset",
              written, purged);

  asper_buf_free(&ident);
  asper_buf_free(&ctxb);
  if (pbufs) {
    for (size_t i = 0; i < np; i++) asper_buf_free(&pbufs[i]);
    free(pbufs);
  }
  if (tx_rels) {
    for (size_t i = 0; i < tx_n; i++) free(tx_rels[i]);
    free(tx_rels);
  }
  os_mutex_unlock(&c->journal_mu);
  os_rwlock_wrunlock(&c->lock);
  return e;
}
