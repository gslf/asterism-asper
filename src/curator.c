/*
 * curator.c — curation cycles, curator-side guardrails, identity quorum,
 * maintenance review and recall.
 *
 * Every entry point here runs holding the cycle slot (worker thread, or the
 * caller in ASPER_NO_THREADS / asper_flush(full) paths), so cycles never
 * overlap. c->lock is taken only for short scans / stats / pending-set
 * updates and is NEVER held across generate(), asper_retrieve or
 * asper_apply_op (which take their own locks).
 *
 * Ownership convention with asper_apply_op: consumed op members are NULLed
 * by the callee; callers always finish with asper_op_free. For INSERT the
 * record pointer is additionally cleared here on success (the table owns it
 * from that point).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"

#define CURATOR_RELATED_PER_TURN 3
#define CURATOR_RELATED_CAP 12
#define CURATOR_CYCLE_MAX_TOKENS 1024
#define CURATOR_REVIEW_MAX_TOKENS 512

/* api.c: the single pre-funnel rejection counter (stats.ops_rejected++ +
 * INFO log). Every drop that never reaches asper_apply_op goes through it
 * exactly once; ops rejected INSIDE the funnel are counted there and must
 * not be re-counted here. */
extern void asper_count_reject(asper_ctx *c, const char *why);

/* Outcome of one curator op after guardrails + funnel. */
typedef enum {
  CYCLE_OP_APPLIED = 0,
  CYCLE_OP_REJECTED,
  CYCLE_OP_PENDING /* identity quorum: recorded, waiting for confirmation */
} cycle_op_result;

/* Identity proposals recorded during the current cycle, so a duplicate line
 * in the same output cannot self-confirm (the "later cycle" rule). */
typedef struct {
  char id[37];
  asper_op_kind kind;
  uint64_t hash;
} cycle_seen;

/* ---- small helpers ------------------------------------------------------ */

/* Pre-funnel drop with op context: "<what>: <why>" via asper_count_reject. */
static void count_drop(asper_ctx *c, const char *what, const char *why)
{
  char buf[192];
  snprintf(buf, sizeof buf, "%s: %s", what, why);
  asper_count_reject(c, buf);
}

static asper_err snapshot_active_project(asper_ctx *c, char **out)
{
  os_rwlock_rdlock(&c->lock);
  *out = asper_strdup(c->active_project);
  bool fail = (c->active_project != NULL && *out == NULL);
  os_rwlock_rdunlock(&c->lock);
  return fail ? ASPER_ERR_NOMEM : ASPER_OK;
}

/* "(identity)" / "(context)" / "(project:slug)" */
static asper_err append_label(asper_buf *b, const asper_record *r)
{
  if (r->section == ASPER_SECTION_PROJECT && r->project)
    return asper_buf_printf(b, "(project:%s)", r->project);
  return asper_buf_printf(b, "(%s)", asper_section_name(r->section));
}

static void free_turns(asper_turn *turns, size_t n)
{
  if (!turns)
    return;
  for (size_t i = 0; i < n; i++)
    free(turns[i].text);
  free(turns);
}

static void free_record_array(asper_record **recs, size_t n)
{
  if (!recs)
    return;
  for (size_t i = 0; i < n; i++)
    asper_record_free_one(recs[i]);
  free(recs);
}

/* Dequeue up to cfg.turn_batch oldest turns (all when force). Ownership of
 * the turn texts moves to *out. */
static asper_err take_turns(asper_ctx *c, bool force, asper_turn **out,
                            size_t *out_n)
{
  *out = NULL;
  *out_n = 0;
  os_mutex_lock(&c->ev_mu);
  size_t take = c->turns_n;
  if (!force && c->cfg.turn_batch > 0 && take > (size_t)c->cfg.turn_batch)
    take = (size_t)c->cfg.turn_batch;
  if (take == 0) {
    os_mutex_unlock(&c->ev_mu);
    return ASPER_OK;
  }
  asper_turn *t = malloc(take * sizeof *t);
  if (!t) {
    os_mutex_unlock(&c->ev_mu);
    return ASPER_ERR_NOMEM;
  }
  memcpy(t, c->turns, take * sizeof *t);
  memmove(c->turns, c->turns + take, (c->turns_n - take) * sizeof *c->turns);
  c->turns_n -= take;
  os_mutex_unlock(&c->ev_mu);
  *out = t;
  *out_n = take;
  return ASPER_OK;
}

/* Put an in-flight batch back at the head of the FIFO after a transient
 * failure. New turns may have arrived while generation was running; those
 * stay behind the restored batch so ordering is preserved. */
static bool restore_turns(asper_ctx *c, asper_turn *turns, size_t n)
{
  asper_turn *grown;
  size_t need, cap;

  if (!turns || n == 0)
    return true;
  os_mutex_lock(&c->ev_mu);
  if (n > SIZE_MAX - c->turns_n) {
    os_mutex_unlock(&c->ev_mu);
    return false;
  }
  need = n + c->turns_n;
  if (need > c->turns_cap) {
    cap = c->turns_cap ? c->turns_cap : 16;
    while (cap < need) {
      if (cap > SIZE_MAX / 2) {
        cap = need;
        break;
      }
      cap *= 2;
    }
    grown = realloc(c->turns, cap * sizeof *grown);
    if (!grown) {
      os_mutex_unlock(&c->ev_mu);
      return false;
    }
    c->turns = grown;
    c->turns_cap = cap;
  }
  memmove(c->turns + n, c->turns, c->turns_n * sizeof *c->turns);
  memcpy(c->turns, turns, n * sizeof *turns);
  c->turns_n += n;
  memset(turns, 0, n * sizeof *turns); /* ownership moved back to the FIFO */
  os_cond_signal(&c->ev_cv);
  os_mutex_unlock(&c->ev_mu);
  return true;
}

