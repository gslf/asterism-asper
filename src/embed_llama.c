/*
 * embed_llama.c — llama.cpp embedding backend.
 *
 * Two llama contexts share one loaded model, each guarded by its own
 * mutex: the query context serves the hot path from any thread, the
 * passage context serves the apply funnel (any API thread) and the
 * curator's dedup probe (worker thread) without contending with queries.
 * Output is mean-pooled by llama.cpp (LLAMA_POOLING_TYPE_MEAN) and
 * L2-normalized here.
 *
 * This file also owns the process-wide llama.cpp bootstrap shared with
 * curator_llama.c: asper_llama_backend_init().
 */

#include "asper_internal.h"

#include <string.h>

#ifdef ASPER_WITH_LLAMA

#include <math.h>
#include <stdlib.h>

/* llama.h/ggml.h are not pedantic-C99-clean (anonymous unions, typedef
 * redefinitions); include them with those diagnostics off. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc11-extensions"
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "llama.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#if !defined(ASPER_NO_THREADS)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif
#endif

/* ---- process-wide llama bootstrap (shared with curator_llama.c) --------- */

/* llama.cpp logs to stderr by default. The library must never write to
 * stdout/stderr on its own initiative, and this global callback has no
 * asper_ctx to forward into asper_log, so it deliberately drops every
 * record: llama output is silenced entirely. Asper reports model
 * load/inference failures through its own error and log channels. */
static void asper_llama_log_silent(enum ggml_log_level level,
                                   const char *text, void *ud)
{
  (void)level;
  (void)text;
  (void)ud;
}

static void asper_llama_boot(void)
{
  llama_log_set(asper_llama_log_silent, NULL);
  llama_backend_init();
}

#if defined(ASPER_NO_THREADS)

void asper_llama_backend_init(void)
{
  static bool done = false;
  if (!done) {
    done = true;
    asper_llama_boot();
  }
}

#elif defined(_WIN32)

static BOOL CALLBACK asper_llama_boot_cb(PINIT_ONCE once, PVOID param,
                                         PVOID *ctx)
{
  (void)once;
  (void)param;
  (void)ctx;
  asper_llama_boot();
  return TRUE;
}

void asper_llama_backend_init(void)
{
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(&once, asper_llama_boot_cb, NULL, NULL);
}

#else /* POSIX threads */

static pthread_once_t asper_llama_once = PTHREAD_ONCE_INIT;

void asper_llama_backend_init(void)
{
  pthread_once(&asper_llama_once, asper_llama_boot);
}

#endif

/* ---- backend state ------------------------------------------------------ */

#define ELL_N_CTX 512 /* modest embedding window; inputs are truncated */

typedef struct {
  struct llama_model *model;
  const struct llama_vocab *vocab;
  struct llama_context *lctx_passage; /* any thread, guarded by p_mu:
                                         the apply funnel embeds passages
                                         from API threads, the curator's
                                         dedup embeds from the worker   */
  struct llama_context *lctx_query;   /* any thread, guarded by q_mu   */
  os_mutex q_mu;
  os_mutex p_mu;
  int dim;
  int n_ctx;
  bool encoder_only;
  char *query_prefix;   /* owned copies: the backend must not depend on */
  char *passage_prefix; /* cfg lifetime                                 */
} ell_ud;

/* Tokenize text into a malloc'd array using the negative-return resize
 * convention (first call sizes, second call fills). *out_tok is NULL when
 * the text yields zero tokens. */
static asper_err ell_tokenize(const struct llama_vocab *vocab,
                              const char *text, bool add_special,
                              bool parse_special, llama_token **out_tok,
                              int32_t *out_n)
{
  int32_t len, n, got;
  llama_token *tok;

  *out_tok = NULL;
  *out_n = 0;
  if (strlen(text) > (size_t)INT32_MAX)
    return ASPER_ERR_INVALID;
  len = (int32_t)strlen(text);
  n = llama_tokenize(vocab, text, len, NULL, 0, add_special, parse_special);
  if (n == INT32_MIN)
    return ASPER_ERR_MODEL;
  if (n < 0)
    n = -n;
  if (n == 0)
    return ASPER_OK;
  tok = (llama_token *)malloc((size_t)n * sizeof *tok);
  if (tok == NULL)
    return ASPER_ERR_NOMEM;
  got = llama_tokenize(vocab, text, len, tok, n, add_special, parse_special);
  if (got < 0) {
    free(tok);
    return ASPER_ERR_MODEL;
  }
  *out_tok = tok;
  *out_n = got;
  return ASPER_OK;
}

