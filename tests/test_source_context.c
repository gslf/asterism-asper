/* Selection must keep old pins, recent evidence and bounded payload ownership. */
#include "asper_test.h"
#include "fakes.h"
#include "source_internal.h"

static fake_clock clock_state;
static fake_curator curator;

static asper_ctx *open_fixture(char root[256]) {
  if (!asper_test_tmpdir(root)) return NULL;
  fake_clock_set(&clock_state, 1785319920LL); fake_curator_init(&curator);
  asper_embedder emb = fake_embedder_make();
  asper_curator_iface cur = fake_curator_iface_make(&curator);
  asper_clock clock = fake_clock_make(&clock_state);
  asper_open_params params = {0}; params.memory_root = root;
  asper_ctx *c = NULL;
  if (asper_open_with(&params, &emb, &cur, &clock, &c) != ASPER_OK) return NULL;
  asper_worker_stop(c); asper_set_logger(c, NULL, NULL); return c;
}
static void close_fixture(asper_ctx *c, const char *root) {
  asper_close(c); fake_curator_dispose(&curator); asper_test_rmtree(root);
}
static asper_err event(asper_ctx *c, const char *text, int pinned, char id[37]) {
  asper_event_input input = {0}; input.scope = "context";
  input.kind = ASPER_EVENT_DIAGNOSTIC; input.text = text; input.pinned = pinned;
  return asper_event_append(c, &input, id);
}
static int lines(const char *text, void *ud) {
  (void)ud; int n = 0;
  for (; *text; text++) if (*text == '\n') n++;
  return n;
}
static int zero(const char *text, void *ud) { (void)text; (void)ud; return 0; }
static asper_context_request request(void) {
  asper_context_request req = {0}; req.scope = "context"; req.query = "continue";
  req.history_tokens = 4; req.count_tokens = lines; return req;
}

TEST(old_pins_and_recent_tail_keep_chronological_order) {
  char root[256], ids[6][37], text[32]; asper_ctx *c = open_fixture(root); ASSERT_TRUE(c);
  for (int i = 0; i < 6; i++) {
    snprintf(text, sizeof text, "item-%d", i);
    ASSERT_OK(event(c, text, i == 0, ids[i]));
  }
  ASSERT_OK(asper_event_set_pinned(c, "context", ids[0], 0));
  ASSERT_OK(asper_event_set_pinned(c, "context", ids[1], 1));
  ASSERT_OK(asper_event_set_pinned(c, "context", ids[1], 0));
  ASSERT_OK(asper_event_set_pinned(c, "context", ids[1], 1));
  asper_context_request req = request(); asper_context_pack pack;
  ASSERT_OK(asper_context_materialize(c, &req, &pack));
  ASSERT_EQ_INT(pack.events_available, 6); ASSERT_EQ_INT(pack.events_included, 3);
  char *old = strstr(pack.context_text, "item-1"), *next = strstr(pack.context_text, "item-4");
  char *last = strstr(pack.context_text, "item-5");
  ASSERT_TRUE(old && next && last && old < next && next < last);
  ASSERT_TRUE(!strstr(pack.context_text, "item-0") && !strstr(pack.context_text, "item-2"));
  ASSERT_TRUE(pack.context_tokens <= req.history_tokens);
  asper_context_pack_free(&pack); close_fixture(c, root);
}

TEST(zero_token_counts_cannot_bypass_byte_or_event_limits) {
  char root[256]; asper_ctx *c = open_fixture(root); ASSERT_TRUE(c);
  size_t size = 1024u * 1024u;
  char *text = malloc(size + 1); ASSERT_TRUE(text); memset(text, 'x', size); text[size] = 0;
  for (int i = 0; i < 6; i++) ASSERT_OK(event(c, text, 0, NULL));
  free(text);
  asper_context_request req = request(); req.count_tokens = zero; req.history_tokens = SIZE_MAX;
  asper_context_pack pack; ASSERT_OK(asper_context_materialize(c, &req, &pack));
  ASSERT_EQ_INT(pack.events_available, 6); ASSERT_EQ_INT(pack.events_included, 3);
  ASSERT_TRUE(strlen(pack.context_text) <= 4u * 1024u * 1024u);
  asper_context_pack_free(&pack);
  /* A separate small-event scope reaches the row bound before the byte bound. */
  asper_event_input input = {0}; input.scope = "rows"; input.kind = ASPER_EVENT_DIAGNOSTIC; input.text = "small";
  for (size_t i = 0; i < 4100; i++) ASSERT_OK(asper_event_append(c, &input, NULL));
  req.scope = "rows";
  ASSERT_OK(asper_context_materialize(c, &req, &pack));
  ASSERT_EQ_INT(pack.events_available, 4100); ASSERT_EQ_INT(pack.events_included, 4096);
  asper_context_pack_free(&pack);
  req.history_tokens = 0;
  ASSERT_OK(asper_context_materialize(c, &req, &pack));
  ASSERT_EQ_INT(pack.events_included, 0); ASSERT_EQ_STR(pack.context_text, "");
  asper_context_pack_free(&pack); close_fixture(c, root);
}

