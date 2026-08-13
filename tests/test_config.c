/* test_config.c — config.c: §11 defaults, overlay of every key type,
 * wrong-type failures, unknown-key tolerance, enum strings. */

#include "asper_test.h"

#include "asper_internal.h"
#include "fakes.h"

#define T0 1785319920LL
#define DAY 86400LL

static fake_clock g_clk;
static fake_curator g_cur;

static asper_ctx *open_with_config(const char *root, const char *config) {
  asper_open_params p;
  asper_embedder emb = fake_embedder_make();
  asper_curator_iface ci = fake_curator_iface_make(&g_cur);
  asper_clock ck = fake_clock_make(&g_clk);
  asper_ctx *c = NULL;
  p.memory_root = root;
  p.config_path = config;
  if (asper_open_with(&p, &emb, &ci, &ck, &c) != ASPER_OK) return NULL;
  asper_set_logger(c, NULL, NULL);
  return c;
}

static asper_err open_err(const char *root, const char *config) {
  asper_open_params p;
  asper_embedder emb = fake_embedder_make();
  asper_curator_iface ci = fake_curator_iface_make(&g_cur);
  asper_clock ck = fake_clock_make(&g_clk);
  asper_ctx *c = NULL;
  asper_err e;
  p.memory_root = root;
  p.config_path = config;
  e = asper_open_with(&p, &emb, &ci, &ck, &c);
  if (e == ASPER_OK) asper_close(c);
  return e;
}

TEST(defaults_match_spec) {
  asper_config cfg;
  asper_config_defaults(&cfg);
  /* storage */
  ASSERT_EQ_INT(cfg.journal_sync, ASPER_SYNC_BATCH);
  ASSERT_EQ_INT(cfg.journal_max_ops, 2048);
  ASSERT_TRUE(!cfg.audit_log);
  /* logging */
  ASSERT_TRUE(cfg.log_path == NULL);
  ASSERT_EQ_INT(cfg.log_level, ASPER_LOG_INFO);
  ASSERT_EQ_INT(cfg.log_max_size_kb, 5120);
  ASSERT_EQ_INT(cfg.log_max_files, 3);
  ASSERT_TRUE(!cfg.log_sync);
  /* curator */
  ASSERT_EQ_STR(cfg.curator_model_path,
                "models/qwen2.5-1.5b-instruct-q4_k_m.gguf");
  ASSERT_EQ_INT(cfg.curator_ctx, 4096);
  ASSERT_EQ_INT(cfg.curator_threads, 4);
  ASSERT_TRUE(cfg.instruction_path == NULL);
  /* embedding */
  ASSERT_EQ_STR(cfg.embed_model_path, "models/multilingual-e5-small-q8_0.gguf");
  ASSERT_EQ_INT(cfg.embed_dim, 384);
  ASSERT_EQ_STR(cfg.query_prefix, "");
  ASSERT_EQ_STR(cfg.passage_prefix, "");
  /* budgets */
  ASSERT_EQ_INT(cfg.identity_tokens, 400);
  ASSERT_EQ_INT(cfg.context_tokens, 600);
  ASSERT_EQ_INT(cfg.project_tokens, 800);
  /* injection */
  ASSERT_EQ_INT(cfg.token_estimator, ASPER_TOKENS_CURATOR);
  ASSERT_TRUE(cfg.template_path == NULL);
  /* retrieval */
  ASSERT_EQ_INT(cfg.k_context, 6);
  ASSERT_EQ_INT(cfg.k_project, 8);
  ASSERT_EQ_DBL(cfg.min_similarity, 0.25, 1e-12);
  ASSERT_EQ_DBL(cfg.w_similarity, 0.70, 1e-12);
  ASSERT_EQ_DBL(cfg.w_relevance, 0.20, 1e-12);
  ASSERT_EQ_DBL(cfg.w_recency, 0.10, 1e-12);
  /* curation */
  ASSERT_EQ_INT(cfg.turn_batch, 4);
  ASSERT_EQ_INT(cfg.idle_flush_s, 30);
  ASSERT_EQ_INT(cfg.max_ops_per_cycle, 8);
  ASSERT_EQ_DBL(cfg.dup_threshold, 0.92, 1e-12);
  ASSERT_EQ_INT(cfg.transcript_tokens, 1200);
  ASSERT_EQ_INT(cfg.event_queue_max, 256);
  ASSERT_EQ_INT(cfg.identity_confirm_window_s, 7 * DAY);
  ASSERT_EQ_INT(cfg.maintenance_interval_s, DAY);
  ASSERT_EQ_INT(cfg.review_batch, 16);
  /* recall */
  ASSERT_EQ_INT(cfg.recall_k, 12);
  ASSERT_EQ_INT(cfg.recall_answer_tokens, 160);
  ASSERT_EQ_INT(cfg.recall_timeout_s, 10);
  /* decay */
  ASSERT_EQ_INT(cfg.context_half_life_s, 30 * DAY);
  ASSERT_EQ_INT(cfg.project_half_life_s, 90 * DAY);
  ASSERT_EQ_DBL(cfg.prune_threshold, 0.15, 1e-12);
  ASSERT_EQ_INT(cfg.purge_after_s, 60 * DAY);
  /* limits + projects */
  ASSERT_EQ_INT(cfg.identity_max_records, 64);
  ASSERT_EQ_INT(cfg.content_max_chars, 500);
  ASSERT_TRUE(cfg.autocreate);
  asper_config_free(&cfg);
}