static asper_err ell_embed_one(ell_ud *u, struct llama_context *lctx,
                               const char *prefix, const char *text,
                               float *out)
{
  char *full = NULL;
  const char *input = text;
  llama_token *tok = NULL;
  int32_t n_tok = 0;
  const float *emb;
  double norm;
  asper_err e;
  int i, rc;

  if (input == NULL)
    input = "";
  if (prefix != NULL && prefix[0] != '\0') {
    size_t pl = strlen(prefix), tl = strlen(input);
    full = (char *)malloc(pl + tl + 1);
    if (full == NULL)
      return ASPER_ERR_NOMEM;
    memcpy(full, prefix, pl);
    memcpy(full + pl, input, tl + 1);
    input = full;
  }

  e = ell_tokenize(u->vocab, input, true, false, &tok, &n_tok);
  if (e == ASPER_OK && n_tok == 0) {
    /* Some vocabs tokenize "" to nothing even with add_special; embed a
     * single space so the call still produces a vector. */
    e = ell_tokenize(u->vocab, " ", true, false, &tok, &n_tok);
    if (e == ASPER_OK && n_tok == 0)
      e = ASPER_ERR_MODEL;
  }
  free(full);
  if (e != ASPER_OK) {
    free(tok);
    return e;
  }
  if (n_tok > u->n_ctx)
    n_tok = u->n_ctx; /* truncate: whole-input embedding is best effort */

  /* Reset any sequence state left by the previous call (NULL-safe for
   * memory-less encoder contexts). */
  llama_memory_clear(llama_get_memory(lctx), true);

  /* llama_batch_get_one: seq 0, auto positions; with embeddings enabled
   * every token is an output, which mean pooling requires. */
  {
    struct llama_batch batch = llama_batch_get_one(tok, n_tok);
    rc = u->encoder_only ? llama_encode(lctx, batch)
                         : llama_decode(lctx, batch);
  }
  free(tok);
  if (rc != 0)
    return ASPER_ERR_MODEL;

  emb = llama_get_embeddings_seq(lctx, 0);
  if (emb == NULL)
    return ASPER_ERR_MODEL;

  norm = 0.0;
  for (i = 0; i < u->dim; i++)
    norm += (double)emb[i] * (double)emb[i];
  norm = sqrt(norm);
  if (norm > 0.0) {
    for (i = 0; i < u->dim; i++)
      out[i] = (float)((double)emb[i] / norm);
  } else {
    for (i = 0; i < u->dim; i++)
      out[i] = 0.0f;
  }
  return ASPER_OK;
}

static asper_err ell_embed(void *ud, const char *text, int is_query,
                           float *out)
{
  ell_ud *u = (ell_ud *)ud;
  asper_err e;

  if (is_query) {
    os_mutex_lock(&u->q_mu);
    e = ell_embed_one(u, u->lctx_query, u->query_prefix, text, out);
    os_mutex_unlock(&u->q_mu);
  } else {
    os_mutex_lock(&u->p_mu);
    e = ell_embed_one(u, u->lctx_passage, u->passage_prefix, text, out);
    os_mutex_unlock(&u->p_mu);
  }
  return e;
}

static void ell_destroy(void *ud)
{
  ell_ud *u = (ell_ud *)ud;

  if (u == NULL)
    return;
  if (u->lctx_query != NULL)
    llama_free(u->lctx_query);
  if (u->lctx_passage != NULL)
    llama_free(u->lctx_passage);
  if (u->model != NULL)
    llama_model_free(u->model);
  os_mutex_destroy(&u->q_mu);
  os_mutex_destroy(&u->p_mu);
  free(u->query_prefix);
  free(u->passage_prefix);
  free(u);
}

/* Basename of path with any trailing ".gguf" stripped, truncated to fit. */
static void ell_model_id(const char *path, char out[128])
{
  const char *base = path;
  const char *p;
  size_t n;

  for (p = path; *p != '\0'; p++) {
    if (*p == '/' || *p == '\\')
      base = p + 1;
  }
  n = strlen(base);
  if (n > 5 && strcmp(base + n - 5, ".gguf") == 0)
    n -= 5;
  if (n >= 128)
    n = 127;
  memcpy(out, base, n);
  out[n] = '\0';
}

