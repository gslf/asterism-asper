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
} native_provider;

typedef struct {
  asmodel_manager *manager;
  char id[ASMODEL_ID_MAX];
  asper_ctx *ctx;
  int embedding_dim;
} model_ref;

static int model_error(asper_err e) {
  switch (e) {
    case ASPER_OK: return ASMODEL_OK;
    case ASPER_ERR_TIMEOUT: return ASMODEL_ERR_TIMEOUT;
    case ASPER_ERR_CANCELLED: return ASMODEL_ERR_CANCELLED;
    case ASPER_ERR_LIMIT: return ASMODEL_ERR_LIMIT;
    case ASPER_ERR_NOMEM: return ASMODEL_ERR_NOMEM;
    case ASPER_ERR_INVALID: return ASMODEL_ERR_INVALID;
    case ASPER_ERR_BUSY: return ASMODEL_ERR_BUSY;
    case ASPER_ERR_NOT_FOUND: return ASMODEL_ERR_NOT_FOUND;
    case ASPER_ERR_UNSUPPORTED: return ASMODEL_ERR_UNSUPPORTED;
    default: return ASMODEL_ERR_BACKEND;
  }
}
static asper_err memory_error(asmodel_err e) {
  switch (e) {
    case ASMODEL_OK: return ASPER_OK;
    case ASMODEL_ERR_TIMEOUT: return ASPER_ERR_TIMEOUT;
    case ASMODEL_ERR_CANCELLED: return ASPER_ERR_CANCELLED;
    case ASMODEL_ERR_LIMIT: return ASPER_ERR_LIMIT;
    case ASMODEL_ERR_NOMEM: return ASPER_ERR_NOMEM;
    case ASMODEL_ERR_INVALID: return ASPER_ERR_INVALID;
    case ASMODEL_ERR_BUSY: return ASPER_ERR_BUSY;
    case ASMODEL_ERR_NOT_FOUND: return ASPER_ERR_NOT_FOUND;
    case ASMODEL_ERR_UNSUPPORTED: return ASPER_ERR_UNSUPPORTED;
    default: return ASPER_ERR_MODEL;
  }
}

static int native_generate(void *ud, const asmodel_input *input,
                           const char *grammar,
                           const asmodel_generate_params *params,
                           asmodel_token_fn token_fn, void *token_ud,
                           volatile int *cancel, char **out,
                           int *out_in, int *out_gen) {
  native_provider *p = (native_provider *)ud;
  asper_err e;
  asmodel_generate_params request = *params;
  asmodel_generation_info local = {0};
  if (!request.result_info) request.result_info = &local;
  e = p->curator.generate(p->curator.ud,input,grammar,NULL,&request,cancel,out);
  if (out_in) *out_in = request.result_info->input_tokens;
  if (out_gen) *out_gen = request.result_info->output_tokens;
  if (token_fn && *out) token_fn(*out,strlen(*out),token_ud);
  return model_error(e);
}

static int native_embed(void *ud, const char *const *texts, size_t count, int is_query,
                        const asmodel_embed_params *params, float *out) {
  native_provider *p = ud;
  int64_t started = os_monotonic_ms();
  asmodel_embedding_info total = {0}; total.usage_known = 1;
  int result = ASMODEL_OK;
  for (size_t i = 0; i < count; i++) {
    asmodel_embedding_info row = {0};
    asmodel_embed_params request = *params; request.result_info = &row;
    if (request.cancel && *request.cancel) { result = ASMODEL_ERR_CANCELLED; break; }
    if (request.deadline_ms > 0 && (request.deadline_ms -= os_monotonic_ms()-started) <= 0) {
      result = ASMODEL_ERR_TIMEOUT; break;
    }
    result = model_error(p->embedder.embed(p->embedder.ud,texts[i],is_query,&request,out+i*(size_t)p->embedder.dim));
    total.completed += row.completed;
    total.usage_known = total.usage_known && row.usage_known;
    if (row.input_tokens < 0 || row.input_tokens > INT32_MAX-total.input_tokens) total.usage_known = 0;
    else total.input_tokens += row.input_tokens;
    if (result != ASMODEL_OK) { snprintf(total.error,sizeof total.error,"%s",row.error); break; }
  }
  if (params->result_info) *params->result_info = total;
  return result;
}

static int native_count(void *ud, const char *text) {
  native_provider *p = (native_provider *)ud;
  return p->curator.count_tokens ?
      p->curator.count_tokens(p->curator.ud, text) : -1;
}

