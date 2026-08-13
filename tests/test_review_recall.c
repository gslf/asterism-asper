/* test_review_recall.c — maintenance review via time travel (§7.6) and the
 * recall channel (§7.7) through the scripted fake curator. */

#include "asper_test.h"

#include "asper_internal.h"
#include "fakes.h"

#define T0 1785319920LL /* 2026-07-29T10:12:00Z */
#define DAY 86400LL

static fake_clock g_clk;
static fake_curator g_cur;

static asper_ctx *open_store(const char *root, int with_curator) {
  asper_open_params p;
  asper_embedder emb = fake_embedder_make();
  asper_curator_iface ci = fake_curator_iface_make(&g_cur);
  asper_clock ck = fake_clock_make(&g_clk);
  asper_ctx *c = NULL;
  p.memory_root = root;
  p.config_path = NULL;
  if (asper_open_with(&p, &emb, with_curator ? &ci : NULL, &ck, &c) !=
      ASPER_OK)
    return NULL;
  asper_set_logger(c, NULL, NULL);
  return c;
}

TEST(review_deprecate_confirmed) {
  char root[256], id[37];
  asper_ctx *c;
  asper_record **out = NULL;
  size_t n = 0;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, 1);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "Obsolete printer configuration note", 0, id));
  /* 80 days idle: eff = 0.8 * 0.5^(80/30) ~= 0.126 < prune 0.15 */
  fake_clock_set(&g_clk, T0 + 80 * DAY);
  ASSERT_TRUE(fake_curator_push(&g_cur, "DEPRECATE M1 | stale\n"));
  ASSERT_OK(asper_flush(c, 1)); /* forces the maintenance review */
  ASSERT_TRUE(g_cur.calls >= 1);
  ASSERT_TRUE(g_cur.last_grammar != NULL);
  ASSERT_TRUE(strstr(g_cur.last_grammar, "KEEP") != NULL); /* review gbnf */
  ASSERT_TRUE(strstr(g_cur.last_user,
                     "Obsolete printer configuration note") != NULL);
  /* the record is now deprecated: hidden from active listings */
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 0);
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 1, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_TRUE(asper_record_deprecated(out[0]));
  asper_records_free(out, n);
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_deprecated, 1);
  ASSERT_EQ_INT(st.records_context, 0);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(review_keep_rescues) {
  char root[256], id[37];
  asper_ctx *c;
  asper_record **out = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, 1);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "Still relevant fact about the user", 0, id));
  fake_clock_set(&g_clk, T0 + 80 * DAY);
  ASSERT_TRUE(fake_curator_push(&g_cur, "KEEP M1\n"));
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 1); /* still active */
  ASSERT_TRUE(!asper_record_deprecated(out[0]));
  /* KEEP refreshes last_access and adds the standard boost */
  ASSERT_EQ_INT(asper_record_last_access(out[0]), T0 + 80 * DAY);
  ASSERT_EQ_DBL(asper_record_relevance(out[0]), 0.85, 1e-9);
  ASSERT_EQ_INT(asper_record_access_count(out[0]), 0);
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(purge_after_window_removes_physically) {
  char root[256], path[512], id[37];
  asper_ctx *c;
  asper_record **out = NULL;
  size_t n = 0;
  char *text;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, 1);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "Temporary note headed for purge", 0, id));
  ASSERT_OK(asper_memory_deprecate(c, id, "test"));
  /* deprecated 61 days > purge_after (P60D): gone at the next compaction */
  fake_clock_set(&g_clk, T0 + 61 * DAY);
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 1, &out, &n));
  ASSERT_EQ_INT(n, 0);
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_deprecated, 0);
  snprintf(path, sizeof path, "%s/context.xcdn", root);
  text = asper_test_read_file(path, NULL);
  if (text != NULL) {
    ASSERT_TRUE(strstr(text, "Temporary note headed for purge") == NULL);
    free(text);
  }
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(deprecated_kept_before_purge_window) {
  char root[256], id[37];
  asper_ctx *c;
  asper_record **out = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, 1);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "Deprecated but recoverable", 0, id));
  ASSERT_OK(asper_memory_deprecate(c, id, "maybe"));
  fake_clock_set(&g_clk, T0 + 30 * DAY); /* inside the 60-day window */
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 1, &out, &n));
  ASSERT_EQ_INT(n, 1); /* still on disk, still recoverable */
  ASSERT_TRUE(asper_record_deprecated(out[0]));
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(recall_answer_cite_boost) {
  char root[256], id[37];
  asper_ctx *c;
  char *answer = NULL;
  asper_record **cited = NULL;
  asper_record **out = NULL;
  size_t cited_n = 0, n = 0;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, 1);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "The user speaks Italian", 0, id));
  ASSERT_TRUE(fake_curator_push(&g_cur,
                                "ANSWER | The user speaks Italian.\n"
                                "CITE M1\n"));
  ASSERT_OK(asper_recall(c, "what language does the user speak", &answer,
                         &cited, &cited_n));
  ASSERT_EQ_STR(answer, "The user speaks Italian.");
  ASSERT_EQ_INT(cited_n, 1);
  ASSERT_EQ_STR(asper_record_id(cited[0]), id);
  ASSERT_EQ_STR(asper_record_content(cited[0]), "The user speaks Italian");
  asper_free(answer);
  asper_records_free(cited, cited_n);
  /* the recall grammar and the question reached the curator */
  ASSERT_TRUE(g_cur.last_grammar != NULL);
  ASSERT_TRUE(strstr(g_cur.last_grammar, "ANSWER") != NULL);
  ASSERT_TRUE(strstr(g_cur.last_user, "what language does the user speak") !=
              NULL);
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.recalls_served, 1);
  /* cited records get the standard access boost once the batch flushes */
  ASSERT_OK(asper_flush(c, 0));
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(asper_record_access_count(out[0]), 1);
  ASSERT_EQ_DBL(asper_record_relevance(out[0]), 0.85, 1e-9);
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(recall_nomem_maps_to_not_found) {
  char root[256], id[37];
  asper_ctx *c;
  char *answer = (char *)"poison";
  asper_record **cited = NULL;
  size_t cited_n = 99;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, 1);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "The user speaks Italian", 0, id));
  ASSERT_TRUE(fake_curator_push(&g_cur, "NOMEM\n"));
  ASSERT_ERR(asper_recall(c, "what language does the user speak", &answer,
                          &cited, &cited_n),
             ASPER_ERR_NOT_FOUND);
  ASSERT_TRUE(answer == NULL);
  ASSERT_EQ_INT(cited_n, 0);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(recall_without_curator_is_model_error) {
  char root[256], id[37];
  asper_ctx *c;
  char *answer = NULL;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, 0); /* degraded: no curator backend */
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "The user speaks Italian", 0, id));
  ASSERT_ERR(asper_recall(c, "what language does the user speak", &answer,
                          NULL, NULL),
             ASPER_ERR_MODEL);
  ASSERT_TRUE(answer == NULL);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(recall_citations_optional) {
  char root[256], id[37];
  asper_ctx *c;
  char *answer = NULL;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, 1);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "The user speaks Italian", 0, id));
  ASSERT_TRUE(fake_curator_push(&g_cur, "ANSWER | Italian.\nCITE M1\n"));
  ASSERT_OK(asper_recall(c, "what language does the user speak", &answer,
                         NULL, NULL));
  ASSERT_EQ_STR(answer, "Italian.");
  asper_free(answer);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST_LIST = {
    TEST_ENTRY(review_deprecate_confirmed),
    TEST_ENTRY(review_keep_rescues),
    TEST_ENTRY(purge_after_window_removes_physically),
    TEST_ENTRY(deprecated_kept_before_purge_window),
    TEST_ENTRY(recall_answer_cite_boost),
    TEST_ENTRY(recall_nomem_maps_to_not_found),
    TEST_ENTRY(recall_without_curator_is_model_error),
    TEST_ENTRY(recall_citations_optional),
};

RUN_ALL_TESTS()
