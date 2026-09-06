/* Real process exits exercise the boundary between effects and acknowledgement. */
#include "asper_test.h"
#include "curation_receipt.h"
#include "store_files.h"
#include "fakes.h"
#ifndef _WIN32
#include <sys/wait.h>
#endif

static fake_curator curator;
static fake_clock clock_state;
static int stop_stage;
static asper_ctx *fault_context;
static int completed_ops;
static asper_err injected_error(int stage) { return stage == stop_stage ? ASPER_ERR_IO : ASPER_OK; }
static asper_err uncertain_journal(int stage) {
  if (stage == 2 && ++completed_ops == 2) fault_context->store.journal_fault = 3;
  return ASPER_OK;
}
#ifndef _WIN32
static asper_err crash(int stage) { if (stage == stop_stage) _exit(80 + stage); return ASPER_OK; }
#endif

static asper_ctx *open_test(const char *root) {
  asper_open_params p = {0}; p.memory_root = root;
  asper_ctx *c = NULL;
  asper_embedder emb = fake_embedder_make();
  asper_curator_iface cur = fake_curator_iface_make(&curator);
  fake_clock_set(&clock_state, 1785319920);
  asper_clock clk = fake_clock_make(&clock_state);
  if (asper_open_with(&p, &emb, &cur, &clk, &c) != ASPER_OK) return NULL;
  asper_worker_stop(c); c->no_threads = true;
  c->cfg.dup_threshold = 2; /* Isolate receipt tests from semantic dedup. */
  asper_set_logger(c, NULL, NULL); return c;
}
static int prepare(asper_ctx *c) {
  return fake_curator_push(&curator, "INSERT context | Otters inhabit freshwater rivers\n"
      "INSERT context | Copper conducts electrical current\n") &&
      fake_event_append(c, "receipt", ASPER_EVENT_USER, "retain both facts") == ASPER_OK &&
      fake_event_append(c, "receipt", ASPER_EVENT_ASSISTANT, "observed") == ASPER_OK;
}
static size_t records(asper_ctx *c) {
  asper_record **rows = NULL; size_t n = 0;
  if (asper_memory_list(c, ASPER_SECTION_ANY, NULL, 1, &rows, &n) != ASPER_OK) return SIZE_MAX;
  asper_records_free(rows, n); return n;
}
static size_t receipts(asper_ctx *c) {
  char *path = os_path_join(c->store.root, CURATION_HISTORY);
  asper_event *rows = NULL; size_t n = 0; unsigned long long next = 0;
  asper_err e = asper_event_log_page(path, "", 0, 100, &rows, &n, &next);
  free(path);
  for (size_t i = 0; e == ASPER_OK && i < n; i++) {
    asmodel_json_value *decoded = curation_receipt_decode(rows[i].text);
    if (!decoded) e = ASPER_ERR_PARSE;
    asmodel_json_free(decoded);
  }
  asper_events_free(rows, n); return e == ASPER_OK ? n : SIZE_MAX;
}

TEST(interrupted_batches_never_regenerate_and_completed_receipts_recover) {
#ifndef _WIN32
  for (stop_stage = 1; stop_stage <= 7; stop_stage++) {
    char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
    pid_t pid = fork(); ASSERT_TRUE(pid >= 0);
    if (!pid) {
      fake_curator_init(&curator);
      asper_ctx *c = open_test(root);
      if (!c || !prepare(c)) _exit(30);
      c->store.curation_checkpoint = crash;
      (void)asper_curation_cycle(c, true); _exit(31);
    }
    int status = 0; ASSERT_EQ_INT(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)); ASSERT_EQ_INT(WEXITSTATUS(status), 80 + stop_stage);
    fake_curator_init(&curator);
    asper_ctx *c = open_test(root); ASSERT_TRUE(c != NULL);
    size_t expected = stop_stage == 1 ? 0 : stop_stage == 2 ? 1 : 2;
    ASSERT_EQ_INT(records(c), expected);
    int calls = curator.calls;
    if (stop_stage < 4) {
      ASSERT_ERR(asper_flush(c, 1), ASPER_ERR_BUSY);
      ASSERT_TRUE(c->store.curation_receipt != NULL);
      ASSERT_ERR(asper_store_compact(c), ASPER_ERR_BUSY);
      ASSERT_EQ_INT(c->turns_n, 0);
      ASSERT_EQ_INT(receipts(c), 0);
    } else {
      ASSERT_OK(asper_flush(c, 1));
      ASSERT_EQ_INT(receipts(c), 1);
      ASSERT_TRUE(c->store.curation_receipt == NULL);
    }
    ASSERT_EQ_INT(curator.calls, calls);
    asper_close(c);
    c = open_test(root); ASSERT_TRUE(c != NULL);
    ASSERT_EQ_INT(records(c), expected);
    ASSERT_EQ_INT(receipts(c), stop_stage < 4 ? 0 : 1);
    asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
  }
