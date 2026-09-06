/* Durable intent precedes mutations; completion precedes source acknowledgement. */
#include "curation_receipt.h"
#include "store_files.h"
#include <stdlib.h>
#include <string.h>

static int put(asmodel_json_value *o, const char *key, const char *value) {
  return asmodel_json_object_set(o, key, asmodel_json_string(value));
}
static int number(asmodel_json_value *o, const char *key, size_t n) {
  return n > INT64_MAX ? -1 : asmodel_json_object_set(o, key, asmodel_json_int((long long)n));
}
static const char *value(const asmodel_json_value *o, const char *key) {
  return asmodel_json_string_value(asmodel_json_object_get(o, key));
}
static asper_err persist(asper_ctx *c, const asmodel_json_value *receipt) {
  char *text = asmodel_json_write(receipt, 0);
  char *path = os_path_join(c->store.root, CURATION_PENDING);
  asmodel_json_value *checked = text ? curation_receipt_decode(text) : NULL;
  asper_err e = !path || !text ? ASPER_ERR_NOMEM : strlen(text) > CURATION_RECEIPT_MAX ?
      ASPER_ERR_LIMIT : !checked ? ASPER_ERR_PARSE : asper_store_file_write(c, path, text, strlen(text), true);
  asmodel_json_free(checked); free(path); free(text); return e;
}
asper_err asper_curation_checkpoint(asper_ctx *c, int stage) {
  return c->store.curation_checkpoint ? c->store.curation_checkpoint(stage) : ASPER_OK;
}
asper_err asper_curation_guard(asper_ctx *c) {
  os_rwlock_rdlock(&c->lock);
  bool pending = c->store.curation_receipt != NULL;
  os_rwlock_rdunlock(&c->lock);
  return pending ? asper_seterr(c, ASPER_ERR_BUSY,
      "curation suspended; close the host and inspect curation.pending with scripts/store.py") : ASPER_OK;
}

asper_err asper_curation_admit(asper_ctx *c, size_t sources) {
  asper_err e = asper_source_curated_admit(c, sources);
  if (e != ASPER_OK) return e;
  char *path = os_path_join(c->store.root, CURATION_HISTORY);
  if (!path) return ASPER_ERR_NOMEM;
  asper_event_files fs;
  e = asper_event_files_open(&fs, path);
  if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  if (e == ASPER_OK && fs.bytes > ASPER_EVENT_LOG_BYTES - CURATION_RECEIPT_MAX - 512)
    e = ASPER_ERR_LIMIT;
  asper_event_files_close(&fs); free(path); return e;
}

/* Idempotent terminal history append. A retained guard prevents another cycle
 * or compaction, so an uncertain append can only be this log's final event. */
static asper_err archive_receipt(asper_ctx *c, const char *text) {
  char *path = os_path_join(c->store.root, CURATION_HISTORY);
  if (!path) return ASPER_ERR_NOMEM;
  asper_event_files fs;
  asper_err e = asper_event_files_open(&fs, path);
  bool present = false;
  if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  if (e == ASPER_OK && fs.count) {
    asper_event last;
    e = asper_event_files_read(&fs, path, fs.count, &last);
    if (e == ASPER_OK && !strcmp(last.id, value(c->store.curation_receipt, "id"))) {
      present = true;
      if (strcmp(last.text, text)) e = ASPER_ERR_PARSE;
      else e = os_fsync(fs.log);
    }
    free(last.text);
  }
  asper_event_files_close(&fs);
  if (e == ASPER_OK && !present) {
    asper_event event = {0}; event.kind = ASPER_EVENT_DIAGNOSTIC; event.text = (char *)text;
    strcpy(event.id, value(c->store.curation_receipt, "id"));
    e = asper_event_log_append(path, &event);
  }
  free(path); return e;
}

/* Called with the store write lock, or during open before threads exist. */
static asper_err finalize(asper_ctx *c) {
  asmodel_json_value *receipt = c->store.curation_receipt;
  const asmodel_json_value *sources = asmodel_json_object_get(receipt, "sources");
  size_t n = asmodel_json_array_len(sources);
  asper_turn *turns = calloc(n, sizeof *turns);
  char *text = asmodel_json_write(receipt, 0);
  char *path = os_path_join(c->store.root, CURATION_PENDING);
  asper_err e = !turns || !text || !path ? ASPER_ERR_NOMEM : ASPER_OK;
  for (size_t i = 0; e == ASPER_OK && i < n; i++)
    strcpy(turns[i].source_id, asmodel_json_string_value(asmodel_json_array_at(sources, i)));
  if (e == ASPER_OK) e = archive_receipt(c, text);
  if (e == ASPER_OK) e = asper_curation_checkpoint(c, 5);
  if (e == ASPER_OK) e = asper_source_mark_curated(c, turns, n);
  if (e == ASPER_OK) e = asper_curation_checkpoint(c, 6);
  if (e == ASPER_OK) e = os_remove_file(path);
  if (e == ASPER_OK) e = os_sync_parent(path);
  if (e == ASPER_OK) e = asper_curation_checkpoint(c, 7);
  if (e == ASPER_OK) { asmodel_json_free(receipt); c->store.curation_receipt = NULL; }
  free(path); free(text); free(turns); return e;
}

