/* Project registry, checked snapshot loading and operation projection. */
#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"
#include "xcdn.h"
#include "store_files.h"

/* ═══════════════════════ records ═══════════════════════ */

size_t asper_store_project_index(const asper_store *st, const char *slug,
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
  (void)asper_store_project_index(&c->store, slug, &found);
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
  pos = asper_store_project_index(st, slug, &found);
  if (found) return ASPER_OK;
  if (st->projects_n >= ASPER_PROJECT_LIMIT) return ASPER_ERR_LIMIT;
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
/* Snapshot corruption is an error; invalid records are never silently dropped. */
static asper_err store_load_section_file(asper_ctx *c, const char *path,
                                         asper_section want, const char *slug) {
  char *text = NULL; size_t len;
  asper_err e = asper_store_file_read(path,true,ASPER_SECTION_MAX,&text,&len);
  if (e == ASPER_ERR_NOT_FOUND) return ASPER_OK;
  if (e != ASPER_OK) return asper_seterr(c,e,"store: invalid snapshot %s",path);
  xcdn_document_t *doc = xcdn_parse_str(text,len,NULL);
  free(text);
  if (!doc) return ASPER_ERR_PARSE;
  for (size_t i = 0; e == ASPER_OK && i < doc->values_len; i++) {
    xcdn_node_t *node = doc->values[i];
    asper_record *rec = NULL;
    if (xcdn_node_tag_count(node) != 1 || !xcdn_node_has_tag(node,"memory")) e = ASPER_ERR_PARSE;
    else e = asper_record_from_node(c,node,&rec);
    if (e == ASPER_OK && (rec->section != want ||
        (want == ASPER_SECTION_PROJECT && (!rec->project || strcmp(rec->project,slug))))) e = ASPER_ERR_PARSE;
    if (e == ASPER_OK) e = asper_table_add(&c->store.table,rec);
    if (e != ASPER_OK) asper_record_free_one(rec);
  }
  xcdn_document_free(doc);
  return e == ASPER_ERR_INVALID ? ASPER_ERR_PARSE : e;
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

  if ((e = asper_compact_recover(c)) != ASPER_OK) goto fail;

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
  if (os_fsync(st->journal_fp) != ASPER_OK || os_sync_parent(st->journal_path) != ASPER_OK) {
    e = ASPER_ERR_IO; goto fail;
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
      /* A changed claim cannot inherit the previous claim's tool evidence. */
      long long expiry=rec->evidence.expires_at;
      memset(&rec->evidence,0,sizeof rec->evidence);
      rec->evidence.kind=op->declared_update ? ASPER_EVIDENCE_DECLARED : ASPER_EVIDENCE_INFERRED;
      rec->evidence.confidence=op->declared_update ? 1.0 : 0.5;
      rec->evidence.confidence_kind=ASPER_CONFIDENCE_HEURISTIC;
      rec->evidence.observed_at=op->at;
      rec->evidence.expires_at=op->declared_update ? 0 : expiry ? expiry : op->at+30*86400;
      snprintf(rec->evidence.provenance,sizeof rec->evidence.provenance,
               "%s",op->declared_update ? "user:manual-update" : "curator:unverified-update");
      for (size_t i=0;i<rec->source_refs_n;i++) free(rec->source_refs[i]);
      free(rec->source_refs);rec->source_refs=NULL;rec->source_refs_n=0;
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