static size_t drop_all_turns(asper_ctx *c)
{
  os_mutex_lock(&c->ev_mu);
  size_t n = c->turns_n;
  for (size_t i = 0; i < n; i++)
    free(c->turns[i].text);
  c->turns_n = 0;
  os_mutex_unlock(&c->ev_mu);
  return n;
}

static const char *load_instruction(asper_ctx *c, char **heap_out)
{
  *heap_out = NULL;
  if (c->cfg.instruction_path) {
    char *buf = NULL;
    if (os_read_file(c->cfg.instruction_path, &buf, NULL) == ASPER_OK &&
        buf != NULL) {
      *heap_out = buf;
      return buf;
    }
    free(buf);
    asper_log(c, ASPER_LOG_WARN, "curator",
              "cannot read instruction file %s; using built-in",
              c->cfg.instruction_path);
  }
  return ASPER_DEFAULT_INSTRUCTION;
}

/* "USER: text" / "ASSISTANT: text" as a malloc'd string. */
static char *turn_line(const asper_turn *t)
{
  asper_buf b;
  asper_buf_init(&b);
  const char *role = (t->role == ASPER_ROLE_USER) ? "USER" : "ASSISTANT";
  if (asper_buf_printf(&b, "%s: %s", role, t->text ? t->text : "") !=
      ASPER_OK) {
    asper_buf_free(&b);
    return NULL;
  }
  return asper_buf_detach(&b);
}

/* .score desc, id asc (determinism for handle ordering); collect_related
 * stores the cosine in .score, so this is similarity order. */
static int rec_score_desc_cmp(const void *pa, const void *pb)
{
  const asper_record *a = *(asper_record *const *)pa;
  const asper_record *b = *(asper_record *const *)pb;
  if (a->score > b->score)
    return -1;
  if (a->score < b->score)
    return 1;
  return strcmp(a->id, b->id);
}

/* eff asc (stored in .score), id asc — review candidate order. */
static int rec_eff_asc_cmp(const void *pa, const void *pb)
{
  const asper_record *a = *(asper_record *const *)pa;
  const asper_record *b = *(asper_record *const *)pb;
  if (a->score < b->score)
    return -1;
  if (a->score > b->score)
    return 1;
  return strcmp(a->id, b->id);
}

/* ---- related-memory collection ------------------------------------------- */

/* Per-turn top-3 retrieval in SIMILARITY order, union by id keeping
 * the best cosine (clones carry it in .score via score_is_cos), capped at
 * 12 in cos desc, id asc order. Returned clones are owned by the caller. */
static asper_err collect_related(asper_ctx *c, const asper_turn *turns,
                                 size_t n_turns, const char *project,
                                 asper_record ***out, size_t *out_n)
{
  asper_record **uniq = NULL;
  size_t n = 0, cap = 0;

  *out = NULL;
  *out_n = 0;

  for (size_t i = 0; i < n_turns; i++) {
    if (asper_str_blank(turns[i].text))
      continue;
    asper_record **res = NULL;
    size_t res_n = 0;
    asper_err rc =
        asper_retrieve_ex(c, turns[i].text, ASPER_SECTION_ANY, project,
                          CURATOR_RELATED_PER_TURN, c->cfg.min_similarity, 1,
                          &res, &res_n);
    if (rc != ASPER_OK) {
      asper_log(c, ASPER_LOG_WARN, "curator",
                "related-memory retrieval failed (%s)", asper_err_name(rc));
      continue;
    }
    for (size_t j = 0; j < res_n; j++) {
      asper_record *r = res[j];
      size_t k = 0;
      for (; k < n; k++)
        if (strcmp(uniq[k]->id, r->id) == 0)
          break;
      if (k < n) {
        if (r->score > uniq[k]->score)
          uniq[k]->score = r->score;
        asper_record_free_one(r);
        continue;
      }
      if (n == cap) {
        size_t nc = cap ? cap * 2 : 8;
        asper_record **nu = realloc(uniq, nc * sizeof *nu);
        if (!nu) {
          asper_record_free_one(r);
          for (size_t m = j + 1; m < res_n; m++)
            asper_record_free_one(res[m]);
          free(res);
          free_record_array(uniq, n);
          return ASPER_ERR_NOMEM;
        }
        uniq = nu;
        cap = nc;
      }
      uniq[n++] = r;
    }
    free(res);
  }

  if (n > 1)
    qsort(uniq, n, sizeof *uniq, rec_score_desc_cmp);
  while (n > CURATOR_RELATED_CAP)
    asper_record_free_one(uniq[--n]);

  *out = uniq;
  *out_n = n;
  return ASPER_OK;
}

/* ---- cycle prompt --------------------------------------------------------- */