static void native_destroy(void *ud) {
  native_provider *p = (native_provider *)ud;
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
  native_provider *p;
  asper_err e;
  if (spec->backend == ASMODEL_BACKEND_OPENAI)
    return asmodel_openai_provider_create(spec, out, error, error_size);
  p = (native_provider *)calloc(1, sizeof *p);
  if (!p) return -1;
  p->ctx = c;
  memset(out, 0, sizeof *out);
  if (spec->embedding) {
    p->kind = 1;
    e = asper_embedder_llama_create(c, &p->embedder);
    if (e == ASPER_OK && p->embedder.dim != spec->embedding_dim)
      e = asper_seterr(c,ASPER_ERR_CONFIG,"embedding dimension %d differs from configured %d",
                       p->embedder.dim,spec->embedding_dim);
    if (e == ASPER_OK) out->embed = native_embed;
  } else {
    p->kind = 2;
    e = asper_curator_llama_create(c, &p->curator);
    if (e == ASPER_OK) {
      out->generate = native_generate;
      out->count_tokens = native_count;
    }
  }
  if (e != ASPER_OK) {
    snprintf(error, error_size, "%s", asper_last_error(c));
    native_destroy(p);
    return -1;
  }
  out->userdata = p;
  out->destroy = native_destroy;
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
  curator.remote_provider =
      (asmodel_remote_provider)c->cfg.curator_remote_provider;
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
  const char *values[] = {c->cfg.embed_revision,c->cfg.embed_tokenizer,c->cfg.embed_pooling,
                         c->cfg.query_prefix,c->cfg.passage_prefix};
  char *dest[] = {embed.pipeline.revision,embed.pipeline.tokenizer,embed.pipeline.pooling,
                  embed.pipeline.query_prefix,embed.pipeline.document_prefix};
  size_t sizes[] = {sizeof embed.pipeline.revision,sizeof embed.pipeline.tokenizer,sizeof embed.pipeline.pooling,
                    sizeof embed.pipeline.query_prefix,sizeof embed.pipeline.document_prefix};
  for (size_t i = 0; i < 5; i++) {
    if (!values[i] || strlen(values[i]) >= sizes[i]) return ASPER_ERR_CONFIG;
    strcpy(dest[i],values[i]);
  }
  if (embed.backend == ASMODEL_BACKEND_EMBEDDED) {
    uint8_t hash[32];
    embed.pipeline.revision[0] = 0; embed.pipeline.tokenizer[0] = 0;
    if (asper_sha256_file(embed.path,hash) == ASPER_OK) {
      for (size_t i = 0; i < 32; i++) snprintf(embed.pipeline.revision+2*i,3,"%02x",hash[i]);
      strcpy(embed.pipeline.tokenizer,embed.pipeline.revision);
    }
    strcpy(embed.pipeline.pooling,"llama-mean-v1");
  }
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
                               const asmodel_embed_params *control, float *out) {
  model_ref *r = (model_ref *)ud;
  asmodel_embed_params params = control ? *control : (asmodel_embed_params){0};
  if (!control) params.deadline_ms = r->ctx->cfg.recall_timeout_s > INT64_MAX/1000 ?
      INT64_MAX : r->ctx->cfg.recall_timeout_s*1000;
  asmodel_err e = asmodel_embed(r->manager,r->id,&text,1,is_query,&params,out,
                               (size_t)r->embedding_dim);
  return memory_error(e);
}

static int managed_count(void *ud, const char *text) {
  model_ref *r = (model_ref *)ud;
  return asmodel_count_tokens(r->manager, r->id, text);
}

static asper_err managed_generate(void *ud, const asmodel_input *input,
                                  const char *grammar, const asper_output_contract *contract,
                                  const asmodel_generate_params *params, volatile int *cancel, char **out) {
  model_ref *r = (model_ref *)ud;
  asmodel_generate_params p = *params;
  asmodel_generation_info local = {0};
  asmodel_generation_info *info = p.result_info ? p.result_info : &local;
  char *schema = contract ? asper_output_schema(contract) : NULL;
  if (contract && !schema) return ASPER_ERR_NOMEM;
  p.output_schema = schema; p.require_constraint = grammar != NULL || schema != NULL;
  p.result_info = info;
  asmodel_err e = asmodel_generate(r->manager, r->id, input, grammar, &p,
                                   NULL, NULL, cancel, out, NULL, NULL);
  free(schema);
  if (e == ASMODEL_OK) return info->json_output ? asper_output_decode(out) : ASPER_OK;
  return asper_seterr(r->ctx,memory_error(e),"model '%s': %s",r->id,info->error);

}

static void ref_destroy(void *ud) { free(ud); }

static model_ref *make_ref(asper_ctx *c, asmodel_manager *m, const char *id) {
  model_ref *r;
  if (!m || !id || !id[0]) return NULL;
  r = (model_ref *)calloc(1, sizeof *r);
  if (!r) return NULL;
  r->manager = m;
  r->ctx = c;
  snprintf(r->id, sizeof r->id, "%s", id);
  return r;
}

/* Missing remote revisions intentionally invalidate derived vectors on reopen. */
static asper_err pipeline_hash(asper_ctx *c, const char *id, uint8_t out[32]) {
  char *key = NULL;
  asmodel_err e = asmodel_manager_embedding_key(c->model_manager,id,&key);
  if (e == ASMODEL_OK) asper_sha256(key,strlen(key),out);
  else if (e == ASMODEL_ERR_UNSUPPORTED) {
    char nonce[37]; asper_uuid_v4(nonce); asper_sha256(nonce,strlen(nonce),out);
    asper_log(c,ASPER_LOG_WARN,"model","embedding pipeline revision is unknown; persistent vectors will be rebuilt on reopen");
  } else return memory_error(e);
  free(key); return ASPER_OK;
}

asper_err asper_models_bind(asper_ctx *c, const asper_model_binding *binding,
                            asper_embedder *emb, asper_curator_iface *cur) {
  if (asmodel_abi_version() != ASMODEL_ABI_VERSION) return ASPER_ERR_CONFIG;
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
  er = embed_ready ? make_ref(c,c->model_manager,eid)
                   : NULL;
  cr = curator_ready ? make_ref(c,c->model_manager,cid) : NULL;
  if ((embed_ready && !er) || (curator_ready && !cr)) {
    free(er); free(cr); return ASPER_ERR_NOMEM;
  }
  if (er) {
    emb->ud = er;
    emb->dim = binding ? binding->embedding_dim : c->cfg.embed_dim;
    er->embedding_dim = emb->dim;
    snprintf(emb->model_id, sizeof emb->model_id, "%s", eid);
    asper_err e = pipeline_hash(c,eid,emb->model_hash);
    if (e != ASPER_OK) { free(er); free(cr); memset(emb,0,sizeof *emb); return e; }
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
