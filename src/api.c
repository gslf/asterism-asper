/*
 * api.c — public API surface, the apply() mutation funnel, open/close wiring.
 *
 * Implements data flow, concurrency, and the public C API, plus the
 * "Guardrail split", "asper_open sequence" and "build_prompt" sections of
 * docs/IMPLEMENTATION_NOTES.md.
 */

#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"

/* Defined in worker.c; asper_flush(1) uses them for the cycle slot. */
extern void asper_cycle_slot_acquire(asper_ctx *c);
extern void asper_cycle_slot_release(asper_ctx *c);

/* ---- version / errors --------------------------------------------------- */

#define ASPER_STR2_(x) #x
#define ASPER_STR_(x) ASPER_STR2_(x)

const char *asper_version(void) {
  return ASPER_STR_(ASPER_VERSION_MAJOR) "." ASPER_STR_(
      ASPER_VERSION_MINOR) "." ASPER_STR_(ASPER_VERSION_PATCH);
}

const char *asper_err_name(asper_err e) {
  switch (e) {
  case ASPER_OK: return "ASPER_OK";
  case ASPER_ERR_IO: return "ASPER_ERR_IO";
  case ASPER_ERR_PARSE: return "ASPER_ERR_PARSE";
  case ASPER_ERR_CONFIG: return "ASPER_ERR_CONFIG";
  case ASPER_ERR_MODEL: return "ASPER_ERR_MODEL";
  case ASPER_ERR_NOT_FOUND: return "ASPER_ERR_NOT_FOUND";
  case ASPER_ERR_LOCKED: return "ASPER_ERR_LOCKED";
  case ASPER_ERR_INVALID: return "ASPER_ERR_INVALID";
  case ASPER_ERR_BUSY: return "ASPER_ERR_BUSY";
  case ASPER_ERR_NOMEM: return "ASPER_ERR_NOMEM";
  default: return "ASPER_ERR_UNKNOWN";
  }
}

asper_err asper_seterr(asper_ctx *c, asper_err e, const char *fmt, ...) {
  va_list ap;
  if (!c || !fmt) return e;
  va_start(ap, fmt);
  os_mutex_lock(&c->err_mu);
  vsnprintf(c->err_buf, sizeof c->err_buf, fmt, ap);
  os_mutex_unlock(&c->err_mu);
  va_end(ap);
  return e;
}

const char *asper_last_error(const asper_ctx *c) {
  return c ? c->err_buf : "";
}

/* Curator-side pre-funnel rejection accounting: increments
 * stats.ops_rejected under the write lock. Non-static on purpose: curator.c
 * declares it `extern` and calls it for pre-funnel curator drops (bad
 * lines, caps, guardrails, quorum pendings) that never reach asper_apply_op,
 * so ops_applied/ops_rejected are incremented in exactly one module (this
 * one). */
void asper_count_reject(asper_ctx *c, const char *why);

void asper_count_reject(asper_ctx *c, const char *why) {
  if (!c) return;
  os_rwlock_wrlock(&c->lock);
  c->stats.ops_rejected++;
  os_rwlock_wrunlock(&c->lock);
  asper_log(c, ASPER_LOG_INFO, "curator", "op rejected: %s",
            why ? why : "guardrail");
}

/* Default host log callback: WARN and ERROR to stderr until replaced. The
 * documented single exception to "the library never writes to stderr". Lives
 * here because log.c exports no default-callback symbol. */
static void asper_default_log_cb(int level, const char *msg, void *ud) {
  (void)ud;
  if (level <= ASPER_LOG_WARN && msg) fprintf(stderr, "%s\n", msg);
}

/* ---- memory management -------------------------------------------------- */

void asper_free(void *p) { free(p); }

void asper_records_free(asper_record **r, size_t n) {
  size_t i;
  if (!r) return;
  for (i = 0; i < n; i++) asper_record_free_one(r[i]);
  free(r);
}

void asper_strings_free(char **s, size_t n) {
  size_t i;
  if (!s) return;
  for (i = 0; i < n; i++) free(s[i]);
  free(s);
}

/* ---- record accessors --------------------------------------------------- */

const char *asper_record_id(const asper_record *r) {
  return r ? r->id : NULL;
}

asper_section asper_record_section(const asper_record *r) {
  return r ? r->section : ASPER_SECTION_IDENTITY;
}

const char *asper_record_project(const asper_record *r) {
  return r ? r->project : NULL;
}

const char *asper_record_content(const asper_record *r) {
  return r ? r->content : NULL;
}

const char *asper_record_source(const asper_record *r) {
  return r ? asper_source_name(r->source) : NULL;
}

long long asper_record_created_at(const asper_record *r) {
  return r ? (long long)r->created_at : 0;
}

long long asper_record_updated_at(const asper_record *r) {
  return r ? (long long)r->updated_at : 0;
}

long long asper_record_last_access(const asper_record *r) {
  return r ? (long long)r->last_access : 0;
}

unsigned asper_record_access_count(const asper_record *r) {
  return r ? (unsigned)r->access_count : 0;
}

double asper_record_relevance(const asper_record *r) {
  return r ? r->relevance : 0.0;
}

int asper_record_locked(const asper_record *r) {
  return (r && r->locked) ? 1 : 0;
}

int asper_record_deprecated(const asper_record *r) {
  return (r && r->deprecated) ? 1 : 0;
}

const char *asper_record_supersedes(const asper_record *r) {
  return (r && r->supersedes[0] != '\0') ? r->supersedes : NULL;
}

size_t asper_record_tag_count(const asper_record *r) {
  return r ? r->tags_n : 0;
}

const char *asper_record_tag(const asper_record *r, size_t i) {
  return (r && i < r->tags_n) ? r->tags[i] : NULL;
}

size_t asper_record_source_ref_count(const asper_record *r) {
  return r ? r->source_refs_n : 0;
}

const char *asper_record_source_ref(const asper_record *r, size_t i) {
  return r && i < r->source_refs_n ? r->source_refs[i] : NULL;
}

double asper_record_score(const asper_record *r) {
  return r ? r->score : 0.0;
}

/* ---- logging ------------------------------------------------------------ */

void asper_set_logger(asper_ctx *c, asper_log_fn fn, void *userdata) {
  if (!c) return;
  os_mutex_lock(&c->log_mu);
  c->log_cb = fn;
  c->log_ud = fn ? userdata : NULL;
  os_mutex_unlock(&c->log_mu);
}