static asper_err build_cycle_prompt(asper_ctx *c, asper_record *const *mems,
                                    size_t n_mems, const asper_turn *turns,
                                    size_t n_turns, char **out)
{
  asper_buf b;
  asper_buf_init(&b);
  asper_err e = ASPER_OK;

  *out = NULL;

  if (n_mems == 0) {
    e = asper_buf_appends(&b, "Existing memories: (none)\n");
  } else {
    e = asper_buf_appends(&b, "Existing memories:\n");
    for (size_t i = 0; i < n_mems && e == ASPER_OK; i++) {
      e = asper_buf_printf(&b, "[M%zu] ", i + 1);
      if (e == ASPER_OK)
        e = append_label(&b, mems[i]);
      if (e == ASPER_OK)
        e = asper_buf_printf(&b, " %s\n",
                             mems[i]->content ? mems[i]->content : "");
    }
  }
  if (e == ASPER_OK)
    e = asper_buf_appends(&b, "\nConversation:\n");

  /* Trim oldest turns so the transcript fits cfg.transcript_tokens; the
   * newest turn is always kept. */
  size_t start = n_turns;
  if (e == ASPER_OK && n_turns > 0) {
    int total = 0;
    while (start > 0) {
      char *line = turn_line(&turns[start - 1]);
      if (!line) {
        e = ASPER_ERR_NOMEM;
        break;
      }
      int cost = asper_estimate_tokens(c, line);
      free(line);
      if (total + cost > c->cfg.transcript_tokens && start < n_turns)
        break;
      total += cost;
      start--;
    }
  }
  for (size_t i = start; i < n_turns && e == ASPER_OK; i++) {
    char *line = turn_line(&turns[i]);
    if (!line) {
      e = ASPER_ERR_NOMEM;
      break;
    }
    e = asper_buf_appends(&b, line);
    free(line);
    if (e == ASPER_OK)
      e = asper_buf_appendc(&b, '\n');
  }

  if (e != ASPER_OK) {
    asper_buf_free(&b);
    return e;
  }
  *out = asper_buf_detach(&b);
  return *out ? ASPER_OK : ASPER_ERR_NOMEM;
}

/* ---- identity quorum ------------------------------------------------------ */

/* Prune expired pendings, then either confirm-and-remove a matching entry
 * from an earlier cycle (*confirmed = true) or record a new pending. */
static asper_err quorum_check(asper_ctx *c, const char *id,
                              asper_op_kind kind, uint64_t hash,
                              asper_time now, bool *confirmed)
{
  *confirmed = false;
  os_rwlock_wrlock(&c->lock);
  size_t i = 0;
  while (i < c->pending_n) {
    asper_pending *p = &c->pending[i];
    if (now - p->first_at > c->cfg.identity_confirm_window_s) {
      c->pending[i] = c->pending[c->pending_n - 1];
      c->pending_n--;
      continue;
    }
    if (!*confirmed && p->kind == kind && p->content_hash == hash &&
        strcmp(p->id, id) == 0) {
      c->pending[i] = c->pending[c->pending_n - 1];
      c->pending_n--;
      *confirmed = true;
      continue;
    }
    i++;
  }
  if (!*confirmed) {
    if (c->pending_n == c->pending_cap) {
      size_t nc = c->pending_cap ? c->pending_cap * 2 : 8;
      asper_pending *np = realloc(c->pending, nc * sizeof *np);
      if (!np) {
        os_rwlock_wrunlock(&c->lock);
        return ASPER_ERR_NOMEM;
      }
      c->pending = np;
      c->pending_cap = nc;
    }
    asper_pending *p = &c->pending[c->pending_n++];
    memcpy(p->id, id, sizeof p->id);
    p->kind = kind;
    p->content_hash = hash;
    p->first_at = now;
  }
  os_rwlock_wrunlock(&c->lock);
  return ASPER_OK;
}

/* ---- op execution -------------------------------------------------------- */

/* Run one op through the funnel; `what` is a short description for logs. */
static cycle_op_result funnel_apply(asper_ctx *c, asper_op *op,
                                    const char *what)
{
  asper_err rc = asper_apply_op(c, op, true);
  if (rc == ASPER_OK) {
    op->record = NULL; /* INSERT: the table owns the record now */
    asper_op_free(op);
    asper_log(c, ASPER_LOG_INFO, "curator", "%s applied", what);
    return CYCLE_OP_APPLIED;
  }
  asper_op_free(op);
  asper_log(c, ASPER_LOG_INFO, "curator", "%s rejected: %s", what,
            asper_err_name(rc));
  return CYCLE_OP_REJECTED;
}