#endif
}

TEST(live_receipt_errors_suspend_retry_and_preserve_the_journal) {
  for (stop_stage = 1; stop_stage <= 7; stop_stage++) {
    char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
    fake_curator_init(&curator);
    asper_ctx *c = open_test(root); ASSERT_TRUE(c && prepare(c));
    c->store.curation_checkpoint = injected_error;
    ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_IO);
    ASSERT_EQ_INT(curator.calls, 1);
    ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_BUSY);
    ASSERT_ERR(asper_store_compact(c), ASPER_ERR_BUSY);
    ASSERT_EQ_INT(curator.calls, 1);
    c->store.curation_checkpoint = NULL;
    asper_close(c);
    c = open_test(root); ASSERT_TRUE(c != NULL);
    ASSERT_EQ_INT(records(c), stop_stage == 1 ? 0 : stop_stage == 2 ? 1 : 2);
    ASSERT_EQ_INT(receipts(c), stop_stage < 4 ? 0 : 1);
    asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
  }
}

TEST(malformed_guard_fails_open_and_atomic_ack_is_idempotent) {
  char root[256], path[512]; ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&curator);
  asper_ctx *c = open_test(root); ASSERT_TRUE(c && prepare(c));
  stop_stage = 1; c->store.curation_checkpoint = injected_error;
  ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_IO);
  asper_turn turns[2] = {0};
  for (size_t i = 0; i < 2; i++) strcpy(turns[i].source_id, c->turns[i].source_id);
  ASSERT_OK(asper_source_mark_curated(c, turns, 2));
  ASSERT_OK(asper_source_mark_curated(c, turns, 2));
  snprintf(path, sizeof path, "%s/curated-events.log", root);
  uint64_t size = 0; ASSERT_OK(os_file_size(path, &size)); ASSERT_EQ_INT(size, 74);
  c->store.curation_checkpoint = NULL;
  asper_close(c);
  snprintf(path, sizeof path, "%s/curation.pending", root);
  ASSERT_OK(os_write_file(path, "", 0));
  ASSERT_TRUE(open_test(root) == NULL);
  fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(receipt_schema_rejects_forged_completion_and_embedded_nul) {
  char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&curator);
  asper_ctx *c = open_test(root); ASSERT_TRUE(c && prepare(c));
  stop_stage = 1; c->store.curation_checkpoint = injected_error;
  ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_IO);
  asmodel_json_value *o = asmodel_json_clone(c->store.curation_receipt);
  ASSERT_TRUE(o != NULL);
  ASSERT_EQ_INT(asmodel_json_object_set(o, "outcome", asmodel_json_string("processed")), 0);
  char *text = asmodel_json_write(o, 0); ASSERT_TRUE(text != NULL);
  ASSERT_TRUE(curation_receipt_decode(text) == NULL);
  free(text); asmodel_json_free(o);
  text = asmodel_json_write(c->store.curation_receipt, 0); ASSERT_TRUE(text != NULL);
  /* Keep valid JSON but inject a decoded NUL into a trusted scope field. */
  char *scope = strstr(text, "\"scope\":\"receipt\""); ASSERT_TRUE(scope != NULL);
  memcpy(scope + strlen("\"scope\":\""), "\\u0000x", 7);
  ASSERT_TRUE(curation_receipt_decode(text) == NULL);
  free(text);
  c->store.curation_checkpoint = NULL;
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(full_flush_drains_bounded_batches_and_history_quota_precedes_inference) {
  char root[256], path[512]; ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&curator);
  asper_ctx *c = open_test(root); ASSERT_TRUE(c != NULL);
  for (size_t i = 0; i <= CURATION_BATCH_MAX; i++)
    ASSERT_OK(fake_event_append(c, "many", ASPER_EVENT_USER, "one observed event"));
  c->no_threads = false; /* Exercise the threaded host's full-flush entry path. */
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_EQ_INT(c->turns_n, 0); ASSERT_EQ_INT(curator.calls, 2);
  ASSERT_EQ_INT(receipts(c), 2);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);

  ASSERT_TRUE(asper_test_tmpdir(root)); fake_curator_init(&curator);
  c = open_test(root); ASSERT_TRUE(c && prepare(c));
  snprintf(path, sizeof path, "%s/curation.events", root);
  ASSERT_OK(os_write_file(path, "", 0));
  ASSERT_OK(os_truncate(path, (uint64_t)ASPER_EVENT_LOG_BYTES + 1));
  ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_LIMIT);
  ASSERT_EQ_INT(curator.calls, 0); ASSERT_EQ_INT(c->turns_n, 2);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(checked_replacements_and_backups_do_not_follow_temporary_aliases) {
#ifndef _WIN32
  char root[256], target[512], alias[520], outside[512];
  ASSERT_TRUE(asper_test_tmpdir(root));
  snprintf(target, sizeof target, "%s/record.xcdn", root);
  snprintf(alias, sizeof alias, "%s.tmp", target);
  snprintf(outside, sizeof outside, "%s/external.txt", root);
  ASSERT_OK(os_write_file(outside, "keep", 4));
  ASSERT_EQ_INT(symlink(outside, alias), 0);
  ASSERT_TRUE(os_blob_create(alias) == NULL);
  ASSERT_OK(asper_store_file_write(NULL, target, "checked", 7, true));
  char *data = asper_test_read_file(outside, NULL);
  ASSERT_EQ_STR(data, "keep"); free(data);
  char *decoded = NULL;
  ASSERT_ERR(asper_store_file_read(alias, false, 100, &decoded, NULL), ASPER_ERR_INVALID);
  ASSERT_ERR(asper_store_file_copy(alias, NULL, NULL, NULL), ASPER_ERR_INVALID);
  ASSERT_OK(asper_store_file_copy(target, outside, NULL, NULL));
  ASSERT_OK(asper_store_file_read(outside, true, 100, &decoded, NULL));
  ASSERT_EQ_STR(decoded, "checked"); free(decoded);
  asper_test_rmtree(root);
#endif
}

