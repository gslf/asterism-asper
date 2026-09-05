/* test_journal.c — journal.c: op serialize/replay round trip, torn tail,
 * mid-file corruption, replay tolerance. */

#include "asper_test.h"

#include "asper_internal.h"
#include "fakes.h"
#include "event_log.h"

#define T0 1785319920LL /* 2026-07-29T10:12:00Z */

#define ID1 "11111111-1111-4111-8111-111111111111"
#define ID2 "22222222-2222-4222-8222-222222222222"

static fake_clock g_clk;
static fake_curator g_cur;

static asper_ctx *open_store(const char *root) {
  asper_open_params p;
  memset(&p, 0, sizeof p);
  asper_embedder emb = fake_embedder_make();
  asper_curator_iface ci = fake_curator_iface_make(&g_cur);
  asper_clock ck = fake_clock_make(&g_clk);
  asper_ctx *c = NULL;
  p.memory_root = root;
  p.config_path = NULL;
  if (asper_open_with(&p, &emb, &ci, &ck, &c) != ASPER_OK) return NULL;
  asper_set_logger(c, NULL, NULL);
  return c;
}

static asper_record *mk_record(const char *id, asper_section s,
                               const char *content, asper_source src,
                               double relevance, asper_time at) {
  asper_record *r = asper_record_new();
  if (!r) return NULL;
  strcpy(r->id, id);
  r->section = s;
  r->project = NULL;
  r->content = asper_strdup(content);
  if (!r->content) {
    asper_record_free_one(r);
    return NULL;
  }
  r->source = src;
  r->created_at = r->updated_at = r->last_access = at;
  r->access_count = 0;
  r->relevance = relevance;
  r->locked = false;
  r->deprecated = false;
  r->emb_row = -1;
  return r;
}

/* Build actual checked frames, independent of the append/projection path. */
static int put_line(asper_buf *buf, asper_op *op) {
  static unsigned long long sequence;
  if (!buf->len) sequence = 0;
  asper_buf text; asper_buf_init(&text);
  asper_event event = {0};
  FILE *f = tmpfile();
  int ok = f && asper_op_serialize(op,&text) == ASPER_OK;
  if (ok) {
    event.text = text.data; event.sequence = ++sequence; event.at = op->at;
    event.kind = ASPER_EVENT_DIAGNOSTIC; asper_uuid_v4(event.id);
    ok = asper_event_frame_write(f,&event) == ASPER_OK;
  }
  if (ok) {
    rewind(f); char chunk[1024]; size_t n;
    while ((n = fread(chunk,1,sizeof chunk,f)) > 0)
      if (asper_buf_append(buf,chunk,n) != ASPER_OK) { ok = 0; break; }
    if (ferror(f)) ok = 0;
  }
  if (f) fclose(f);
  asper_buf_free(&text); asper_op_free(op); return ok;
}

/* Build the 8-op journal exercising every op kind. Returns heap text. */
static char *build_full_journal(size_t *out_len, size_t *out_lines) {
  asper_buf buf;
  asper_op op;
  size_t i, lines;
  asper_buf_init(&buf);

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_INSERT;
  op.at = T0;
  op.record = mk_record(ID1, ASPER_SECTION_CONTEXT, "The user drinks tea",
                        ASPER_SRC_MANUAL, 0.60, T0);
  if (!op.record || !put_line(&buf, &op)) goto fail;

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_INSERT;
  op.at = T0 + 1;
  op.record = mk_record(ID2, ASPER_SECTION_IDENTITY, "You are Asper",
                        ASPER_SRC_SEED, 1.0, T0 + 1);
  if (!op.record || !put_line(&buf, &op)) goto fail;

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_UPDATE;
  op.at = T0 + 2;
  strcpy(op.id, ID1);
  op.content = asper_strdup("The user drinks green tea");
  if (!op.content || !put_line(&buf, &op)) goto fail;

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_ACCESS;
  op.at = T0 + 3;
  op.ids = (char(*)[37])malloc(2 * sizeof(*op.ids));
  if (!op.ids) goto fail;
  op.ids_n = 2;
  strcpy(op.ids[0], ID1);
  strcpy(op.ids[1], ID2);
  if (!put_line(&buf, &op)) goto fail;

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_SET_LOCKED;
  op.at = T0 + 4;
  strcpy(op.id, ID2);
  op.value = true;
  if (!put_line(&buf, &op)) goto fail;

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_DEPRECATE;
  op.at = T0 + 5;
  strcpy(op.id, ID1);
  op.reason = asper_strdup("obsolete");
  if (!op.reason || !put_line(&buf, &op)) goto fail;

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_KEEP;
  op.at = T0 + 6;
  strcpy(op.id, ID1);
  if (!put_line(&buf, &op)) goto fail;

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_PROJECT_CREATE;
  op.at = T0 + 7;
  op.project = asper_strdup("thesis");
  if (!op.project || !put_line(&buf, &op)) goto fail;

  /* the journal invariant: one compact value per line, no raw newlines */
  lines = 0;
  for (i = 0; i < buf.len; i++)
    if (buf.data[i] == '\n') lines++;
  if (lines != 16) goto fail;
  lines /= 2;

  *out_len = buf.len;
  *out_lines = lines;
  return asper_buf_detach(&buf);
fail:
  asper_buf_free(&buf);
  return NULL;
}

