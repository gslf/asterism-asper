/* Durable backlog stays on disk; admitted and in-flight inputs share one cap. */
#include "asper_test.h"
#include "fakes.h"
#include "source_internal.h"

static fake_curator curator;
static fake_clock clock_state;
static asper_curator_iface delegate;
static asper_ctx *context;
static uint64_t seen;
static size_t next_input, duplicate, append_on_generate, capacity_violations;

/* Opening a store may start the worker before the caller receives its handle. */
static asper_err startup_generate(void *ud, const asmodel_input *input, const char *grammar,
    const asper_output_contract *contract, const asmodel_generate_params *params,
    volatile int *cancel, char **out) {
  (void)ud; (void)input; (void)grammar; (void)contract; (void)params; (void)cancel;
  *out = NULL; return ASPER_ERR_BUSY;
}

static asper_err append(asper_ctx *c, const char *scope) {
  char text[32]; snprintf(text, sizeof text, "input-%03zu", next_input++);
  return fake_event_append(c, scope, ASPER_EVENT_USER, text);
}
static asper_err generate(void *ud, const asmodel_input *input, const char *grammar,
    const asper_output_contract *contract, const asmodel_generate_params *params,
    volatile int *cancel, char **out) {
  size_t extra = append_on_generate; append_on_generate = 0;
  for (size_t i = 0; i < extra; i++) {
    asper_err e = append(context, i % 2 ? "new-scope" : "inputs");
    if (e != ASPER_OK) return e;
    if (context->turns_cap < context->turns_n + context->turns_inflight) capacity_violations++;
  }
  asper_err e = delegate.generate(ud, input, grammar, contract, params, cancel, out);
  if (e != ASPER_OK) return e;
  for (size_t i = 0; i < next_input && i < 64; i++) {
    char text[32]; snprintf(text, sizeof text, "input-%03zu", i);
    if (strstr(curator.last_user, text)) {
      uint64_t bit = (uint64_t)1 << i;
      if (seen & bit) duplicate++;
      seen |= bit;
    }
  }
  return ASPER_OK;
}
static asper_ctx *open_test(const char *root, size_t cap) {
  char path[512], config[128]; snprintf(path, sizeof path, "%s/queue.xcdn", root);
  snprintf(config, sizeof config, "#asper_config {curation:{event_queue_max:%zu}}", cap);
  if (os_write_file(path, config, strlen(config)) != ASPER_OK) return NULL;
  fake_curator_init(&curator); fake_clock_set(&clock_state, 1785319920);
  delegate = fake_curator_iface_make(&curator);
  asper_curator_iface cur = delegate; cur.generate = startup_generate;
  asper_embedder emb = fake_embedder_make(); asper_clock clk = fake_clock_make(&clock_state);
  asper_open_params p = {0}; p.memory_root = root; p.config_path = path;
  asper_ctx *c = NULL;
  if (asper_open_with(&p, &emb, &cur, &clk, &c) != ASPER_OK) return NULL;
  asper_worker_stop(c); c->no_threads = true; asper_set_logger(c, NULL, NULL);
  c->curator.generate = generate;
  context = c; seen = 0; duplicate = next_input = append_on_generate = capacity_violations = 0;
  return c;
}
static int bounded(asper_ctx *c, size_t cap) {
  asper_stats status;
  return asper_get_stats(c, &status) == ASPER_OK &&
      status.curation_queued + status.curation_inflight <= cap &&
      status.curation_bytes <= ASPER_QUEUE_BYTES && status.curation_queue_limit == cap;
}

