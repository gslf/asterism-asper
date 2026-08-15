/* test_decay_scoring.c — decay.c: eff(), score(), deterministic tie-break. */

#include "asper_test.h"

#include "asper_internal.h"

#define T0 1785319920LL /* 2026-07-29T10:12:00Z */
#define DAY 86400LL

static void base_record(asper_record *r, asper_section s, double relevance,
                        asper_time t) {
  memset(r, 0, sizeof *r);
  strcpy(r->id, "11111111-1111-4111-8111-111111111111");
  r->section = s;
  r->relevance = relevance;
  r->created_at = r->updated_at = r->last_access = t;
  r->emb_row = -1;
}

TEST(half_life_defaults) {
  asper_config cfg;
  asper_config_defaults(&cfg);
  ASSERT_EQ_INT(asper_half_life_s(&cfg, ASPER_SECTION_CONTEXT),
                30 * DAY); /* P30D */
  ASSERT_EQ_INT(asper_half_life_s(&cfg, ASPER_SECTION_PROJECT),
                90 * DAY); /* P90D */
  asper_config_free(&cfg);
}

TEST(eff_halves_at_half_life) {
  asper_config cfg;
  asper_record r;
  asper_config_defaults(&cfg);
  base_record(&r, ASPER_SECTION_CONTEXT, 0.8, T0);
  ASSERT_EQ_DBL(asper_eff_relevance(&cfg, &r, T0), 0.8, 1e-12);
  /* exactly one half-life after last_access => exactly half */
  ASSERT_EQ_DBL(asper_eff_relevance(&cfg, &r, T0 + 30 * DAY), 0.4, 1e-9);
  ASSERT_EQ_DBL(asper_eff_relevance(&cfg, &r, T0 + 60 * DAY), 0.2, 1e-9);
  ASSERT_EQ_DBL(asper_eff_relevance(&cfg, &r, T0 + 90 * DAY), 0.1, 1e-9);
  /* project section decays with its own (longer) half-life */
  base_record(&r, ASPER_SECTION_PROJECT, 0.8, T0);
  ASSERT_EQ_DBL(asper_eff_relevance(&cfg, &r, T0 + 90 * DAY), 0.4, 1e-9);
  ASSERT_EQ_DBL(asper_eff_relevance(&cfg, &r, T0 + 30 * DAY),
                0.8 * pow(0.5, 30.0 / 90.0), 1e-9);
  asper_config_free(&cfg);
}

TEST(identity_never_decays) {
  asper_config cfg;
  asper_record r;
  asper_config_defaults(&cfg);
  base_record(&r, ASPER_SECTION_IDENTITY, 0.7, T0);
  ASSERT_EQ_DBL(asper_eff_relevance(&cfg, &r, T0), 0.7, 1e-12);
  ASSERT_EQ_DBL(asper_eff_relevance(&cfg, &r, T0 + 365 * DAY), 0.7, 1e-12);
  ASSERT_EQ_DBL(asper_eff_relevance(&cfg, &r, T0 + 3650 * DAY), 0.7, 1e-12);
  asper_config_free(&cfg);
}

TEST(score_weights) {
  asper_config cfg;
  asper_record r;
  asper_config_defaults(&cfg);
  /* fresh record: eff = 0.8, recency factor = 1
   * score = 0.70*cos + 0.20*0.8 + 0.10*1 */
  base_record(&r, ASPER_SECTION_CONTEXT, 0.8, T0);
  ASSERT_EQ_DBL(asper_score_record(&cfg, &r, 1.0, T0), 0.96, 1e-9);
  ASSERT_EQ_DBL(asper_score_record(&cfg, &r, 0.5, T0), 0.61, 1e-9);
  ASSERT_EQ_DBL(asper_score_record(&cfg, &r, 0.0, T0), 0.26, 1e-9);
  /* one half-life stale in updated_at only: recency term halves */
  base_record(&r, ASPER_SECTION_CONTEXT, 0.8, T0);
  r.updated_at = T0 - 30 * DAY;
  ASSERT_EQ_DBL(asper_score_record(&cfg, &r, 0.0, T0),
                0.20 * 0.8 + 0.10 * 0.5, 1e-9);
  /* one half-life stale in last_access only: relevance term halves */
  base_record(&r, ASPER_SECTION_CONTEXT, 0.8, T0);
  r.last_access = T0 - 30 * DAY;
  ASSERT_EQ_DBL(asper_score_record(&cfg, &r, 0.0, T0),
                0.20 * 0.4 + 0.10 * 1.0, 1e-9);
  /* custom weights are honored */
  cfg.w_similarity = 1.0;
  cfg.w_relevance = 0.0;
  cfg.w_recency = 0.0;
  base_record(&r, ASPER_SECTION_CONTEXT, 0.8, T0);
  ASSERT_EQ_DBL(asper_score_record(&cfg, &r, 0.42, T0), 0.42, 1e-12);
  asper_config_free(&cfg);
}