TEST(checkpoint_fallback_reads_latest_and_damage_is_not_silently_omitted) {
  char root[256], path[512]; asper_ctx *c = open_fixture(root); ASSERT_TRUE(c);
  ASSERT_OK(asper_checkpoint_commit(c, "context", "old checkpoint", NULL));
  ASSERT_OK(asper_checkpoint_commit(c, "context", "new checkpoint — 東京", NULL));
  ASSERT_OK(event(c, "a later diagnostic", 0, NULL));
  snprintf(path, sizeof path, "%s/scopes/context/checkpoint.txt", root);
  ASSERT_OK(os_remove_file(path));
  char *text = NULL; ASSERT_OK(asper_checkpoint_load(c, "context", &text));
  ASSERT_EQ_STR(text, "new checkpoint — 東京"); free(text);
  ASSERT_OK(os_write_file(path, "broken\0checkpoint", 17));
  asper_context_request req = request(); req.checkpoint_tokens = 128;
  asper_context_pack pack;
  ASSERT_ERR(asper_context_materialize(c, &req, &pack), ASPER_ERR_PARSE);
  ASSERT_TRUE(!pack.context_text && !pack.system_prompt);
  ASSERT_OK(os_truncate(path, 16u * 1024u * 1024u + 1));
  ASSERT_ERR(asper_checkpoint_load(c, "context", &text), ASPER_ERR_LIMIT); ASSERT_TRUE(!text);
  req.checkpoint_tokens = 0;
  ASSERT_OK(asper_context_materialize(c, &req, &pack));
  asper_context_pack_free(&pack); close_fixture(c, root);
}

typedef struct { asper_ctx *ctx; int called; asper_err error; } callback_state;
static int reentrant_counter(const char *text, void *ud) {
  callback_state *state = ud;
  if (!state->called && strstr(text, "trigger")) {
    state->called = 1; state->error = event(state->ctx, "late append", 0, NULL);
  }
  return lines(text, NULL);
}
TEST(token_callbacks_can_reenter_without_changing_the_captured_prefix) {
  char root[256]; asper_ctx *c = open_fixture(root); ASSERT_TRUE(c);
  ASSERT_OK(event(c, "trigger", 1, NULL)); ASSERT_OK(event(c, "existing tail", 0, NULL));
  callback_state state = {c, 0, ASPER_OK};
  asper_context_request req = request(); req.history_tokens = 128;
  req.count_tokens = reentrant_counter; req.count_userdata = &state;
  asper_context_pack pack; ASSERT_OK(asper_context_materialize(c, &req, &pack));
  ASSERT_EQ_INT(state.called, 1); ASSERT_OK(state.error);
  ASSERT_EQ_INT(pack.events_available, 2); ASSERT_EQ_INT(pack.events_included, 2);
  ASSERT_TRUE(!strstr(pack.context_text, "late append"));
  asper_context_pack_free(&pack);
  asper_event *events = NULL; size_t n;
  ASSERT_OK(asper_event_list(c, "context", &events, &n)); ASSERT_EQ_INT(n, 3);
  asper_events_free(events, n); close_fixture(c, root);
}

static int nonadditive(const char *text, void *ud) {
  (void)ud;
  return strstr(text, "Memory source events") && strstr(text, "payload") ? 1000 : lines(text, NULL);
}
TEST(joined_token_counts_are_checked_before_returning_a_pack) {
  char root[256]; asper_ctx *c = open_fixture(root); ASSERT_TRUE(c);
  ASSERT_OK(event(c, "payload", 0, NULL));
  asper_context_request req = request(); req.count_tokens = nonadditive;
  asper_context_pack pack;
  ASSERT_ERR(asper_context_materialize(c, &req, &pack), ASPER_ERR_LIMIT);
  ASSERT_TRUE(!pack.system_prompt && !pack.context_text && !pack.events_included);
  close_fixture(c, root);
}