static cycle_op_result cycle_do_insert(asper_ctx *c, const asper_cop *cop,
                                       const char *project, asper_time now)
{
  const char *text = cop->text ? cop->text : "";
  const char *sec = asper_section_name(cop->section);
  char what[64];

  snprintf(what, sizeof what, "INSERT %s", sec);

  if (asper_str_blank(text)) {
    count_drop(c, what, "empty text");
    return CYCLE_OP_REJECTED;
  }
  size_t text_chars;
  if (!asper_utf8_count(text, &text_chars)) {
    count_drop(c, what, "content is not valid UTF-8");
    return CYCLE_OP_REJECTED;
  }
  if (c->cfg.content_max_chars > 0 &&
      text_chars > (size_t)c->cfg.content_max_chars) {
    char why[48];
    snprintf(why, sizeof why, "text exceeds %d chars",
             c->cfg.content_max_chars);
    count_drop(c, what, why);
    return CYCLE_OP_REJECTED;
  }
  if (cop->section == ASPER_SECTION_PROJECT && !project) {
    count_drop(c, what, "no active project");
    return CYCLE_OP_REJECTED;
  }

  /* Dedup: only with an embedder; a hit converts the insert into an
   * access boost on the existing record. */
  if (c->has_embedder && c->embedder.dim > 0) {
    float *vec = malloc((size_t)c->embedder.dim * sizeof *vec);
    if (!vec) {
      count_drop(c, what, "out of memory");
      return CYCLE_OP_REJECTED;
    }
    asper_err erc = c->embedder.embed(c->embedder.ud, text, 0, vec);
    if (erc != ASPER_OK) {
      asper_log(c, ASPER_LOG_WARN, "curator",
                "dedup embedding failed (%s); inserting without dedup",
                asper_err_name(erc));
    } else {
      asper_hit hit;
      char dup_id[37] = {0};
      double dup_cos = 0.0;
      os_rwlock_rdlock(&c->lock);
      /* rank_by_cos: the dedup winner is the max-cosine record clearing
       * dup_threshold, not the best composite score. */
      size_t nh = asper_index_scan(
          &c->index, &c->cfg, &c->clock, vec, cop->section,
          cop->section == ASPER_SECTION_PROJECT ? project : NULL, 1,
          c->cfg.dup_threshold, 1, &hit);
      if (nh > 0) {
        memcpy(dup_id, hit.rec->id, sizeof dup_id);
        dup_cos = hit.cos;
      }
      os_rwlock_rdunlock(&c->lock);
      if (dup_id[0] != '\0') {
        free(vec);
        asper_access_note(c, dup_id);
        asper_log(c, ASPER_LOG_INFO, "curator",
                  "%s deduplicated into %s (cos=%.2f)", what, dup_id,
                  dup_cos);
        return CYCLE_OP_APPLIED;
      }
    }
    free(vec);
  }

  asper_record *r = asper_record_new();
  if (!r) {
    count_drop(c, what, "out of memory");
    return CYCLE_OP_REJECTED;
  }
  asper_uuid_v4(r->id);
  r->section = cop->section;
  r->content = asper_strdup(text);
  if (cop->section == ASPER_SECTION_PROJECT)
    r->project = asper_strdup(project);
  if (!r->content ||
      (cop->section == ASPER_SECTION_PROJECT && !r->project)) {
    asper_record_free_one(r);
    count_drop(c, what, "out of memory");
    return CYCLE_OP_REJECTED;
  }
  r->source = ASPER_SRC_CURATOR;
  r->created_at = r->updated_at = r->last_access = now;
  r->access_count = 0;
  r->relevance = 0.60;
  r->locked = false;
  r->deprecated = false;
  r->deprecated_at = 0;
  r->supersedes[0] = '\0';
  r->score = 0.0;
  r->emb_row = -1;

  char what_id[104];
  snprintf(what_id, sizeof what_id, "%s %s", what, r->id);

  asper_op op;
  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_INSERT;
  op.at = now;
  op.record = r;
  return funnel_apply(c, &op, what_id);
}

static bool cycle_seen_has(const cycle_seen *seen, size_t n, const char *id,
                           asper_op_kind kind, uint64_t hash)
{
  for (size_t i = 0; i < n; i++)
    if (seen[i].kind == kind && seen[i].hash == hash &&
        strcmp(seen[i].id, id) == 0)
      return true;
  return false;
}

static cycle_op_result cycle_do_mutate(asper_ctx *c, const asper_cop *cop,
                                       asper_record *const *handles,
                                       cycle_seen **seen, size_t *seen_n,
                                       size_t *seen_cap, asper_time now)
{
  const asper_record *target = handles[cop->handle - 1];
  bool is_upd = (cop->kind == ASPER_COP_UPDATE);
  asper_op_kind kind = is_upd ? ASPER_OP_UPDATE : ASPER_OP_DEPRECATE;
  const char *text = cop->text ? cop->text : "";
  char what[104];

  snprintf(what, sizeof what, "%s M%d -> %s",
           is_upd ? "UPDATE" : "DEPRECATE", cop->handle, target->id);

  if (target->locked) {
    count_drop(c, what, "record locked");
    return CYCLE_OP_REJECTED;
  }
  if (is_upd) {
    if (asper_str_blank(text)) {
      count_drop(c, what, "empty text");
      return CYCLE_OP_REJECTED;
    }
    size_t text_chars;
    if (!asper_utf8_count(text, &text_chars)) {
      count_drop(c, what, "content is not valid UTF-8");
      return CYCLE_OP_REJECTED;
    }
    if (c->cfg.content_max_chars > 0 &&
        text_chars > (size_t)c->cfg.content_max_chars) {
      char why[48];
      snprintf(why, sizeof why, "text exceeds %d chars",
               c->cfg.content_max_chars);
      count_drop(c, what, why);
      return CYCLE_OP_REJECTED;
    }
  }

  if (target->section == ASPER_SECTION_IDENTITY) {
    uint64_t hash = is_upd ? asper_fnv1a64(text, strlen(text))
                           : asper_fnv1a64("", 0);
    if (cycle_seen_has(*seen, *seen_n, target->id, kind, hash)) {
      count_drop(c, what, "duplicate proposal in the same cycle");
      return CYCLE_OP_PENDING;
    }
    /* Ensure room in the same-cycle guard BEFORE touching the pending set:
     * if the guard cannot record the proposal, the op is rejected outright,
     * so a duplicated line can never self-confirm the quorum within one
     * cycle. */
    if (*seen_n == *seen_cap) {
      size_t nc = *seen_cap ? *seen_cap * 2 : 8;
      cycle_seen *ns = realloc(*seen, nc * sizeof *ns);
      if (!ns) {
        count_drop(c, what, "out of memory");
        return CYCLE_OP_REJECTED;
      }
      *seen = ns;
      *seen_cap = nc;
    }
    bool confirmed = false;
    asper_err qrc = quorum_check(c, target->id, kind, hash, now, &confirmed);
    if (qrc != ASPER_OK) {
      count_drop(c, what, asper_err_name(qrc));
      return CYCLE_OP_REJECTED;
    }
    if (!confirmed) {
      cycle_seen *s = &(*seen)[(*seen_n)++];
      memcpy(s->id, target->id, sizeof s->id);
      s->kind = kind;
      s->hash = hash;
      count_drop(c, what, "pending confirmation (identity quorum)");
      return CYCLE_OP_PENDING;
    }
    /* Confirmed re-proposal: fall through and apply. */
  }

  asper_op op;
  memset(&op, 0, sizeof op);
  op.kind = kind;
  op.at = now;
  memcpy(op.id, target->id, sizeof op.id);
  if (is_upd) {
    op.content = asper_strdup(text);
    if (!op.content) {
      count_drop(c, what, "out of memory");
      return CYCLE_OP_REJECTED;
    }
  } else if (cop->text) {
    op.reason = asper_strdup(cop->text);
    if (!op.reason) {
      count_drop(c, what, "out of memory");
      return CYCLE_OP_REJECTED;
    }
  }
  return funnel_apply(c, &op, what);
}

