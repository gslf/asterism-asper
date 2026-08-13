/* fakes.c — deterministic test doubles for libasper. See fakes.h. */

#include "fakes.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- fake embedder ------------------------------------------------------ */

static int fake_is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
         ch == '\v' || ch == '\f';
}

void fake_embed_text(const char *text, float out[FAKE_EMBED_DIM]) {
  const char *p = text ? text : "";
  double norm = 0.0;
  size_t i;
  for (i = 0; i < FAKE_EMBED_DIM; i++) out[i] = 0.0f;
  while (*p) {
    char word[128];
    size_t w = 0;
    while (*p && fake_is_space(*p)) p++;
    while (*p && !fake_is_space(*p)) {
      char ch = *p++;
      if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
      if (w + 1 < sizeof word) word[w++] = ch;
    }
    if (w > 0) out[asper_fnv1a64(word, w) % FAKE_EMBED_DIM] += 1.0f;
  }
  for (i = 0; i < FAKE_EMBED_DIM; i++)
    norm += (double)out[i] * (double)out[i];
  if (norm > 0.0) {
    norm = sqrt(norm);
    for (i = 0; i < FAKE_EMBED_DIM; i++)
      out[i] = (float)((double)out[i] / norm);
  }
}

static asper_err fake_embed_cb(void *ud, const char *text, int is_query,
                               float *out) {
  (void)ud;
  (void)is_query;
  if (!text || !out) return ASPER_ERR_INVALID;
  fake_embed_text(text, out);
  return ASPER_OK;
}

static void fake_noop_destroy(void *ud) { (void)ud; }

asper_embedder fake_embedder_make(void) {
  asper_embedder e;
  memset(&e, 0, sizeof e);
  e.ud = NULL;
  e.dim = FAKE_EMBED_DIM;
  strcpy(e.model_id, "fake-embedder");
  memset(e.model_hash, 0x11, sizeof e.model_hash);
  e.embed = fake_embed_cb;
  e.destroy = fake_noop_destroy;
  return e;
}

/* ---- fake curator ------------------------------------------------------- */

void fake_curator_init(fake_curator *fc) { memset(fc, 0, sizeof *fc); }

void fake_curator_dispose(fake_curator *fc) {
  size_t i;
  if (!fc) return;
  for (i = 0; i < fc->n; i++) free(fc->replies[i]);
  free(fc->replies);
  free(fc->last_system);
  free(fc->last_user);
  free(fc->last_grammar);
  memset(fc, 0, sizeof *fc);
}

int fake_curator_push(fake_curator *fc, const char *reply) {
  char *copy = asper_strdup(reply);
  if (!copy) return 0;
  if (fc->n == fc->cap) {
    size_t ncap = fc->cap ? fc->cap * 2 : 8;
    char **nr = (char **)realloc(fc->replies, ncap * sizeof *nr);
    if (!nr) {
      free(copy);
      return 0;
    }
    fc->replies = nr;
    fc->cap = ncap;
  }
  fc->replies[fc->n++] = copy;
  return 1;
}

static asper_err fake_curator_generate(void *ud, const char *system_prompt,
                                       const char *user_prompt,
                                       const char *gbnf, int max_tokens,
                                       char **out_text) {
  fake_curator *fc = (fake_curator *)ud;
  const char *reply = "NOOP\n";
  (void)max_tokens;
  if (!fc || !out_text) return ASPER_ERR_INVALID;
  free(fc->last_system);
  fc->last_system = asper_strdup(system_prompt);
  free(fc->last_user);
  fc->last_user = asper_strdup(user_prompt);
  free(fc->last_grammar);
  fc->last_grammar = asper_strdup(gbnf);
  fc->calls++;
  if (fc->next < fc->n) reply = fc->replies[fc->next++];
  *out_text = asper_strdup(reply);
  return *out_text ? ASPER_OK : ASPER_ERR_NOMEM;
}

static int fake_curator_count_tokens(void *ud, const char *text) {
  size_t len;
  (void)ud;
  if (!text) return 0;
  len = strlen(text);
  if (len == 0) return 0;
  return len < 4 ? 1 : (int)(len / 4);
}

asper_curator_iface fake_curator_iface_make(fake_curator *fc) {
  asper_curator_iface it;
  memset(&it, 0, sizeof it);
  it.ud = fc;
  it.generate = fake_curator_generate;
  it.count_tokens = fake_curator_count_tokens;
  it.destroy = fake_noop_destroy;
  return it;
}

/* ---- fake clock --------------------------------------------------------- */

static asper_time fake_clock_now_cb(void *ud) {
  return ((const fake_clock *)ud)->now;
}

void fake_clock_set(fake_clock *fc, int64_t t) { fc->now = t; }

asper_clock fake_clock_make(fake_clock *fc) {
  asper_clock c;
  c.ud = fc;
  c.now = fake_clock_now_cb;
  return c;
}