static void mk_hit(asper_hit *h, asper_record *r, const char *id,
                   double score, asper_time updated) {
  base_record(r, ASPER_SECTION_CONTEXT, 0.8, T0);
  strcpy(r->id, id);
  r->updated_at = updated;
  h->rec = r;
  h->cos = 0.5;
  h->score = score;
}

static int hit_cmp_qsort(const void *a, const void *b) {
  return asper_hit_cmp((const asper_hit *)a, (const asper_hit *)b);
}

TEST(hit_tie_break_ordering) {
  asper_record ra, rb, rc, rd;
  asper_hit h[4];
  /* rank: score desc, then updated_at desc, then id asc */
  mk_hit(&h[0], &ra, "cccccccc-0000-4000-8000-000000000000", 0.5, T0);
  mk_hit(&h[1], &rb, "aaaaaaaa-0000-4000-8000-000000000000", 0.5, T0);
  mk_hit(&h[2], &rc, "dddddddd-0000-4000-8000-000000000000", 0.5, T0 + 10);
  mk_hit(&h[3], &rd, "bbbbbbbb-0000-4000-8000-000000000000", 0.9, T0 - 99);
  /* pairwise */
  ASSERT_TRUE(asper_hit_cmp(&h[3], &h[0]) < 0); /* higher score first   */
  ASSERT_TRUE(asper_hit_cmp(&h[0], &h[3]) > 0);
  ASSERT_TRUE(asper_hit_cmp(&h[2], &h[0]) < 0); /* newer updated first  */
  ASSERT_TRUE(asper_hit_cmp(&h[1], &h[0]) < 0); /* smaller id first     */
  ASSERT_TRUE(asper_hit_cmp(&h[0], &h[1]) > 0);
  ASSERT_TRUE(asper_hit_cmp(&h[0], &h[0]) == 0);
  /* full sort is deterministic */
  qsort(h, 4, sizeof h[0], hit_cmp_qsort);
  ASSERT_EQ_STR(h[0].rec->id, "bbbbbbbb-0000-4000-8000-000000000000");
  ASSERT_EQ_STR(h[1].rec->id, "dddddddd-0000-4000-8000-000000000000");
  ASSERT_EQ_STR(h[2].rec->id, "aaaaaaaa-0000-4000-8000-000000000000");
  ASSERT_EQ_STR(h[3].rec->id, "cccccccc-0000-4000-8000-000000000000");
}

TEST(prune_threshold_boundary) {
  /* the review candidacy math the maintenance cycle relies on */
  asper_config cfg;
  asper_record r;
  double eff;
  asper_config_defaults(&cfg);
  ASSERT_EQ_DBL(cfg.prune_threshold, 0.15, 1e-12);
  base_record(&r, ASPER_SECTION_CONTEXT, 0.8, T0);
  /* 80 days: 0.8 * 0.5^(80/30) ~= 0.126 < 0.15 => candidate */
  eff = asper_eff_relevance(&cfg, &r, T0 + 80 * DAY);
  ASSERT_EQ_DBL(eff, 0.8 * pow(0.5, 80.0 / 30.0), 1e-9);
  ASSERT_TRUE(eff < cfg.prune_threshold);
  /* 30 days: 0.4 => not a candidate */
  ASSERT_TRUE(asper_eff_relevance(&cfg, &r, T0 + 30 * DAY) >
              cfg.prune_threshold);
  asper_config_free(&cfg);
}

TEST_LIST = {
    TEST_ENTRY(half_life_defaults),
    TEST_ENTRY(eff_halves_at_half_life),
    TEST_ENTRY(identity_never_decays),
    TEST_ENTRY(score_weights),
    TEST_ENTRY(hit_tie_break_ordering),
    TEST_ENTRY(prune_threshold_boundary),
};

RUN_ALL_TESTS()
