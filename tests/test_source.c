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
  unsigned long long cursor = 0;
  ASSERT_OK(asper_event_search(c, in.scope, "line", 0, 1, &events, &n, &cursor));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(cursor, 1);
  ASSERT_EQ_INT(events[0].pinned, 1);
  asper_events_free(events, n);
  ASSERT_OK(asper_event_search(c, in.scope, "line", cursor, 1, &events, &n, &cursor));
  ASSERT_EQ_INT(n, 0);
  asper_events_free(events, n);
  ASSERT_ERR(asper_event_search(c, in.scope, "", 0, 0, &events, &n, &cursor), ASPER_ERR_INVALID);

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

TEST(object_range_rejects_corruption_outside_the_slice) {
  char root[256], ref[72], path[512];
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, 1785319920LL); fake_curator_init(&g_cur);
  asper_ctx *c = open_store(root); ASSERT_TRUE(c);
  ASSERT_OK(asper_object_put(c, "original data", 13, ref));
  snprintf(path, sizeof path, "%s/objects/%s.bin", root, ref + 7);
  ASSERT_OK(os_write_file(path, "original datX", 13));
  void *slice = NULL; size_t n = 0;
  asper_err e = asper_object_read(c, ref, 0, 1, &slice, &n);
  int empty = !slice && n == 0;
  asper_free(slice); asper_close(c); fake_curator_dispose(&g_cur); asper_test_rmtree(root);
  ASSERT_EQ_INT(e, ASPER_ERR_PARSE); ASSERT_TRUE(empty);
}

TEST(object_limits_empty_and_invalid_ranges) {
  char root[256], ref[72], path[512];
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, 1785319920LL); fake_curator_init(&g_cur);
  asper_ctx *c = open_store(root); ASSERT_TRUE(c);
  ASSERT_OK(asper_object_put(c, NULL, 0, ref));
  void *slice = NULL; size_t n = 0;
  ASSERT_OK(asper_object_read(c, ref, 0, 0, &slice, &n));
  ASSERT_EQ_INT(n, 0); asper_free(slice); slice = NULL;
  ASSERT_ERR(asper_object_read(c, ref, SIZE_MAX, SIZE_MAX, &slice, &n), ASPER_ERR_INVALID);
  ASSERT_ERR(asper_object_put(c, "x", 64u * 1024u * 1024u + 1, ref), ASPER_ERR_LIMIT);
  ASSERT_EQ_INT(ref[0], 0);
  strcpy(ref, "shared input");
  ASSERT_OK(asper_object_put(c, ref, 12, ref));
  ASSERT_OK(asper_object_read(c, ref, 0, 0, &slice, &n));
  ASSERT_TRUE(n == 12 && !memcmp(slice, "shared input", 12));
  asper_free(slice); slice = NULL;
  ASSERT_OK(asper_object_put(c, "one", 3, ref));
  snprintf(path, sizeof path, "%s/objects/%s.bin", c->store.root, ref + 7);
  ASSERT_OK(os_truncate(path, 64u * 1024u * 1024u + 1));
  ASSERT_ERR(asper_object_read(c, ref, 0, 1, &slice, &n), ASPER_ERR_LIMIT);
  ASSERT_TRUE(!slice && !n);
  asper_close(c); fake_curator_dispose(&g_cur); asper_test_rmtree(root);
}
#ifndef _WIN32
TEST(object_aliases_and_special_files_are_not_read) {
  char root[256], ref[72], path[512], other[512];
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, 1785319920LL); fake_curator_init(&g_cur);
  asper_ctx *c = open_store(root); ASSERT_TRUE(c);
  ASSERT_OK(asper_object_put(c, "known", 5, ref));
  snprintf(path, sizeof path, "%s/objects/%s.bin", c->store.root, ref + 7);
  snprintf(other, sizeof other, "%s/external.bin", root);
  ASSERT_OK(os_write_file(other, "known", 5));
  ASSERT_EQ_INT(unlink(path), 0); ASSERT_EQ_INT(symlink(other, path), 0);
  void *slice = NULL; size_t n = 0;
  ASSERT_ERR(asper_object_read(c, ref, 0, 1, &slice, &n), ASPER_ERR_INVALID);
  ASSERT_EQ_INT(unlink(path), 0); ASSERT_EQ_INT(mkfifo(path, 0600), 0);
  ASSERT_ERR(asper_object_read(c, ref, 0, 1, &slice, &n), ASPER_ERR_INVALID);
  asper_close(c); fake_curator_dispose(&g_cur); asper_test_rmtree(root);
}
#endif

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
  ASSERT_TRUE(fwrite("AEV2 2 torn", 1, 11, f) == 11);
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