/* ---- the apply() funnel (notes "Guardrail split") ------------------------ */

static size_t identity_active_count(const asper_ctx *c) {
  size_t i, n = 0;
  for (i = 0; i < c->store.table.n; i++) {
    const asper_record *r = c->store.table.recs[i];
    if (r->section == ASPER_SECTION_IDENTITY && !r->deprecated) n++;
  }
  return n;
}

/* Passage-embed rec and (re)insert its index row. Used only by the uncommon
 * KEEP restore path; INSERT/UPDATE prepare their vector before taking the
 * global write lock. Failures degrade to WARN and remove stale rows. */
static void apply_index_embed(asper_ctx *c, asper_record *rec) {
  float *vec;
  asper_err e;
  if (!c->has_embedder || c->index.dim <= 0 || !rec || rec->deprecated)
    return;
  vec = malloc((size_t)c->index.dim * sizeof *vec);
  if (!vec) {
    asper_log(c, ASPER_LOG_WARN, "index", "embed skipped for %s: no memory",
              rec->id);
    return;
  }
  e = c->embedder.embed(c->embedder.ud, rec->content, 0, vec);
  if (e != ASPER_OK) {
    asper_log(c, ASPER_LOG_WARN, "embed", "embedding failed for %s: %s",
              rec->id, asper_err_name(e));
    if (rec->emb_row >= 0) {
      asper_index_remove(&c->index, rec);
      c->cache_dirty = true;
    }
  } else {
    e = asper_index_put(&c->index, rec, vec);
    if (e != ASPER_OK)
      asper_log(c, ASPER_LOG_WARN, "index", "index put failed for %s: %s",
                rec->id, asper_err_name(e));
    else
      c->cache_dirty = true;
  }
  free(vec);
}

/* Universal validation, notes "Guardrail split". Write lock held. Fills
 * *out_target for id-addressed ops and `why` with a human-readable reason
 * on rejection. Source-independent except for the locked guardrail on
 * KEEP, which applies only when from_curator (review can race a lock taken
 * between candidate collection and apply). */
static asper_err apply_validate(asper_ctx *c, const asper_op *op,
                                bool from_curator,
                                asper_record **out_target, char *why,
                                size_t why_sz) {
  asper_record *t;
  size_t content_chars;
  *out_target = NULL;

  switch (op->kind) {
  case ASPER_OP_INSERT: {
    const asper_record *r = op->record;
    if (!r) {
      snprintf(why, why_sz, "insert without record");
      return ASPER_ERR_INVALID;
    }
    if (!asper_uuid_valid(r->id)) {
      snprintf(why, why_sz, "invalid record id");
      return ASPER_ERR_INVALID;
    }
    if (r->section != ASPER_SECTION_IDENTITY &&
        r->section != ASPER_SECTION_CONTEXT &&
        r->section != ASPER_SECTION_PROJECT) {
      snprintf(why, why_sz, "invalid section");
      return ASPER_ERR_INVALID;
    }
    if (asper_str_blank(r->content)) {
      snprintf(why, why_sz, "empty content");
      return ASPER_ERR_INVALID;
    }
    if (!asper_utf8_count(r->content, &content_chars)) {
      snprintf(why, why_sz, "content is not valid UTF-8");
      return ASPER_ERR_INVALID;
    }
    if (content_chars > (size_t)c->cfg.content_max_chars) {
      snprintf(why, why_sz, "content exceeds %d chars",
               c->cfg.content_max_chars);
      return ASPER_ERR_INVALID;
    }
    if (r->section == ASPER_SECTION_PROJECT) {
      if (!r->project || !asper_slug_valid(r->project)) {
        snprintf(why, why_sz, "invalid project slug");
        return ASPER_ERR_INVALID;
      }
      if (!asper_store_project_known(c, r->project)) {
        snprintf(why, why_sz, "unknown project '%s'", r->project);
        return ASPER_ERR_NOT_FOUND;
      }
    } else if (r->project) {
      snprintf(why, why_sz, "project set on non-project record");
      return ASPER_ERR_INVALID;
    }
    if (r->section == ASPER_SECTION_IDENTITY &&
        identity_active_count(c) >= (size_t)c->cfg.identity_max_records) {
      snprintf(why, why_sz, "identity record cap reached (%d)",
               c->cfg.identity_max_records);
      return ASPER_ERR_INVALID;
    }
    if (asper_table_get(&c->store.table, r->id)) {
      snprintf(why, why_sz, "duplicate id %s", r->id);
      return ASPER_ERR_INVALID;
    }
    return ASPER_OK;
  }

  case ASPER_OP_UPDATE:
    if (asper_str_blank(op->content)) {
      snprintf(why, why_sz, "empty content");
      return ASPER_ERR_INVALID;
    }
    if (!asper_utf8_count(op->content, &content_chars)) {
      snprintf(why, why_sz, "content is not valid UTF-8");
      return ASPER_ERR_INVALID;
    }
    if (content_chars > (size_t)c->cfg.content_max_chars) {
      snprintf(why, why_sz, "content exceeds %d chars",
               c->cfg.content_max_chars);
      return ASPER_ERR_INVALID;
    }
    t = asper_table_get(&c->store.table, op->id);
    if (!t) {
      snprintf(why, why_sz, "unknown id %s", op->id);
      return ASPER_ERR_NOT_FOUND;
    }
    if (t->locked) {
      snprintf(why, why_sz, "record %s is locked", op->id);
      return ASPER_ERR_LOCKED;
    }
    if (t->deprecated) {
      snprintf(why, why_sz, "record %s is deprecated", op->id);
      return ASPER_ERR_INVALID;
    }
    *out_target = t;
    return ASPER_OK;

  case ASPER_OP_DEPRECATE:
    t = asper_table_get(&c->store.table, op->id);
    if (!t) {
      snprintf(why, why_sz, "unknown id %s", op->id);
      return ASPER_ERR_NOT_FOUND;
    }
    if (t->locked) {
      snprintf(why, why_sz, "record %s is locked", op->id);
      return ASPER_ERR_LOCKED;
    }
    *out_target = t;
    return ASPER_OK;

  case ASPER_OP_KEEP:
    t = asper_table_get(&c->store.table, op->id);
    if (!t) {
      snprintf(why, why_sz, "unknown id %s", op->id);
      return ASPER_ERR_NOT_FOUND;
    }
    /* The locked guardrail covers curator KEEP too. Host KEEP on
     * locked records stays allowed (rescue path), and journal replay goes
     * through asper_store_apply, not this funnel. */
    if (from_curator && t->locked) {
      snprintf(why, why_sz, "record %s is locked", op->id);
      return ASPER_ERR_LOCKED;
    }
    *out_target = t;
    return ASPER_OK;

  case ASPER_OP_SET_LOCKED:
    t = asper_table_get(&c->store.table, op->id);
    if (!t) {
      snprintf(why, why_sz, "unknown id %s", op->id);
      return ASPER_ERR_NOT_FOUND;
    }
    *out_target = t;
    return ASPER_OK;

  case ASPER_OP_ACCESS:
    if (op->ids_n > 0 && !op->ids) {
      snprintf(why, why_sz, "access op without ids");
      return ASPER_ERR_INVALID;
    }
    return ASPER_OK;

  case ASPER_OP_PROJECT_CREATE:
    if (!op->project || !asper_slug_valid(op->project)) {
      snprintf(why, why_sz, "invalid project slug");
      return ASPER_ERR_INVALID;
    }
    return ASPER_OK;

  default:
    snprintf(why, why_sz, "unknown op kind");
    return ASPER_ERR_INVALID;
  }
}

