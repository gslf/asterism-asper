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
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201) /* nameless struct/union */
#endif
#include "llama.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "llama_guard.h"

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
  bool kv_cache;
  llama_token *cached_prompt;
  int32_t cached_prompt_n;
  volatile int *cancel;
  int64_t deadline;
} cll_ud;

static asper_err cll_control(const cll_ud *u) {
  if (u->cancel && *u->cancel) return ASPER_ERR_CANCELLED;
  if (u->deadline > 0 && os_monotonic_ms() >= u->deadline) return ASPER_ERR_TIMEOUT;
  return ASPER_OK;
}
static bool cll_abort(void *ud) { return cll_control(ud) != ASPER_OK; }

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
  if (strlen(text) > (size_t)INT32_MAX)
    return ASPER_ERR_INVALID;
  len = (int32_t)strlen(text);
  n = asper_llg_tokenize(vocab, text, len, NULL, 0, add_special,
                         parse_special);
  if (n == INT32_MIN)
    return ASPER_ERR_MODEL;
  if (n < 0)
    n = -n;
  if (n == 0)
    return ASPER_OK;
  tok = (llama_token *)malloc((size_t)n * sizeof *tok);
  if (tok == NULL)
    return ASPER_ERR_NOMEM;
  got = asper_llg_tokenize(vocab, text, len, tok, n, add_special,
                           parse_special);
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

/* Template rendering preserves roles; this C backend accepts text blocks only. */
static asper_err cll_apply_template(const char *tmpl, const asmodel_input *input, char **out) {
  struct llama_chat_message messages[256];
  char *content[256] = {0};
  asper_err e = ASPER_ERR_UNSUPPORTED;
  *out = NULL;
  if (asmodel_input_validate(input) != ASMODEL_OK) return ASPER_ERR_INVALID;
  for (size_t i = 0; i < input->count; i++) {
    asmodel_err result = asmodel_message_text(&input->messages[i],&content[i]);
    if (result != ASMODEL_OK) { e = result == ASMODEL_ERR_NOMEM ? ASPER_ERR_NOMEM : ASPER_ERR_UNSUPPORTED; goto done; }
    messages[i].role = asmodel_role_name(input->messages[i].role);
    messages[i].content = content[i];
  }
  int32_t need = asper_llg_chat_apply_template(tmpl,messages,input->count,true,NULL,0);
  if (need < 0 || need == INT32_MAX) goto done;
  char *buf = malloc((size_t)need+1);
  if (!buf) { e = ASPER_ERR_NOMEM; goto done; }
  int32_t got = asper_llg_chat_apply_template(tmpl,messages,input->count,true,buf,need+1);
  if (got < 0 || got > need) { free(buf); goto done; }
  buf[got] = 0; *out = buf; e = ASPER_OK;
done:
  for (size_t i = 0; i < input->count; i++) free(content[i]);
  return e;
}

