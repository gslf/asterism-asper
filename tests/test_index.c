/* test_index.c — index.c: put/remove row integrity, cosine, filtered scans,
 * deterministic top-k ordering. */

#include "asper_test.h"

#include "asper_internal.h"
#include "fakes.h"

#define T0 1785319920LL /* 2026-07-29T10:12:00Z */
#define DIM 4

static asper_record *mk_rec(const char *id, asper_section s,
                            const char *project, int deprecated) {
  asper_record *r = asper_record_new();
  if (!r) return NULL;
  strcpy(r->id, id);
  r->section = s;
  r->project = project ? asper_strdup(project) : NULL;
  r->content = asper_strdup("x");
  r->source = ASPER_SRC_MANUAL;
  r->created_at = r->updated_at = r->last_access = T0;
  r->relevance = 0.8;
  r->deprecated = deprecated != 0;
  r->emb_row = -1;
  return r;
}

TEST(put_overwrite_and_vec) {
  asper_index ix;
  asper_record *r1 = mk_rec("aaaaaaaa-0000-4000-8000-000000000001",
                            ASPER_SECTION_CONTEXT, NULL, 0);
  asper_record *r2 = mk_rec("aaaaaaaa-0000-4000-8000-000000000002",
                            ASPER_SECTION_CONTEXT, NULL, 0);
  const float v34[DIM] = {3.0f, 4.0f, 0.0f, 0.0f};
  const float vy[DIM] = {0.0f, 1.0f, 0.0f, 0.0f};
  const float vx[DIM] = {1.0f, 0.0f, 0.0f, 0.0f};
  const float *row;
  ASSERT_TRUE(r1 && r2);
  asper_index_init(&ix, DIM);
  ASSERT_EQ_INT(ix.dim, DIM);
  ASSERT_OK(asper_index_put(&ix, r1, v34));
  ASSERT_OK(asper_index_put(&ix, r2, vy));
  ASSERT_EQ_INT(ix.n, 2);
  ASSERT_EQ_INT(r1->emb_row, 0);
  ASSERT_EQ_INT(r2->emb_row, 1);
  /* rows are stored L2-normalized */
  row = asper_index_vec(&ix, r1);
  ASSERT_TRUE(row != NULL);
  ASSERT_EQ_DBL(row[0], 0.6, 1e-6);
  ASSERT_EQ_DBL(row[1], 0.8, 1e-6);
  ASSERT_EQ_DBL(row[2], 0.0, 1e-6);
  /* overwrite keeps the same row */
  ASSERT_OK(asper_index_put(&ix, r1, vx));
  ASSERT_EQ_INT(ix.n, 2);
  ASSERT_EQ_INT(r1->emb_row, 0);
  row = asper_index_vec(&ix, r1);
  ASSERT_EQ_DBL(row[0], 1.0, 1e-6);
  ASSERT_EQ_DBL(row[1], 0.0, 1e-6);
  asper_index_free(&ix);
  asper_record_free_one(r1);
  asper_record_free_one(r2);
}

TEST(remove_swaps_last_row) {
  asper_index ix;
  asper_record *r[3];
  const float vx[DIM] = {1.0f, 0.0f, 0.0f, 0.0f};
  const float vy[DIM] = {0.0f, 1.0f, 0.0f, 0.0f};
  const float vz[DIM] = {0.0f, 0.0f, 1.0f, 0.0f};
  const float *row;
  size_t i;
  r[0] = mk_rec("aaaaaaaa-0000-4000-8000-000000000001",
                ASPER_SECTION_CONTEXT, NULL, 0);
  r[1] = mk_rec("aaaaaaaa-0000-4000-8000-000000000002",
                ASPER_SECTION_CONTEXT, NULL, 0);
  r[2] = mk_rec("aaaaaaaa-0000-4000-8000-000000000003",
                ASPER_SECTION_CONTEXT, NULL, 0);
  ASSERT_TRUE(r[0] && r[1] && r[2]);
  asper_index_init(&ix, DIM);
  ASSERT_OK(asper_index_put(&ix, r[0], vx));
  ASSERT_OK(asper_index_put(&ix, r[1], vy));
  ASSERT_OK(asper_index_put(&ix, r[2], vz));
  asper_index_remove(&ix, r[0]);
  ASSERT_EQ_INT(ix.n, 2);
  ASSERT_EQ_INT(r[0]->emb_row, -1);
  /* the last record moved into the vacated row, emb_row fixed up */
  ASSERT_EQ_INT(r[2]->emb_row, 0);
  ASSERT_EQ_INT(r[1]->emb_row, 1);
  for (i = 0; i < ix.n; i++) ASSERT_EQ_INT(ix.recs[i]->emb_row, (int)i);
  row = asper_index_vec(&ix, r[2]);
  ASSERT_TRUE(row != NULL);
  ASSERT_EQ_DBL(row[2], 1.0, 1e-6); /* the moved vector is intact */
  ASSERT_EQ_DBL(row[0], 0.0, 1e-6);
  asper_index_free(&ix);
  for (i = 0; i < 3; i++) asper_record_free_one(r[i]);
}

