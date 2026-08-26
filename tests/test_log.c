/* test_log.c — log.c: line format with the injected clock, level gate,
 * size-based rotation with max_files, host callback fan-out. */

#include "asper_test.h"

#include "asper_internal.h"
#include "fakes.h"

#define T0 1785319920LL /* 2026-07-29T10:12:00Z */

static fake_clock g_clk;
static fake_curator g_cur;

static asper_ctx *open_with_log(const char *root, const char *level,
                                int max_size_kb, int max_files,
                                char log_path[512]) {
  char cfg_path[512];
  char cfg_text[1024];
  asper_open_params p;
  memset(&p, 0, sizeof p);
  asper_embedder emb = fake_embedder_make();
  asper_curator_iface ci = fake_curator_iface_make(&g_cur);
  asper_clock ck = fake_clock_make(&g_clk);
  asper_ctx *c = NULL;
  snprintf(log_path, 512, "%s/asper.log", root);
  snprintf(cfg_path, sizeof cfg_path, "%s/log_config.xcdn", root);
  snprintf(cfg_text, sizeof cfg_text,
           "#asper_config { logging: { path: \"%s\", level: \"%s\","
           " max_size_kb: %d, max_files: %d } }\n",
           log_path, level, max_size_kb, max_files);
  if (os_write_file(cfg_path, cfg_text, strlen(cfg_text)) != ASPER_OK)
    return NULL;
  p.memory_root = root;
  p.config_path = cfg_path;
  if (asper_open_with(&p, &emb, &ci, &ck, &c) != ASPER_OK) return NULL;
  asper_set_logger(c, NULL, NULL); /* keep stderr quiet; file sink stays */
  return c;
}

