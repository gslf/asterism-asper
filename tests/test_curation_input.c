/* Acknowledgements cover precisely the source events sent to the curator. */
#include "asper_test.h"
#include "curation_input.h"
#include "curation_receipt.h"
#include "fakes.h"

static fake_curator curator;
static fake_clock clock_state;
static asper_curator_iface delegate;
static asper_ctx *context;
static unsigned seen, duplicates;
static size_t per_call[32], next_event;
static bool append_during_count;
static int count_mode;

static asper_err capture(void *ud, const asmodel_input *input, const char *grammar,
    const asper_output_contract *contract, const asmodel_generate_params *params,
    volatile int *cancel, char **out) {
  asper_err e = delegate.generate(ud, input, grammar, contract, params, cancel, out);
  if (e != ASPER_OK || curator.calls > 32) return e;
  for (size_t i = 0; i < next_event && i < 32; i++) {
    char key[32]; snprintf(key, sizeof key, "event-%02zu", i);
    if (strstr(curator.last_user, key)) {
      unsigned bit = 1u << i;
      if (seen & bit) duplicates++;
      seen |= bit; per_call[curator.calls - 1]++;
    }
  }
  return e;
}
static int count(void *ud, const char *text) {
  if (append_during_count && strstr(text, "Conversation:")) {
    append_during_count = false;
    char value[32]; snprintf(value, sizeof value, "event-%02zu", next_event++);
    if (fake_event_append(context, "input", ASPER_EVENT_USER, value) != ASPER_OK) return -1;
  }
  if (count_mode == 1) return 0;
  if (count_mode == 2) return strstr(text, "event-01") ? 10000 : 1;
  return delegate.count_tokens(ud, text);
}
static asper_ctx *open_test(const char *root) {
  fake_curator_init(&curator); fake_clock_set(&clock_state, 1785319920);
  delegate = fake_curator_iface_make(&curator);
  asper_curator_iface cur = delegate; cur.generate = capture; cur.count_tokens = count;
  asper_embedder emb = fake_embedder_make(); asper_clock clk = fake_clock_make(&clock_state);
  asper_open_params p = {0}; p.memory_root = root;
  asper_ctx *c = NULL;
  if (asper_open_with(&p, &emb, &cur, &clk, &c) != ASPER_OK) return NULL;
  asper_worker_stop(c); c->no_threads = true; asper_set_logger(c, NULL, NULL);
  context = c; seen = duplicates = 0; next_event = 0; count_mode = 0;
  append_during_count = false; memset(per_call, 0, sizeof per_call); return c;
}
static int append(asper_ctx *c, char id[37]) {
  char value[32]; snprintf(value, sizeof value, "event-%02zu", next_event++);
  asper_event_input event = {.scope="input", .kind=ASPER_EVENT_USER, .text=value};
  return asper_event_append(c, &event, id) == ASPER_OK;
}