TEST(op_roundtrip_all_kinds) {
  char root[256], path[512];
  char *journal;
  size_t jlen = 0, jlines = 0, n = 0, i;
  asper_ctx *c;
  asper_record **out = NULL;
  const asper_record *r1 = NULL, *r2 = NULL;
  char **slugs = NULL;
  size_t slug_n = 0;
  asper_stats st;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0 + 100);
  fake_curator_init(&g_cur);
  journal = build_full_journal(&jlen, &jlines);
  ASSERT_TRUE(journal != NULL);
  ASSERT_EQ_INT(jlines, 8);
  snprintf(path, sizeof path, "%s/journal.xcdn", root);
  ASSERT_OK(os_write_file(path, journal, jlen));
  free(journal);

  c = open_store(root);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_get_stats(c, &st));
  ASSERT_EQ_INT(st.journal_ops, 8);
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_ANY, NULL, 1, &out, &n));
  ASSERT_EQ_INT(n, 2);
  for (i = 0; i < n; i++) {
    if (strcmp(asper_record_id(out[i]), ID1) == 0) r1 = out[i];
    if (strcmp(asper_record_id(out[i]), ID2) == 0) r2 = out[i];
  }
  ASSERT_TRUE(r1 != NULL && r2 != NULL);
  /* R1: updated, boosted by ACCESS, deprecated then rescued by KEEP */
  ASSERT_EQ_STR(asper_record_content(r1), "The user drinks green tea");
  ASSERT_EQ_INT(asper_record_updated_at(r1), T0 + 2);
  ASSERT_EQ_INT(asper_record_access_count(r1), 1);
  ASSERT_EQ_INT(asper_record_last_access(r1), T0 + 6); /* KEEP refresh */
  ASSERT_EQ_DBL(asper_record_relevance(r1), 0.70, 1e-9); /* .60+.05+.05 */
  ASSERT_TRUE(!asper_record_deprecated(r1));
  /* R2: locked via SET_LOCKED, relevance capped at 1.0 */
  ASSERT_EQ_STR(asper_record_content(r2), "You are Asper");
  ASSERT_TRUE(asper_record_locked(r2));
  ASSERT_EQ_INT(asper_record_access_count(r2), 1);
  ASSERT_EQ_DBL(asper_record_relevance(r2), 1.0, 1e-9);
  ASSERT_EQ_STR(asper_record_source(r2), "seed");
  asper_records_free(out, n);
  ASSERT_OK(asper_project_list(c, &slugs, &slug_n));
  ASSERT_EQ_INT(slug_n, 1);
  ASSERT_EQ_STR(slugs[0], "thesis");
  asper_strings_free(slugs, slug_n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(torn_tail_truncated_and_recovered) {
  char root[256], path[512];
  char *journal;
  size_t jlen = 0, jlines = 0, n = 0;
  asper_buf buf;
  asper_ctx *c;
  asper_record **out = NULL;
  uint64_t sz = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0 + 100);
  fake_curator_init(&g_cur);
  journal = build_full_journal(&jlen, &jlines);
  ASSERT_TRUE(journal != NULL);
  asper_buf_init(&buf);
  ASSERT_OK(asper_buf_append(&buf, journal, jlen));
  free(journal);
  /* a crash mid-append: half a value, no trailing newline */
  ASSERT_OK(asper_buf_appends(&buf, "AEV2 "));
  snprintf(path, sizeof path, "%s/journal.xcdn", root);
  ASSERT_OK(os_write_file(path, buf.data, buf.len));
  asper_buf_free(&buf);

  c = open_store(root);
  ASSERT_TRUE(c != NULL); /* torn tail is recoverable, not fatal */
  /* the file was truncated back to the last good line */
  ASSERT_OK(os_file_size(path, &sz));
  ASSERT_EQ_INT(sz, jlen);
  /* all good ops were preserved */
  ASSERT_OK(asper_memory_list(c, ASPER_SECTION_ANY, NULL, 1, &out, &n));
  ASSERT_EQ_INT(n, 2);
  asper_records_free(out, n);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(midfile_corruption_fails_open) {
  char root[256], path[512];
  char *journal;
  size_t jlen = 0, jlines = 0;
  asper_buf buf;
  asper_open_params p;
  memset(&p, 0, sizeof p);
  asper_embedder emb = fake_embedder_make();
  asper_curator_iface ci;
  asper_clock ck;
  asper_ctx *c = NULL;
  uint64_t before = 0, after = 0;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  ci = fake_curator_iface_make(&g_cur);
  ck = fake_clock_make(&g_clk);
  journal = build_full_journal(&jlen, &jlines);
  ASSERT_TRUE(journal != NULL);
  asper_buf_init(&buf);
  ASSERT_OK(asper_buf_append(&buf, journal, jlen));
  free(journal);
  ASSERT_OK(asper_buf_appends(&buf, "{{{{\n"));
  /* good line AFTER the corruption: the error is mid-file, not a torn tail */
  {
    asper_op op;
    memset(&op, 0, sizeof op);
    op.kind = ASPER_OP_PROJECT_CREATE;
    op.at = T0 + 8;
    op.project = asper_strdup("after-corruption");
    ASSERT_TRUE(op.project != NULL);
    ASSERT_TRUE(put_line(&buf, &op));
  }
  snprintf(path, sizeof path, "%s/journal.xcdn", root);
  ASSERT_OK(os_write_file(path, buf.data, buf.len));
  asper_buf_free(&buf);
  ASSERT_OK(os_file_size(path, &before));

  p.memory_root = root;
  p.config_path = NULL;
  ASSERT_ERR(asper_open_with(&p, &emb, &ci, &ck, &c), ASPER_ERR_PARSE);
  /* the store is left untouched for manual repair */
  ASSERT_OK(os_file_size(path, &after));
  ASSERT_EQ_INT(after, before);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(replay_rejects_unappliable_ops) {
  /* A checked frame with an impossible transition still invalidates recovery. */
  char root[256], path[512];
  asper_buf buf;
  asper_op op;
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk, T0);
  fake_curator_init(&g_cur);
  asper_buf_init(&buf);
  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_UPDATE;
  op.at = T0;
  strcpy(op.id, "99999999-9999-4999-8999-999999999999");
  op.content = asper_strdup("update on a record that never existed");
  ASSERT_TRUE(op.content != NULL);
  ASSERT_TRUE(put_line(&buf, &op));
  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_INSERT;
  op.at = T0 + 1;
  op.record = mk_record(ID1, ASPER_SECTION_CONTEXT, "Still applied fine",
                        ASPER_SRC_MANUAL, 0.8, T0 + 1);
  ASSERT_TRUE(op.record != NULL);
  ASSERT_TRUE(put_line(&buf, &op));
  snprintf(path, sizeof path, "%s/journal.xcdn", root);
  ASSERT_OK(os_write_file(path, buf.data, buf.len));
  asper_buf_free(&buf);

  c = open_store(root);
  ASSERT_TRUE(c == NULL);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(short_write_and_flush_roll_back) {
  for (int fault = 1; fault <= 2; fault++) {
    char root[256], path[512], id[37];
    ASSERT_TRUE(asper_test_tmpdir(root));
    fake_clock_set(&g_clk,T0); fake_curator_init(&g_cur);
    asper_ctx *c = open_store(root); ASSERT_TRUE(c != NULL);
    ASSERT_OK(asper_memory_insert(c,ASPER_SECTION_CONTEXT,NULL,"Before fault",0,id));
    snprintf(path,sizeof path,"%s/journal.xcdn",root);
    uint64_t before, after; ASSERT_OK(os_file_size(path,&before));
    c->store.journal_fault = fault;
    ASSERT_ERR(asper_memory_insert(c,ASPER_SECTION_CONTEXT,NULL,"Retracted",0,id),ASPER_ERR_IO);
    ASSERT_OK(os_file_size(path,&after)); ASSERT_EQ_INT(before,after);
    ASSERT_TRUE(!c->store.poisoned);
    ASSERT_OK(asper_memory_insert(c,ASPER_SECTION_CONTEXT,NULL,"After recovery",0,id));
    asper_close(c); c = open_store(root); ASSERT_TRUE(c != NULL);
    asper_record **rows = NULL; size_t n;
    ASSERT_OK(asper_memory_list(c,ASPER_SECTION_ANY,NULL,0,&rows,&n)); ASSERT_EQ_INT(n,2);
    asper_records_free(rows,n); asper_close(c);
    fake_curator_dispose(&g_cur); asper_test_rmtree(root);
  }
}

TEST(uncertain_sync_requires_reopen) {
  char root[256], path[512], id[37];
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk,T0); fake_curator_init(&g_cur);
  asper_ctx *c = open_store(root); ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c,ASPER_SECTION_CONTEXT,NULL,"Acknowledged",0,id));
  c->store.journal_fault = 3;
  ASSERT_ERR(asper_memory_insert(c,ASPER_SECTION_CONTEXT,NULL,"Uncertain",0,id),ASPER_ERR_IO);
  ASSERT_TRUE(c->store.poisoned);
  ASSERT_ERR(asper_memory_insert(c,ASPER_SECTION_CONTEXT,NULL,"Must not append",0,id),ASPER_ERR_IO);
  ASSERT_ERR(asper_store_compact(c),ASPER_ERR_IO);
  snprintf(path,sizeof path,"%s/journal.xcdn",root);
  uint64_t before,after; ASSERT_OK(os_file_size(path,&before));
  asper_close(c); ASSERT_OK(os_file_size(path,&after)); ASSERT_EQ_INT(before,after);
  c = open_store(root); ASSERT_TRUE(c != NULL);
  asper_record **rows = NULL; size_t n;
  ASSERT_OK(asper_memory_list(c,ASPER_SECTION_ANY,NULL,0,&rows,&n)); ASSERT_EQ_INT(n,2);
  asper_records_free(rows,n); asper_close(c);
  fake_curator_dispose(&g_cur); asper_test_rmtree(root);
}