TEST(restart_admits_a_bounded_prefix_and_full_flush_reaches_every_scope) {
  char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root, 3); ASSERT_TRUE(c != NULL);
  for (size_t i = 0; i < 24; i++) {
    ASSERT_OK(append(c, i % 3 == 0 ? "second" : "inputs")); ASSERT_TRUE(bounded(c, 3));
  }
  ASSERT_EQ_INT(c->turns_n, 3); ASSERT_TRUE(c->source_backlog);
  asper_close(c); fake_curator_dispose(&curator);
  c = open_test(root, 3); ASSERT_TRUE(c != NULL); next_input = 24;
  ASSERT_TRUE(bounded(c, 3)); ASSERT_EQ_INT(c->turns_n, 3);
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_EQ_INT(seen, ((uint64_t)1 << 24) - 1); ASSERT_EQ_INT(duplicate, 0);
  ASSERT_EQ_INT(c->turns_n, 0); ASSERT_EQ_INT(c->turns_bytes, 0);
  ASSERT_EQ_INT(c->turns_inflight, 0); ASSERT_EQ_INT(c->turns_inflight_bytes, 0);
  ASSERT_TRUE(!c->source_backlog);
  asper_close(c); fake_curator_dispose(&curator);
  c = open_test(root, 3); ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_flush(c, 1)); ASSERT_EQ_INT(curator.calls, 0);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(full_flush_keeps_concurrent_sources_for_the_next_snapshot) {
  char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root, 2); ASSERT_TRUE(c != NULL);
  for (size_t i = 0; i < 5; i++) ASSERT_OK(append(c, "inputs"));
  append_on_generate = 2;
  ASSERT_OK(asper_flush(c, 1)); ASSERT_EQ_INT(seen, 31);
  ASSERT_TRUE(c->source_backlog); ASSERT_TRUE(bounded(c, 2));
  ASSERT_OK(asper_flush(c, 1)); ASSERT_EQ_INT(seen, 127);
  ASSERT_EQ_INT(duplicate, 0); ASSERT_TRUE(!c->source_backlog);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(failed_generation_restores_its_reserved_slots_before_new_durable_inputs) {
  char root[256], ids[2][37]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root, 2); ASSERT_TRUE(c != NULL);
  ASSERT_OK(append(c, "inputs")); ASSERT_OK(append(c, "inputs"));
  for (size_t i = 0; i < 2; i++) strcpy(ids[i], c->turns[i].source_id);
  append_on_generate = 2; fake_curator_fail_next(&curator, ASPER_ERR_MODEL);
  ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_MODEL);
  ASSERT_TRUE(bounded(c, 2)); ASSERT_EQ_INT(c->turns_n, 2);
  ASSERT_EQ_INT(c->turns_inflight, 0);
  for (size_t i = 0; i < 2; i++) ASSERT_EQ_STR(c->turns[i].source_id, ids[i]);
  ASSERT_OK(asper_flush(c, 1)); ASSERT_EQ_INT(seen, 15);
  ASSERT_EQ_INT(duplicate, 0);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(byte_pressure_preserves_backlog_and_oversized_transcripts_do_not_run) {
  char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root, 256); ASSERT_TRUE(c != NULL);
  size_t bytes = 10u * 1024u * 1024u;
  char *text = malloc(bytes + 1); ASSERT_TRUE(text != NULL);
  memset(text, 'x', bytes); text[bytes] = 0;
  for (size_t i = 0; i < 4; i++) {
    ASSERT_OK(fake_event_append(c, "large", ASPER_EVENT_USER, text));
    ASSERT_TRUE(bounded(c, 256));
  }
  free(text); ASSERT_EQ_INT(c->turns_n, 3);
  ASSERT_EQ_INT(c->turns_bytes, 3 * (bytes + 1)); ASSERT_TRUE(c->source_backlog);
  ASSERT_ERR(asper_flush(c, 1), ASPER_ERR_LIMIT); ASSERT_EQ_INT(curator.calls, 0);
  ASSERT_TRUE(bounded(c, 256)); ASSERT_EQ_INT(c->turns_inflight_bytes, 0);
  asper_close(c); fake_curator_dispose(&curator);
  c = open_test(root, 256); ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(c->turns_n, 3); ASSERT_TRUE(bounded(c, 256));
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(new_admission_reserves_array_capacity_for_retry_without_another_allocation) {
  char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root, 32); ASSERT_TRUE(c != NULL);
  for (size_t i = 0; i < 16; i++) ASSERT_OK(append(c, "inputs"));
  append_on_generate = 16; fake_curator_fail_next(&curator, ASPER_ERR_MODEL);
  ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_MODEL);
  ASSERT_EQ_INT(capacity_violations, 0); ASSERT_EQ_INT(c->turns_n, 32);
  ASSERT_TRUE(bounded(c, 32)); ASSERT_EQ_INT(c->turns_inflight, 0);
  ASSERT_OK(asper_flush(c, 1)); ASSERT_EQ_INT(seen, ((uint64_t)1 << 32) - 1);
  ASSERT_EQ_INT(duplicate, 0);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(deferred_corrupt_payload_is_an_error_and_never_acknowledged) {
  char root[256], path[512]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root, 1); ASSERT_TRUE(c != NULL);
  for (size_t i = 0; i < 3; i++) ASSERT_OK(append(c, "inputs"));
  snprintf(path, sizeof path, "%s/scopes/inputs/events.log", root);
  char *data = NULL; size_t bytes = 0; ASSERT_OK(os_read_file(path, &data, &bytes));
  char *payload = strstr(data, "input-001"); ASSERT_TRUE(payload != NULL);
  payload[6] = '9'; ASSERT_OK(os_write_file(path, data, bytes)); free(data);
  ASSERT_ERR(asper_flush(c, 1), ASPER_ERR_PARSE);
  ASSERT_EQ_INT(seen, 1); ASSERT_EQ_INT(c->turns_n, 0); ASSERT_TRUE(c->source_backlog);
  char (*ids)[37] = NULL; size_t n = 0;
  ASSERT_OK(asper_source_curated_load(c, &ids, &n)); ASSERT_EQ_INT(n, 1); free(ids);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(queue_configuration_has_an_enforced_range) {
  char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
  ASSERT_TRUE(open_test(root, 0) == NULL); fake_curator_dispose(&curator);
  ASSERT_TRUE(open_test(root, ASPER_QUEUE_EVENTS + 1) == NULL); fake_curator_dispose(&curator);
  asper_test_rmtree(root);
}

