/* test_source.c — lossless scoped events, objects, checkpoints and context. */

#include "asper_test.h"

#include "asper_internal.h"
#include "fakes.h"

static fake_clock g_clk;
static fake_curator g_cur;

static asper_ctx *open_store(const char *root) {
  asper_open_params p;
  asper_embedder emb = fake_embedder_make();
  asper_curator_iface ci = fake_curator_iface_make(&g_cur);
  asper_clock ck = fake_clock_make(&g_clk);
  asper_ctx *c = NULL;
  memset(&p, 0, sizeof p);
  p.memory_root = root;
  if (asper_open_with(&p, &emb, &ci, &ck, &c) != ASPER_OK) return NULL;
  asper_set_logger(c, NULL, NULL);
  return c;
}

TEST(event_roundtrip_and_pinning) {
  char root[256], id[37];
  asper_ctx *c;
  asper_event_input in;
  asper_event *events = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, 1785319920LL);
  fake_curator_init(&g_cur);
  c = open_store(root);
  ASSERT_TRUE(c != NULL);
  memset(&in, 0, sizeof in);
  in.scope = "session-a";
  in.kind = ASPER_EVENT_USER;
  in.text = "line one\nline two â";
  ASSERT_OK(asper_event_append(c, &in, id));
  ASSERT_OK(asper_event_list(c, in.scope, &events, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(events[0].id, id);
  ASSERT_EQ_STR(events[0].text, in.text);
  ASSERT_EQ_INT(events[0].sequence, 1);
  ASSERT_EQ_INT(events[0].pinned, 0);
  asper_events_free(events, n);
  events = NULL;
  ASSERT_OK(asper_event_set_pinned(c, in.scope, id, 1));
  ASSERT_OK(asper_event_list(c, in.scope, &events, &n));
  ASSERT_EQ_INT(events[0].pinned, 1);
  asper_events_free(events, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(object_range_and_dedup) {
  char root[256], a[72], b[72];
  asper_ctx *c;
  void *slice = NULL;
  size_t n = 0;
  static const unsigned char payload[] = {0, 1, 2, 3, 4, 255};
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, 1785319920LL);
  fake_curator_init(&g_cur);
  c = open_store(root);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_object_put(c, payload, sizeof payload, a));
  ASSERT_OK(asper_object_put(c, payload, sizeof payload, b));
  ASSERT_EQ_STR(a, b);
  ASSERT_OK(asper_object_read(c, a, 2, 3, &slice, &n));
  ASSERT_EQ_INT(n, 3);
  ASSERT_TRUE(memcmp(slice, payload + 2, 3) == 0);
  asper_free(slice);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(checkpoint_and_context_survive_reopen) {
  char root[256], id[37];
  asper_ctx *c;
  asper_event_input in;
  asper_context_request req;
  asper_context_pack pack;
  char *checkpoint = NULL;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, 1785319920LL);
  fake_curator_init(&g_cur);
  c = open_store(root);
  ASSERT_TRUE(c != NULL);
  memset(&in, 0, sizeof in);
  in.scope = "main";
  in.kind = ASPER_EVENT_ASSISTANT;
  in.text = "The build currently fails in parser.c.";
  ASSERT_OK(asper_event_append(c, &in, NULL));
  ASSERT_OK(asper_checkpoint_commit(
      c, "main", "goal: fix parser\nunresolved: parser.c failure", id));
  asper_close(c);
  fake_curator_dispose(&g_cur);

  fake_curator_init(&g_cur);
  c = open_store(root);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_checkpoint_load(c, "main", &checkpoint));
  ASSERT_EQ_STR(checkpoint, "goal: fix parser\nunresolved: parser.c failure");
  asper_free(checkpoint);
  memset(&req, 0, sizeof req);
  req.scope = "main";
  req.base_system_prompt = "system";
  req.query = "continue";
  req.history_tokens = 128;
  req.checkpoint_tokens = 64;
  ASSERT_OK(asper_context_materialize(c, &req, &pack));
  ASSERT_TRUE(strstr(pack.context_text, "Working checkpoint") != NULL);
  ASSERT_TRUE(strstr(pack.context_text, "parser.c failure") != NULL);
  ASSERT_TRUE(strstr(pack.context_text, "build currently fails") != NULL);
  ASSERT_EQ_INT(pack.events_available, 2);
  asper_context_pack_free(&pack);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(torn_tail_is_repaired_without_losing_complete_events) {
  char root[256], path[512];
  asper_ctx *c;
  asper_event_input in;
  asper_event *events = NULL;
  size_t n = 0;
  FILE *f;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, 1785319920LL);
  fake_curator_init(&g_cur);
  c = open_store(root);
  ASSERT_TRUE(c != NULL);
  memset(&in, 0, sizeof in);
  in.scope = "repair";
  in.kind = ASPER_EVENT_USER;
  in.text = "complete";
  ASSERT_OK(asper_event_append(c, &in, NULL));
  snprintf(path, sizeof path, "%s/scopes/repair/events.log", root);
  f = fopen(path, "ab");
  ASSERT_TRUE(f != NULL);
  ASSERT_TRUE(fwrite("AEV1 2 torn", 1, 11, f) == 11);
  fclose(f);
  ASSERT_OK(asper_event_list(c, "repair", &events, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(events[0].text, "complete");
  asper_events_free(events, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(uncurated_events_replay_once_after_restart) {
  char root[256];
  asper_ctx *c;
  asper_event_input in;
  asper_record **records = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, 1785319920LL);
  fake_curator_init(&g_cur);
  c = open_store(root);
  ASSERT_TRUE(c != NULL);
  memset(&in, 0, sizeof in);
  in.scope = "restart";
  in.kind = ASPER_EVENT_USER;
  in.text = "I prefer exact durable memory.";
  ASSERT_OK(asper_event_append(c, &in, NULL));
  in.kind = ASPER_EVENT_ASSISTANT;
  in.text = "noted";
  ASSERT_OK(asper_event_append(c, &in, NULL));
  /* Close below the batch threshold: only the exact source is durable. */
  asper_close(c);
  fake_curator_dispose(&g_cur);

  fake_curator_init(&g_cur);
  ASSERT_TRUE(fake_curator_push(
      &g_cur, "INSERT context | User prefers exact durable memory\n"));
  c = open_store(root);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_EQ_INT(g_cur.calls, 1);
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0,
                              &records, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(asper_record_source_ref_count(records[0]), 2);
  asper_records_free(records, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);

  /* The durable acknowledgement prevents spending tokens on it again. */
  fake_curator_init(&g_cur);
  c = open_store(root);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_EQ_INT(g_cur.calls, 0);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST_LIST = {
    TEST_ENTRY(event_roundtrip_and_pinning),
    TEST_ENTRY(object_range_and_dedup),
    TEST_ENTRY(checkpoint_and_context_survive_reopen),
    TEST_ENTRY(torn_tail_is_repaired_without_losing_complete_events),
    TEST_ENTRY(uncurated_events_replay_once_after_restart),
};

RUN_ALL_TESTS()
