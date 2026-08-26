/* test_store.c — store.c: open, journal presence, compaction, hand-written
 * xCDN section files, project files, tolerant loading, manual guardrails. */

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

#ifndef ASPER_NO_THREADS
typedef struct {
  asper_ctx *c;
  int first;
  int count;
  asper_err error;
} concurrent_arg;

static void *concurrent_insert(void *ud) {
  concurrent_arg *a = (concurrent_arg *)ud;
  for (int i = 0; i < a->count; i++) {
    char text[96], id[37];
    snprintf(text, sizeof text, "Concurrent memory %d", a->first + i);
    a->error = asper_memory_insert(a->c, ASPER_SECTION_CONTEXT, NULL, text, 0,
                                   id);
    if (a->error != ASPER_OK) break;
  }
  return NULL;
}

static void *concurrent_cache_save(void *ud) {
  concurrent_arg *a = (concurrent_arg *)ud;
  for (int i = 0; i < a->count; i++) {
    a->error = asper_cache_save(a->c);
    if (a->error != ASPER_OK) break;
  }
  return NULL;
}
#endif

TEST(open_empty_root) {
  char root[256], path[512];
  asper_ctx *c;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  /* the manifest is created on first open */
  snprintf(path, sizeof path, "%s/manifest.xcdn", root);
  ASSERT_TRUE(os_file_exists(path));
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.records_identity, 0);
  ASSERT_EQ_INT(st.records_context, 0);
  ASSERT_EQ_INT(st.records_project, 0);
  ASSERT_EQ_INT(st.records_deprecated, 0);
  ASSERT_EQ_INT(st.journal_ops, 0);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(insert_journal_reopen) {
  char root[256], path[512], id[37];
  asper_ctx *c;
  char *journal;
  asper_record **out = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "The user drinks tea", 0, id));
  ASSERT_TRUE(asper_uuid_valid(id));
  /* the op is journaled before it is applied */
  snprintf(path, sizeof path, "%s/journal.xcdn", root);
  journal = asper_test_read_file(path, NULL);
  ASSERT_TRUE(journal != NULL);
  ASSERT_TRUE(strstr(journal, id) != NULL);
  ASSERT_TRUE(strstr(journal, "insert") != NULL);
  free(journal);
  asper_close(c);
  /* the record survives a reopen */
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_id(out[0]), id);
  ASSERT_EQ_STR(asper_record_content(out[0]), "The user drinks tea");
  ASSERT_EQ_STR(asper_record_source(out[0]), "manual");
  ASSERT_EQ_DBL(asper_record_relevance(out[0]), 0.80, 1e-9);
  ASSERT_EQ_INT(asper_record_created_at(out[0]), T0);
  ASSERT_TRUE(asper_record_project(out[0]) == NULL);
  ASSERT_TRUE(!asper_record_locked(out[0]));
  ASSERT_TRUE(!asper_record_deprecated(out[0]));
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(journal_only_replay) {
  /* A store whose only state is a journal (no section files, no close-time
   * compaction) must come back through replay. */
  char root[256], path[512], id[37];
  asper_ctx *c;
  asper_buf buf;
  asper_op op;
  asper_record *r;
  asper_record **out = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  strcpy(id, "11111111-1111-4111-8111-111111111111");
  r = asper_record_new();
  ASSERT_TRUE(r != NULL);
  strcpy(r->id, id);
  r->section = ASPER_SECTION_CONTEXT;
  r->project = NULL;
  r->content = asper_strdup("Replayed straight from the journal");
  ASSERT_TRUE(r->content != NULL);
  r->source = ASPER_SRC_MANUAL;
  r->created_at = r->updated_at = r->last_access = T0;
  r->relevance = 0.8;
  r->emb_row = -1;
  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_INSERT;
  op.at = T0;
  op.record = r;
  asper_buf_init(&buf);
  ASSERT_OK(asper_op_serialize(&op, &buf));
  ASSERT_OK(asper_buf_appendc(&buf, '\n'));
  asper_op_free(&op);
  snprintf(path, sizeof path, "%s/journal.xcdn", root);
  ASSERT_OK(os_write_file(path, buf.data, buf.len));
  asper_buf_free(&buf);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_id(out[0]), id);
  ASSERT_EQ_STR(asper_record_content(out[0]),
                "Replayed straight from the journal");
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(compaction_moves_journal_to_sections) {
  char root[256], path[512], id[37];
  asper_ctx *c;
  char *text;
  uint64_t sz = 999;
  asper_record **out = NULL;
  size_t n = 0;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "Compaction target fact", 0, id));
  ASSERT_OK(asper_flush(c, 1));
  /* journal reset to empty */
  snprintf(path, sizeof path, "%s/journal.xcdn", root);
  ASSERT_OK(os_file_size(path, &sz));
  ASSERT_EQ_INT(sz, 0);
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.journal_ops, 0);
  ASSERT_EQ_INT(st.last_compaction_at, T0);
  /* the record now lives in the section file */
  snprintf(path, sizeof path, "%s/context.xcdn", root);
  text = asper_test_read_file(path, NULL);
  ASSERT_TRUE(text != NULL);
  ASSERT_TRUE(strstr(text, "Compaction target fact") != NULL);
  ASSERT_TRUE(strstr(text, id) != NULL);
  free(text);
  asper_close(c);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_content(out[0]), "Compaction target fact");
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