static const char FULL_OVERLAY[] =
    "#asper_config {\n"
    "  storage: { journal_sync: \"always\", journal_max_ops: 100,"
    " audit_log: true },\n"
    "  logging: { path: null, level: \"debug\", max_size_kb: 64,"
    " max_files: 2, sync: true },\n"
    "  curator: { model_path: \"custom/curator.gguf\", ctx: 2048,"
    " threads: 2, instruction_path: null },\n"
    "  embedding: { model_path: \"custom/embed.gguf\", dim: 16,"
    " query_prefix: \"query: \", passage_prefix: \"passage: \" },\n"
    "  budgets: { identity_tokens: 100, context_tokens: 200,"
    " project_tokens: 300 },\n"
    "  injection: { token_estimator: \"heuristic\", template_path: null },\n"
    "  retrieval: { k_context: 3, k_project: 4, min_similarity: 0.5,"
    " w_similarity: 0.6, w_relevance: 0.3, w_recency: 0.1 },\n"
    "  curation: { turn_batch: 2, idle_flush: r\"PT45S\","
    " max_ops_per_cycle: 4, dup_threshold: 0.95, transcript_tokens: 800,"
    " event_queue_max: 32, identity_confirm_window: r\"P3D\","
    " maintenance_interval: r\"PT12H\", review_batch: 8 },\n"
    "  recall: { k: 6, answer_tokens: 80, timeout: r\"PT5S\" },\n"
    "  decay: { context_half_life: r\"P10D\", project_half_life: r\"P20D\","
    " prune_threshold: 0.2, purge_after: r\"P30D\" },\n"
    "  limits: { identity_max_records: 10, content_max_chars: 120 },\n"
    "  projects: { autocreate: false },\n"
    "}\n";