/* ---- batch epilogue ------------------------------------------------------ */

static void cycle_epilogue(asper_ctx *c)
{
  asper_err rc = asper_access_flush(c);
  if (rc != ASPER_OK)
    asper_log(c, ASPER_LOG_WARN, "curator", "access flush failed (%s)",
              asper_err_name(rc));
  if (c->cfg.journal_sync == ASPER_SYNC_BATCH) {
    rc = asper_journal_sync(c);
    if (rc != ASPER_OK)
      asper_log(c, ASPER_LOG_WARN, "curator", "journal sync failed (%s)",
                asper_err_name(rc));
  }
}

/* ---- curation cycle ------------------------------------------------------- */

asper_err asper_curation_cycle(asper_ctx *c, bool force)
{
  int64_t t0 = os_monotonic_ms();
  asper_time now = asper_clock_now(&c->clock);

  if (!c->has_curator) {
    size_t dropped = drop_all_turns(c);
    if (dropped > 0)
      asper_log(c, ASPER_LOG_DEBUG, "curator",
                "no curator model: %zu turn(s) dropped", dropped);
    return ASPER_OK;
  }

  asper_turn *turns = NULL;
  size_t n_turns = 0;
  asper_err rc = take_turns(c, force, &turns, &n_turns);
  if (rc != ASPER_OK)
    return asper_seterr(c, rc, "curation cycle: out of memory");
  if (n_turns == 0)
    return ASPER_OK;

  char *project = NULL;
  asper_record **handles = NULL;
  size_t n_handles = 0;
  char *prompt = NULL;
  char *gbnf = NULL;
  char *reply = NULL;
  char *instr_heap = NULL;
  const char *instr = NULL;
  asper_cop *cops = NULL;
  size_t n_cops = 0, bad = 0;

  rc = snapshot_active_project(c, &project);
  if (rc != ASPER_OK)
    goto fail;

  rc = collect_related(c, turns, n_turns, project, &handles, &n_handles);
  if (rc != ASPER_OK)
    goto fail;

  rc = build_cycle_prompt(c, handles, n_handles, turns, n_turns, &prompt);
  if (rc != ASPER_OK)
    goto fail;

  gbnf = asper_gbnf_build(ASPER_GRAMMAR_CURATION, n_handles,
                          c->cfg.content_max_chars);
  if (!gbnf) {
    rc = ASPER_ERR_NOMEM;
    goto fail;
  }

  instr = load_instruction(c, &instr_heap);
  rc = c->curator.generate(c->curator.ud, instr, prompt, gbnf,
                           CURATOR_CYCLE_MAX_TOKENS, 0, &reply);
  if (rc != ASPER_OK) {
    asper_log(c, ASPER_LOG_ERROR, "curator",
              "generation failed (%s): retaining %zu turn(s) for retry",
              asper_err_name(rc), n_turns);
    asper_seterr(c, rc, "curator generation failed");
    goto out;
  }

  rc = asper_protocol_parse(reply, n_handles, &cops, &n_cops, &bad);
  if (rc != ASPER_OK) {
    asper_seterr(c, rc, "curation cycle: out of memory");
    goto out;
  }

  {
    /* Local tallies feed the INFO summary line only; stats.ops_rejected is
     * bumped once per drop — inside asper_apply_op for funnel rejections,
     * via asper_count_reject for everything dropped before the funnel. */
    size_t applied = 0, rejected = bad, pending = 0, processed = 0;
    cycle_seen *seen = NULL;
    size_t seen_n = 0, seen_cap = 0;

    for (size_t i = 0; i < bad; i++)
      asper_count_reject(c, "malformed protocol line");

    for (size_t i = 0; i < n_cops; i++) {
      const asper_cop *cop = &cops[i];
      if (cop->kind == ASPER_COP_NOOP)
        continue;
      if (c->cfg.max_ops_per_cycle > 0 &&
          processed >= (size_t)c->cfg.max_ops_per_cycle) {
        char why[48];
        rejected++;
        snprintf(why, sizeof why, "max_ops_per_cycle (%d) reached",
                 c->cfg.max_ops_per_cycle);
        asper_count_reject(c, why);
        continue;
      }
      processed++;

      cycle_op_result res;
      switch (cop->kind) {
      case ASPER_COP_INSERT:
        res = cycle_do_insert(c, cop, project, now);
        break;
      case ASPER_COP_UPDATE:
      case ASPER_COP_DEPRECATE:
        res = cycle_do_mutate(c, cop, handles, &seen, &seen_n, &seen_cap,
                              now);
        break;
      default: /* KEEP / ANSWER / CITE / NOMEM are invalid here */
        asper_count_reject(c, "verb not valid in a curation cycle");
        res = CYCLE_OP_REJECTED;
        break;
      }
      if (res == CYCLE_OP_APPLIED)
        applied++;
      else if (res == CYCLE_OP_REJECTED)
        rejected++;
      else
        pending++;
    }
    free(seen);
    (void)pending; /* visible via its own INFO lines */

    cycle_epilogue(c);

    size_t cycle_no;
    os_rwlock_wrlock(&c->lock);
    c->stats.cycles_run++;
    cycle_no = c->stats.cycles_run;
    c->stats.last_cycle_at = now;
    os_rwlock_wrunlock(&c->lock);

    asper_log(c, ASPER_LOG_INFO, "curator",
              "cycle #%zu: applied=%zu rejected=%zu (%.1fs)", cycle_no,
              applied, rejected,
              (double)(os_monotonic_ms() - t0) / 1000.0);
  }
  rc = ASPER_OK;

out:
  if (rc != ASPER_OK && !restore_turns(c, turns, n_turns))
    asper_log(c, ASPER_LOG_ERROR, "curator",
              "could not restore %zu turn(s) after failure: out of memory",
              n_turns);
  asper_cops_free(cops, n_cops);
  free(reply);
  free(instr_heap);
  free(gbnf);
  free(prompt);
  free_record_array(handles, n_handles);
  free(project);
  free_turns(turns, n_turns);
  return rc;

fail:
  asper_log(c, ASPER_LOG_ERROR, "curator",
            "cycle aborted (%s): retaining %zu turn(s) for retry",
            asper_err_name(rc),
            n_turns);
  asper_seterr(c, rc, "curation cycle failed: %s", asper_err_name(rc));
  goto out;
}

