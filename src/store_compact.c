/* Snapshot projection and the compaction commit boundary. */
#include "store_files.h"
#include <stdlib.h>
#include <string.h>
static asper_err checkpoint(asper_store *st, asper_err e, int stage) {
  return e == ASPER_OK && st->compact_checkpoint ? st->compact_checkpoint(stage) : e;
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
  bool tx_active = false, commit_uncertain = false;

  asper_buf_init(&ident);
  asper_buf_init(&ctxb);

  os_rwlock_wrlock(&c->lock);
  os_mutex_lock(&c->journal_mu);
  if (st->poisoned || st->curation_receipt) {
    asper_err unavailable = st->poisoned ? ASPER_ERR_IO : ASPER_ERR_BUSY;
    os_mutex_unlock(&c->journal_mu); os_rwlock_wrunlock(&c->lock);
    return unavailable;
  }
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
    e = asper_compact_begin(c, tx_rels, tx_n);
    tx_active = (e == ASPER_OK);
    e = checkpoint(st,e,1);
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
          r->project ? asper_store_project_index(st, r->project, &found) : 0;
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
    e = asper_store_file_write(c, st->identity_path, ident.data ? ident.data : "",
                           ident.len, true);
  e = checkpoint(st,e,2);
  if (e == ASPER_OK)
    e = asper_store_file_write(c, st->context_path, ctxb.data ? ctxb.data : "",
                           ctxb.len, true);
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
    e = asper_store_file_write(c, ppath, pbufs[i].data ? pbufs[i].data : "",
                           pbufs[i].len, true);
    free(ppath);
  }

  e = checkpoint(st,e,3);

  /* Reset the journal: the active transaction rolls all section files and
   * this journal back together if the process stops before commit. */
  if (e == ASPER_OK) {
    if (st->journal_fp) {
      fclose(st->journal_fp);
      st->journal_fp = NULL;
    }
    e = asper_store_file_write(c, st->journal_path, "", 0, false);
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

  e = checkpoint(st,e,4);
  if (e == ASPER_OK && tx_active) {
    e = asper_compact_commit(c, tx_rels, tx_n);
    e = checkpoint(st,e,5);
    if (e == ASPER_OK) tx_active = false;
    else { commit_uncertain = true; st->poisoned = true; }
  }
  if (commit_uncertain && st->journal_fp) {
    fclose(st->journal_fp); st->journal_fp = NULL;
  }
  if (e != ASPER_OK && tx_active && !commit_uncertain) {
    asper_err recovery;
    if (st->journal_fp) {
      fclose(st->journal_fp);
      st->journal_fp = NULL;
    }
    recovery = asper_compact_recover(c);
    st->journal_fp = os_fopen(st->journal_path, "ab");
    st->journal_ops = journal_ops_before;
    if (recovery != ASPER_OK || !st->journal_fp) st->poisoned = true;
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
