/* Shared model-manager bridge. Standalone Asper owns a manager; embedding
 * hosts may lend one with asper_open_at_with_models. */
#include "asper_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CURATOR_ID "asper.curator"
#define EMBED_ID "asper.embedding"

typedef struct {
  asper_ctx *ctx;
  int kind;
  asper_embedder embedder;
  asper_curator_iface curator;
} legacy_provider;

typedef struct {
  asmodel_manager *manager;
  char id[ASMODEL_ID_MAX];
  asper_ctx *ctx;
  int apply_embedding_prefix;
} model_ref;

static int legacy_generate(void *ud, const char *sys, const char *user,
                           const char *grammar,
                           const asmodel_generate_params *params,
                           asmodel_token_fn token_fn, void *token_ud,
                           volatile int *cancel, char **out,
                           int *out_in, int *out_gen) {
  legacy_provider *p = (legacy_provider *)ud;
  asper_err e;
  if (cancel && *cancel) return -1;
  e = p->curator.generate(p->curator.ud, sys, user, grammar,
                          params->max_tokens, params->deadline_ms, out);
  if (e != ASPER_OK) return -1;
  if (out_in) *out_in = 0;
  if (out_gen) *out_gen = p->curator.count_tokens && *out ?
      p->curator.count_tokens(p->curator.ud, *out) : 0;
  if (token_fn && *out) token_fn(*out, strlen(*out), token_ud);
  return 0;
}

static int legacy_embed(void *ud, const char *text, int is_query, float *out) {
  legacy_provider *p = (legacy_provider *)ud;
  return p->embedder.embed(p->embedder.ud, text, is_query, out) == ASPER_OK
             ? 0 : -1;
}

static int legacy_count(void *ud, const char *text) {
  legacy_provider *p = (legacy_provider *)ud;
  return p->curator.count_tokens ?
      p->curator.count_tokens(p->curator.ud, text) : -1;
}

static void legacy_destroy(void *ud) {
  legacy_provider *p = (legacy_provider *)ud;
  if (!p) return;
  if (p->kind == 1 && p->embedder.destroy)
    p->embedder.destroy(p->embedder.ud);
  if (p->kind == 2 && p->curator.destroy)
    p->curator.destroy(p->curator.ud);
  free(p);
}

static int manager_loader(void *ud, const asmodel_spec *spec,
                          asmodel_provider *out, char *error,
                          size_t error_size) {
  asper_ctx *c = (asper_ctx *)ud;
  legacy_provider *p;
  asper_err e;
  if (spec->backend == ASMODEL_BACKEND_OPENAI)
    return asmodel_openai_provider_create(spec, out, error, error_size);
  p = (legacy_provider *)calloc(1, sizeof *p);
  if (!p) return -1;
  p->ctx = c;
  memset(out, 0, sizeof *out);
  if (spec->embedding) {
    p->kind = 1;
    e = asper_embedder_llama_create(c, &p->embedder);
    if (e == ASPER_OK) out->embed = legacy_embed;
  } else {
    p->kind = 2;
    e = asper_curator_llama_create(c, &p->curator);
    if (e == ASPER_OK) {
      out->generate = legacy_generate;
      out->count_tokens = legacy_count;
    }
  }
  if (e != ASPER_OK) {
    snprintf(error, error_size, "%s", asper_last_error(c));
    legacy_destroy(p);
    return -1;
  }
  out->userdata = p;
  out->destroy = legacy_destroy;
  return 0;
}

static size_t estimated_ram_mb(const char *path, int configured) {
  uint64_t bytes = 0;
  if (configured > 0) return (size_t)configured;
  if (path && os_file_size(path, &bytes) == ASPER_OK)
    return (size_t)((bytes + 1024 * 1024 - 1) / (1024 * 1024));
  return 0;
}