static const char HAND_IDENTITY[] =
    "// hand-edited identity store\n"
    "#memory {\n"
    "  id: u\"7c9e6679-7425-40de-944f-e07fc1f90ae7\",\n"
    "  section: \"identity\",\n"
    "  project: null,\n"
    "  content: \"\"\"You are Asper.\n"
    "Stay concise, stay \"kind\".\"\"\",\n"
    "  source: \"seed\",\n"
    "  created_at: t\"2026-07-29T10:12:00Z\",\n"
    "  updated_at: t\"2026-07-29T10:12:00Z\",\n"
    "  last_access: t\"2026-07-29T10:12:00Z\",\n"
    "  access_count: 3,\n"
    "  relevance: 1.0,\n"
    "  locked: true,\n"
    "  status: \"active\",\n"
    "  supersedes: null,\n"
    "  tags: [\"persona\", \"seed\"],\n"
    "}\n";

TEST(handwritten_identity_file) {
  char root[256], path[512];
  asper_ctx *c;
  asper_record **out = NULL;
  size_t n = 0;
  const char *id = "7c9e6679-7425-40de-944f-e07fc1f90ae7";
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0 + 100);
  fake_curator_init(&g_cur);
  snprintf(path, sizeof path, "%s/identity.xcdn", root);
  ASSERT_OK(os_write_file(path, HAND_IDENTITY, sizeof HAND_IDENTITY - 1));
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_IDENTITY, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_id(out[0]), id);
  ASSERT_EQ_INT(asper_record_section(out[0]), ASPER_SECTION_IDENTITY);
  ASSERT_EQ_STR(asper_record_content(out[0]),
                "You are Asper.\nStay concise, stay \"kind\".");
  ASSERT_EQ_STR(asper_record_source(out[0]), "seed");
  ASSERT_EQ_INT(asper_record_created_at(out[0]), T0);
  ASSERT_EQ_INT(asper_record_access_count(out[0]), 3);
  ASSERT_EQ_DBL(asper_record_relevance(out[0]), 1.0, 1e-9);
  ASSERT_TRUE(asper_record_locked(out[0]));
  ASSERT_TRUE(!asper_record_deprecated(out[0]));
  ASSERT_TRUE(asper_record_supersedes(out[0]) == NULL);
  ASSERT_EQ_INT(asper_record_tag_count(out[0]), 2);
  ASSERT_EQ_STR(asper_record_tag(out[0], 0), "persona");
  ASSERT_EQ_STR(asper_record_tag(out[0], 1), "seed");
  asper_records_free(out, n);
  /* locked records refuse manual mutation until unlocked */
  ASSERT_ERR(asper_memory_update(c, id, "rewritten"), ASPER_ERR_LOCKED);
  ASSERT_ERR(asper_memory_deprecate(c, id, "nope"), ASPER_ERR_LOCKED);
  ASSERT_OK(asper_memory_set_locked(c, id, 0));
  ASSERT_OK(asper_memory_update(c, id, "You are Asper, rewritten."));
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_IDENTITY, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_content(out[0]), "You are Asper, rewritten.");
  ASSERT_EQ_INT(asper_record_updated_at(out[0]), T0 + 100);
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(project_files_on_disk) {
  char root[256], path[512], id[37];
  asper_ctx *c;
  char *text;
  asper_record **out = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_project_select(c, "thesis"));
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_PROJECT, "thesis",
                                "Chapter two is due friday", 0, id));
  ASSERT_OK(asper_flush(c, 1));
  snprintf(path, sizeof path, "%s/projects/thesis.xcdn", root);
  ASSERT_TRUE(os_file_exists(path));
  text = asper_test_read_file(path, NULL);
  ASSERT_TRUE(text != NULL);
  ASSERT_TRUE(strstr(text, "Chapter two is due friday") != NULL);
  free(text);
  asper_close(c);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_PROJECT, "thesis", 0, &out,
                              &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_project(out[0]), "thesis");
  ASSERT_EQ_STR(asper_record_content(out[0]), "Chapter two is due friday");
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