/* ---- maintenance review --------------------------------------------------- */

/* Active, unlocked, non-identity records with eff < prune_threshold; clones
 * with eff stashed in .score, sorted eff asc, capped at review_batch. */
static asper_err review_collect(asper_ctx *c, asper_time now,
                                asper_record ***out, size_t *out_n)
{
  asper_record **cands = NULL;
  size_t n = 0, cap = 0;
  asper_err rc = ASPER_OK;

  *out = NULL;
  *out_n = 0;

  os_rwlock_rdlock(&c->lock);
  for (size_t i = 0; i < c->store.table.n; i++) {
    asper_record *r = c->store.table.recs[i];
    if (r->deprecated || r->locked || r->section == ASPER_SECTION_IDENTITY)
      continue;
    double eff = asper_eff_relevance(&c->cfg, r, now);
    if (eff >= c->cfg.prune_threshold)
      continue;
    asper_record *cl = asper_record_clone(r);
    if (!cl) {
      rc = ASPER_ERR_NOMEM;
      break;
    }
    cl->score = eff;
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 16;
      asper_record **na = realloc(cands, nc * sizeof *na);
      if (!na) {
        asper_record_free_one(cl);
        rc = ASPER_ERR_NOMEM;
        break;
      }
      cands = na;
      cap = nc;
    }
    cands[n++] = cl;
  }
  os_rwlock_rdunlock(&c->lock);

  if (rc != ASPER_OK) {
    free_record_array(cands, n);
    return rc;
  }
  if (n > 1)
    qsort(cands, n, sizeof *cands, rec_eff_asc_cmp);
  if (c->cfg.review_batch > 0)
    while (n > (size_t)c->cfg.review_batch)
      asper_record_free_one(cands[--n]);
  *out = cands;
  *out_n = n;
  return ASPER_OK;
}

static asper_err build_review_prompt(asper_record *const *cands, size_t n,
                                     asper_time now, char **out)
{
  asper_buf b;
  asper_buf_init(&b);
  asper_err e = asper_buf_appends(&b, "Candidate memories:\n");

  *out = NULL;
  for (size_t i = 0; i < n && e == ASPER_OK; i++) {
    const asper_record *r = cands[i];
    long long idle_d = (long long)((now - r->last_access) / 86400);
    if (idle_d < 0)
      idle_d = 0;
    e = asper_buf_printf(&b, "[M%zu] (", i + 1);
    if (e == ASPER_OK) {
      if (r->section == ASPER_SECTION_PROJECT && r->project)
        e = asper_buf_printf(&b, "project:%s", r->project);
      else
        e = asper_buf_appends(&b, asper_section_name(r->section));
    }
    if (e == ASPER_OK)
      e = asper_buf_printf(&b, ", idle %lldd) %s\n", idle_d,
                           r->content ? r->content : "");
  }
  if (e != ASPER_OK) {
    asper_buf_free(&b);
    return e;
  }
  *out = asper_buf_detach(&b);
  return *out ? ASPER_OK : ASPER_ERR_NOMEM;
}