TEST(cosine_known_vectors) {
  const float vx[DIM] = {1.0f, 0.0f, 0.0f, 0.0f};
  const float vy[DIM] = {0.0f, 1.0f, 0.0f, 0.0f};
  const float vd[DIM] = {0.70710678f, 0.70710678f, 0.0f, 0.0f};
  float vneg[DIM] = {-1.0f, 0.0f, 0.0f, 0.0f};
  ASSERT_EQ_DBL(asper_cosine(vx, vx, DIM), 1.0, 1e-6);
  ASSERT_EQ_DBL(asper_cosine(vx, vy, DIM), 0.0, 1e-6);
  ASSERT_EQ_DBL(asper_cosine(vx, vd, DIM), 0.70710678, 1e-6);
  ASSERT_EQ_DBL(asper_cosine(vy, vd, DIM), 0.70710678, 1e-6);
  ASSERT_EQ_DBL(asper_cosine(vx, vneg, DIM), -1.0, 1e-6);
}

/* Fixture for the scan tests: identity, two context (one low-sim), one
 * per project, one deprecated. All vectors relative to query (1,0,0,0). */
typedef struct {
  asper_index ix;
  asper_record *ri, *rc1, *rc2, *rp1, *rp2, *rdep;
  asper_config cfg;
  fake_clock fclk;
  asper_clock clk;
  float q[DIM];
} scan_fix;

static int scan_fix_init(scan_fix *f) {
  const float vx[DIM] = {1.0f, 0.0f, 0.0f, 0.0f};
  const float vlow[DIM] = {1.0f, 3.0f, 0.0f, 0.0f}; /* cos ~= 0.316 */
  memset(f, 0, sizeof *f);
  asper_config_defaults(&f->cfg);
  fake_clock_set(&f->fclk, T0);
  f->clk = fake_clock_make(&f->fclk);
  f->q[0] = 1.0f;
  asper_index_init(&f->ix, DIM);
  f->ri = mk_rec("aaaaaaaa-0000-4000-8000-00000000000a",
                 ASPER_SECTION_IDENTITY, NULL, 0);
  f->rc1 = mk_rec("bbbbbbbb-0000-4000-8000-00000000000b",
                  ASPER_SECTION_CONTEXT, NULL, 0);
  f->rc2 = mk_rec("cccccccc-0000-4000-8000-00000000000c",
                  ASPER_SECTION_CONTEXT, NULL, 0);
  f->rp1 = mk_rec("dddddddd-0000-4000-8000-00000000000d",
                  ASPER_SECTION_PROJECT, "thesis", 0);
  f->rp2 = mk_rec("eeeeeeee-0000-4000-8000-00000000000e",
                  ASPER_SECTION_PROJECT, "other", 0);
  f->rdep = mk_rec("ffffffff-0000-4000-8000-00000000000f",
                   ASPER_SECTION_CONTEXT, NULL, 1);
  if (!f->ri || !f->rc1 || !f->rc2 || !f->rp1 || !f->rp2 || !f->rdep)
    return 0;
  return asper_index_put(&f->ix, f->ri, vx) == ASPER_OK &&
         asper_index_put(&f->ix, f->rc1, vx) == ASPER_OK &&
         asper_index_put(&f->ix, f->rc2, vlow) == ASPER_OK &&
         asper_index_put(&f->ix, f->rp1, vx) == ASPER_OK &&
         asper_index_put(&f->ix, f->rp2, vx) == ASPER_OK &&
         asper_index_put(&f->ix, f->rdep, vx) == ASPER_OK;
}