TEST(event_index_rebuild_and_late_page) {
  char root[256], path[512], text[64];
  asper_event_input in = {0};
  asper_event *events = NULL;
  size_t n = 0;
  unsigned long long cursor = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, 1785319920LL);
  fake_curator_init(&g_cur);
  asper_ctx *c = open_store(root);
  ASSERT_TRUE(c != NULL);
  in.scope = "pages"; in.kind = ASPER_EVENT_DIAGNOSTIC;
  for (int i = 1; i <= 40; i++) {
    snprintf(text, sizeof text, "event %d", i); in.text = text;
    ASSERT_OK(asper_event_append(c, &in, NULL));
  }
  snprintf(path, sizeof path, "%s/scopes/pages/events.log.idx", root);
  ASSERT_OK(os_remove_file(path));
  ASSERT_OK(asper_event_search(c, "pages", "event", 35, 2, &events, &n, &cursor));
  ASSERT_EQ_INT(n, 2); ASSERT_EQ_INT(cursor, 37);
  ASSERT_EQ_STR(events[0].text, "event 36");
  asper_events_free(events, n);
  /* Damage a non-final index row; it is repaired without losing source data. */
  FILE *f = fopen(path, "r+b");
  ASSERT_TRUE(f != NULL);
  ASSERT_EQ_INT(fseek(f, 8 + 37*48, SEEK_SET), 0);
  ASSERT_TRUE(fputc(0xff, f) != EOF); fclose(f);
  ASSERT_OK(asper_event_search(c, "pages", "event", cursor, 2, &events, &n, &cursor));
  ASSERT_EQ_INT(n, 2); ASSERT_EQ_INT(cursor, 39);
  ASSERT_EQ_STR(events[0].text, "event 38");
  asper_events_free(events, n);
  in.text = "event 41";
  ASSERT_OK(asper_event_append(c, &in, NULL));
  ASSERT_OK(asper_event_search(c, "pages", "41", cursor, 2, &events, &n, &cursor));
  ASSERT_EQ_INT(n, 1); ASSERT_EQ_INT(cursor, 41);
  asper_events_free(events, n);
  asper_close(c); fake_curator_dispose(&g_cur); asper_test_rmtree(root);
}

TEST(event_corruption_is_not_repaired_as_a_torn_tail) {
  char root[256], path[512];
  asper_event_input in = {0};
  asper_event *events = NULL;
  char *data = NULL;
  size_t n = 0, bytes = 0;
  uint64_t preserved;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, 1785319920LL);
  fake_curator_init(&g_cur);
  asper_ctx *c = open_store(root);
  ASSERT_TRUE(c != NULL);
  in.kind = ASPER_EVENT_DIAGNOSTIC; in.text = "complete";
  for (int variant = 0; variant < 2; variant++) {
    in.scope = variant ? "metadata" : "payload";
    ASSERT_OK(asper_event_append(c, &in, NULL));
    snprintf(path, sizeof path, "%s/scopes/%s/events.log", root, in.scope);
    ASSERT_OK(os_read_file(path, &data, &bytes));
    if (variant) {
      char *len = strstr(data, " 0 8 ");
      ASSERT_TRUE(len != NULL);
      len[3] = '9'; /* A corrupted length must not cause truncation. */
    } else {
      char *body = strchr(data, '\n');
      ASSERT_TRUE(body != NULL); body[1] = 'C';
    }
    ASSERT_OK(os_write_file(path, data, bytes)); free(data); data = NULL;
    ASSERT_ERR(asper_event_list(c, in.scope, &events, &n), ASPER_ERR_PARSE);
    ASSERT_TRUE(events == NULL); ASSERT_EQ_INT(n, 0);
    ASSERT_OK(os_file_size(path, &preserved)); ASSERT_TRUE(preserved == bytes);
    ASSERT_ERR(asper_event_append(c, &in, NULL), ASPER_ERR_PARSE);
  }
  asper_close(c); fake_curator_dispose(&g_cur); asper_test_rmtree(root);
}

TEST_LIST = {
    TEST_ENTRY(event_roundtrip_and_pinning),
    TEST_ENTRY(event_index_rebuild_and_late_page),
    TEST_ENTRY(event_corruption_is_not_repaired_as_a_torn_tail),
    TEST_ENTRY(object_range_and_dedup),
    TEST_ENTRY(object_range_rejects_corruption_outside_the_slice),
    TEST_ENTRY(object_limits_empty_and_invalid_ranges),
#ifndef _WIN32
    TEST_ENTRY(object_aliases_and_special_files_are_not_read),
#endif
    TEST_ENTRY(checkpoint_and_context_survive_reopen),
    TEST_ENTRY(torn_tail_is_repaired_without_losing_complete_events),
    TEST_ENTRY(uncurated_events_replay_once_after_restart),
};

RUN_ALL_TESTS()