TEST(overlay_every_key_type) {
  char root[256], cfg_path[512];
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  snprintf(cfg_path, sizeof cfg_path, "%s/config.xcdn", root);
  ASSERT_OK(os_write_file(cfg_path, FULL_OVERLAY, sizeof FULL_OVERLAY - 1));
  c = open_with_config(root, cfg_path);
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(c->cfg.journal_sync, ASPER_SYNC_ALWAYS);   /* enum string */
  ASSERT_EQ_INT(c->cfg.journal_max_ops, 100);              /* int         */
  ASSERT_TRUE(c->cfg.audit_log);                           /* bool        */
  ASSERT_TRUE(c->cfg.log_path == NULL);                    /* null        */
  ASSERT_EQ_INT(c->cfg.log_level, ASPER_LOG_DEBUG);
  ASSERT_EQ_INT(c->cfg.log_max_size_kb, 64);
  ASSERT_EQ_INT(c->cfg.log_max_files, 2);
  ASSERT_TRUE(c->cfg.log_sync);
  ASSERT_EQ_STR(c->cfg.curator_model_path, "custom/curator.gguf"); /* str */
  ASSERT_EQ_INT(c->cfg.curator_ctx, 2048);
  ASSERT_EQ_INT(c->cfg.curator_threads, 2);
  ASSERT_TRUE(c->cfg.instruction_path == NULL);
  ASSERT_EQ_STR(c->cfg.embed_model_path, "custom/embed.gguf");
  ASSERT_EQ_INT(c->cfg.embed_dim, 16);
  ASSERT_EQ_STR(c->cfg.query_prefix, "query: ");
  ASSERT_EQ_STR(c->cfg.passage_prefix, "passage: ");
  ASSERT_EQ_INT(c->cfg.identity_tokens, 100);
  ASSERT_EQ_INT(c->cfg.context_tokens, 200);
  ASSERT_EQ_INT(c->cfg.project_tokens, 300);
  ASSERT_EQ_INT(c->cfg.token_estimator, ASPER_TOKENS_HEURISTIC);
  ASSERT_TRUE(c->cfg.template_path == NULL);
  ASSERT_EQ_INT(c->cfg.k_context, 3);
  ASSERT_EQ_INT(c->cfg.k_project, 4);
  ASSERT_EQ_DBL(c->cfg.min_similarity, 0.5, 1e-12);        /* double      */
  ASSERT_EQ_DBL(c->cfg.w_similarity, 0.6, 1e-12);
  ASSERT_EQ_DBL(c->cfg.w_relevance, 0.3, 1e-12);
  ASSERT_EQ_DBL(c->cfg.w_recency, 0.1, 1e-12);
  ASSERT_EQ_INT(c->cfg.turn_batch, 2);
  ASSERT_EQ_INT(c->cfg.idle_flush_s, 45);                  /* duration    */
  ASSERT_EQ_INT(c->cfg.max_ops_per_cycle, 4);
  ASSERT_EQ_DBL(c->cfg.dup_threshold, 0.95, 1e-12);
  ASSERT_EQ_INT(c->cfg.transcript_tokens, 800);
  ASSERT_EQ_INT(c->cfg.event_queue_max, 32);
  ASSERT_EQ_INT(c->cfg.identity_confirm_window_s, 3 * DAY);
  ASSERT_EQ_INT(c->cfg.maintenance_interval_s, 12 * 3600);
  ASSERT_EQ_INT(c->cfg.review_batch, 8);
  ASSERT_EQ_INT(c->cfg.recall_k, 6);
  ASSERT_EQ_INT(c->cfg.recall_answer_tokens, 80);
  ASSERT_EQ_INT(c->cfg.recall_timeout_s, 5);
  ASSERT_EQ_INT(c->cfg.context_half_life_s, 10 * DAY);
  ASSERT_EQ_INT(c->cfg.project_half_life_s, 20 * DAY);
  ASSERT_EQ_DBL(c->cfg.prune_threshold, 0.2, 1e-12);
  ASSERT_EQ_INT(c->cfg.purge_after_s, 30 * DAY);
  ASSERT_EQ_INT(c->cfg.identity_max_records, 10);
  ASSERT_EQ_INT(c->cfg.content_max_chars, 120);
  ASSERT_TRUE(!c->cfg.autocreate);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(partial_overlay_keeps_defaults) {
  char root[256], cfg_path[512];
  static const char cfg_text[] =
      "#asper_config { retrieval: { k_context: 2 } }\n";
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  snprintf(cfg_path, sizeof cfg_path, "%s/config.xcdn", root);
  ASSERT_OK(os_write_file(cfg_path, cfg_text, sizeof cfg_text - 1));
  c = open_with_config(root, cfg_path);
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(c->cfg.k_context, 2);       /* overlaid          */
  ASSERT_EQ_INT(c->cfg.k_project, 8);       /* default untouched */
  ASSERT_EQ_INT(c->cfg.journal_max_ops, 2048);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(wrong_types_fail_with_err_config) {
  char root[256], cfg_path[512];
  static const char *bad[] = {
      /* string where an int is expected */
      "#asper_config { storage: { journal_max_ops: \"many\" } }\n",
      /* number where a string enum is expected */
      "#asper_config { logging: { level: 5 } }\n",
      /* number where a duration is expected (plain ISO strings are OK) */
      "#asper_config { curation: { idle_flush: 30 } }\n",
      /* string where a bool is expected */
      "#asper_config { storage: { audit_log: \"yes\" } }\n",
      /* unknown enum string value */
      "#asper_config { storage: { journal_sync: \"sometimes\" } }\n",
      /* unknown token-estimator value */
      "#asper_config { injection: { token_estimator: \"psychic\" } }\n",
  };
  size_t i;
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    ASSERT_TRUE(asper_test_tmpdir(root));
    fake_curator_init(&g_cur);
    fake_clock_set(&g_clk, T0);
    snprintf(cfg_path, sizeof cfg_path, "%s/config.xcdn", root);
    ASSERT_OK(os_write_file(cfg_path, bad[i], strlen(bad[i])));
    if (open_err(root, cfg_path) != ASPER_ERR_CONFIG) {
      ASPER_FAILF("config %u not rejected with ASPER_ERR_CONFIG",
                  (unsigned)i);
      fake_curator_dispose(&g_cur);
      asper_test_rmtree(root);
      return;
    }
    fake_curator_dispose(&g_cur);
    asper_test_rmtree(root);
  }
}

TEST(unknown_keys_tolerated) {
  char root[256], cfg_path[512];
  static const char cfg_text[] =
      "#asper_config {\n"
      "  future_section: { anything: 1, nested: { deep: true } },\n"
      "  storage: { journal_max_ops: 99, brand_new_key: \"x\" },\n"
      "}\n";
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  snprintf(cfg_path, sizeof cfg_path, "%s/config.xcdn", root);
  ASSERT_OK(os_write_file(cfg_path, cfg_text, sizeof cfg_text - 1));
  c = open_with_config(root, cfg_path);
  ASSERT_TRUE(c != NULL); /* unknown keys only warn */
  ASSERT_EQ_INT(c->cfg.journal_max_ops, 99);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(enum_strings_all_values) {
  char root[256], cfg_path[512];
  static const char cfg_text[] =
      "#asper_config {\n"
      "  storage: { journal_sync: \"never\" },\n"
      "  logging: { level: \"error\" },\n"
      "  injection: { token_estimator: \"curator\" },\n"
      "}\n";
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  snprintf(cfg_path, sizeof cfg_path, "%s/config.xcdn", root);
  ASSERT_OK(os_write_file(cfg_path, cfg_text, sizeof cfg_text - 1));
  c = open_with_config(root, cfg_path);
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(c->cfg.journal_sync, ASPER_SYNC_NEVER);
  ASSERT_EQ_INT(c->cfg.log_level, ASPER_LOG_ERROR);
  ASSERT_EQ_INT(c->cfg.token_estimator, ASPER_TOKENS_CURATOR);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(missing_config_path_is_error) {
  char root[256], cfg_path[512];
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  snprintf(cfg_path, sizeof cfg_path, "%s/does_not_exist.xcdn", root);
  /* an explicitly-passed config path that cannot be read must not be
   * silently ignored */
  ASSERT_TRUE(open_err(root, cfg_path) != ASPER_OK);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST_LIST = {
    TEST_ENTRY(defaults_match_spec),
    TEST_ENTRY(overlay_every_key_type),
    TEST_ENTRY(partial_overlay_keeps_defaults),
    TEST_ENTRY(wrong_types_fail_with_err_config),
    TEST_ENTRY(unknown_keys_tolerated),
    TEST_ENTRY(enum_strings_all_values),
    TEST_ENTRY(missing_config_path_is_error),
};

RUN_ALL_TESTS()
