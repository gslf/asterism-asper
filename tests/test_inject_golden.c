/* test_inject_golden.c — inject.c + asper_build_prompt: byte-identical
 * golden prompts, empty-section removal, budget trimming, heuristic
 * estimator. Word choices are tuned against the fake bag-of-words embedder
 * (dim 16) so retrieval order and floor exclusions are forced:
 *   cos("user drinks green tea", C1) ~= 0.833
 *   cos("user drinks green tea", C2) ~= 0.452   (>= 0.25 floor)
 *   cos("quartz falcon heron nest", C1) = 0.0
 *   cos("quartz falcon heron nest", C2) ~= 0.151 (< 0.25 floor)
 *   cos("when is the thesis chapter due", P1) ~= 0.894 */

#include "asper_test.h"

#include "asper_internal.h"
#include "fakes.h"

#define T0 1785319920LL /* 2026-07-29T10:12:00Z */

#define I1 "You are Asper, a careful assistant."
#define I2 "You answer briefly and precisely."
#define C1 "The user drinks green tea every morning."
#define C2 "The user prefers strong coffee at work."
#define P1 "Chapter two of the thesis is due friday."

static fake_clock g_clk;
static fake_curator g_cur;

static asper_ctx *open_store(const char *root, const char *config) {
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

/* Seed identity out of created_at order: I2 first (unlocked), then I1
 * (locked). Locked entries render first, so I1 must render before I2. */
static int seed_identity(asper_ctx *c) {
  char id[37];
  fake_clock_set(&g_clk, T0);
  if (asper_memory_insert(c, ASPER_SECTION_IDENTITY, NULL, I2, 0, id) !=
      ASPER_OK)
    return 0;
  fake_clock_set(&g_clk, T0 + 10);
  return asper_memory_insert(c, ASPER_SECTION_IDENTITY, NULL, I1, 1, id) ==
         ASPER_OK;
}

static const char GOLDEN_FULL[] =
    "BASE\n"
    "\n"
    "# Memory\n"
    "\n"
    "## Who you are\n"
    "- " I1 "\n"
    "- " I2 "\n"
    "\n"
    "## What you remember about the user and the environment\n"
    "- " C1 "\n"
    "- " C2 "\n";

static const char GOLDEN_IDENTITY_ONLY[] =
    "BASE\n"
    "\n"
    "# Memory\n"
    "\n"
    "## Who you are\n"
    "- " I1 "\n"
    "- " I2 "\n";

static const char GOLDEN_PROJECT[] =
    "BASE\n"
    "\n"
    "# Memory\n"
    "\n"
    "## Who you are\n"
    "- " I1 "\n"
    "- " I2 "\n"
    "\n"
    "## Active project: thesis\n"
    "- " P1 "\n";

static const char GOLDEN_TRIMMED[] =
    "BASE\n"
    "\n"
    "# Memory\n"
    "\n"
    "## Who you are\n"
    "- " I1 "\n"
    "- " I2 "\n"
    "\n"
    "## What you remember about the user and the environment\n"
    "- " C1 "\n";

TEST(golden_identity_and_context) {
  char root[256], id[37];
  asper_ctx *c;
  char *prompt = NULL, *again = NULL;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(seed_identity(c));
  fake_clock_set(&g_clk, T0 + 20);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL, C1, 0, id));
  fake_clock_set(&g_clk, T0 + 30);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL, C2, 0, id));

  ASSERT_OK(asper_build_prompt(c, "BASE", "user drinks green tea", &prompt));
  ASSERT_EQ_STR(prompt, GOLDEN_FULL);
  /* rendering is deterministic: identical inputs => identical bytes */
  ASSERT_OK(asper_build_prompt(c, "BASE", "user drinks green tea", &again));
  ASSERT_EQ_STR(again, prompt);
  asper_free(again);
  asper_free(prompt);

  /* below-floor query: the context section vanishes with its heading,
   * and with no active project the project segment is gone too */
  prompt = NULL;
  ASSERT_OK(
      asper_build_prompt(c, "BASE", "quartz falcon heron nest", &prompt));
  ASSERT_EQ_STR(prompt, GOLDEN_IDENTITY_ONLY);
  asper_free(prompt);

  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(golden_active_project) {
  char root[256], id[37];
  asper_ctx *c;
  char *prompt = NULL;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(seed_identity(c));
  fake_clock_set(&g_clk, T0 + 20);
  ASSERT_OK(asper_project_select(c, "thesis"));
  ASSERT_OK(
      asper_memory_insert(c, ASPER_SECTION_PROJECT, "thesis", P1, 0, id));
  /* no context records at all: that section is removed entirely while the
   * project block renders with its name */
  ASSERT_OK(asper_build_prompt(c, "BASE", "when is the thesis chapter due",
                               &prompt));
  ASSERT_EQ_STR(prompt, GOLDEN_PROJECT);
  asper_free(prompt);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(nothing_injected_returns_base) {
  char root[256];
  asper_ctx *c;
  char *prompt = NULL;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_store(root, NULL);
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_build_prompt(c, "BASE", "hello there", &prompt));
  ASSERT_EQ_STR(prompt, "BASE"); /* base returned unchanged */
  asper_free(prompt);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(budget_trimming_heuristic_estimator) {
  /* context budget 10 heuristic tokens: C1's line is exactly 43 bytes
   * (10 tokens) and fits; adding C2's line (38 bytes, 9 tokens) would
   * overflow, so C2 is dropped from the tail. */
  char root[256], cfg_path[512], id[37];
  static const char cfg_text[] =
      "#asper_config {\n"
      "  injection: { token_estimator: \"heuristic\" },\n"
      "  budgets: { context_tokens: 10 },\n"
      "}\n";
  asper_ctx *c;
  char *prompt = NULL;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  snprintf(cfg_path, sizeof cfg_path, "%s/trim_config.xcdn", root);
  ASSERT_OK(os_write_file(cfg_path, cfg_text, sizeof cfg_text - 1));
  c = open_store(root, cfg_path);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(seed_identity(c));
  fake_clock_set(&g_clk, T0 + 20);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL, C1, 0, id));
  fake_clock_set(&g_clk, T0 + 30);
  ASSERT_OK(asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL, C2, 0, id));
  ASSERT_OK(asper_build_prompt(c, "BASE", "user drinks green tea", &prompt));
  ASSERT_EQ_STR(prompt, GOLDEN_TRIMMED);
  asper_free(prompt);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(estimate_tokens_paths) {
  /* the heuristic path must not involve the curator tokenizer */
  char root[256], cfg_path[512];
  static const char cfg_text[] =
      "#asper_config { injection: { token_estimator: \"heuristic\" } }\n";
  asper_ctx *c;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  snprintf(cfg_path, sizeof cfg_path, "%s/h_config.xcdn", root);
  ASSERT_OK(os_write_file(cfg_path, cfg_text, sizeof cfg_text - 1));
  c = open_store(root, cfg_path);
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(asper_estimate_tokens(c, "abcdefgh"), 2); /* 8 bytes / 4 */
  ASSERT_EQ_INT(asper_estimate_tokens(c, "ab"), 1);       /* min 1       */
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST_LIST = {
    TEST_ENTRY(golden_identity_and_context),
    TEST_ENTRY(golden_active_project),
    TEST_ENTRY(nothing_injected_returns_base),
    TEST_ENTRY(budget_trimming_heuristic_estimator),
    TEST_ENTRY(estimate_tokens_paths),
};

RUN_ALL_TESTS()