TEST(deferred_inputs_keep_their_original_project_across_restart) {
  char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root, 1); ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_project_select(c, "first"));
  ASSERT_OK(append(c, "inputs")); ASSERT_OK(append(c, "inputs"));
  ASSERT_OK(asper_project_select(c, "second")); ASSERT_OK(append(c, "inputs"));
  asper_close(c); fake_curator_dispose(&curator);
  c = open_test(root, 1); ASSERT_TRUE(c != NULL); next_input = 3;
  for (size_t i = 0; i < 3; i++) {
    ASSERT_OK(asper_source_replay_pending(c)); ASSERT_EQ_INT(c->turns_n, 1);
    ASSERT_EQ_STR(c->turns[0].project, i < 2 ? "first" : "second");
    ASSERT_OK(asper_curation_cycle(c, true));
  }
  ASSERT_EQ_INT(seen, 7); ASSERT_EQ_INT(duplicate, 0);
  /* An opaque caller-supplied source object remains unchanged and unscoped. */
  char ref[72]; ASSERT_OK(asper_object_put(c, "raw", 3, ref));
  asper_event_input input = {"inputs", ASPER_EVENT_USER, "opaque origin", ref, 0};
  ASSERT_OK(asper_event_append(c, &input, NULL)); ASSERT_EQ_INT(c->turns_n, 1);
  ASSERT_EQ_STR(c->turns[0].project, "");
  asper_event *events = NULL; size_t n = 0;
  ASSERT_OK(asper_event_list(c, "inputs", &events, &n)); ASSERT_EQ_INT(n, 4);
  ASSERT_EQ_STR(events[3].object_ref, ref); asper_events_free(events, n);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(missing_or_malformed_origin_cannot_be_silently_treated_as_unscoped) {
  for (int missing = 0; missing < 2; missing++) {
    char root[256], ref[72]; ASSERT_TRUE(asper_test_tmpdir(root));
    asper_ctx *c = open_test(root, 1); ASSERT_TRUE(c != NULL);
    if (missing) {
      strcpy(ref, "sha256:"); memset(ref + 7, '0', 64); ref[71] = 0;
    } else {
      const char *bad = "#turn {project: 42}";
      ASSERT_OK(asper_object_put(c, bad, strlen(bad), ref));
    }
    asper_event_input input = {"inputs", ASPER_EVENT_USER, "unvalidated origin", ref, 0};
    ASSERT_OK(asper_event_append(c, &input, NULL)); ASSERT_EQ_INT(c->turns_n, 0);
    ASSERT_ERR(asper_flush(c, 1), missing ? ASPER_ERR_NOT_FOUND : ASPER_ERR_PARSE);
    ASSERT_EQ_INT(curator.calls, 0); ASSERT_TRUE(c->source_backlog);
    asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
  }
}