TEST(file_sink_line_format) {
  char root[256], log_path[512];
  asper_ctx *c;
  char *text;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_with_log(root, "debug", 5120, 3, log_path);
  ASSERT_TRUE(c != NULL);
  asper_log(c, ASPER_LOG_INFO, "store", "unit test line %d", 7);
  asper_log(c, ASPER_LOG_WARN, "curator", "warn-token");
  asper_log(c, ASPER_LOG_DEBUG, "index", "debug-token");
  /* the injected clock stamps the records */
  fake_clock_set(&g_clk, T0 + 3600);
  asper_log(c, ASPER_LOG_ERROR, "recall", "second-stamp");
  asper_close(c); /* close flushes the sink */
  text = asper_test_read_file(log_path, NULL);
  ASSERT_TRUE(text != NULL);
  /* exact line shape: ts, level %-5s, subsystem %-8s, message */
  ASSERT_TRUE(strstr(text,
                     "2026-07-29T10:12:00Z INFO  store    unit test line 7\n")
              != NULL);
  ASSERT_TRUE(strstr(text, "2026-07-29T10:12:00Z WARN  curator  warn-token\n")
              != NULL);
  ASSERT_TRUE(strstr(text, "2026-07-29T10:12:00Z DEBUG index    debug-token\n")
              != NULL);
  ASSERT_TRUE(strstr(text, "2026-07-29T11:12:00Z ERROR recall   second-stamp\n")
              != NULL);
  free(text);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(level_gate) {
  char root[256], log_path[512];
  asper_ctx *c;
  char *text;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  c = open_with_log(root, "warn", 5120, 3, log_path);
  ASSERT_TRUE(c != NULL);
  asper_log(c, ASPER_LOG_INFO, "store", "gate-info-token");
  asper_log(c, ASPER_LOG_DEBUG, "store", "gate-debug-token");
  asper_log(c, ASPER_LOG_WARN, "store", "gate-warn-token");
  asper_log(c, ASPER_LOG_ERROR, "store", "gate-error-token");
  asper_close(c);
  text = asper_test_read_file(log_path, NULL);
  ASSERT_TRUE(text != NULL);
  ASSERT_TRUE(strstr(text, "gate-warn-token") != NULL);
  ASSERT_TRUE(strstr(text, "gate-error-token") != NULL);
  ASSERT_TRUE(strstr(text, "gate-info-token") == NULL);
  ASSERT_TRUE(strstr(text, "gate-debug-token") == NULL);
  free(text);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST(rotation_size_cap_max_files) {
  char root[256], log_path[512], rotated[600];
  asper_ctx *c;
  char filler[121];
  uint64_t sz = 0;
  int i;
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  /* rotate above 1 KB, keep 2 files total: asper.log + asper.log.1 */
  c = open_with_log(root, "info", 1, 2, log_path);
  ASSERT_TRUE(c != NULL);
  memset(filler, 'm', sizeof filler - 1);
  filler[sizeof filler - 1] = '\0';
  for (i = 0; i < 60; i++)
    asper_log(c, ASPER_LOG_INFO, "store", "%04d %s", i, filler);
  asper_close(c);
  ASSERT_TRUE(os_file_exists(log_path));
  snprintf(rotated, sizeof rotated, "%s.1", log_path);
  ASSERT_TRUE(os_file_exists(rotated));
  snprintf(rotated, sizeof rotated, "%s.2", log_path);
  ASSERT_TRUE(!os_file_exists(rotated)); /* max_files respected */
  /* rotated-out files stay near the cap: cap + one line of slack */
  snprintf(rotated, sizeof rotated, "%s.1", log_path);
  ASSERT_OK(os_file_size(rotated, &sz));
  ASSERT_TRUE(sz <= 1024 + 256);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

typedef struct {
  int calls;
  int last_level;
  char last_msg[512];
} cb_sink;

static void log_cb(int level, const char *msg, void *userdata) {
  cb_sink *s = (cb_sink *)userdata;
  s->calls++;
  s->last_level = level;
  snprintf(s->last_msg, sizeof s->last_msg, "%s", msg ? msg : "");
}

TEST(callback_receives_records) {
  char root[256];
  asper_open_params p;
  memset(&p, 0, sizeof p);
  asper_embedder emb = fake_embedder_make();
  asper_curator_iface ci;
  asper_clock ck;
  asper_ctx *c = NULL;
  cb_sink sink;
  int calls_after_warn;
  memset(&sink, 0, sizeof sink);
  ASSERT_TRUE(asper_test_tmpdir(root));
  fake_curator_init(&g_cur);
  fake_clock_set(&g_clk, T0);
  ci = fake_curator_iface_make(&g_cur);
  ck = fake_clock_make(&g_clk);
  p.memory_root = root;
  p.config_path = NULL; /* no file sink; the callback is independent */
  ASSERT_OK(asper_open_with(&p, &emb, &ci, &ck, &c));
  asper_set_logger(c, log_cb, &sink);
  asper_log(c, ASPER_LOG_WARN, "store", "cb-token %d", 5);
  ASSERT_TRUE(sink.calls >= 1);
  ASSERT_EQ_INT(sink.last_level, ASPER_LOG_WARN);
  ASSERT_TRUE(strstr(sink.last_msg, "cb-token 5") != NULL);
  calls_after_warn = sink.calls;
  asper_log(c, ASPER_LOG_INFO, "inject", "cb-info-token");
  ASSERT_TRUE(sink.calls > calls_after_warn);
  ASSERT_EQ_INT(sink.last_level, ASPER_LOG_INFO);
  ASSERT_TRUE(strstr(sink.last_msg, "cb-info-token") != NULL);
  /* fn NULL disables the callback */
  asper_set_logger(c, NULL, NULL);
  calls_after_warn = sink.calls;
  asper_log(c, ASPER_LOG_ERROR, "store", "not-delivered");
  ASSERT_EQ_INT(sink.calls, calls_after_warn);
  asper_close(c);
  fake_curator_dispose(&g_cur);
  asper_test_rmtree(root);
}

TEST_LIST = {
    TEST_ENTRY(file_sink_line_format),
    TEST_ENTRY(level_gate),
    TEST_ENTRY(rotation_size_cap_max_files),
    TEST_ENTRY(callback_receives_records),
};

RUN_ALL_TESTS()