asper_err asper_apply_op(asper_ctx *c, asper_op *op, bool from_curator) {
  char why[160];
  char idbuf[37];
  asper_record *target = NULL;
  const char *kname;
  asper_err e;
  asper_err embed_e = ASPER_OK;
  float *prepared_vec = NULL;
  const char *embed_text = NULL;
  size_t access_n = 0;

  if (!c || !op) return ASPER_ERR_INVALID;
  kname = asper_op_kind_name(op->kind);
  if (op->at == 0) op->at = asper_clock_now(&c->clock);
  why[0] = '\0';
  idbuf[0] = '\0';

  /* Embedding is the slowest mutation step. Prepare it without c->lock so
   * retrieval and unrelated writes are not stalled by model inference. The
   * authoritative validation is still repeated below while holding the lock. */
  if (op->kind == ASPER_OP_INSERT && op->record)
    embed_text = op->record->content;
  else if (op->kind == ASPER_OP_UPDATE)
    embed_text = op->content;
  if (embed_text && c->has_embedder && c->index.dim > 0) {
    size_t chars;
    if (!asper_str_blank(embed_text) && asper_utf8_count(embed_text, &chars) &&
        chars <= (size_t)c->cfg.content_max_chars) {
      prepared_vec = malloc((size_t)c->index.dim * sizeof *prepared_vec);
      if (!prepared_vec)
        embed_e = ASPER_ERR_NOMEM;
      else
        embed_e = c->embedder.embed(c->embedder.ud, embed_text, 0,
                                    prepared_vec);
    }
  }

  os_rwlock_wrlock(&c->lock);
  e = apply_validate(c, op, from_curator, &target, why, sizeof why);
  if (e != ASPER_OK) {
    c->stats.ops_rejected++;
    os_rwlock_wrunlock(&c->lock);
    free(prepared_vec);
    asper_log(c, ASPER_LOG_INFO, "store", "%s rejected: %s", kname, why);
    return asper_seterr(c, e, "%s rejected: %s", kname, why);
  }

  /* Appended to the journal first, then applied to memory. */
  e = asper_journal_append(c, op, !from_curator);
  if (e != ASPER_OK) {
    os_rwlock_wrunlock(&c->lock);
    free(prepared_vec);
    return asper_seterr(c, e, "journal append failed for %s", kname);
  }

  {
    asper_record *ins = op->record;
    if (op->kind == ASPER_OP_INSERT && ins) ins->emb_row = -1;
    e = asper_store_apply(c, op);
    if (e != ASPER_OK) {
      /* Journal-present but skipped in memory: same tolerance as replay. */
      c->stats.ops_rejected++;
      os_rwlock_wrunlock(&c->lock);
      free(prepared_vec);
      asper_log(c, ASPER_LOG_WARN, "store", "%s journaled but not applied: %s",
                kname, asper_err_name(e));
      return asper_seterr(c, e, "%s failed to apply", kname);
    }
    if (op->kind == ASPER_OP_INSERT) {
      op->record = NULL; /* the table owns it now */
      target = ins;
    }
  }

  switch (op->kind) {
  case ASPER_OP_INSERT:
  case ASPER_OP_UPDATE:
    if (prepared_vec && embed_e == ASPER_OK) {
      e = asper_index_put(&c->index, target, prepared_vec);
      if (e != ASPER_OK)
        asper_log(c, ASPER_LOG_WARN, "index", "index put failed for %s: %s",
                  target->id, asper_err_name(e));
      else
        c->cache_dirty = true;
    } else if (target && target->emb_row >= 0) {
      /* An UPDATE must never retain the vector for its old content. */
      asper_index_remove(&c->index, target);
      c->cache_dirty = true;
    }
    break;
  case ASPER_OP_KEEP:
    /* Restore path: a rescued record lost its row at deprecation. */
    if (target && target->emb_row < 0) apply_index_embed(c, target);
    break;
  case ASPER_OP_DEPRECATE:
    if (target && target->emb_row >= 0) {
      asper_index_remove(&c->index, target);
      c->cache_dirty = true;
    }
    break;
  default:
    break;
  }

  c->stats.ops_applied++;
  if (target) memcpy(idbuf, target->id, sizeof idbuf);
  access_n = op->ids_n;
  os_rwlock_wrunlock(&c->lock);
  if (embed_text && embed_e != ASPER_OK)
    asper_log(c, ASPER_LOG_WARN, "embed", "embedding failed for %s: %s",
              idbuf, asper_err_name(embed_e));
  free(prepared_vec);

  if (op->kind == ASPER_OP_ACCESS)
    asper_log(c, ASPER_LOG_DEBUG, "store", "access batch applied: %zu id(s)",
              access_n);
  else if (op->kind == ASPER_OP_PROJECT_CREATE)
    asper_log(c, ASPER_LOG_INFO, "store", "project '%s' created",
              op->project);
  else
    asper_log(c, ASPER_LOG_INFO, "store", "%s %s applied", kname, idbuf);
  return ASPER_OK;
}

/* ---- open / close ------------------------------------------------------- */

/* Frees everything a (possibly partial) context owns. `store_opened` tells
 * whether asper_store_open succeeded: before that only store.root is ours to
 * free (asper_store_open is assumed to clean up after its own failure). */