asper_err asper_maintenance_review(asper_ctx *c, bool force)
{
  int64_t t0 = os_monotonic_ms();
  asper_time now = asper_clock_now(&c->clock);

  os_mutex_lock(&c->ev_mu);
  if (!force && c->last_maintenance != 0 &&
      now - c->last_maintenance < c->cfg.maintenance_interval_s) {
    os_mutex_unlock(&c->ev_mu);
    return ASPER_OK;
  }

  /* Bumped whenever the review runs, even when it decides to do nothing. */
  c->last_maintenance = now;
  os_mutex_unlock(&c->ev_mu);

  if (!c->has_curator) {
    asper_log(c, ASPER_LOG_DEBUG, "curator",
              "maintenance review skipped: no curator model");
    return ASPER_OK;
  }

  asper_record **cands = NULL;
  size_t n_cands = 0;
  asper_err rc = review_collect(c, now, &cands, &n_cands);
  if (rc != ASPER_OK)
    return asper_seterr(c, rc, "maintenance review: out of memory");
  if (n_cands == 0) {
    asper_log(c, ASPER_LOG_DEBUG, "curator", "review: no candidates");
    return ASPER_OK;
  }

  char *prompt = NULL;
  char *gbnf = NULL;
  char *reply = NULL;
  asper_cop *cops = NULL;
  size_t n_cops = 0, bad = 0;

  rc = build_review_prompt(cands, n_cands, now, &prompt);
  if (rc != ASPER_OK) {
    asper_seterr(c, rc, "maintenance review: out of memory");
    goto out;
  }
  gbnf = asper_gbnf_build(ASPER_GRAMMAR_REVIEW, n_cands,
                          c->cfg.content_max_chars);
  if (!gbnf) {
    rc = asper_seterr(c, ASPER_ERR_NOMEM, "maintenance review: out of memory");
    goto out;
  }

  rc = c->curator.generate(c->curator.ud, ASPER_REVIEW_INSTRUCTION, prompt,
                           gbnf, CURATOR_REVIEW_MAX_TOKENS, 0, &reply);
  if (rc != ASPER_OK) {
    asper_log(c, ASPER_LOG_ERROR, "curator", "review generation failed (%s)",
              asper_err_name(rc));
    asper_seterr(c, rc, "review generation failed");
    goto out;
  }

  rc = asper_protocol_parse(reply, n_cands, &cops, &n_cops, &bad);
  if (rc != ASPER_OK) {
    asper_seterr(c, rc, "maintenance review: out of memory");
    goto out;
  }

  {
    /* Candidates are never locked or identity, so verdicts go straight to
     * the funnel; review_batch is the batch cap, max_ops_per_cycle does not
     * apply to review verdicts. Local tallies feed the INFO summary line
     * only; pre-funnel drops go through asper_count_reject, funnel
     * rejections are counted inside asper_apply_op. */
    size_t applied = 0, rejected = bad;

    for (size_t i = 0; i < bad; i++)
      asper_count_reject(c, "malformed protocol line");

    for (size_t i = 0; i < n_cops; i++) {
      const asper_cop *cop = &cops[i];
      if (cop->kind == ASPER_COP_NOOP)
        continue;
      if (cop->kind != ASPER_COP_DEPRECATE && cop->kind != ASPER_COP_KEEP) {
        rejected++;
        asper_count_reject(c, "verb not valid in a review cycle");
        continue;
      }
      const asper_record *target = cands[cop->handle - 1];
      char what[104];
      snprintf(what, sizeof what, "%s M%d -> %s",
               cop->kind == ASPER_COP_KEEP ? "KEEP" : "DEPRECATE",
               cop->handle, target->id);

      asper_op op;
      memset(&op, 0, sizeof op);
      op.at = now;
      memcpy(op.id, target->id, sizeof op.id);
      if (cop->kind == ASPER_COP_KEEP) {
        op.kind = ASPER_OP_KEEP;
      } else {
        op.kind = ASPER_OP_DEPRECATE;
        if (cop->text) {
          op.reason = asper_strdup(cop->text);
          if (!op.reason) {
            rejected++;
            count_drop(c, what, "out of memory");
            continue;
          }
        }
      }
      if (funnel_apply(c, &op, what) == CYCLE_OP_APPLIED)
        applied++;
      else
        rejected++;
    }

    cycle_epilogue(c);

    asper_log(c, ASPER_LOG_INFO, "curator",
              "review: applied=%zu rejected=%zu (%.1fs)", applied, rejected,
              (double)(os_monotonic_ms() - t0) / 1000.0);
  }
  rc = ASPER_OK;

out:
  asper_cops_free(cops, n_cops);
  free(reply);
  free(gbnf);
  free(prompt);
  free_record_array(cands, n_cands);
  return rc;
}

/* ---- recall --------------------------------------------------------------- */

static asper_err build_recall_prompt(const char *question,
                                     asper_record *const *cands, size_t n,
                                     char **out)
{
  asper_buf b;
  asper_buf_init(&b);
  asper_err e = asper_buf_printf(&b, "Question: %s\n\nMemories:\n", question);

  *out = NULL;
  for (size_t i = 0; i < n && e == ASPER_OK; i++) {
    e = asper_buf_printf(&b, "[M%zu] ", i + 1);
    if (e == ASPER_OK)
      e = append_label(&b, cands[i]);
    if (e == ASPER_OK)
      e = asper_buf_printf(&b, " %s\n",
                           cands[i]->content ? cands[i]->content : "");
  }
  if (e != ASPER_OK) {
    asper_buf_free(&b);
    return e;
  }
  *out = asper_buf_detach(&b);
  return *out ? ASPER_OK : ASPER_ERR_NOMEM;
}