static void scan_fix_free(scan_fix *f) {
  asper_index_free(&f->ix);
  asper_record_free_one(f->ri);
  asper_record_free_one(f->rc1);
  asper_record_free_one(f->rc2);
  asper_record_free_one(f->rp1);
  asper_record_free_one(f->rp2);
  asper_record_free_one(f->rdep);
  asper_config_free(&f->cfg);
}

static int hits_contain(const asper_hit *hits, size_t n,
                        const asper_record *r) {
  size_t i;
  for (i = 0; i < n; i++)
    if (hits[i].rec == r) return 1;
  return 0;
}

TEST(scan_filters) {
  scan_fix f;
  asper_hit hits[16];
  size_t n;
  ASSERT_TRUE(scan_fix_init(&f));

  /* ANY + project NULL: project records are excluded entirely (identity +
   * context + the ACTIVE project only) */
  n = asper_index_scan(&f.ix, &f.cfg, &f.clk, f.q, ASPER_SECTION_ANY, NULL,
                       16, 0.0, 0, hits);
  ASSERT_EQ_INT(n, 3);
  ASSERT_TRUE(hits_contain(hits, n, f.ri));
  ASSERT_TRUE(hits_contain(hits, n, f.rc1));
  ASSERT_TRUE(hits_contain(hits, n, f.rc2));
  ASSERT_TRUE(!hits_contain(hits, n, f.rdep)); /* deprecated excluded */

  /* ANY + project filter: other projects' records excluded */
  n = asper_index_scan(&f.ix, &f.cfg, &f.clk, f.q, ASPER_SECTION_ANY,
                       "thesis", 16, 0.0, 0, hits);
  ASSERT_EQ_INT(n, 4);
  ASSERT_TRUE(hits_contain(hits, n, f.ri));
  ASSERT_TRUE(hits_contain(hits, n, f.rc1));
  ASSERT_TRUE(hits_contain(hits, n, f.rc2));
  ASSERT_TRUE(hits_contain(hits, n, f.rp1));
  ASSERT_TRUE(!hits_contain(hits, n, f.rp2));

  /* single-section scans */
  n = asper_index_scan(&f.ix, &f.cfg, &f.clk, f.q, ASPER_SECTION_CONTEXT,
                       NULL, 16, 0.0, 0, hits);
  ASSERT_EQ_INT(n, 2);
  ASSERT_TRUE(hits_contain(hits, n, f.rc1) && hits_contain(hits, n, f.rc2));
  n = asper_index_scan(&f.ix, &f.cfg, &f.clk, f.q, ASPER_SECTION_PROJECT,
                       "thesis", 16, 0.0, 0, hits);
  ASSERT_EQ_INT(n, 1);
  ASSERT_TRUE(hits[0].rec == f.rp1);

  /* min_sim floor: rc2 (cos ~0.316) drops at 0.4, survives at 0.25 */
  n = asper_index_scan(&f.ix, &f.cfg, &f.clk, f.q, ASPER_SECTION_CONTEXT,
                       NULL, 16, 0.4, 0, hits);
  ASSERT_EQ_INT(n, 1);
  ASSERT_TRUE(hits[0].rec == f.rc1);
  n = asper_index_scan(&f.ix, &f.cfg, &f.clk, f.q, ASPER_SECTION_CONTEXT,
                       NULL, 16, 0.25, 0, hits);
  ASSERT_EQ_INT(n, 2);
  /* the low-cos hit carries its cosine and a lower score */
  ASSERT_TRUE(hits[0].rec == f.rc1);
  ASSERT_TRUE(hits[1].rec == f.rc2);
  ASSERT_EQ_DBL(hits[0].cos, 1.0, 1e-6);
  ASSERT_EQ_DBL(hits[1].cos, 1.0 / sqrt(10.0), 1e-6);
  ASSERT_TRUE(hits[0].score > hits[1].score);

  /* k cap keeps the best */
  n = asper_index_scan(&f.ix, &f.cfg, &f.clk, f.q, ASPER_SECTION_CONTEXT,
                       NULL, 1, 0.0, 0, hits);
  ASSERT_EQ_INT(n, 1);
  ASSERT_TRUE(hits[0].rec == f.rc1);

  scan_fix_free(&f);
}