static void ctx_destroy(asper_ctx *c, bool store_opened) {
  size_t i;
  if (c->has_embedder && c->embedder.destroy)
    c->embedder.destroy(c->embedder.ud);
  if (c->has_curator && c->curator.destroy)
    c->curator.destroy(c->curator.ud);
  asper_models_shutdown(c);
  asper_index_free(&c->index);
  if (store_opened) {
    asper_store_close(c);
  } else {
    free(c->store.root);
    c->store.root = NULL;
  }
  for (i = 0; i < c->turns_n; i++) free(c->turns[i].text);
  free(c->turns);
  free(c->access_batch);
  free(c->pending);
  free(c->active_project);
  asper_log_close(c);
  asper_config_free(&c->cfg);
  os_cond_destroy(&c->done_cv);
  os_cond_destroy(&c->ev_cv);
  os_mutex_destroy(&c->ev_mu);
  os_mutex_destroy(&c->log_mu);
  os_mutex_destroy(&c->err_mu);
  os_mutex_destroy(&c->source_mu);
  os_mutex_destroy(&c->cache_mu);
  os_mutex_destroy(&c->journal_mu);
  os_rwlock_destroy(&c->lock);
  free(c);
}

/* Join a relative model path onto base (NULL base or an absolute path —
 * leading '/', or a Windows drive prefix — leaves *slot untouched). */
static asper_err cfg_base_join(const char *base, char **slot) {
  char *joined;
  const char *s = *slot;
  if (base == NULL || s == NULL || s[0] == '\0' || os_path_is_abs(s))
    return ASPER_OK;
  joined = os_path_join(base, s);
  if (joined == NULL) return ASPER_ERR_NOMEM;
  free(*slot);
  *slot = joined;
  return ASPER_OK;
}

static asper_err asper_open_impl(const asper_open_params *p,
                                 const char *base_dir,
                                 const asper_model_binding *models,
                                 const asper_embedder *emb,
                                 const asper_curator_iface *cur,
                                 const asper_clock *clk, asper_ctx **out) {
  asper_ctx *c;
  asper_err e;
  bool store_opened = false;
  int dim;
  asper_embedder managed_emb;
  asper_curator_iface managed_cur;
  bool managed_attempted = false;

  if (!out) return ASPER_ERR_INVALID;
  *out = NULL;
  if (!p || !p->memory_root || p->memory_root[0] == '\0')
    return ASPER_ERR_INVALID;

  c = calloc(1, sizeof *c);
  if (!c) return ASPER_ERR_NOMEM;

  /* 1. Locks first: every later failure path may destroy them. */
  os_rwlock_init(&c->lock);
  os_mutex_init(&c->journal_mu);
  os_mutex_init(&c->cache_mu);
  os_mutex_init(&c->source_mu);
  os_mutex_init(&c->err_mu);
  os_mutex_init(&c->ev_mu);
  os_mutex_init(&c->log_mu);
  os_cond_init(&c->ev_cv);
  os_cond_init(&c->done_cv);

  asper_config_defaults(&c->cfg);
  e = asper_config_load(c, &c->cfg, p->config_path);
  if (e != ASPER_OK) goto fail;
  /* Relative model paths resolve against the host-provided base dir
   * (embedding hosts pass their engine root); NULL keeps the legacy
   * cwd-relative behavior for the standalone CLI. */
  e = cfg_base_join(base_dir, &c->cfg.curator_model_path);
  if (e == ASPER_OK) e = cfg_base_join(base_dir, &c->cfg.embed_model_path);
  if (e != ASPER_OK) goto fail;

  /* 2. Logging: file sink per config + default stderr WARN+ callback. */
  e = asper_log_open(c);
  if (e != ASPER_OK) goto fail;
  c->log_cb = asper_default_log_cb;
  c->log_ud = NULL;

  /* 3. Clock. */
  c->clock = clk ? *clk : asper_clock_system();

  /* 4. Store: directories, manifest, section files, journal replay. */
  c->store.root = asper_strdup(p->memory_root);
  if (!c->store.root) {
    e = ASPER_ERR_NOMEM;
    goto fail;
  }
  e = asper_store_open(c);
  if (e != ASPER_OK) goto fail;
  store_opened = true;

  memset(&managed_emb, 0, sizeof managed_emb);
  memset(&managed_cur, 0, sizeof managed_cur);
  if (!emb && !cur) {
    managed_attempted = true;
    e = asper_models_bind(c, models, &managed_emb, &managed_cur);
    if (e != ASPER_OK) goto fail;
    emb = managed_emb.embed ? &managed_emb : NULL;
    cur = managed_cur.generate ? &managed_cur : NULL;
  }

  /* 5. Backends: provided vtables win; else llama when available. Missing
   * models never fail the open — the context runs degraded with a WARN. */
  if (emb) {
    c->embedder = *emb;
    c->has_embedder = true;
  } else if (!managed_attempted) {
    asper_embedder eb;
    asper_err be;
    memset(&eb, 0, sizeof eb);
    be = asper_embedder_llama_create(c, &eb);
    if (be == ASPER_OK) {
      c->embedder = eb;
      c->has_embedder = true;
    } else {
      asper_log(c, ASPER_LOG_WARN, "embed",
                "embedder unavailable (%s): retrieval degraded",
                asper_err_name(be));
    }
  }
  if (c->has_embedder && c->embedder.dim <= 0) {
    asper_log(c, ASPER_LOG_WARN, "embed",
              "embedder reports invalid dim %d: disabled", c->embedder.dim);
    if (c->embedder.destroy) c->embedder.destroy(c->embedder.ud);
    memset(&c->embedder, 0, sizeof c->embedder);
    c->has_embedder = false;
  }
  if (cur) {
    c->curator = *cur;
    c->has_curator = true;
  } else if (!managed_attempted) {
    asper_curator_iface ci;
    asper_err ce;
    memset(&ci, 0, sizeof ci);
    ce = asper_curator_llama_create(c, &ci);
    if (ce == ASPER_OK) {
      c->curator = ci;
      c->has_curator = true;
    } else {
      asper_log(c, ASPER_LOG_WARN, "curator",
                "curator unavailable (%s): curation degraded",
                asper_err_name(ce));
    }
  }

  /* 6. Embedding reconciliation + index + cache. */
  dim = c->has_embedder ? c->embedder.dim
                        : (c->cfg.embed_dim > 0 ? c->cfg.embed_dim : 384);
  if (c->has_embedder) {
    asper_manifest *m = &c->store.manifest;
    bool mismatch =
        m->embed_dim != c->embedder.dim || !m->has_model_hash ||
        memcmp(m->embed_model_hash, c->embedder.model_hash, 32) != 0 ||
        strncmp(m->embed_model_id, c->embedder.model_id,
                sizeof m->embed_model_id) != 0;
    if (mismatch) {
      if (m->has_model_hash)
        asper_log(c, ASPER_LOG_WARN, "embed",
                  "embedding model changed: cache will be rebuilt");
      snprintf(m->embed_model_id, sizeof m->embed_model_id, "%s",
               c->embedder.model_id);
      m->embed_dim = c->embedder.dim;
      memcpy(m->embed_model_hash, c->embedder.model_hash, 32);
      m->has_model_hash = true;
      e = asper_manifest_save(c, m);
      if (e != ASPER_OK) goto fail;
    }
  }
  asper_index_init(&c->index, dim);
  if (c->has_embedder) {
    size_t *stale = NULL, stale_n = 0;
    e = asper_cache_load(c, &stale, &stale_n);
    if (e != ASPER_OK) {
      free(stale);
      goto fail;
    }
    if (stale_n > 0) {
      float *vec = malloc((size_t)dim * sizeof *vec);
      size_t i, done = 0;
      if (!vec) {
        free(stale);
        e = ASPER_ERR_NOMEM;
        goto fail;
      }
      for (i = 0; i < stale_n; i++) {
        asper_record *r;
        asper_err ee;
        if (stale[i] >= c->store.table.n) continue;
        r = c->store.table.recs[stale[i]];
        if (!r || r->deprecated) continue;
        ee = c->embedder.embed(c->embedder.ud, r->content, 0, vec);
        if (ee != ASPER_OK) {
          asper_log(c, ASPER_LOG_WARN, "embed", "re-embed failed for %s: %s",
                    r->id, asper_err_name(ee));
          continue;
        }
        ee = asper_index_put(&c->index, r, vec);
        if (ee != ASPER_OK) {
          asper_log(c, ASPER_LOG_WARN, "index", "index put failed for %s: %s",
                    r->id, asper_err_name(ee));
          continue;
        }
        done++;
        if (done % 256 == 0)
          asper_log(c, ASPER_LOG_INFO, "embed", "re-embedded %zu/%zu records",
                    done, stale_n);
      }
      free(vec);
      if (done > 0) {
        asper_log(c, ASPER_LOG_INFO, "embed",
                  "re-embedded %zu record(s) at load", done);
        if (asper_cache_save(c) == ASPER_OK)
          c->cache_dirty = false;
        else {
          c->cache_dirty = true;
          asper_log(c, ASPER_LOG_WARN, "embed",
                    "embedding cache save failed; will retry later");
        }
      }
    }
    free(stale);
  }

  /* 7. Restore semantic work from the durable exact source before the
   * worker starts. A hard crash can lose RAM, never the curation input. */
  e = asper_source_replay_pending(c);
  if (e != ASPER_OK) goto fail;

  /* 8. Worker. */
  e = asper_worker_start(c);
  if (e != ASPER_OK) goto fail;

  asper_log(c, ASPER_LOG_INFO, "store", "opened %s: %zu records, %zu projects",
            c->store.root, c->store.table.n, c->store.projects_n);
  *out = c;
  return ASPER_OK;

fail:
  /* 8. Full cleanup; the code alone is returned (ctx no longer exists).
   * Ownership of provided vtable userdata transferred on install, so their
   * destroy hooks run here too. */
  ctx_destroy(c, store_opened);
  return e;
}