TEST(complete_payload_corruption_is_never_discarded) {
  char root[256],path[512]; size_t n,lines; uint64_t after;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_clock_set(&g_clk,T0); fake_curator_init(&g_cur);
  char *journal = build_full_journal(&n,&lines); ASSERT_TRUE(journal != NULL);
  char *claim = strstr(journal,"green tea"); ASSERT_TRUE(claim != NULL); claim[0] = 'G';
  snprintf(path,sizeof path,"%s/journal.xcdn",root);
  ASSERT_OK(os_write_file(path,journal,n)); free(journal);
  ASSERT_TRUE(open_store(root) == NULL);
  ASSERT_OK(os_file_size(path,&after)); ASSERT_EQ_INT(after,n);
  fake_curator_dispose(&g_cur); asper_test_rmtree(root);
}

TEST_LIST = {
    TEST_ENTRY(short_write_and_flush_roll_back),
    TEST_ENTRY(uncertain_sync_requires_reopen),
    TEST_ENTRY(complete_payload_corruption_is_never_discarded),
    TEST_ENTRY(op_roundtrip_all_kinds),
    TEST_ENTRY(torn_tail_truncated_and_recovered),
    TEST_ENTRY(midfile_corruption_fails_open),
    TEST_ENTRY(replay_rejects_unappliable_ops),
};

RUN_ALL_TESTS()
