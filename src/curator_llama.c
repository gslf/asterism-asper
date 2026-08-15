/*
 * curator_llama.c — llama.cpp curator backend.
 *
 * Greedy, grammar-constrained generation over a small instruct GGUF.
 * Prompts go through the model's embedded chat template
 * (llama_chat_apply_template); an unsupported template falls back to the
 * llama.cpp "chatml" default. generate() runs on the worker thread only;
 * count_tokens() may run on any thread (llama tokenization is
 * thread-safe and touches no context state).
 */

#include "asper_internal.h"

#include <string.h>

#ifdef ASPER_WITH_LLAMA

#include <limits.h>
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

/* Process-wide llama bootstrap, defined in embed_llama.c. */
void asper_llama_backend_init(void);

#define CLL_N_BATCH 512 /* prompt evaluation chunk size */

typedef struct {
  struct llama_model *model;
  const struct llama_vocab *vocab;
  struct llama_context *lctx; /* worker thread only */
  const char *chat_template;  /* model-owned; NULL = chatml fallback */
  int n_ctx;
  int n_batch;
} cll_ud;

/* Tokenize text into a malloc'd array using the negative-return resize
 * convention. *out_tok is NULL when the text yields zero tokens. */
static asper_err cll_tokenize(const struct llama_vocab *vocab,
                              const char *text, bool add_special,
                              bool parse_special, llama_token **out_tok,
                              int32_t *out_n)
{
  int32_t len, n, got;
  llama_token *tok;

  *out_tok = NULL;
  *out_n = 0;
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

/* Append the detokenized piece for one token, resize convention. Special
 * tokens render as nothing (special=false): the grammar never produces
 * them and EOG stops generation before this is reached. */
static asper_err cll_append_piece(const struct llama_vocab *vocab,
                                  llama_token tok, asper_buf *b)
{
  char small[128];
  char *big;
  int32_t n, got;
  asper_err e;

  n = llama_token_to_piece(vocab, tok, small, (int32_t)sizeof small, 0,
                           false);
  if (n >= 0)
    return asper_buf_append(b, small, (size_t)n);
  if (n == INT32_MIN)
    return ASPER_ERR_MODEL;
  big = (char *)malloc((size_t)-n);
  if (big == NULL)
    return ASPER_ERR_NOMEM;
  got = llama_token_to_piece(vocab, tok, big, -n, 0, false);
  e = got < 0 ? ASPER_ERR_MODEL : asper_buf_append(b, big, (size_t)got);
  free(big);
  return e;
}

/* Render system + user through the chat template with the assistant
 * prompt appended, using the negative-return / resize-and-retry
 * convention. Unsupported model templates fall back to chatml (tmpl
 * NULL). *out is malloc'd. */
static asper_err cll_apply_template(const char *tmpl,
                                    const char *system_prompt,
                                    const char *user_prompt, char **out)
{
  struct llama_chat_message msgs[2];
  int32_t need, got;
  char *buf;

  msgs[0].role = "system";
  msgs[0].content = system_prompt;
  msgs[1].role = "user";
  msgs[1].content = user_prompt;

  need = llama_chat_apply_template(tmpl, msgs, 2, true, NULL, 0);
  if (need < 0 && tmpl != NULL) {
    tmpl = NULL; /* chatml fallback */
    need = llama_chat_apply_template(tmpl, msgs, 2, true, NULL, 0);
  }
  if (need < 0)
    return ASPER_ERR_MODEL;
  buf = (char *)malloc((size_t)need + 1);
  if (buf == NULL)
    return ASPER_ERR_NOMEM;
  got = llama_chat_apply_template(tmpl, msgs, 2, true, buf,
                                  (int32_t)(need + 1));
  if (got < 0 || got > need) {
    free(buf);
    return ASPER_ERR_MODEL;
  }
  buf[got] = '\0';
  *out = buf;
  return ASPER_OK;
}

static asper_err cll_generate(void *ud, const char *system_prompt,
                              const char *user_prompt, const char *gbnf,
                              int max_tokens, char **out_text)
{
  cll_ud *u = (cll_ud *)ud;
  char *prompt = NULL;
  llama_token *tok = NULL;
  int32_t n_tok = 0;
  struct llama_sampler *chain = NULL;
  struct llama_sampler *greedy;
  asper_buf outbuf;
  asper_err e;
  int32_t i, n_past;
  int produced, limit;

  *out_text = NULL;
  asper_buf_init(&outbuf);

  e = cll_apply_template(u->chat_template,
                         system_prompt != NULL ? system_prompt : "",
                         user_prompt != NULL ? user_prompt : "", &prompt);
  if (e != ASPER_OK)
    goto out;

  /* add_special true so BOS-requiring vocabs (Llama-3-class instruct
   * models) get BOS — chat templates do not emit it themselves and
   * add_bos=false vocabs (e.g. Qwen) are unaffected; parse_special true
   * so role markers become their special tokens. Matches the upstream
   * convention of add_special on the first prompt segment. */
  e = cll_tokenize(u->vocab, prompt, true, true, &tok, &n_tok);
  if (e != ASPER_OK)
    goto out;
  if (n_tok == 0 || n_tok >= u->n_ctx) {
    e = ASPER_ERR_MODEL; /* empty or context-overflowing prompt */
    goto out;
  }

  llama_memory_clear(llama_get_memory(u->lctx), true);

  /* Fresh sampler chain per call: grammar state is per-generation. */
  chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
  if (chain == NULL) {
    e = ASPER_ERR_NOMEM;
    goto out;
  }
  if (gbnf != NULL) {
    struct llama_sampler *grammar =
        llama_sampler_init_grammar(u->vocab, gbnf, "root");
    if (grammar == NULL) {
      e = ASPER_ERR_MODEL; /* GBNF failed to parse */
      goto out;
    }
    llama_sampler_chain_add(chain, grammar);
  }
  greedy = llama_sampler_init_greedy();
  if (greedy == NULL) {
    e = ASPER_ERR_NOMEM;
    goto out;
  }
  llama_sampler_chain_add(chain, greedy);

  for (i = 0; i < n_tok; i += u->n_batch) {
    int32_t chunk = n_tok - i < u->n_batch ? n_tok - i : u->n_batch;
    struct llama_batch batch = llama_batch_get_one(tok + i, chunk);
    if (llama_decode(u->lctx, batch) != 0) {
      e = ASPER_ERR_MODEL;
      goto out;
    }
  }

  limit = max_tokens > 0 ? max_tokens : INT_MAX;
  n_past = n_tok;
  produced = 0;
  while (produced < limit) {
    llama_token t = llama_sampler_sample(chain, u->lctx, -1);
    if (llama_vocab_is_eog(u->vocab, t))
      break;
    e = cll_append_piece(u->vocab, t, &outbuf);
    if (e != ASPER_OK)
      goto out;
    produced++;
    if (produced >= limit || n_past >= u->n_ctx)
      break;
    {
      struct llama_batch batch = llama_batch_get_one(&t, 1);
      if (llama_decode(u->lctx, batch) != 0) {
        e = ASPER_ERR_MODEL;
        goto out;
      }
    }
    n_past++;
  }

  *out_text = asper_buf_detach(&outbuf);
  if (*out_text == NULL) {
    *out_text = (char *)malloc(1);
    if (*out_text == NULL) {
      e = ASPER_ERR_NOMEM;
      goto out;
    }
    (*out_text)[0] = '\0';
  }
  e = ASPER_OK;

out:
  if (chain != NULL)
    llama_sampler_free(chain);
  free(tok);
  free(prompt);
  asper_buf_free(&outbuf);
  return e;
}

static int cll_count_tokens(void *ud, const char *text)
{
  cll_ud *u = (cll_ud *)ud;
  int32_t n;

  if (text == NULL)
    return 0;
  n = llama_tokenize(u->vocab, text, (int32_t)strlen(text), NULL, 0, false,
                     false);
  if (n == INT32_MIN)
    return -1;
  return n < 0 ? (int)-n : (int)n;
}

static void cll_destroy(void *ud)
{
  cll_ud *u = (cll_ud *)ud;

  if (u == NULL)
    return;
  if (u->lctx != NULL)
    llama_free(u->lctx);
  if (u->model != NULL)
    llama_model_free(u->model);
  free(u);
}

asper_err asper_curator_llama_create(asper_ctx *c, asper_curator_iface *out)
{
  const char *path = c->cfg.curator_model_path;
  struct llama_model_params mparams;
  struct llama_context_params cparams;
  cll_ud *u;
  asper_err e;
  int hw, threads, n_ctx;

  memset(out, 0, sizeof *out);
  if (asper_str_blank(path))
    return asper_seterr(c, ASPER_ERR_MODEL, "curator.model_path is not set");

  asper_llama_backend_init();

  u = (cll_ud *)calloc(1, sizeof *u);
  if (u == NULL)
    return asper_seterr(c, ASPER_ERR_NOMEM, "out of memory");

  mparams = llama_model_default_params();
  mparams.n_gpu_layers = 0;
  u->model = llama_model_load_from_file(path, mparams);
  if (u->model == NULL) {
    e = asper_seterr(c, ASPER_ERR_MODEL, "failed to load curator model: %s",
                     path);
    goto fail;
  }
  u->vocab = llama_model_get_vocab(u->model);
  if (u->vocab == NULL) {
    e = asper_seterr(c, ASPER_ERR_MODEL, "curator model has no vocab: %s",
                     path);
    goto fail;
  }
  u->chat_template = llama_model_chat_template(u->model, NULL);

  n_ctx = c->cfg.curator_ctx > 0 ? c->cfg.curator_ctx : 4096;
  hw = os_hardware_threads();
  threads = c->cfg.curator_threads > 0 ? c->cfg.curator_threads : 4;
  if (threads > hw)
    threads = hw;

  cparams = llama_context_default_params();
  cparams.n_ctx = (uint32_t)n_ctx;
  cparams.n_batch = CLL_N_BATCH;
  cparams.n_ubatch = CLL_N_BATCH;
  cparams.n_threads = threads;
  cparams.n_threads_batch = threads;

  u->lctx = llama_init_from_model(u->model, cparams);
  if (u->lctx == NULL) {
    e = asper_seterr(c, ASPER_ERR_MODEL,
                     "failed to create curator context: %s", path);
    goto fail;
  }
  u->n_ctx = (int)llama_n_ctx(u->lctx);
  u->n_batch = (int)llama_n_batch(u->lctx);
  if (u->n_batch <= 0)
    u->n_batch = CLL_N_BATCH;

  out->ud = u;
  out->generate = cll_generate;
  out->count_tokens = cll_count_tokens;
  out->destroy = cll_destroy;

  asper_log(c, ASPER_LOG_INFO, "curator",
            "model loaded: %s (n_ctx=%d, threads=%d, params=%lluM, "
            "template=%s)",
            path, u->n_ctx, threads,
            (unsigned long long)(llama_model_n_params(u->model) / 1000000u),
            u->chat_template != NULL ? "model" : "chatml");
  return ASPER_OK;

fail:
  cll_destroy(u);
  memset(out, 0, sizeof *out);
  return e;
}

#else /* !ASPER_WITH_LLAMA */

asper_err asper_curator_llama_create(asper_ctx *c, asper_curator_iface *out)
{
  memset(out, 0, sizeof *out);
  return asper_seterr(c, ASPER_ERR_MODEL,
                      "libasper built without llama.cpp support");
}

#endif /* ASPER_WITH_LLAMA */