static const char MIXED_CONTEXT[] =
    "#memory {\n"
    "  id: u\"22222222-2222-4222-8222-222222222222\",\n"
    "  section: \"context\",\n"
    "  project: null,\n"
    "  content: \"The one good record\",\n"
    "  source: \"manual\",\n"
    "  created_at: t\"2026-07-29T10:12:00Z\",\n"
    "  updated_at: t\"2026-07-29T10:12:00Z\",\n"
    "  last_access: t\"2026-07-29T10:12:00Z\",\n"
    "  access_count: 0,\n"
    "  relevance: 0.8,\n"
    "  locked: false,\n"
    "  status: \"active\",\n"
    "  supersedes: null,\n"
    "  tags: [],\n"
    "}\n"
    "\n"
    "#memory {\n"
    "  id: u\"33333333-3333-4333-8333-333333333333\",\n"
    "  section: \"sideways\",\n"
    "  content: \"Unknown section, must be skipped\",\n"
    "}\n"
    "\n"
    "#note { anything: 1 }\n";

TEST(bad_records_skipped_on_open) {
  char root[256], path[512];
  asper_ctx *c;
  asper_record **out = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  snprintf(path, sizeof path, "%s/context.xcdn", root);
  ASSERT_OK(os_write_file(path, MIXED_CONTEXT, sizeof MIXED_CONTEXT - 1));
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL); /* unknown/bad values never fail the open */
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 1, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_content(out[0]), "The one good record");
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(manual_op_validation) {
  char root[256], id[37], big[600], unicode[1001];
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  /* empty / whitespace-only content */
  ASSERT_ERR(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL, "", 0, id),
             ASPER_ERR_INVALID);
  ASSERT_ERR(
      asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL, "  \t ", 0, id),
      ASPER_ERR_INVALID);
  /* content_max_chars counts characters, not encoded bytes. The two bytes
   * of U+00E9 come from a string literal rather than (char)0xc3 casts:
   * char is signed under MSVC, so casting a constant above CHAR_MAX is
   * C4310 — fatal under /W4 /WX. */
  for (size_t i = 0; i < 500; i++) memcpy(unicode + i * 2, "\xc3\xa9", 2);
  unicode[1000] = '\0';
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL, unicode, 0,
                                id));
  /* 599 ASCII characters exceed the default 500-character limit. */
  memset(big, 'x', sizeof big - 1);
  big[sizeof big - 1] = '\0';
  ASSERT_ERR(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL, big, 0, id),
             ASPER_ERR_INVALID);
  /* project section requires a project */
  ASSERT_ERR(
      asper_memory_insert(c, ASPER_SECTION_PROJECT, NULL, "fact", 0, id),
      ASPER_ERR_INVALID);
  /* unknown target ids */
  ASSERT_ERR(asper_memory_update(c, "44444444-4444-4444-8444-444444444444",
                                 "new content"),
             ASPER_ERR_NOT_FOUND);
  ASSERT_ERR(
      asper_memory_deprecate(c, "44444444-4444-4444-8444-444444444444", NULL),
      ASPER_ERR_NOT_FOUND);
  ASSERT_TRUE(asper_last_error(c) != NULL);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(interrupted_compaction_is_rolled_back_on_open) {
  char root[256], path[512], backup[540], marker[512], id[37];
  char *data;
  size_t len;
  asper_ctx *c;
  asper_record **out = NULL;
  size_t n = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL,
                                "Compaction rollback sentinel", 0, id));
  asper_close(c); /* establish a fully compacted baseline */

  snprintf(path, sizeof path, "%s/context.xcdn", root);
  data = asper_test_read_file(path, &len);
  ASSERT_TRUE(data != NULL);
  snprintf(backup, sizeof backup, "%s.compact.bak", path);
  ASSERT_OK(os_write_file(backup, data, len));
  free(data);
  snprintf(path, sizeof path, "%s/journal.xcdn", root);
  data = asper_test_read_file(path, &len);
  ASSERT_TRUE(data != NULL);
  snprintf(backup, sizeof backup, "%s.compact.bak", path);
  ASSERT_OK(os_write_file(backup, data, len));
  free(data);

  snprintf(path, sizeof path, "%s/context.xcdn", root);
  ASSERT_OK(os_write_file(path, "corrupt", 7));
  snprintf(path, sizeof path, "%s/journal.xcdn", root);
  ASSERT_OK(os_write_file(path, "corrupt", 7));
  snprintf(marker, sizeof marker, "%s/compact.pending", root);
  {
    static const char pending[] = "E context.xcdn\nE journal.xcdn\n";
    ASSERT_OK(os_write_file(marker, pending, sizeof pending - 1));
  }

  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(!os_file_exists(marker));
  snprintf(backup, sizeof backup, "%s/context.xcdn.compact.bak", root);
  ASSERT_TRUE(!os_file_exists(backup));
  snprintf(backup, sizeof backup, "%s/journal.xcdn.compact.bak", root);
  ASSERT_TRUE(!os_file_exists(backup));
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(asper_record_content(out[0]), "Compaction rollback sentinel");
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(concurrent_mutation_and_cache_save) {
#ifndef ASPER_NO_THREADS
  char root[256];
  asper_ctx *c;
  asper_record **out = NULL;
  size_t n = 0;
  os_thread threads[6];
  concurrent_arg args[6];
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  for (int i = 0; i < 4; i++) {
    args[i].c = c; args[i].first = i * 25; args[i].count = 25;
    args[i].error = ASPER_OK;
    ASSERT_OK(os_thread_start(&threads[i], concurrent_insert, &args[i]));
  }
  for (int i = 4; i < 6; i++) {
    args[i].c = c; args[i].first = 0; args[i].count = 20;
    args[i].error = ASPER_OK;
    ASSERT_OK(os_thread_start(&threads[i], concurrent_cache_save, &args[i]));
  }
  for (int i = 0; i < 6; i++) os_thread_join(&threads[i]);
  for (int i = 0; i < 6; i++) ASSERT_OK(args[i].error);
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &out, &n));
  ASSERT_EQ_INT(n, 100);
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
#endif
}

TEST_LIST = {
    TEST_ENTRY(open_empty_root),
    TEST_ENTRY(insert_journal_reopen),
    TEST_ENTRY(journal_only_replay),
    TEST_ENTRY(compaction_moves_journal_to_sections),
    TEST_ENTRY(handwritten_identity_file),
    TEST_ENTRY(project_files_on_disk),
    TEST_ENTRY(bad_records_skipped_on_open),
    TEST_ENTRY(manual_op_validation),
    TEST_ENTRY(interrupted_compaction_is_rolled_back_on_open),
    TEST_ENTRY(concurrent_mutation_and_cache_save),
};

RUN_ALL_TESTS()
