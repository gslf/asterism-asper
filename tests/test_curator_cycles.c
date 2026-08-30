/* test_curator_cycles.c — curator.c/grammar.c: scripted end-to-end cycles,
 * every guardrail, identity quorum, dedup, protocol parsing, GBNF. */

#include "asper_test.h"

#include "asper_internal.h"
#include "fakes.h"

#define T0 1785319920LL /* 2026-07-29T10:12:00Z */
#define DAY 86400LL

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

/* One deterministic curation cycle: two observed turns (below the default
 * turn_batch of 4, so the worker stays idle) then a forced flush. */
static int run_cycle(asper_ctx *c, const char *user_text) {
  if (fake_event_append(c, "test", ASPER_EVENT_USER, user_text) != ASPER_OK)
    return 0;
  if (fake_event_append(c, "test", ASPER_EVENT_ASSISTANT, "ok noted") !=
      ASPER_OK)
    return 0;
  return asper_flush(c, 1) == ASPER_OK;
}

static size_t count_with_content(asper_ctx *c, asper_section s,
                                 const char *needle) {
  asper_record **out = NULL;
  size_t n = 0, i, hits = 0;
  if (asper_memory_list(c, s, NULL, 1, &out, &n) != ASPER_OK) return 0;
  for (i = 0; i < n; i++)
    if (strstr(asper_record_content(out[i]), needle) != NULL) hits++;
  asper_records_free(out, n);
  return hits;
}