asper_err asper_recall_run(asper_ctx *c, const char *question,
                           int64_t deadline_ms,
                           char **out_answer, asper_record ***out_cited,
                           size_t *out_cited_n)
{
  int64_t t0 = os_monotonic_ms();

  if (out_answer)
    *out_answer = NULL;
  if (out_cited)
    *out_cited = NULL;
  if (out_cited_n)
    *out_cited_n = 0;
  if (!out_answer || !question || asper_str_blank(question))
    return asper_seterr(c, ASPER_ERR_INVALID, "recall: invalid question");
  if (!c->has_curator)
    return asper_seterr(c, ASPER_ERR_MODEL,
                        "recall requires a curator model");

  char *project = NULL;
  asper_err rc = snapshot_active_project(c, &project);
  if (rc != ASPER_OK)
    return asper_seterr(c, rc, "recall: out of memory");

  asper_record **cands = NULL;
  size_t n_cands = 0;
  rc = asper_retrieve(c, question, ASPER_SECTION_ANY, project,
                      c->cfg.recall_k > 0 ? (size_t)c->cfg.recall_k : 0,
                      c->cfg.min_similarity, &cands, &n_cands);
  free(project);
  if (rc != ASPER_OK)
    return rc;
  if (n_cands == 0) {
    free_record_array(cands, n_cands);
    asper_log(c, ASPER_LOG_DEBUG, "recall",
              "no candidates above the similarity floor");
    return asper_seterr(c, ASPER_ERR_NOT_FOUND, "no relevant memories");
  }

  char *prompt = NULL;
  char *gbnf = NULL;
  char *reply = NULL;
  asper_cop *cops = NULL;
  size_t n_cops = 0, bad = 0;
  bool *seen = NULL;
  size_t *cites = NULL;
  size_t n_cites = 0;
  const char *answer_text = NULL;

  rc = build_recall_prompt(question, cands, n_cands, &prompt);
  if (rc != ASPER_OK) {
    asper_seterr(c, rc, "recall: out of memory");
    goto out;
  }
  gbnf = asper_gbnf_build(ASPER_GRAMMAR_RECALL, n_cands,
                          c->cfg.content_max_chars);
  if (!gbnf) {
    rc = asper_seterr(c, ASPER_ERR_NOMEM, "recall: out of memory");
    goto out;
  }

  rc = c->curator.generate(c->curator.ud, ASPER_RECALL_INSTRUCTION, prompt,
                           gbnf, c->cfg.recall_answer_tokens, deadline_ms,
                           &reply);
  if (rc != ASPER_OK) {
    asper_log(c, ASPER_LOG_ERROR, "recall", "generation failed (%s)",
              asper_err_name(rc));
    asper_seterr(c, rc, "recall generation failed");
    goto out;
  }

  rc = asper_protocol_parse(reply, n_cands, &cops, &n_cops, &bad);
  if (rc != ASPER_OK) {
    asper_seterr(c, rc, "recall: out of memory");
    goto out;
  }

  seen = calloc(n_cands, sizeof *seen);
  cites = malloc(n_cands * sizeof *cites);
  if (!seen || !cites) {
    rc = asper_seterr(c, ASPER_ERR_NOMEM, "recall: out of memory");
    goto out;
  }
  for (size_t i = 0; i < n_cops; i++) {
    const asper_cop *cop = &cops[i];
    if (cop->kind == ASPER_COP_ANSWER && !answer_text && cop->text)
      answer_text = cop->text;
    else if (cop->kind == ASPER_COP_CITE && !seen[cop->handle - 1]) {
      seen[cop->handle - 1] = true;
      cites[n_cites++] = (size_t)cop->handle - 1;
    }
  }

  if (!answer_text || asper_str_blank(answer_text)) {
    asper_log(c, ASPER_LOG_INFO, "recall", "no answer (NOMEM, %.1fs)",
              (double)(os_monotonic_ms() - t0) / 1000.0);
    rc = asper_seterr(c, ASPER_ERR_NOT_FOUND, "no relevant memories");
    goto out;
  }

  *out_answer = asper_strdup(answer_text);
  if (!*out_answer) {
    rc = asper_seterr(c, ASPER_ERR_NOMEM, "recall: out of memory");
    goto out;
  }

  if (out_cited && out_cited_n && n_cites > 0) {
    asper_record **arr = malloc(n_cites * sizeof *arr);
    if (!arr) {
      free(*out_answer);
      *out_answer = NULL;
      rc = asper_seterr(c, ASPER_ERR_NOMEM, "recall: out of memory");
      goto out;
    }
    for (size_t i = 0; i < n_cites; i++) {
      arr[i] = asper_record_clone(cands[cites[i]]);
      if (!arr[i]) {
        free_record_array(arr, i);
        free(*out_answer);
        *out_answer = NULL;
        rc = asper_seterr(c, ASPER_ERR_NOMEM, "recall: out of memory");
        goto out;
      }
    }
    *out_cited = arr;
    *out_cited_n = n_cites;
  }

  /* Cited records receive the standard access boost even when the caller
   * discards the citations. */
  for (size_t i = 0; i < n_cites; i++)
    asper_access_note(c, cands[cites[i]]->id);
  if (n_cites > 0) {
    asper_err frc = asper_access_flush(c);
    if (frc != ASPER_OK)
      asper_log(c, ASPER_LOG_WARN, "recall", "access flush failed (%s)",
                asper_err_name(frc));
  }

  os_rwlock_wrlock(&c->lock);
  c->stats.recalls_served++;
  os_rwlock_wrunlock(&c->lock);

  asper_log(c, ASPER_LOG_INFO, "recall",
            "answered with %zu citation(s) (%.1fs)", n_cites,
            (double)(os_monotonic_ms() - t0) / 1000.0);
  rc = ASPER_OK;

out:
  free(cites);
  free(seen);
  asper_cops_free(cops, n_cops);
  free(reply);
  free(gbnf);
  free(prompt);
  free_record_array(cands, n_cands);
  return rc;
}