asper_err asper_open_with(const asper_open_params *p,
                          const asper_embedder *emb,
                          const asper_curator_iface *cur,
                          const asper_clock *clk, asper_ctx **out) {
  return asper_open_impl(p, NULL, NULL, emb, cur, clk, out);
}

asper_err asper_open(const asper_open_params *p, asper_ctx **out) {
  return asper_open_impl(p, NULL, NULL, NULL, NULL, NULL, out);
}

asper_err asper_open_at(const asper_open_params *p, const char *base_dir,
                        asper_ctx **out) {
  return asper_open_impl(p, base_dir, NULL, NULL, NULL, NULL, out);
}

asper_err asper_open_at_with_models(const asper_open_params *p,
                                    const char *base_dir,
                                    const asper_model_binding *models,
                                    asper_ctx **out) {
  if (!models || !models->manager) return ASPER_ERR_INVALID;
  return asper_open_impl(p, base_dir, models, NULL, NULL, NULL, out);
}

void asper_close(asper_ctx *c) {
  if (!c) return;
  asper_worker_stop(c);
  /* Final durability pass. Close never runs curation: turns still queued are
   * discarded (the journal only ever holds applied ops). */
  (void)asper_access_flush(c);
  (void)asper_journal_sync(c);
  (void)asper_store_compact(c);
  if (c->has_embedder && asper_cache_needs_save(c))
    (void)asper_cache_save(c);
  asper_log(c, ASPER_LOG_INFO, "store", "context closed");
  ctx_destroy(c, true);
}

/* ---- hot path ----------------------------------------------------------- */

asper_err asper_enqueue_turn(asper_ctx *c, asper_role role,
                             const char *text_utf8, asper_time now,
                             const char *source_id) {
  char *copy;
  if (!c) return ASPER_ERR_INVALID;
  if (role != ASPER_ROLE_USER && role != ASPER_ROLE_ASSISTANT)
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid role");
  if (asper_str_blank(text_utf8))
    return asper_seterr(c, ASPER_ERR_INVALID, "empty turn text");
  if (!asper_utf8_count(text_utf8, NULL))
    return asper_seterr(c, ASPER_ERR_INVALID, "turn text is not valid UTF-8");
  if (!asper_uuid_valid(source_id))
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid source event id");

  copy = asper_strdup(text_utf8);
  if (!copy) return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");

  os_mutex_lock(&c->ev_mu);
  if (c->turns_n == c->turns_cap) {
    size_t ncap = c->turns_cap ? c->turns_cap * 2 : 16;
    asper_turn *nt = realloc(c->turns, ncap * sizeof *nt);
    if (!nt) {
      os_mutex_unlock(&c->ev_mu);
      free(copy);
      return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
    }
    c->turns = nt;
    c->turns_cap = ncap;
  }
  c->turns[c->turns_n].role = role;
  c->turns[c->turns_n].text = copy;
  c->turns[c->turns_n].at = now;
  memcpy(c->turns[c->turns_n].source_id, source_id, 37);
  c->turns_n++;
  c->last_turn_at = now;
  /* Wake the worker on EVERY enqueued turn so the idle-flush deadline
   * arms even for a partial batch; turn_batch only decides when a cycle is
   * due, not when the worker wakes (notes "Post-review amendments"). */
  if (!c->no_threads && c->worker_running)
    os_cond_signal(&c->ev_cv);
  os_mutex_unlock(&c->ev_mu);
  return ASPER_OK;
}