static asper_err register_local(asper_ctx *c) {
  asmodel_limits limits;
  asmodel_spec curator, embed;
  asmodel_err me;
  memset(&limits, 0, sizeof limits);
  limits.max_resident = (size_t)c->cfg.models_max_resident;
  limits.max_ram_mb = (size_t)c->cfg.models_max_ram_mb;
  limits.max_vram_mb = (size_t)c->cfg.models_max_vram_mb;
  me = asmodel_manager_create(&limits, manager_loader, c, &c->model_manager);
  if (me != ASMODEL_OK) return ASPER_ERR_NOMEM;
  c->owns_model_manager = true;

  memset(&curator, 0, sizeof curator);
  curator.id = CURATOR_ID;
  curator.backend = (asmodel_backend)c->cfg.curator_backend;
  curator.path = c->cfg.curator_model_path;
  curator.base_url = c->cfg.curator_base_url;
  curator.remote_model = c->cfg.curator_remote_model;
  curator.api_key_env = c->cfg.curator_api_key_env;
  curator.api_grammar = c->cfg.curator_api_grammar;
  curator.context_tokens = c->cfg.curator_ctx;
  curator.threads = c->cfg.curator_threads;
  curator.gpu_layers = c->cfg.curator_gpu_layers;
  curator.ram_mb = estimated_ram_mb(curator.path, c->cfg.curator_ram_mb);
  curator.vram_mb = (size_t)c->cfg.curator_vram_mb;
  curator.warm = 1; curator.kv_cache = c->cfg.curator_kv_cache;

  memset(&embed, 0, sizeof embed);
  embed.id = EMBED_ID;
  embed.backend = (asmodel_backend)c->cfg.embed_backend;
  embed.path = c->cfg.embed_model_path;
  embed.base_url = c->cfg.embed_base_url;
  embed.remote_model = c->cfg.embed_remote_model;
  embed.api_key_env = c->cfg.embed_api_key_env;
  embed.context_tokens = 512;
  embed.gpu_layers = c->cfg.embed_gpu_layers;
  embed.embedding = 1; embed.embedding_dim = c->cfg.embed_dim;
  embed.ram_mb = estimated_ram_mb(embed.path, c->cfg.embed_ram_mb);
  embed.vram_mb = (size_t)c->cfg.embed_vram_mb;
  embed.warm = 1; embed.kv_cache = 1;
  me = asmodel_manager_register(c->model_manager, &curator);
  if (me == ASMODEL_OK)
    me = asmodel_manager_register(c->model_manager, &embed);
  if (me != ASMODEL_OK)
    return asper_seterr(c, ASPER_ERR_CONFIG, "model manager: %s",
                        asmodel_manager_last_error(c->model_manager));
  me = asmodel_manager_warm(c->model_manager);
  if (me != ASMODEL_OK)
    asper_log(c, ASPER_LOG_WARN, "model",
              "one or more models failed warm-up: %s",
              asmodel_manager_last_error(c->model_manager));
  return ASPER_OK;
}

static asper_err managed_embed(void *ud, const char *text, int is_query,
                               float *out) {
  model_ref *r = (model_ref *)ud;
  char *input = NULL;
  const char *send = text;
  if (r->apply_embedding_prefix) {
    const char *prefix = is_query ? r->ctx->cfg.query_prefix
                                  : r->ctx->cfg.passage_prefix;
    size_t a = prefix ? strlen(prefix) : 0, b = strlen(text);
    input = (char *)malloc(a + b + 1);
    if (!input) return ASPER_ERR_NOMEM;
    if (a) memcpy(input, prefix, a);
    memcpy(input + a, text, b + 1);
    send = input;
  }
  {
    asmodel_err e = asmodel_embed(r->manager, r->id, send, is_query, out);
    free(input);
    return e == ASMODEL_OK
             ? ASPER_OK : ASPER_ERR_MODEL;
  }
}

static int managed_count(void *ud, const char *text) {
  model_ref *r = (model_ref *)ud;
  return asmodel_count_tokens(r->manager, r->id, text);
}

static asper_err managed_generate(void *ud, const char *sys, const char *user,
                                  const char *grammar, int max_tokens,
                                  int64_t deadline_ms, char **out) {
  model_ref *r = (model_ref *)ud;
  asmodel_generate_params p;
  memset(&p, 0, sizeof p);
  p.max_tokens = max_tokens; p.deadline_ms = deadline_ms;
  return asmodel_generate(r->manager, r->id, sys, user, grammar, &p,
                          NULL, NULL, NULL, out, NULL, NULL) == ASMODEL_OK
             ? ASPER_OK : ASPER_ERR_MODEL;
}