TEST(scan_deterministic_order) {
  /* equal vectors + w_recency 0 => equal scores; the order must fall back
   * to updated_at desc, then id asc */
  asper_config cfg;
  fake_clock fclk = {0};
  asper_clock clk;
  asper_index ix;
  asper_record *ra, *rb, *rc;
  const float vx[DIM] = {1.0f, 0.0f, 0.0f, 0.0f};
  float q[DIM] = {1.0f, 0.0f, 0.0f, 0.0f};
  asper_hit hits[8];
  size_t n;
  asper_config_defaults(&cfg);
  cfg.w_recency = 0.0;
  fake_clock_set(&fclk, T0 + 1000);
  clk = fake_clock_make(&fclk);
  asper_index_init(&ix, DIM);
  ra = mk_rec("bbbbbbbb-0000-4000-8000-000000000001",
              ASPER_SECTION_CONTEXT, NULL, 0);
  rb = mk_rec("aaaaaaaa-0000-4000-8000-000000000002",
              ASPER_SECTION_CONTEXT, NULL, 0);
  rc = mk_rec("cccccccc-0000-4000-8000-000000000003",
              ASPER_SECTION_CONTEXT, NULL, 0);
  ASSERT_TRUE(ra && rb && rc);
  rc->updated_at = T0 + 500; /* newest */
  ASSERT_OK(asper_index_put(&ix, ra, vx));
  ASSERT_OK(asper_index_put(&ix, rb, vx));
  ASSERT_OK(asper_index_put(&ix, rc, vx));
  n = asper_index_scan(&ix, &cfg, &clk, q, ASPER_SECTION_CONTEXT, NULL, 8,
                       0.0, 0, hits);
  ASSERT_EQ_INT(n, 3);
  ASSERT_TRUE(hits[0].rec == rc); /* newest updated_at first */
  ASSERT_TRUE(hits[1].rec == rb); /* then id ascending       */
  ASSERT_TRUE(hits[2].rec == ra);
  asper_index_free(&ix);
  asper_record_free_one(ra);
  asper_record_free_one(rb);
  asper_record_free_one(rc);
  asper_config_free(&cfg);
}

TEST(fake_embedder_model) {
  /* sanity of the shared fake embedder used across the suite */
  asper_embedder e = fake_embedder_make();
  float a[FAKE_EMBED_DIM], b[FAKE_EMBED_DIM];
  size_t i;
  double dot = 0.0, na = 0.0;
  ASSERT_EQ_INT(e.dim, 16);
  ASSERT_EQ_STR(e.model_id, "fake-embedder");
  for (i = 0; i < 32; i++) ASSERT_EQ_INT(e.model_hash[i], 0x11);
  ASSERT_OK(e.embed(e.ud, "green tea", 1, a));
  ASSERT_OK(e.embed(e.ud, "tea green GREEN tea", 0, b));
  for (i = 0; i < FAKE_EMBED_DIM; i++) {
    dot += (double)a[i] * b[i];
    na += (double)a[i] * a[i];
  }
  ASSERT_EQ_DBL(na, 1.0, 1e-6);  /* L2-normalized */
  ASSERT_EQ_DBL(dot, 1.0, 1e-6); /* same bag of words, case-folded */
  fake_embed_text("", a);
  for (i = 0; i < FAKE_EMBED_DIM; i++) ASSERT_EQ_DBL(a[i], 0.0, 1e-12);
}

TEST_LIST = {
    TEST_ENTRY(put_overwrite_and_vec),
    TEST_ENTRY(remove_swaps_last_row),
    TEST_ENTRY(cosine_known_vectors),
    TEST_ENTRY(scan_filters),
    TEST_ENTRY(scan_deterministic_order),
    TEST_ENTRY(fake_embedder_model),
};

RUN_ALL_TESTS()