asper_err asper_memory_render(asper_ctx *c, const char *base_system_prompt,
                              const char *user_message, char **out_prompt) {
  asper_record **idr = NULL, **ctxr = NULL, **prr = NULL;
  size_t idn = 0, ctxn = 0, prn = 0, i;
  char *proj = NULL;
  bool had_proj;
  asper_err e;

  if (!c) return ASPER_ERR_INVALID;
  if (!out_prompt)
    return asper_seterr(c, ASPER_ERR_INVALID, "out_prompt is NULL");
  *out_prompt = NULL;
  if (!user_message)
    return asper_seterr(c, ASPER_ERR_INVALID, "user_message is NULL");
  if (!asper_utf8_count(user_message, NULL) ||
      (base_system_prompt && !asper_utf8_count(base_system_prompt, NULL)))
    return asper_seterr(c, ASPER_ERR_INVALID,
                        "prompt input is not valid UTF-8");

  /* Active project snapshot, taken once and used consistently. */
  os_rwlock_rdlock(&c->lock);
  had_proj = c->active_project != NULL;
  proj = asper_strdup(c->active_project);
  os_rwlock_rdunlock(&c->lock);
  if (had_proj && !proj)
    return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");

  e = asper_collect_identity(c, &idr, &idn);
  if (e != ASPER_OK) goto out;
  e = asper_retrieve(c, user_message, ASPER_SECTION_CONTEXT, NULL,
                     (size_t)(c->cfg.k_context > 0 ? c->cfg.k_context : 6),
                     c->cfg.min_similarity, &ctxr, &ctxn);
  if (e != ASPER_OK) goto out;
  if (proj) {
    e = asper_retrieve(c, user_message, ASPER_SECTION_PROJECT, proj,
                       (size_t)(c->cfg.k_project > 0 ? c->cfg.k_project : 8),
                       c->cfg.min_similarity, &prr, &prn);
    if (e != ASPER_OK) goto out;
  }

  e = asper_inject_render(c, base_system_prompt ? base_system_prompt : "",
                          idr, idn, ctxr, ctxn, prr, prn, proj, out_prompt);
  if (e != ASPER_OK) goto out;

  /* Access notes for the records that survived budget trimming: inject.c
   * marks dropped clones with score == -1 (IMPLEMENTATION_NOTES). */
  for (i = 0; i < idn; i++)
    if (idr[i]->score != -1.0) asper_access_note(c, idr[i]->id);
  for (i = 0; i < ctxn; i++)
    if (ctxr[i]->score != -1.0) asper_access_note(c, ctxr[i]->id);
  for (i = 0; i < prn; i++)
    if (prr[i]->score != -1.0) asper_access_note(c, prr[i]->id);

out:
  asper_records_free(idr, idn);
  asper_records_free(ctxr, ctxn);
  asper_records_free(prr, prn);
  free(proj);
  return e;
}

/* ---- recall -------------------------------------------------------------- */

asper_err asper_recall(asper_ctx *c, const char *question, char **out_answer,
                       asper_record ***out_cited, size_t *out_cited_n) {
  asper_err e;
  int64_t timeout_ms;
  int64_t deadline;

  if (!c) return ASPER_ERR_INVALID;
  if (out_answer) *out_answer = NULL;
  if (out_cited) *out_cited = NULL;
  if (out_cited_n) *out_cited_n = 0;
  if (!out_answer)
    return asper_seterr(c, ASPER_ERR_INVALID, "out_answer is required");
  if (asper_str_blank(question))
    return asper_seterr(c, ASPER_ERR_INVALID, "empty recall question");
  if (!asper_utf8_count(question, NULL))
    return asper_seterr(c, ASPER_ERR_INVALID,
                        "recall question is not valid UTF-8");
  if (!c->has_curator)
    return asper_seterr(c, ASPER_ERR_MODEL,
                        "recall unavailable: no curator model");

  timeout_ms = c->cfg.recall_timeout_s > 0
                   ? (int64_t)c->cfg.recall_timeout_s * 1000
                   : 0;
  deadline = timeout_ms > 0 ? os_monotonic_ms() + timeout_ms : 0;

  if (c->no_threads) {
    e = asper_recall_run(c, question, deadline, out_answer, out_cited,
                         out_cited_n);
    if (e == ASPER_ERR_BUSY)
      return asper_seterr(c, e, "recall timed out after %lld s",
                          (long long)c->cfg.recall_timeout_s);
    return e;
  }

  /* Acquire the cycle slot with timeout (protocol in asper_internal.h). */
  {
    os_mutex_lock(&c->ev_mu);
    c->recall_waiting++;
    while (c->cycle_busy) {
      int64_t rem = deadline - os_monotonic_ms();
      if (rem <= 0) {
        c->recall_waiting--;
        os_cond_signal(&c->ev_cv); /* let the worker resume */
        os_mutex_unlock(&c->ev_mu);
        return asper_seterr(c, ASPER_ERR_BUSY, "recall timed out after %lld s",
                            (long long)c->cfg.recall_timeout_s);
      }
      (void)os_cond_timedwait(&c->done_cv, &c->ev_mu, rem);
    }
    c->cycle_busy = true;
    c->recall_waiting--;
    os_mutex_unlock(&c->ev_mu);
  }

  /* The curator generation runs on the CALLER thread holding the slot. */
  e = asper_recall_run(c, question, deadline, out_answer, out_cited,
                       out_cited_n);

  os_mutex_lock(&c->ev_mu);
  c->cycle_busy = false;
  os_cond_broadcast(&c->done_cv);
  os_cond_signal(&c->ev_cv);
  os_mutex_unlock(&c->ev_mu);
  if (e == ASPER_ERR_BUSY)
    return asper_seterr(c, e, "recall timed out after %lld s",
                        (long long)c->cfg.recall_timeout_s);
  return e;
}

/* ---- projects ----------------------------------------------------------- */

