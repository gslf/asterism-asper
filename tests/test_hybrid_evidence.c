#include "asper_test.h"
#include "asper_internal.h"
#include "fakes.h"

TEST(exact_path_symbol_beats_misleading_vector) {
  float query[] = {1, 0}, wrong[] = {1, 0}, right[] = {0, 1};
  asper_search_document d[] = {
      {"src/cache.c", "cache_open", "semantic cache persistence", wrong},
      {"src/journal.c", "journal_commit",
       "journal_commit reports ERR_TORN_WRITE", right},
      {"tests/journal.c", "test_commit", "journal fixture", NULL}};
  asper_search_hit h[3];
  size_t n = 0;
  ASSERT_OK(asper_hybrid_search(
      d, 3, "src/journal.c journal_commit ERR_TORN_WRITE", query, 2, 3, h, &n));
  ASSERT_EQ_INT(h[0].index, 1);
  ASSERT_TRUE(h[0].bm25 > 0);
  ASSERT_TRUE(h[0].exact >= 4);
  ASSERT_OK(asper_hybrid_search(d, 3, "ERR_TORN_WRITE", NULL, 0, 3, h, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(h[0].index, 1);
  ASSERT_OK(asper_hybrid_search(d, 3, "quokka_moon", NULL, 0, 3, h, &n));
  ASSERT_EQ_INT(n, 0);
}
TEST(evidence_roundtrip_expiry_and_manual_edit) {
  char root[256], id[37];
  asper_ctx *c = NULL;
  asper_open_params p;
  fake_clock fc = {0};
  fake_clock_set(&fc, 1785319920);
  asper_clock clk = fake_clock_make(&fc);
  ASSERT_TRUE(asper_test_tmpdir(root));
  memset(&p, 0, sizeof p);
  p.memory_root = root;
  ASSERT_OK(asper_open_with(&p, NULL, NULL, &clk, &c));
  asper_evidence ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = ASPER_EVIDENCE_INFERRED;
  ev.confidence = .4;
  ev.observed_at = 1785319920;
  ev.expires_at = 1785319980;
  strcpy(ev.provenance, "tool:test-123");
  strcpy(ev.workspace, "/repo/project");
  strcpy(ev.commit, "abc123");
  ASSERT_OK(asper_memory_insert_evidenced(c, ASPER_SECTION_CONTEXT, NULL,
                                          "journal_commit fails ERR_TORN_WRITE",
                                          0, &ev, id));
  asper_record **r = NULL;
  size_t n = 0;
  ASSERT_OK(asper_memory_search(c, ASPER_SECTION_CONTEXT, NULL,
                                "ERR_TORN_WRITE", 5, &r, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_evidence(r[0])->workspace, "/repo/project");
  asper_records_free(r, n);
  asper_close(c);
  ASSERT_OK(asper_open_with(&p, NULL, NULL, &clk, &c));
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &r, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_evidence(r[0])->commit, "abc123");
  ASSERT_EQ_DBL(asper_record_evidence(r[0])->confidence, .4, 1e-8);
  asper_records_free(r, n);
  fake_clock_set(&fc, 1785320000);
  ASSERT_OK(asper_memory_search(c, ASPER_SECTION_CONTEXT, NULL,
                                "ERR_TORN_WRITE", 5, &r, &n));
  ASSERT_EQ_INT(n, 0);
  asper_records_free(r, n);
  ASSERT_OK(asper_memory_update(c, id, "journal_commit now verified"));
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &r, &n));
  ASSERT_EQ_INT(asper_record_evidence(r[0])->kind, ASPER_EVIDENCE_DECLARED);
  ASSERT_EQ_STR(asper_record_evidence(r[0])->workspace, "");
  asper_records_free(r, n);
  asper_close(c);
  asper_test_rmtree(root);
}
TEST(explicit_project_materialization_isolated) {
  char root[256], id[37];
  asper_ctx *c = NULL;
  asper_open_params p = {0};
  ASSERT_TRUE(asper_test_tmpdir(root));
  p.memory_root = root;
  ASSERT_OK(asper_open(&p, &c));
  ASSERT_OK(asper_project_select(c, "alpha"));
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_PROJECT, "alpha",
                                "uniqueAlphaToken", 0, id));
  ASSERT_OK(asper_project_select(c, "beta"));
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_PROJECT, "beta",
                                "uniqueBetaToken", 0, id));
  asper_context_request req = {0};
  asper_context_pack pack;
  req.scope = "alpha";
  req.query = "uniqueAlphaToken uniqueBetaToken";
  req.base_system_prompt = "BASE";
  ASSERT_OK(asper_context_materialize_project(c, &req, "alpha", &pack));
  ASSERT_TRUE(strstr(pack.system_prompt, "uniqueAlphaToken") != NULL);
  ASSERT_TRUE(strstr(pack.system_prompt, "uniqueBetaToken") == NULL);
  asper_context_pack_free(&pack);
  asper_close(c);
  asper_test_rmtree(root);
}
TEST_LIST = {TEST_ENTRY(exact_path_symbol_beats_misleading_vector),
             TEST_ENTRY(evidence_roundtrip_expiry_and_manual_edit),
             TEST_ENTRY(explicit_project_materialization_isolated)};
RUN_ALL_TESTS()