static void ref_destroy(void *ud) { free(ud); }

static model_ref *make_ref(asper_ctx *c, asmodel_manager *m, const char *id,
                           int apply_embedding_prefix) {
  model_ref *r;
  if (!m || !id || !id[0]) return NULL;
  r = (model_ref *)calloc(1, sizeof *r);
  if (!r) return NULL;
  r->manager = m;
  r->ctx = c;
  r->apply_embedding_prefix = apply_embedding_prefix;
  snprintf(r->id, sizeof r->id, "%s", id);
  return r;
}

static void pipeline_hash(asper_ctx *c, const char *id, uint8_t out[32]) {
  uint8_t base[32];
  asper_sha256_ctx sh;
  memset(base, 0, sizeof base);
  if (c->cfg.embed_backend == ASMODEL_BACKEND_EMBEDDED)
    (void)asper_sha256_file(c->cfg.embed_model_path, base);
  else {
    const char *model = c->cfg.embed_remote_model
                            ? c->cfg.embed_remote_model : id;
    asper_sha256_init(&sh);
    asper_sha256_update(&sh, model, strlen(model));
    if (c->cfg.embed_base_url)
      asper_sha256_update(&sh, c->cfg.embed_base_url,
                          strlen(c->cfg.embed_base_url));
    asper_sha256_final(&sh, base);
  }
  asper_embedding_pipeline_hash(base, c->cfg.query_prefix,
                                c->cfg.passage_prefix, out);
}

asper_err asper_models_bind(asper_ctx *c, const asper_model_binding *binding,
                            asper_embedder *emb, asper_curator_iface *cur) {
  model_ref *er, *cr;
  const char *eid, *cid;
  int embed_ready = 1, curator_ready = 1;
  memset(emb, 0, sizeof *emb); memset(cur, 0, sizeof *cur);
  if (binding) {
    if (!binding->manager) return ASPER_ERR_INVALID;
    c->model_manager = binding->manager;
    eid = binding->embedding_model_id;
    cid = binding->curator_model_id;
  } else {
    asper_err e = register_local(c);
    asmodel_model_stats stats[2];
    size_t i, n;
    if (e != ASPER_OK) return e;
    eid = EMBED_ID; cid = CURATOR_ID;
    embed_ready = curator_ready = 0;
    n = asmodel_manager_stats(c->model_manager, stats, 2);
    for (i = 0; i < n && i < 2; ++i) {
      if (strcmp(stats[i].id, eid) == 0) embed_ready = stats[i].loads > 0;
      if (strcmp(stats[i].id, cid) == 0) curator_ready = stats[i].loads > 0;
    }
  }
  er = embed_ready ? make_ref(c, c->model_manager, eid,
                              binding != NULL ||
                              c->cfg.embed_backend == ASMODEL_BACKEND_OPENAI)
                   : NULL;
  cr = curator_ready ? make_ref(c, c->model_manager, cid, 0) : NULL;
  if ((embed_ready && !er) || (curator_ready && !cr)) {
    free(er); free(cr); return ASPER_ERR_NOMEM;
  }
  if (er) {
    emb->ud = er;
    emb->dim = binding ? binding->embedding_dim : c->cfg.embed_dim;
    snprintf(emb->model_id, sizeof emb->model_id, "%s", eid);
    if (binding)
      asper_embedding_pipeline_hash(binding->embedding_model_hash,
                                    c->cfg.query_prefix,
                                    c->cfg.passage_prefix, emb->model_hash);
    else pipeline_hash(c, eid, emb->model_hash);
    emb->embed = managed_embed; emb->destroy = ref_destroy;
  }
  if (cr) {
    cur->ud = cr; cur->generate = managed_generate;
    cur->count_tokens = managed_count; cur->destroy = ref_destroy;
  }
  return ASPER_OK;
}

void asper_models_shutdown(asper_ctx *c) {
  if (c && c->owns_model_manager) asmodel_manager_destroy(c->model_manager);
  if (c) { c->model_manager = NULL; c->owns_model_manager = false; }
}