asper_err asper_project_select(asper_ctx *c, const char *slug) {
  bool known;
  char *dup;
  asper_err e;

  if (!c) return ASPER_ERR_INVALID;

  if (!slug) {
    os_rwlock_wrlock(&c->lock);
    free(c->active_project);
    c->active_project = NULL;
    os_rwlock_wrunlock(&c->lock);
    return ASPER_OK;
  }
  if (!asper_slug_valid(slug))
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid project slug '%s'",
                        slug);

  os_rwlock_rdlock(&c->lock);
  known = asper_store_project_known(c, slug);
  os_rwlock_rdunlock(&c->lock);

  if (!known) {
    if (!c->cfg.autocreate)
      return asper_seterr(c, ASPER_ERR_NOT_FOUND, "unknown project '%s'",
                          slug);
    {
      asper_op op;
      memset(&op, 0, sizeof op);
      op.kind = ASPER_OP_PROJECT_CREATE;
      op.at = asper_clock_now(&c->clock);
      op.project = asper_strdup(slug);
      if (!op.project)
        return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
      e = asper_apply_op(c, &op, false);
      asper_op_free(&op);
      if (e != ASPER_OK) return e;
    }
  }

  dup = asper_strdup(slug);
  if (!dup) return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
  os_rwlock_wrlock(&c->lock);
  free(c->active_project);
  c->active_project = dup;
  os_rwlock_wrunlock(&c->lock);
  return ASPER_OK;
}

asper_err asper_project_list(asper_ctx *c, char ***out_slugs, size_t *out_n) {
  char **arr = NULL;
  size_t n, i;

  if (!c) return ASPER_ERR_INVALID;
  if (!out_slugs || !out_n)
    return asper_seterr(c, ASPER_ERR_INVALID, "NULL output argument");
  *out_slugs = NULL;
  *out_n = 0;

  os_rwlock_rdlock(&c->lock);
  n = c->store.projects_n;
  if (n > 0) {
    arr = calloc(n, sizeof *arr);
    if (!arr) {
      os_rwlock_rdunlock(&c->lock);
      return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n; i++) {
      arr[i] = asper_strdup(c->store.projects[i]);
      if (!arr[i]) {
        os_rwlock_rdunlock(&c->lock);
        asper_strings_free(arr, i);
        return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
      }
    }
  }
  os_rwlock_rdunlock(&c->lock);

  *out_slugs = arr;
  *out_n = n;
  return ASPER_OK;
}

asper_err asper_project_active(asper_ctx *c, char **out_slug) {
  bool had;
  char *dup;

  if (!c) return ASPER_ERR_INVALID;
  if (!out_slug)
    return asper_seterr(c, ASPER_ERR_INVALID, "NULL output argument");
  *out_slug = NULL;

  os_rwlock_rdlock(&c->lock);
  had = c->active_project != NULL;
  dup = asper_strdup(c->active_project);
  os_rwlock_rdunlock(&c->lock);
  if (had && !dup) return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
  *out_slug = dup;
  return ASPER_OK;
}

/* ---- direct memory access ----------------------------------------------- */

asper_err asper_memory_insert(asper_ctx *c, asper_section s,
                              const char *project, const char *content,
                              int locked, char out_id[37]) {
  asper_record *r;
  asper_op op;
  asper_err e;
  char *slug = NULL;
  char idbuf[37];
  asper_time now;

  if (!c) return ASPER_ERR_INVALID;
  if (s != ASPER_SECTION_IDENTITY && s != ASPER_SECTION_CONTEXT &&
      s != ASPER_SECTION_PROJECT)
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid section for insert");
  if (asper_str_blank(content))
    return asper_seterr(c, ASPER_ERR_INVALID, "empty content");

  if (s == ASPER_SECTION_PROJECT) {
    if (project) {
      slug = asper_strdup(project);
      if (!slug) return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
    } else {
      bool had;
      os_rwlock_rdlock(&c->lock);
      had = c->active_project != NULL;
      slug = asper_strdup(c->active_project);
      os_rwlock_rdunlock(&c->lock);
      if (had && !slug)
        return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
      if (!slug)
        return asper_seterr(c, ASPER_ERR_INVALID,
                            "project insert with no project and none active");
    }
    /* Existence + slug validity are (re)checked inside the funnel under the
     * write lock; unknown projects reject with ASPER_ERR_NOT_FOUND —
     * autocreation applies only to asper_project_select. */
  }

  now = asper_clock_now(&c->clock);
  r = asper_record_new();
  if (!r) {
    free(slug);
    return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
  }
  asper_uuid_v4(r->id);
  memcpy(idbuf, r->id, sizeof idbuf);
  r->section = s;
  r->project = slug; /* ownership moves to the record */
  r->content = asper_strdup(content);
  if (!r->content) {
    asper_record_free_one(r);
    return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
  }
  r->source = ASPER_SRC_MANUAL; /* the API path is always "manual"; seed
                                   records enter via hand-edited files only */
  r->created_at = now;
  r->updated_at = now;
  r->last_access = now;
  r->access_count = 0;
  r->relevance = 0.80;
  r->locked = locked != 0;
  r->deprecated = false;
  r->deprecated_at = 0;
  r->supersedes[0] = '\0';
  r->score = 0.0;
  r->emb_row = -1;

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_INSERT;
  op.at = now;
  op.record = r;
  e = asper_apply_op(c, &op, false);
  asper_op_free(&op); /* frees the record only when apply did not take it */
  if (e == ASPER_OK && out_id) memcpy(out_id, idbuf, sizeof idbuf);
  return e;
}

asper_err asper_memory_update(asper_ctx *c, const char *id,
                              const char *content) {
  asper_op op;
  asper_err e;

  if (!c) return ASPER_ERR_INVALID;
  if (!id || !asper_uuid_valid(id))
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid record id");
  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_UPDATE;
  op.at = asper_clock_now(&c->clock);
  memcpy(op.id, id, 37);
  op.content = asper_strdup(content);
  if (content && !op.content)
    return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
  e = asper_apply_op(c, &op, false);
  asper_op_free(&op);
  return e;
}