TEST(full_flush_covers_every_input_and_receipts_match_actual_prompts) {
  char root[256], ids[20][37]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root); ASSERT_TRUE(c != NULL);
  c->cfg.transcript_tokens = 40;
  for (size_t i = 0; i < 20; i++) ASSERT_TRUE(append(c, ids[i]));
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_EQ_INT(c->turns_n, 0); ASSERT_TRUE(curator.calls > 1);
  ASSERT_EQ_INT(seen, (1u << 20) - 1); ASSERT_EQ_INT(duplicates, 0);
  char *path = os_path_join(root, CURATION_HISTORY);
  asper_event *events = NULL; size_t n = 0, checked = 0; unsigned long long cursor = 0;
  ASSERT_OK(asper_event_log_page(path, "", 0, 32, &events, &n, &cursor)); free(path);
  ASSERT_EQ_INT(n, curator.calls);
  for (size_t i = 0; i < n; i++) {
    asmodel_json_value *receipt = curation_receipt_decode(events[i].text); ASSERT_TRUE(receipt != NULL);
    const asmodel_json_value *sources = asmodel_json_object_get(receipt, "sources");
    ASSERT_EQ_INT(asmodel_json_array_len(sources), per_call[i]);
    for (size_t j = 0; j < per_call[i]; j++) {
      ASSERT_TRUE(checked < 20);
      ASSERT_EQ_STR(asmodel_json_string_value(asmodel_json_array_at(sources, j)), ids[checked++]);
    }
    asmodel_json_free(receipt);
  }
  ASSERT_EQ_INT(checked, 20); asper_events_free(events, n);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(failure_restores_selected_prefix_before_omitted_tail_and_new_input) {
  char root[256], ids[3][37]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root); ASSERT_TRUE(c != NULL);
  c->cfg.transcript_tokens = 8;
  for (size_t i = 0; i < 3; i++) ASSERT_TRUE(append(c, ids[i]));
  append_during_count = true;
  fake_curator_fail_next(&curator, ASPER_ERR_MODEL);
  ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_MODEL);
  ASSERT_EQ_INT(c->turns_n, 4);
  for (size_t i = 0; i < 3; i++) ASSERT_EQ_STR(c->turns[i].source_id, ids[i]);
  ASSERT_EQ_STR(c->turns[3].text, "event-03");
  ASSERT_TRUE(c->store.curation_receipt == NULL);
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_EQ_INT(seen, 15); ASSERT_EQ_INT(duplicates, 0);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(oversized_first_input_is_not_forced_or_acknowledged) {
  char root[256], id[37]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root); ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(append(c, id)); c->cfg.transcript_tokens = 1;
  ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_LIMIT);
  ASSERT_EQ_INT(curator.calls, 0); ASSERT_EQ_INT(c->turns_n, 1);
  ASSERT_TRUE(c->store.curation_receipt == NULL);
  char *path = os_path_join(root, "curated-events.log"); ASSERT_TRUE(!os_file_exists(path)); free(path);
  c->cfg.transcript_tokens = 40; c->curator.count_tokens = NULL;
  ASSERT_OK(asper_curation_cycle(c, true)); ASSERT_EQ_INT(c->turns_n, 0);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(joined_counts_and_zero_counter_cannot_bypass_transcript_limits) {
  char root[256], id[37]; ASSERT_TRUE(asper_test_tmpdir(root));
  asper_ctx *c = open_test(root); ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(append(c, id)); ASSERT_TRUE(append(c, id));
  count_mode = 2; c->cfg.transcript_tokens = 10;
  ASSERT_OK(asper_curation_cycle(c, true));
  ASSERT_EQ_INT(seen, 1); ASSERT_EQ_INT(c->turns_n, 1);
  ASSERT_EQ_STR(c->turns[0].text, "event-01");
  ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_LIMIT);
  count_mode = 1;
  char *large = malloc(CURATION_TRANSCRIPT_BYTES + 1); ASSERT_TRUE(large != NULL);
  memset(large, 'x', CURATION_TRANSCRIPT_BYTES); large[CURATION_TRANSCRIPT_BYTES] = 0;
  asper_turn turn = {0}; turn.text = large; strcpy(turn.source_id, id);
  size_t included = 99; char *transcript = NULL;
  ASSERT_ERR(asper_curation_transcript(c, &turn, 1, &included, &transcript), ASPER_ERR_LIMIT);
  ASSERT_EQ_INT(included, 0); ASSERT_TRUE(transcript == NULL); free(large);
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST_LIST = {
  TEST_ENTRY(full_flush_covers_every_input_and_receipts_match_actual_prompts),
  TEST_ENTRY(failure_restores_selected_prefix_before_omitted_tail_and_new_input),
  TEST_ENTRY(oversized_first_input_is_not_forced_or_acknowledged),
  TEST_ENTRY(joined_counts_and_zero_counter_cannot_bypass_transcript_limits),
};
RUN_ALL_TESTS()