TEST(scripted_insert_applied) {
  char root[256];
  asper_ctx *c;
  asper_stats st;
  asper_record **out = NULL;
  size_t n = 0;
  char *section_file, path[512];
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(
      fake_curator_push(&g_cur, "INSERT context | The user drinks green tea\n"));
  /* a full turn_batch (4) plus the flush: exactly one cycle consumes it */
  ASSERT_OK(fake_event_append(c, "test", ASPER_EVENT_USER,
                              "i drink green tea every day"));
  ASSERT_OK(fake_event_append(c, "test", ASPER_EVENT_ASSISTANT, "ok noted"));
  ASSERT_OK(fake_event_append(c, "test", ASPER_EVENT_USER, "thanks"));
  ASSERT_OK(fake_event_append(c, "test", ASPER_EVENT_ASSISTANT, "sure"));
  ASSERT_OK(asper_flush(c, 1));

  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.cycles_run, 1);
  ASSERT_EQ_INT(st.ops_applied, 1);
  ASSERT_EQ_INT(st.records_context, 1);
  ASSERT_EQ_INT(st.last_cycle_at, T0);
  /* table */
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_content(out[0]), "The user drinks green tea");
  ASSERT_EQ_STR(asper_record_source(out[0]), "curator");
  ASSERT_EQ_INT(asper_record_source_ref_count(out[0]), 4);
  ASSERT_TRUE(asper_record_source_ref(out[0], 0) != NULL);
  ASSERT_TRUE(asper_record_source_ref(out[0], 4) == NULL);
  ASSERT_EQ_DBL(asper_record_relevance(out[0]), 0.60, 1e-9);
  asper_records_free(out, n);
  /* index: the new record is retrievable by similarity */
  out = NULL;
  n = 0;
  ASSERT_OK(asper_memory_search(c, ASPER_SECTION_CONTEXT, NULL, "green tea",
                                5, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_TRUE(asper_record_score(out[0]) > 0.0);
  asper_records_free(out, n);
  /* journal was compacted into the section file */
  ASSERT_EQ_INT(st.journal_ops, 0);
  snprintf(path, sizeof path, "%s/context.xcdn", root);
  section_file = asper_test_read_file(path, NULL);
  ASSERT_TRUE(section_file != NULL);
  ASSERT_TRUE(strstr(section_file, "The user drinks green tea") != NULL);
  ASSERT_TRUE(strstr(section_file, "source_refs") != NULL);
  free(section_file);
  /* the curator saw instruction, transcript and empty-memory marker */
  ASSERT_TRUE(g_cur.calls >= 1);
  ASSERT_TRUE(g_cur.last_system != NULL);
  ASSERT_TRUE(strstr(g_cur.last_system, "long-term memory") != NULL);
  ASSERT_TRUE(g_cur.last_user != NULL);
  ASSERT_TRUE(strstr(g_cur.last_user, "USER: i drink green tea every day") !=
              NULL);
  ASSERT_TRUE(strstr(g_cur.last_user, "ASSISTANT: ok noted") != NULL);
  ASSERT_TRUE(strstr(g_cur.last_user, "(none)") != NULL);
  ASSERT_TRUE(g_cur.last_grammar != NULL);
  ASSERT_TRUE(strstr(g_cur.last_grammar, "root") != NULL);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(overlong_content_rejected) {
  char root[256], script[600];
  asper_ctx *c;
  asper_stats st;
  size_t i, pos;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  pos = (size_t)snprintf(script, sizeof script, "INSERT context | ");
  for (i = 0; i < 501; i++) script[pos + i] = 'x'; /* 501 > 500 cap */
  script[pos + 501] = '\n';
  script[pos + 502] = '\0';
  ASSERT_TRUE(fake_curator_push(&g_cur, script));
  ASSERT_TRUE(run_cycle(c, "remember this wall of text"));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_context, 0);
  ASSERT_EQ_INT(st.ops_applied, 0);
  ASSERT_EQ_INT(st.ops_rejected, 1);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(transient_generation_failure_retains_turns) {
  char root[256];
  asper_ctx *c;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(fake_curator_push(&g_cur,
                                "INSERT context | Retried fact survives\n"));
  fake_curator_fail_next(&g_cur, ASPER_ERR_MODEL);
  ASSERT_OK(fake_event_append(c, "test", ASPER_EVENT_USER,
                              "remember this fact"));
  ASSERT_OK(fake_event_append(c, "test", ASPER_EVENT_ASSISTANT, "noted"));
  ASSERT_ERR(asper_flush(c, 1), ASPER_ERR_MODEL);
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_context, 0);
  ASSERT_OK(asper_flush(c, 1));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_context, 1);
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_CONTEXT,
                                   "Retried fact survives"), 1);
  ASSERT_EQ_INT(g_cur.calls, 2);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(insert_project_without_active_project_dropped) {
  char root[256];
  asper_ctx *c;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(
      fake_curator_push(&g_cur, "INSERT project | Chapter draft is ready\n"));
  ASSERT_TRUE(run_cycle(c, "the chapter draft is ready"));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_project, 0);
  ASSERT_EQ_INT(st.ops_rejected, 1);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(update_on_locked_dropped) {
  char root[256], id[37];
  asper_ctx *c;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "The user name is Dario", 1, id));
  /* the sole record => handle M1; cos(turn, record) ~= 0.52 */
  ASSERT_TRUE(
      fake_curator_push(&g_cur, "UPDATE M1 | The user name is Mario\n"));
  ASSERT_TRUE(run_cycle(c, "who is dario"));
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_CONTEXT,
                                   "The user name is Dario"),
                1);
  ASSERT_EQ_INT(
      count_with_content(c, ASPER_SECTION_CONTEXT, "The user name is Mario"),
      0);
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.ops_rejected, 1);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(identity_quorum_same_op_applies_second_time) {
  char root[256], id[37];
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_IDENTITY, NULL,
                                "The assistant tone is formal", 0, id));
  /* first proposal: pending, not applied */
  ASSERT_TRUE(
      fake_curator_push(&g_cur, "UPDATE M1 | The assistant tone is casual\n"));
  ASSERT_TRUE(run_cycle(c, "make the tone formal please"));
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_IDENTITY,
                                   "The assistant tone is formal"),
                1);
  /* same op re-proposed within the window: applied */
  ASSERT_TRUE(
      fake_curator_push(&g_cur, "UPDATE M1 | The assistant tone is casual\n"));
  ASSERT_TRUE(run_cycle(c, "make the tone formal please"));
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_IDENTITY,
                                   "The assistant tone is casual"),
                1);
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_IDENTITY,
                                   "The assistant tone is formal"),
                0);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(identity_quorum_different_content_restarts) {
  char root[256], id[37];
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_IDENTITY, NULL,
                                "The assistant tone is formal", 0, id));
  ASSERT_TRUE(
      fake_curator_push(&g_cur, "UPDATE M1 | The assistant tone is casual\n"));
  ASSERT_TRUE(run_cycle(c, "make the tone formal please"));
  /* different content: a NEW pending, still nothing applied */
  ASSERT_TRUE(fake_curator_push(&g_cur,
                                "UPDATE M1 | The assistant tone is playful\n"));
  ASSERT_TRUE(run_cycle(c, "make the tone formal please"));
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_IDENTITY,
                                   "The assistant tone is formal"),
                1);
  /* the second proposal confirmed: applied */
  ASSERT_TRUE(fake_curator_push(&g_cur,
                                "UPDATE M1 | The assistant tone is playful\n"));
  ASSERT_TRUE(run_cycle(c, "make the tone formal please"));
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_IDENTITY,
                                   "The assistant tone is playful"),
                1);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(identity_quorum_pending_expires) {
  char root[256], id[37];
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_IDENTITY, NULL,
                                "The assistant tone is formal", 0, id));
  ASSERT_TRUE(
      fake_curator_push(&g_cur, "UPDATE M1 | The assistant tone is casual\n"));
  ASSERT_TRUE(run_cycle(c, "make the tone formal please"));
  /* beyond identity_confirm_window (P7D): the pending expired, so the
   * re-proposal only becomes a fresh pending — still not applied */
  fake_clock_set(&g_clk, T0 + 8 * DAY);
  ASSERT_TRUE(
      fake_curator_push(&g_cur, "UPDATE M1 | The assistant tone is casual\n"));
  ASSERT_TRUE(run_cycle(c, "make the tone formal please"));
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_IDENTITY,
                                   "The assistant tone is formal"),
                1);
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_IDENTITY,
                                   "The assistant tone is casual"),
                0);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(dedup_converts_insert_to_boost) {
  char root[256], id[37];
  asper_ctx *c;
  asper_stats st;
  asper_record **out = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "The user prefers green tea", 0, id));
  /* same bag of words reordered => cos 1.0 >= dup_threshold 0.92 */
  ASSERT_TRUE(fake_curator_push(&g_cur,
                                "INSERT context | green tea the user prefers\n"));
  ASSERT_TRUE(run_cycle(c, "green tea please"));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_context, 1); /* no new record */
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_id(out[0]), id);
  ASSERT_EQ_STR(asper_record_content(out[0]), "The user prefers green tea");
  ASSERT_EQ_INT(asper_record_access_count(out[0]), 1); /* boosted */
  ASSERT_EQ_DBL(asper_record_relevance(out[0]), 0.85, 1e-9);
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(max_ops_per_cycle_cap) {
  char root[256], cfg_path[512];
  static const char cfg_text[] =
      "#asper_config { curation: { max_ops_per_cycle: 2 } }\n";
  asper_ctx *c;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  snprintf(cfg_path, sizeof cfg_path, "%s/cap_config.xcdn", root);
  ASSERT_OK(os_write_file(cfg_path, cfg_text, sizeof cfg_text - 1));
  c = open_store(root, cfg_path);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(fake_curator_push(&g_cur,
                                "INSERT context | Fact one about apples\n"
                                "INSERT context | Fact two about oranges\n"
                                "INSERT context | Fact three about plums\n"));
  ASSERT_TRUE(run_cycle(c, "remember all the fruit facts"));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_context, 2);
  ASSERT_EQ_INT(st.ops_applied, 2);
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_CONTEXT, "apples"), 1);
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_CONTEXT, "oranges"), 1);
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_CONTEXT, "plums"), 0);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(noop_changes_nothing) {
  char root[256];
  asper_ctx *c;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(fake_curator_push(&g_cur, "NOOP\n"));
  ASSERT_TRUE(run_cycle(c, "nothing memorable happened"));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.cycles_run, 1);
  ASSERT_EQ_INT(st.ops_applied, 0);
  ASSERT_EQ_INT(st.records_identity, 0);
  ASSERT_EQ_INT(st.records_context, 0);
  ASSERT_EQ_INT(st.records_project, 0);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(protocol_tolerance_end_to_end) {
  char root[256];
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(fake_curator_push(&g_cur,
                                "THIS IS JUNK\n"
                                "INSERT context | Valid fact stands\n"
                                "MORE ? JUNK |\n"));
  ASSERT_TRUE(run_cycle(c, "some junk and one valid fact"));
  ASSERT_EQ_INT(count_with_content(c, ASPER_SECTION_CONTEXT,
                                   "Valid fact stands"),
                1);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(protocol_parse_unit) {
  asper_cop *ops = NULL;
  size_t n = 0, bad = 0;
  ASSERT_OK(asper_protocol_parse(
      "INSERT context | The user likes tea.\n"
      "INSERT identity | Calm voice\n"
      "UPDATE M2 | Better fact\n"
      "DEPRECATE M1 | outdated\n"
      "KEEP M1\n"
      "NOOP\n",
      2, &ops, &n, &bad));
  ASSERT_EQ_INT(n, 6);
  ASSERT_EQ_INT(bad, 0);
  ASSERT_EQ_INT(ops[0].kind, ASPER_COP_INSERT);
  ASSERT_EQ_INT(ops[0].section, ASPER_SECTION_CONTEXT);
  ASSERT_EQ_STR(ops[0].text, "The user likes tea.");
  ASSERT_EQ_INT(ops[1].kind, ASPER_COP_INSERT);
  ASSERT_EQ_INT(ops[1].section, ASPER_SECTION_IDENTITY);
  ASSERT_EQ_INT(ops[2].kind, ASPER_COP_UPDATE);
  ASSERT_EQ_INT(ops[2].handle, 2);
  ASSERT_EQ_STR(ops[2].text, "Better fact");
  ASSERT_EQ_INT(ops[3].kind, ASPER_COP_DEPRECATE);
  ASSERT_EQ_INT(ops[3].handle, 1);
  ASSERT_EQ_STR(ops[3].text, "outdated");
  ASSERT_EQ_INT(ops[4].kind, ASPER_COP_KEEP);
  ASSERT_EQ_INT(ops[4].handle, 1);
  ASSERT_EQ_INT(ops[5].kind, ASPER_COP_NOOP);
  asper_cops_free(ops, n);
}

TEST(protocol_parse_bad_lines_counted) {
  asper_cop *ops = NULL;
  size_t n = 0, bad = 0;
  ASSERT_OK(asper_protocol_parse(
      "GARBAGE\n"
      "INSERT sideways | wrong section\n"
      "UPDATE M9 | out of range handle\n"
      "INSERT context | still fine\n",
      2, &ops, &n, &bad));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(bad, 3);
  ASSERT_EQ_INT(ops[0].kind, ASPER_COP_INSERT);
  ASSERT_EQ_STR(ops[0].text, "still fine");
  asper_cops_free(ops, n);
  /* last line without a trailing newline still parses */
  ops = NULL;
  n = 0;
  bad = 0;
  ASSERT_OK(asper_protocol_parse("INSERT identity | no newline at end", 0,
                                 &ops, &n, &bad));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(ops[0].section, ASPER_SECTION_IDENTITY);
  ASSERT_EQ_STR(ops[0].text, "no newline at end");
  asper_cops_free(ops, n);
}

TEST(protocol_parse_recall_shapes) {
  asper_cop *ops = NULL;
  size_t n = 0, bad = 0;
  ASSERT_OK(asper_protocol_parse(
      "ANSWER | It is Rome.\n"
      "CITE M1\n"
      "CITE M2\n",
      2, &ops, &n, &bad));
  ASSERT_EQ_INT(n, 3);
  ASSERT_EQ_INT(bad, 0);
  ASSERT_EQ_INT(ops[0].kind, ASPER_COP_ANSWER);
  ASSERT_EQ_STR(ops[0].text, "It is Rome.");
  ASSERT_EQ_INT(ops[1].kind, ASPER_COP_CITE);
  ASSERT_EQ_INT(ops[1].handle, 1);
  ASSERT_EQ_INT(ops[2].kind, ASPER_COP_CITE);
  ASSERT_EQ_INT(ops[2].handle, 2);
  asper_cops_free(ops, n);
  ops = NULL;
  n = 0;
  bad = 0;
  ASSERT_OK(asper_protocol_parse("NOMEM\n", 0, &ops, &n, &bad));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(ops[0].kind, ASPER_COP_NOMEM);
  asper_cops_free(ops, n);
}

TEST(gbnf_shapes) {
  char *g;
  g = asper_gbnf_build(ASPER_GRAMMAR_CURATION, 3, 500);
  ASSERT_TRUE(g != NULL);
  ASSERT_TRUE(strstr(g, "root") != NULL);
  ASSERT_TRUE(strstr(g, "INSERT") != NULL);
  ASSERT_TRUE(strstr(g, "NOOP") != NULL);
  ASSERT_TRUE(strstr(g, "M1") != NULL);
  ASSERT_TRUE(strstr(g, "M3") != NULL);
  ASSERT_TRUE(strstr(g, "M4") == NULL); /* only this cycle's handles */
  ASSERT_TRUE(strstr(g, "KEEP") == NULL); /* KEEP is review-only */
  free(g);
  g = asper_gbnf_build(ASPER_GRAMMAR_REVIEW, 2, 500);
  ASSERT_TRUE(g != NULL);
  ASSERT_TRUE(strstr(g, "KEEP") != NULL);
  ASSERT_TRUE(strstr(g, "DEPRECATE") != NULL);
  ASSERT_TRUE(strstr(g, "NOOP") != NULL);
  ASSERT_TRUE(strstr(g, "INSERT") == NULL);
  ASSERT_TRUE(strstr(g, "ANSWER") == NULL);
  free(g);
  g = asper_gbnf_build(ASPER_GRAMMAR_RECALL, 2, 500);
  ASSERT_TRUE(g != NULL);
  ASSERT_TRUE(strstr(g, "ANSWER") != NULL);
  ASSERT_TRUE(strstr(g, "CITE") != NULL);
  ASSERT_TRUE(strstr(g, "NOMEM") != NULL);
  ASSERT_TRUE(strstr(g, "INSERT") == NULL);
  free(g);
  /* no handles => no handle-bearing productions */
  g = asper_gbnf_build(ASPER_GRAMMAR_CURATION, 0, 500);
  ASSERT_TRUE(g != NULL);
  ASSERT_TRUE(strstr(g, "M1") == NULL);
  ASSERT_TRUE(strstr(g, "INSERT") != NULL);
  free(g);
}

TEST_LIST = {
    TEST_ENTRY(scripted_insert_applied),
    TEST_ENTRY(overlong_content_rejected),
    TEST_ENTRY(transient_generation_failure_retains_turns),
    TEST_ENTRY(insert_project_without_active_project_dropped),
    TEST_ENTRY(update_on_locked_dropped),
    TEST_ENTRY(identity_quorum_same_op_applies_second_time),
    TEST_ENTRY(identity_quorum_different_content_restarts),
    TEST_ENTRY(identity_quorum_pending_expires),
    TEST_ENTRY(dedup_converts_insert_to_boost),
    TEST_ENTRY(max_ops_per_cycle_cap),
    TEST_ENTRY(noop_changes_nothing),
    TEST_ENTRY(protocol_tolerance_end_to_end),
    TEST_ENTRY(protocol_parse_unit),
    TEST_ENTRY(protocol_parse_bad_lines_counted),
    TEST_ENTRY(gbnf_shapes),
    TEST_ENTRY(protocol_parse_recall_shapes),
};

RUN_ALL_TESTS()