asper_err asper_memory_deprecate(asper_ctx *c, const char *id,
                                 const char *reason) {
  asper_op op;
  asper_err e;

  if (!c) return ASPER_ERR_INVALID;
  if (!id || !asper_uuid_valid(id))
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid record id");
  if (reason && !asper_utf8_count(reason, NULL))
    return asper_seterr(c, ASPER_ERR_INVALID,
                        "deprecation reason is not valid UTF-8");

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_DEPRECATE;
  op.at = asper_clock_now(&c->clock);
  memcpy(op.id, id, 37);
  op.reason = asper_strdup(reason); /* NULL reason is fine */
  if (reason && !op.reason)
    return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
  e = asper_apply_op(c, &op, false);
  asper_op_free(&op);
  return e;
}

asper_err asper_memory_set_locked(asper_ctx *c, const char *id, int locked) {
  asper_op op;
  asper_err e;

  if (!c) return ASPER_ERR_INVALID;
  if (!id || !asper_uuid_valid(id))
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid record id");

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_SET_LOCKED;
  op.at = asper_clock_now(&c->clock);
  memcpy(op.id, id, 37);
  op.value = locked != 0;
  e = asper_apply_op(c, &op, false);
  asper_op_free(&op);
  return e;
}

asper_err asper_memory_search(asper_ctx *c, asper_section s,
                              const char *project, const char *query,
                              size_t k, asper_record ***out, size_t *out_n) {
  char *active = NULL;
  asper_err e;

  if (!c) return ASPER_ERR_INVALID;
  if (!out || !out_n)
    return asper_seterr(c, ASPER_ERR_INVALID, "NULL output argument");
  *out = NULL;
  *out_n = 0;
  if (!query)
    return asper_seterr(c, ASPER_ERR_INVALID, "query is NULL");
  if (!asper_utf8_count(query, NULL))
    return asper_seterr(c, ASPER_ERR_INVALID, "query is not valid UTF-8");
  if (s != ASPER_SECTION_IDENTITY && s != ASPER_SECTION_CONTEXT &&
      s != ASPER_SECTION_PROJECT && s != ASPER_SECTION_ANY)
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid section");

  if (k == 0) {
    /* Per-section defaults: k_context / k_project; recall.k for ANY;
     * identity searches reuse k_context. */
    int d;
    switch (s) {
    case ASPER_SECTION_PROJECT:
      d = c->cfg.k_project > 0 ? c->cfg.k_project : 8;
      break;
    case ASPER_SECTION_ANY:
      d = c->cfg.recall_k > 0 ? c->cfg.recall_k : 12;
      break;
    default:
      d = c->cfg.k_context > 0 ? c->cfg.k_context : 6;
      break;
    }
    k = (size_t)d;
  }

  /* NULL project on PROJECT/ANY scans means "the active project" (mirrors
   * recall; notes "Post-review amendments"): snapshot it under the
   * read lock via asper_project_active. With no active project the NULL
   * passes through and project records are simply absent from results. */
  if (!project &&
      (s == ASPER_SECTION_PROJECT || s == ASPER_SECTION_ANY)) {
    e = asper_project_active(c, &active);
    if (e != ASPER_OK) return e;
    project = active;
  }

  e = asper_retrieve(c, query, s, project, k, c->cfg.min_similarity, out,
                     out_n);
  free(active);
  return e;
}

asper_err asper_memory_list(asper_ctx *c, asper_section s,
                            const char *project, int include_deprecated,
                            asper_record ***out, size_t *out_n) {
  if (!c) return ASPER_ERR_INVALID;
  if (!out || !out_n)
    return asper_seterr(c, ASPER_ERR_INVALID, "NULL output argument");
  *out = NULL;
  *out_n = 0;
  if (s != ASPER_SECTION_IDENTITY && s != ASPER_SECTION_CONTEXT &&
      s != ASPER_SECTION_PROJECT && s != ASPER_SECTION_ANY)
    return asper_seterr(c, ASPER_ERR_INVALID, "invalid section");
  return asper_collect_list(c, s, project, include_deprecated != 0, out,
                            out_n);
}

/* ---- lifecycle / maintenance -------------------------------------------- */

asper_err asper_flush(asper_ctx *c, int full) {
  asper_err first = ASPER_OK, e;

  if (!c) return ASPER_ERR_INVALID;

  if (!full) {
    first = asper_access_flush(c);
    e = asper_journal_sync(c);
    if (first == ASPER_OK) first = e;
    return first;
  }

  if (c->no_threads) return asper_run_due_work(c, true, true);

  if (c->has_curator) {
    asper_cycle_slot_acquire(c);
    first = asper_curation_cycle(c, true); /* all queued turns */
    e = asper_maintenance_review(c, true);
    if (first == ASPER_OK) first = e;
    asper_cycle_slot_release(c);
  }
  e = asper_access_flush(c);
  if (first == ASPER_OK) first = e;
  e = asper_store_compact(c);
  if (first == ASPER_OK) first = e;
  if (c->has_embedder && asper_cache_needs_save(c)) {
    e = asper_cache_save(c);
    if (e != ASPER_OK && first == ASPER_OK) first = e;
  }
  return first;
}

asper_err asper_get_stats(asper_ctx *c, asper_stats *out) {
  size_t i;

  if (!c) return ASPER_ERR_INVALID;
  if (!out)
    return asper_seterr(c, ASPER_ERR_INVALID, "NULL output argument");

  os_rwlock_rdlock(&c->lock);
  *out = c->stats;
  out->records_identity = 0;
  out->records_context = 0;
  out->records_project = 0;
  out->records_deprecated = 0;
  for (i = 0; i < c->store.table.n; i++) {
    const asper_record *r = c->store.table.recs[i];
    if (r->deprecated) {
      out->records_deprecated++;
      continue;
    }
    switch (r->section) {
    case ASPER_SECTION_IDENTITY: out->records_identity++; break;
    case ASPER_SECTION_CONTEXT: out->records_context++; break;
    case ASPER_SECTION_PROJECT: out->records_project++; break;
    default: break;
    }
  }
  out->journal_ops = c->store.journal_ops;
  if (c->store.manifest.last_compaction != 0)
    out->last_compaction_at = c->store.manifest.last_compaction;
  os_rwlock_rdunlock(&c->lock);
  return ASPER_OK;
}

asper_err asper_get_readiness(asper_ctx *c, asper_readiness *out) {
  if (!c || !out) return ASPER_ERR_INVALID;
  memset(out, 0, sizeof *out);
  os_rwlock_rdlock(&c->lock);
  out->store_ok = c->store.root != NULL;
  out->embedder_ok = c->has_embedder;
  out->curator_ok = c->has_curator;
  out->index_dim = c->index.dim;
  os_rwlock_rdunlock(&c->lock);
  return ASPER_OK;
}