asper_err asper_curation_recover(asper_ctx *c) {
  char *path = os_path_join(c->store.root, CURATION_PENDING), *text = NULL;
  if (!path) return ASPER_ERR_NOMEM;
  asper_err e = asper_store_file_read(path, true, CURATION_RECEIPT_MAX, &text, NULL);
  free(path);
  if (e == ASPER_ERR_NOT_FOUND) return ASPER_OK;
  if (e == ASPER_OK) {
    c->store.curation_receipt = curation_receipt_decode(text);
    if (!c->store.curation_receipt) e = ASPER_ERR_PARSE;
    else if (strcmp(value(c->store.curation_receipt, "outcome"), "prepared")) e = finalize(c);
    else asper_log(c, ASPER_LOG_WARN, "curator",
        "interrupted batch %s requires offline reconciliation; no curation will be replayed",
        value(c->store.curation_receipt, "id"));
  }
  free(text); return e;
}

asper_err asper_curation_begin(asper_ctx *c, const asper_turn *turns, size_t n,
    asper_record *const *handles, size_t handles_n, const char *proposal) {
  if (!n || n > CURATION_BATCH_MAX || handles_n > 12 || strlen(proposal) > 65536) return ASPER_ERR_LIMIT;
  asmodel_json_value *o = asmodel_json_object();
  char id[37]; asper_uuid_v4(id);
  int bad = !o || number(o, "schema", 1) || put(o, "id", id) || put(o, "scope", turns[0].scope) ||
      put(o, "project", turns[0].project) || number(o, "created_at", (size_t)asper_clock_now(&c->clock)) ||
      put(o, "proposal", proposal) || put(o, "outcome", "prepared") || put(o, "resolution_note", "") ||
      asmodel_json_object_set(o, "counts", asmodel_json_null()) ||
      asmodel_json_object_set(o, "journal_ops_after", asmodel_json_null());
  for (size_t group = 0; !bad && group < 2; group++) {
    asmodel_json_value *a = asmodel_json_array();
    if (!a) { bad = 1; break; }
    for (size_t i = 0; !bad && i < (group ? handles_n : n); i++)
      bad = asmodel_json_array_push(a, asmodel_json_string(group ? handles[i]->id : turns[i].source_id));
    if (bad) asmodel_json_free(a);
    else bad = asmodel_json_object_set(o, group ? "handles" : "sources", a);
  }
  asper_err e = bad ? ASPER_ERR_NOMEM : asper_journal_sync(c);
  os_rwlock_wrlock(&c->lock);
  if (e == ASPER_OK && (c->store.curation_receipt || c->store.poisoned)) e = ASPER_ERR_BUSY;
  if (e == ASPER_OK) {
    long bytes = ftell(c->store.journal_fp);
    if (bytes < 0) e = ASPER_ERR_IO;
    else if (number(o, "journal_ops_before", c->store.journal_ops) ||
             number(o, "journal_bytes_before", (size_t)bytes)) e = ASPER_ERR_NOMEM;
  }
  if (e == ASPER_OK) {
    /* Keep the live guard even on uncertain persistence; no operation follows. */
    c->store.curation_receipt = o; o = NULL;
    e = persist(c, c->store.curation_receipt);
  }
  os_rwlock_wrunlock(&c->lock); asmodel_json_free(o);
  return e == ASPER_OK ? asper_curation_checkpoint(c, 1) : e;
}

asper_err asper_curation_finish(asper_ctx *c, size_t applied, size_t rejected, size_t pending) {
  os_rwlock_wrlock(&c->lock);
  if (c->store.poisoned) { os_rwlock_wrunlock(&c->lock); return ASPER_ERR_IO; }
  asmodel_json_value *o = asmodel_json_clone(c->store.curation_receipt);
  asmodel_json_value *counts = asmodel_json_object();
  int bad = !o || !counts || number(counts, "applied", applied) ||
      number(counts, "rejected", rejected) || number(counts, "pending", pending);
  if (bad) asmodel_json_free(counts);
  else bad = asmodel_json_object_set(o, "counts", counts) || put(o, "outcome", "processed") ||
      number(o, "journal_ops_after", c->store.journal_ops);
  asper_err e = bad ? ASPER_ERR_NOMEM : persist(c, o);
  if (e == ASPER_OK) {
    asmodel_json_free(c->store.curation_receipt); c->store.curation_receipt = o; o = NULL;
    e = asper_curation_checkpoint(c, 4);
    if (e == ASPER_OK) e = finalize(c);
  }
  asmodel_json_free(o); os_rwlock_wrunlock(&c->lock); return e;
}