TEST(source_views_capture_pins_and_reject_frames_damaged_after_open) {
  for (int damage = 0; damage < 4; damage++) {
  char root[256], id[37], path[512]; asper_ctx *c = open_fixture(root); ASSERT_TRUE(c);
  ASSERT_OK(event(c, "observed", 0, id));
  ASSERT_OK(asper_event_set_pinned(c, "context", id, 1));
  asper_source_view view; ASSERT_OK(asper_source_view_open(c, "context", &view));
  ASSERT_OK(asper_event_set_pinned(c, "context", id, 0));
  asper_event item; ASSERT_OK(asper_source_view_read(&view, 1, &item));
  ASSERT_TRUE(item.pinned); free(item.text);
  if (damage == 3) {
    ASSERT_OK(event(c, "an authorized append", 0, NULL));
    ASSERT_OK(asper_source_view_read(&view, 1, &item)); free(item.text);
  }
  snprintf(path, sizeof path, "%s/scopes/context/events.log", root);
  if (!damage) ASSERT_OK(os_truncate(path, 10));
  else if (damage == 1 || damage == 3) {
    char *data = NULL; size_t n;
    ASSERT_OK(os_read_file(path, &data, &n));
    char *payload = strstr(data, "observed"); ASSERT_TRUE(payload); *payload = 'O';
    ASSERT_OK(os_write_file(path, data, n)); free(data);
  } else ASSERT_OK(os_remove_file(path));
  asper_err e = asper_source_view_read(&view, 1, &item);
  int empty = !item.text; free(item.text);
  asper_source_view_close(&view); close_fixture(c, root);
  if (e != ASPER_ERR_PARSE) {
    ASPER_FAILF("damage=%d: got %s, expected ASPER_ERR_PARSE", damage, asper_err_name(e));
    return;
  }
  ASSERT_TRUE(empty);
  }
}

TEST(metadata_selection_does_not_certify_an_unread_payload) {
  char root[256], path[512]; asper_ctx *c = open_fixture(root); ASSERT_TRUE(c);
  ASSERT_OK(event(c, "damaged payload", 1, NULL));
  ASSERT_OK(event(c, "intact final frame", 0, NULL));
  snprintf(path, sizeof path, "%s/scopes/context/events.log", root);
  char *data = NULL; size_t n;
  ASSERT_OK(os_read_file(path, &data, &n));
  char *payload = strstr(data, "damaged payload"); ASSERT_TRUE(payload); *payload = 'D';
  ASSERT_OK(os_write_file(path, data, n)); free(data);
  asper_source_view view; ASSERT_OK(asper_source_view_open(c, "context", &view));
  asper_event item; ASSERT_OK(asper_source_view_head(&view, 1, &item));
  ASSERT_TRUE(item.pinned && !item.text);
  ASSERT_ERR(asper_source_view_read(&view, 1, &item), ASPER_ERR_PARSE); ASSERT_TRUE(!item.text);
  asper_source_view_close(&view);
  asper_context_request req = request(); asper_context_pack pack;
  ASSERT_ERR(asper_context_materialize(c, &req, &pack), ASPER_ERR_PARSE);
  ASSERT_TRUE(!pack.context_text && !pack.system_prompt);
  close_fixture(c, root);
}

TEST(curation_capacity_is_checked_before_spending_inference) {
  char root[256], path[512]; asper_ctx *c = open_fixture(root); ASSERT_TRUE(c);
  asper_event_input input = {0}; input.scope = "context"; input.kind = ASPER_EVENT_USER;
  input.text = "Keep the source acknowledgement within its storage limit.";
  ASSERT_OK(asper_event_append(c, &input, NULL));
  ASSERT_TRUE(fake_curator_push(&curator, "INSERT context | The source remains available\n"));
  snprintf(path, sizeof path, "%s/curated-events.log", root);
  ASSERT_OK(os_write_file(path, "", 0));
  ASSERT_OK(os_truncate(path, (ASPER_CURATED_BYTES / 37) * 37));
  ASSERT_ERR(asper_curation_cycle(c, true), ASPER_ERR_LIMIT);
  ASSERT_EQ_INT(curator.calls, 0); ASSERT_EQ_INT(c->turns_n, 1);
  ASSERT_OK(os_remove_file(path));
  ASSERT_OK(asper_curation_cycle(c, true));
  ASSERT_EQ_INT(curator.calls, 1); ASSERT_EQ_INT(c->turns_n, 0);
  close_fixture(c, root);
}

TEST_LIST = {
  TEST_ENTRY(old_pins_and_recent_tail_keep_chronological_order),
  TEST_ENTRY(zero_token_counts_cannot_bypass_byte_or_event_limits),
  TEST_ENTRY(checkpoint_fallback_reads_latest_and_damage_is_not_silently_omitted),
  TEST_ENTRY(token_callbacks_can_reenter_without_changing_the_captured_prefix),
  TEST_ENTRY(joined_token_counts_are_checked_before_returning_a_pack),
  TEST_ENTRY(source_views_capture_pins_and_reject_frames_damaged_after_open),
  TEST_ENTRY(metadata_selection_does_not_certify_an_unread_payload),
  TEST_ENTRY(curation_capacity_is_checked_before_spending_inference),
};
RUN_ALL_TESTS()