static asper_err cll_generate(void *ud, const asmodel_input *input, const char *gbnf,
                              const asper_output_contract *contract,
                              const asmodel_generate_params *params, volatile int *cancel, char **out_text)
{
  (void)contract;
  cll_ud *u = (cll_ud *)ud;
  char *prompt = NULL;
  llama_token *tok = NULL;
  int32_t n_tok = 0;
  int32_t prompt_start = 0;
  struct llama_sampler *chain = NULL;
  struct llama_sampler *greedy;
  asper_buf outbuf;
  asper_err e;
  int32_t i, n_past;
  int produced = 0, limit;
  bool hit_eog = false, constrained = false;
  asmodel_generation_info local = {0};
  asmodel_generation_info *info = params->result_info ? params->result_info : &local;
  memset(info,0,sizeof *info); info->usage_known = 1;

  *out_text = NULL;
  asper_buf_init(&outbuf);
  int64_t started = os_monotonic_ms();
  u->cancel = cancel;
  u->deadline = params->deadline_ms <= 0 ? 0 : params->deadline_ms > INT64_MAX-started ?
      INT64_MAX : started+params->deadline_ms;
  e = cll_control(u);
  if (e != ASPER_OK) goto out;
  if (params->tools) { e = ASPER_ERR_UNSUPPORTED; goto out; }
  if ((params->require_constraint && !gbnf) || params->reasoning == ASMODEL_REASONING_REQUIRED_ON ||
      params->reasoning == ASMODEL_REASONING_BUDGETED ||
      (params->reasoning == ASMODEL_REASONING_REQUIRED_OFF && !gbnf)) {
    e = ASPER_ERR_UNSUPPORTED; goto out;
  }

  e = cll_apply_template(u->chat_template,
                         input, &prompt);
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

  if (u->kv_cache && u->cached_prompt && u->cached_prompt_n > 0) {
    int32_t common = 0;
    while (common < n_tok && common < u->cached_prompt_n &&
           tok[common] == u->cached_prompt[common]) common++;
    if (common >= n_tok) common = n_tok - 1;
    if (common > 0 &&
        llama_memory_seq_rm(llama_get_memory(u->lctx), -1, common, -1))
      prompt_start = common;
    else
      asper_llg_memory_clear(llama_get_memory(u->lctx), true);
  } else {
    asper_llg_memory_clear(llama_get_memory(u->lctx), true);
  }

  /* Fresh sampler chain per call: grammar state is per-generation. */
  chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
  if (chain == NULL) {
    e = ASPER_ERR_NOMEM;
    goto out;
  }
  if (gbnf != NULL) {
    struct llama_sampler *grammar =
        asper_llg_sampler_init_grammar(u->vocab, gbnf, "root");
    if (grammar == NULL) {
      e = ASPER_ERR_MODEL; /* GBNF failed to parse */
      goto out;
    }
    llama_sampler_chain_add(chain, grammar);
    constrained = true;
  }
  greedy = llama_sampler_init_greedy();
  if (greedy == NULL) {
    e = ASPER_ERR_NOMEM;
    goto out;
  }
  llama_sampler_chain_add(chain, greedy);

  for (i = prompt_start; i < n_tok; i += u->n_batch) {
    if (cll_control(u) != ASPER_OK) {
      e = cll_control(u);
      goto out;
    }
    int32_t chunk = n_tok - i < u->n_batch ? n_tok - i : u->n_batch;
    struct llama_batch batch = llama_batch_get_one(tok + i, chunk);
    if (asper_llg_decode(u->lctx, batch) != 0) {
      e = cll_control(u); if (e == ASPER_OK) e = ASPER_ERR_MODEL;
      goto out;
    }
  }

  limit = params->max_tokens > 0 ? params->max_tokens : INT_MAX;
  n_past = n_tok;
  produced = 0;
  while (produced < limit) {
    if (cll_control(u) != ASPER_OK) {
      e = cll_control(u);
      goto out;
    }
    llama_token t;
    if (asper_llg_sampler_sample(chain, u->lctx, -1, &t) != 0) {
      e = ASPER_ERR_MODEL; goto out;
    }
    if (llama_vocab_is_eog(u->vocab, t)) { hit_eog = true; break; }
    e = cll_append_piece(u->vocab, t, &outbuf);
    if (e != ASPER_OK)
      goto out;
    produced++;
    if (produced >= limit || n_past >= u->n_ctx)
      break;
    {
      struct llama_batch batch = llama_batch_get_one(&t, 1);
      if (asper_llg_decode(u->lctx, batch) != 0) {
        e = cll_control(u); if (e == ASPER_OK) e = ASPER_ERR_MODEL;
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
  e = hit_eog ? ASPER_OK : ASPER_ERR_LIMIT;
  if (u->kv_cache && e == ASPER_OK) {
    llama_token *cached =
        (llama_token *)malloc((size_t)n_tok * sizeof *cached);
    if (cached) {
      memcpy(cached, tok, (size_t)n_tok * sizeof *cached);
      free(u->cached_prompt);
      u->cached_prompt = cached;
      u->cached_prompt_n = n_tok;
    }
  }

out:
  u->cancel = NULL; u->deadline = 0;
  if (!*out_text && outbuf.len) *out_text = asper_buf_detach(&outbuf);
  info->input_tokens = n_tok; info->output_tokens = produced; info->cached_input_tokens = prompt_start;
  info->usage_known = !n_tok || e == ASPER_OK || e == ASPER_ERR_LIMIT;
  info->finish_reason = e == ASPER_OK ? ASMODEL_FINISH_STOP : e == ASPER_ERR_LIMIT ?
      ASMODEL_FINISH_LENGTH : e == ASPER_ERR_CANCELLED ? ASMODEL_FINISH_CANCELLED : ASMODEL_FINISH_ERROR;
  if (constrained) info->applied |= ASMODEL_APPLIED_CONSTRAINT;
  if (e != ASPER_OK) snprintf(info->error,sizeof info->error,"%s",asper_err_name(e));
  if (chain != NULL)
    llama_sampler_free(chain);
  free(tok);
  free(prompt);
  asper_buf_free(&outbuf);
  if (e != ASPER_OK) {
    free(u->cached_prompt);
    u->cached_prompt = NULL;
    u->cached_prompt_n = 0;
    asper_llg_memory_clear(llama_get_memory(u->lctx), true);
  }
  return e;
}

static int cll_count_tokens(void *ud, const char *text)
{
  cll_ud *u = (cll_ud *)ud;
  int32_t n;

  if (text == NULL)
    return 0;
  if (strlen(text) > (size_t)INT32_MAX)
    return -1;
  n = asper_llg_tokenize(u->vocab, text, (int32_t)strlen(text), NULL, 0,
                         false, false);
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
  free(u->cached_prompt);
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
  u->kv_cache = c->cfg.curator_kv_cache;

  mparams = llama_model_default_params();
  /* -1 = every layer in VRAM (llama.h: negative means all); 0 keeps
   * the model on the CPU. CPU-only builds ignore the value. */
  mparams.n_gpu_layers = c->cfg.curator_gpu_layers;
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
  cparams.abort_callback = cll_abort; cparams.abort_callback_data = u;

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