TEST(uncertain_journal_sync_cannot_produce_a_completed_receipt) {
  for (int policy = ASPER_SYNC_BATCH; policy <= ASPER_SYNC_NEVER; policy++) {
    char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
    fake_curator_init(&curator);
    asper_ctx *c = open_test(root); ASSERT_TRUE(c && prepare(c));
    c->cfg.journal_sync = policy; completed_ops = 0; fault_context = c;
    c->store.curation_checkpoint = uncertain_journal;
    ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_IO);
    ASSERT_TRUE(c->store.poisoned);
    ASSERT_EQ_INT(receipts(c), 0);
    c->store.curation_checkpoint = NULL; asper_close(c);
    c = open_test(root); ASSERT_TRUE(c != NULL);
    ASSERT_EQ_INT(records(c), 2);
    ASSERT_ERR(asper_flush(c, 1), ASPER_ERR_BUSY);
    ASSERT_EQ_INT(curator.calls, 1);
    asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
  }
}

TEST_LIST = {
  TEST_ENTRY(interrupted_batches_never_regenerate_and_completed_receipts_recover),
  TEST_ENTRY(live_receipt_errors_suspend_retry_and_preserve_the_journal),
  TEST_ENTRY(malformed_guard_fails_open_and_atomic_ack_is_idempotent),
  TEST_ENTRY(receipt_schema_rejects_forged_completion_and_embedded_nul),
  TEST_ENTRY(full_flush_drains_bounded_batches_and_history_quota_precedes_inference),
  TEST_ENTRY(checked_replacements_and_backups_do_not_follow_temporary_aliases),
  TEST_ENTRY(uncertain_journal_sync_cannot_produce_a_completed_receipt),
};
RUN_ALL_TESTS()