#ifndef ASPER_NO_THREADS
typedef struct { asper_ctx *ctx; size_t number; asper_err error; } producer;
static void *produce(void *arg) {
  producer *p = arg; char scope[32], text[32];
  snprintf(scope, sizeof scope, "parallel-%zu", p->number);
  for (size_t i = 0; i < 12 && p->error == ASPER_OK; i++) {
    snprintf(text, sizeof text, "input-%03zu", p->number * 12 + i);
    p->error = fake_event_append(p->ctx, scope, ASPER_EVENT_USER, text);
  }
  return NULL;
}
TEST(concurrent_producers_share_capacity_and_do_not_duplicate_source_admission) {
  char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root, 3); ASSERT_TRUE(c != NULL);
  producer producers[4]; os_thread threads[4];
  for (size_t i = 0; i < 4; i++) {
    producers[i] = (producer){c, i, ASPER_OK};
    ASSERT_OK(os_thread_start(&threads[i], produce, &producers[i]));
  }
  for (size_t i = 0; i < 4; i++) os_thread_join(&threads[i]);
  for (size_t i = 0; i < 4; i++) ASSERT_OK(producers[i].error);
  next_input = 48; ASSERT_TRUE(bounded(c, 3));
  ASSERT_OK(asper_flush(c, 1)); ASSERT_EQ_INT(seen, ((uint64_t)1 << 48) - 1);
  ASSERT_EQ_INT(duplicate, 0); ASSERT_TRUE(!c->source_backlog);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(worker_drains_disk_backlog_without_a_foreground_flush) {
  char root[256]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root, 2); ASSERT_TRUE(c != NULL);
  for (size_t i = 0; i < 20; i++) ASSERT_OK(append(c, "inputs"));
  c->cfg.turn_batch = 1;
  ASSERT_OK(asper_worker_start(c));
  asper_stats status = {0}; int64_t deadline = os_monotonic_ms() + 10000;
  do {
    os_mutex_lock(&c->ev_mu);
    os_cond_timedwait(&c->done_cv, &c->ev_mu, 10);
    os_mutex_unlock(&c->ev_mu);
    ASSERT_OK(asper_get_stats(c, &status));
  } while ((status.curation_queued || status.curation_inflight || status.curation_backlog) &&
           os_monotonic_ms() < deadline);
  asper_worker_stop(c);
  ASSERT_EQ_INT(seen, ((uint64_t)1 << 20) - 1); ASSERT_EQ_INT(duplicate, 0);
  ASSERT_EQ_INT(status.curation_bytes, 0); ASSERT_TRUE(!status.curation_backlog);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}
#endif

TEST_LIST = {
  TEST_ENTRY(restart_admits_a_bounded_prefix_and_full_flush_reaches_every_scope),
  TEST_ENTRY(full_flush_keeps_concurrent_sources_for_the_next_snapshot),
  TEST_ENTRY(failed_generation_restores_its_reserved_slots_before_new_durable_inputs),
  TEST_ENTRY(byte_pressure_preserves_backlog_and_oversized_transcripts_do_not_run),
  TEST_ENTRY(new_admission_reserves_array_capacity_for_retry_without_another_allocation),
  TEST_ENTRY(deferred_corrupt_payload_is_an_error_and_never_acknowledged),
  TEST_ENTRY(queue_configuration_has_an_enforced_range),
  TEST_ENTRY(deferred_inputs_keep_their_original_project_across_restart),
  TEST_ENTRY(missing_or_malformed_origin_cannot_be_silently_treated_as_unscoped),
#ifndef ASPER_NO_THREADS
  TEST_ENTRY(concurrent_producers_share_capacity_and_do_not_duplicate_source_admission),
  TEST_ENTRY(worker_drains_disk_backlog_without_a_foreground_flush),
#endif
};
RUN_ALL_TESTS()