asper_err asper_embedder_llama_create(asper_ctx *c, asper_embedder *out)
{
  const char *path = c->cfg.embed_model_path;
  struct llama_model_params mparams;
  struct llama_context_params cparams;
  ell_ud *u;
  asper_err e;
  int hw, threads;

  memset(out, 0, sizeof *out);
  if (asper_str_blank(path))
    return asper_seterr(c, ASPER_ERR_MODEL,
                        "embedding.model_path is not set");

  asper_llama_backend_init();

  u = (ell_ud *)calloc(1, sizeof *u);
  if (u == NULL)
    return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
  os_mutex_init(&u->q_mu);
  os_mutex_init(&u->p_mu);

  mparams = llama_model_default_params();
  /* -1 = every layer in VRAM (llama.h: negative means all); 0 keeps
   * the model on the CPU. CPU-only builds ignore the value. */
  mparams.n_gpu_layers = c->cfg.embed_gpu_layers;
  u->model = llama_model_load_from_file(path, mparams);
  if (u->model == NULL) {
    e = asper_seterr(c, ASPER_ERR_MODEL,
                     "failed to load embedding model: %s", path);
    goto fail;
  }
  u->vocab = llama_model_get_vocab(u->model);
  /* Pooled sequence embeddings are sized n_embd_out, which differs from
   * n_embd on projection models (e.g. LFM2). */
  u->dim = llama_model_n_embd_out(u->model);
  if (u->vocab == NULL || u->dim <= 0) {
    e = asper_seterr(c, ASPER_ERR_MODEL,
                     "embedding model has no usable vocab/embedding: %s",
                     path);
    goto fail;
  }
  u->encoder_only = llama_model_has_encoder(u->model) &&
                    !llama_model_has_decoder(u->model);

  hw = os_hardware_threads();
  threads = hw < 4 ? hw : 4;
  cparams = llama_context_default_params();
  cparams.n_ctx = ELL_N_CTX;
  cparams.n_batch = ELL_N_CTX;
  cparams.n_ubatch = ELL_N_CTX; /* pooling needs the input in one ubatch */
  cparams.n_threads = threads;
  cparams.n_threads_batch = threads;
  cparams.embeddings = true;
  cparams.pooling_type = LLAMA_POOLING_TYPE_MEAN;

  u->lctx_passage = llama_init_from_model(u->model, cparams);
  if (u->lctx_passage == NULL) {
    e = asper_seterr(c, ASPER_ERR_MODEL,
                     "failed to create embedding context: %s", path);
    goto fail;
  }
  u->lctx_query = llama_init_from_model(u->model, cparams);
  if (u->lctx_query == NULL) {
    e = asper_seterr(c, ASPER_ERR_MODEL,
                     "failed to create query embedding context: %s", path);
    goto fail;
  }
  /* Truncation bound: the whole input must fit both the context and one
   * logical batch (llama may round n_ctx up past the requested 512). */
  u->n_ctx = (int)llama_n_ctx(u->lctx_passage);
  if ((int)llama_n_batch(u->lctx_passage) < u->n_ctx)
    u->n_ctx = (int)llama_n_batch(u->lctx_passage);
  if (u->n_ctx <= 0)
    u->n_ctx = ELL_N_CTX;

  u->query_prefix =
      asper_strdup(c->cfg.query_prefix != NULL ? c->cfg.query_prefix : "");
  u->passage_prefix =
      asper_strdup(c->cfg.passage_prefix != NULL ? c->cfg.passage_prefix
                                                 : "");
  if (u->query_prefix == NULL || u->passage_prefix == NULL) {
    e = asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");
    goto fail;
  }

  ell_model_id(path, out->model_id);
  e = asper_sha256_file(path, out->model_hash);
  if (e != ASPER_OK) {
    e = asper_seterr(c, e, "failed to hash embedding model file: %s", path);
    goto fail;
  }
  /* Vectors are a product of the weights AND preprocessing.  Store a
   * pipeline hash in the existing cache/manifest hash field so changing a
   * model-owned prefix cannot reuse an incompatible index.  Include role
   * separators because query and passage preprocessing are asymmetric. */
  {
    uint8_t weights_hash[32];
    memcpy(weights_hash, out->model_hash, sizeof weights_hash);
    asper_embedding_pipeline_hash(weights_hash, u->query_prefix,
                                  u->passage_prefix, out->model_hash);
  }

  out->ud = u;
  out->dim = u->dim;
  out->embed = ell_embed;
  out->destroy = ell_destroy;

  asper_log(c, ASPER_LOG_INFO, "embed",
            "model loaded: %s (dim=%d, n_ctx=%d, %s)", out->model_id,
            u->dim, u->n_ctx, u->encoder_only ? "encoder" : "decoder");
  return ASPER_OK;

fail:
  ell_destroy(u);
  memset(out, 0, sizeof *out);
  return e;
}

#else /* !ASPER_WITH_LLAMA */

asper_err asper_embedder_llama_create(asper_ctx *c, asper_embedder *out)
{
  memset(out, 0, sizeof *out);
  return asper_seterr(c, ASPER_ERR_MODEL,
                      "libasper built without llama.cpp support");
}

#endif /* ASPER_WITH_LLAMA */
