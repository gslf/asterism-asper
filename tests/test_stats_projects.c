/* test_stats_projects.c — projects: select/create/list/persistence and the
 * projects.autocreate gate; stats counters across the lifecycle. */

#include "asper_test.h"

#include "asper_internal.h"
#include "fakes.h"

#define T0 1785319920LL /* 2026-07-29T10:12:00Z */

static fake_clock g_clk;
static fake_curator g_cur;

static asper_ctx *open_store(const char *root, const char *config) {
  asper_open_params p;
  memset(&p, 0, sizeof p);
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

TEST(project_select_create_list) {
  char root[256];
  asper_ctx *c;
  char *active = (char *)"poison";
  char **slugs = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  /* no project active on a fresh store */
  ASSERT_OK(asper_project_active(c, &active));
  ASSERT_TRUE(active == NULL);
  /* autocreate on select (default) */
  ASSERT_OK(asper_project_select(c, "thesis"));
  ASSERT_OK(asper_project_active(c, &active));
  ASSERT_EQ_STR(active, "thesis");
  asper_free(active);
  ASSERT_OK(asper_project_select(c, "alpha"));
  ASSERT_OK(asper_project_select(c, "zeta"));
  ASSERT_OK(asper_project_active(c, &active));
  ASSERT_EQ_STR(active, "zeta");
  asper_free(active);
  /* the list is sorted ascending regardless of creation order */
  ASSERT_OK(asper_project_list(c, &slugs, &n));
  ASSERT_EQ_INT(n, 3);
  ASSERT_EQ_STR(slugs[0], "alpha");
  ASSERT_EQ_STR(slugs[1], "thesis");
  ASSERT_EQ_STR(slugs[2], "zeta");
  asper_strings_free(slugs, n);
  /* NULL deselects */
  ASSERT_OK(asper_project_select(c, NULL));
  active = (char *)"poison";
  ASSERT_OK(asper_project_active(c, &active));
  ASSERT_TRUE(active == NULL);
  /* invalid slugs are rejected */
  ASSERT_ERR(asper_project_select(c, "Bad_Slug"), ASPER_ERR_INVALID);
  ASSERT_ERR(asper_project_select(c, "-x"), ASPER_ERR_INVALID);
  ASSERT_ERR(asper_project_select(c, ""), ASPER_ERR_INVALID);
  asper_close(c);
  /* known projects survive a reopen; the active selection does not */
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  slugs = NULL;
  n = 0;
  ASSERT_OK(asper_project_list(c, &slugs, &n));
  ASSERT_EQ_INT(n, 3);
  ASSERT_EQ_STR(slugs[0], "alpha");
  asper_strings_free(slugs, n);
  active = (char *)"poison";
  ASSERT_OK(asper_project_active(c, &active));
  ASSERT_TRUE(active == NULL);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(autocreate_off) {
  char root[256], cfg_path[512], pre_path[512], dir[512];
  static const char cfg_text[] =
      "#asper_config { projects: { autocreate: false } }\n";
  asper_ctx *c;
  char *active = NULL;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  snprintf(cfg_path, sizeof cfg_path, "%s/config.xcdn", root);
  ASSERT_OK(os_write_file(cfg_path, cfg_text, sizeof cfg_text - 1));
  /* a project that already exists on disk is selectable */
  snprintf(dir, sizeof dir, "%s/projects", root);
  ASSERT_OK(os_mkdir_p(dir));
  snprintf(pre_path, sizeof pre_path, "%s/projects/preexist.xcdn", root);
  ASSERT_OK(os_write_file(pre_path, "", 0));
  c = open_store(root, cfg_path);
  ASSERT_TRUE(c != NULL);
  ASSERT_ERR(asper_project_select(c, "brand-new"), ASPER_ERR_NOT_FOUND);
  ASSERT_OK(asper_project_active(c, &active));
  ASSERT_TRUE(active == NULL); /* the failed select left nothing active */
  ASSERT_OK(asper_project_select(c, "preexist"));
  ASSERT_OK(asper_project_active(c, &active));
  ASSERT_EQ_STR(active, "preexist");
  asper_free(active);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(stats_zero_on_fresh_store) {
  char root[256];
  asper_ctx *c;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_identity, 0);
  ASSERT_EQ_INT(st.records_context, 0);
  ASSERT_EQ_INT(st.records_project, 0);
  ASSERT_EQ_INT(st.records_deprecated, 0);
  ASSERT_EQ_INT(st.journal_ops, 0);
  ASSERT_EQ_INT(st.cycles_run, 0);
  ASSERT_EQ_INT(st.ops_applied, 0);
  ASSERT_EQ_INT(st.ops_rejected, 0);
  ASSERT_EQ_INT(st.recalls_served, 0);
  ASSERT_EQ_INT(st.last_cycle_at, 0);
  ASSERT_EQ_INT(st.last_compaction_at, 0);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(stats_counters_lifecycle) {
  char root[256], id_c1[37], id_c2[37], id_i[37], id_p[37];
  asper_ctx *c;
  asper_stats st;
  char *answer = NULL;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  /* three inserts + one project_create land in the journal */
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "Context fact one", 0, id_c1));
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "Context fact two", 0, id_c2));
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_IDENTITY, NULL,
                                "You are Asper", 0, id_i));
  ASSERT_OK(asper_project_select(c, "thesis"));
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_PROJECT, "thesis",
                                "Thesis chapter due", 0, id_p));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_context, 2);
  ASSERT_EQ_INT(st.records_identity, 1);
  ASSERT_EQ_INT(st.records_project, 1);
  ASSERT_EQ_INT(st.records_deprecated, 0);
  ASSERT_EQ_INT(st.journal_ops, 5); /* 4 inserts + project_create */
  /* deprecation moves a record between counters */
  ASSERT_OK(asper_memory_deprecate(c, id_c2, "duplicate"));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_context, 1); /* active count */
  ASSERT_EQ_INT(st.records_deprecated, 1);
  ASSERT_EQ_INT(st.journal_ops, 6);
  /* a scripted cycle: cycles_run, ops_applied, timestamps, compaction */
  ASSERT_TRUE(
      fake_curator_push(&g_cur, "INSERT context | Fresh fact from cycle\n"));
  ASSERT_OK(asper_observe_turn(c, ASPER_ROLE_USER, "note the fresh fact"));
  ASSERT_OK(asper_observe_turn(c, ASPER_ROLE_ASSISTANT, "ok"));
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.cycles_run, 1);
  /* every applied mutation counts: 4 manual inserts + project_create +
   * deprecate + this cycle's curator insert */
  ASSERT_EQ_INT(st.ops_applied, 7);
  ASSERT_EQ_INT(st.records_context, 2);
  ASSERT_EQ_INT(st.last_cycle_at, T0);
  ASSERT_EQ_INT(st.last_compaction_at, T0);
  ASSERT_EQ_INT(st.journal_ops, 0); /* absorbed by compaction */
  /* a rejected curator op is counted */
  {
    char script[600];
    size_t pos = (size_t)snprintf(script, sizeof script, "INSERT context | ");
    size_t i;
    for (i = 0; i < 501; i++) script[pos + i] = 'y';
    script[pos + 501] = '\n';
    script[pos + 502] = '\0';
    ASSERT_TRUE(fake_curator_push(&g_cur, script));
  }
  ASSERT_OK(asper_observe_turn(c, ASPER_ROLE_USER, "another turn"));
  ASSERT_OK(asper_observe_turn(c, ASPER_ROLE_ASSISTANT, "ok"));
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.cycles_run, 2);
  ASSERT_EQ_INT(st.ops_rejected, 1);
  /* a served recall is counted */
  ASSERT_TRUE(fake_curator_push(&g_cur,
                                "ANSWER | A fresh fact exists\nCITE M1\n"));
  ASSERT_OK(asper_recall(c, "what is the fresh fact", &answer, NULL, NULL));
  ASSERT_EQ_STR(answer, "A fresh fact exists");
  asper_free(answer);
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.recalls_served, 1);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST_LIST = {
    TEST_ENTRY(project_select_create_list),
    TEST_ENTRY(autocreate_off),
    TEST_ENTRY(stats_zero_on_fresh_store),
    TEST_ENTRY(stats_counters_lifecycle),
};

RUN_ALL_TESTS()
