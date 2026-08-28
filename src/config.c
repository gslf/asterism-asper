/*
 * config.c — built-in defaults and config.xcdn overlay loading.
 *
 * The config file is parsed with xCDN-C. Three top-level shapes are accepted:
 * a tagged object (#asper_config { ... }), a bare top-level object, and the
 * implicit document-level key/value form the parser produces for untagged
 * "key: value" files — all three reduce to "the first top-level OBJECT value".
 *
 * Unknown keys are logged at WARN and skipped; wrong types, unknown enum
 * strings and out-of-range values fail with ASPER_ERR_CONFIG carrying the
 * dotted key path.
 */

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"
#include "xcdn.h"

/* ──────────────────────────────── defaults ──────────────────────────────── */

void asper_config_defaults(asper_config *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof *cfg);

  /* storage */
  cfg->journal_sync = ASPER_SYNC_BATCH;
  cfg->journal_max_ops = 2048;
  cfg->audit_log = false;

  /* logging */
  cfg->log_path = NULL;
  cfg->log_level = ASPER_LOG_INFO;
  cfg->log_max_size_kb = 5120;
  cfg->log_max_files = 3;
  cfg->log_sync = false;

  /* curator */
  cfg->curator_model_path =
      asper_strdup("models/qwen2.5-1.5b-instruct-q4_k_m.gguf");
  cfg->curator_ctx = 4096;
  cfg->curator_threads = 4;
  cfg->curator_gpu_layers = -1; /* all in VRAM when a GPU backend exists */
  cfg->curator_backend = ASMODEL_BACKEND_EMBEDDED;
  cfg->curator_remote_model = asper_strdup("qwen2.5-1.5b-instruct");
  cfg->curator_api_grammar = asper_strdup("none");
  cfg->curator_kv_cache = true;
  cfg->instruction_path = NULL;

  /* embedding */
  cfg->embed_model_path = asper_strdup("models/multilingual-e5-small-q8_0.gguf");
  cfg->embed_dim = 384;
  cfg->embed_gpu_layers = -1;
  cfg->embed_backend = ASMODEL_BACKEND_EMBEDDED;
  cfg->embed_remote_model = asper_strdup("multilingual-e5-small");
  cfg->models_max_resident = 2;
  /* multilingual-e5 is the distributed/default embedder.  Its training
   * contract requires these prefixes for every language; treating them as
   * optional silently produces lower-quality, incompatible vectors. */
  cfg->query_prefix = asper_strdup("query: ");
  cfg->passage_prefix = asper_strdup("passage: ");

  /* budgets */
  cfg->identity_tokens = 400;
  cfg->context_tokens = 600;
  cfg->project_tokens = 800;

  /* injection */
  cfg->token_estimator = ASPER_TOKENS_CURATOR;
  cfg->template_path = NULL;

  /* retrieval */
  cfg->k_context = 6;
  cfg->k_project = 8;
  cfg->min_similarity = 0.25;
  cfg->w_similarity = 0.70;
  cfg->w_relevance = 0.20;
  cfg->w_recency = 0.10;

  /* curation (durations pre-converted to seconds) */
  cfg->turn_batch = 4;
  cfg->idle_flush_s = 30;                          /* PT30S */
  cfg->max_ops_per_cycle = 8;
  cfg->dup_threshold = 0.92;
  cfg->transcript_tokens = 1200;
  cfg->event_queue_max = 256;
  cfg->identity_confirm_window_s = 7 * 86400;      /* P7D  */
  cfg->maintenance_interval_s = 86400;             /* P1D  */
  cfg->review_batch = 16;

  /* recall */
  cfg->recall_k = 12;
  cfg->recall_answer_tokens = 160;
  cfg->recall_timeout_s = 10;                      /* PT10S */

  /* decay */
  cfg->context_half_life_s = (int64_t)30 * 86400;  /* P30D */
  cfg->project_half_life_s = (int64_t)90 * 86400;  /* P90D */
  cfg->prune_threshold = 0.15;
  cfg->purge_after_s = (int64_t)60 * 86400;        /* P60D */

  /* limits */
  cfg->identity_max_records = 64;
  cfg->content_max_chars = 500;

  /* projects */
  cfg->autocreate = true;
}

void asper_config_free(asper_config *cfg) {
  if (!cfg) return;
  free(cfg->log_path);
  free(cfg->curator_model_path);
  free(cfg->curator_base_url);
  free(cfg->curator_remote_model);
  free(cfg->curator_api_key_env);
  free(cfg->curator_api_grammar);
  free(cfg->instruction_path);
  free(cfg->embed_model_path);
  free(cfg->embed_base_url);
  free(cfg->embed_remote_model);
  free(cfg->embed_api_key_env);
  free(cfg->query_prefix);
  free(cfg->passage_prefix);
  free(cfg->template_path);
  cfg->log_path = NULL;
  cfg->curator_model_path = NULL;
  cfg->instruction_path = NULL;
  cfg->embed_model_path = NULL;
  cfg->query_prefix = NULL;
  cfg->passage_prefix = NULL;
  cfg->template_path = NULL;
}

/* ─────────────────────────── key table ─────────────────────────── */

typedef struct {
  const char *name;
  int value;
} enum_map;

static const enum_map SYNC_MAP[] = {
    {"batch", ASPER_SYNC_BATCH},
    {"always", ASPER_SYNC_ALWAYS},
    {"never", ASPER_SYNC_NEVER},
    {NULL, 0},
};

static const enum_map ESTIMATOR_MAP[] = {
    {"curator", ASPER_TOKENS_CURATOR},
    {"heuristic", ASPER_TOKENS_HEURISTIC},
    {NULL, 0},
};

static const enum_map LEVEL_MAP[] = {
    {"error", ASPER_LOG_ERROR},
    {"warn", ASPER_LOG_WARN},
    {"info", ASPER_LOG_INFO},
    {"debug", ASPER_LOG_DEBUG},
    {NULL, 0},
};

static const enum_map BACKEND_MAP[] = {
    {"embedded", ASMODEL_BACKEND_EMBEDDED},
    {"openai", ASMODEL_BACKEND_OPENAI},
    {NULL, 0},
};

typedef enum {
  K_INT,  /* int, must be >= 1                                   */
  K_NNI,  /* int, must be >= 0                                   */
  K_GPU,  /* int, must be >= -1 (gpu_layers: -1 = all, 0 = CPU)  */
  K_DUR,  /* r"..." or string ISO-8601 duration -> int64 seconds */
  K_F01,  /* double in [0,1]; INT or FLOAT accepted              */
  K_BOOL, /* bool                                                */
  K_STR,  /* string, strdup-replaces the previous value          */
  K_NSTR, /* nullable string: null clears to NULL                */
  K_ENUM  /* string mapped through emap                          */
} cfg_kind;

typedef struct {
  const char *group;
  const char *key;
  cfg_kind kind;
  size_t off;           /* offsetof into asper_config */
  const enum_map *emap; /* K_ENUM only                */
} cfg_key;

#define OFF(field) offsetof(asper_config, field)

static const cfg_key CFG_KEYS[] = {
    {"storage", "journal_sync", K_ENUM, OFF(journal_sync), SYNC_MAP},
    {"storage", "journal_max_ops", K_INT, OFF(journal_max_ops), NULL},
    {"storage", "audit_log", K_BOOL, OFF(audit_log), NULL},

    {"logging", "path", K_NSTR, OFF(log_path), NULL},
    {"logging", "level", K_ENUM, OFF(log_level), LEVEL_MAP},
    {"logging", "max_size_kb", K_INT, OFF(log_max_size_kb), NULL},
    {"logging", "max_files", K_INT, OFF(log_max_files), NULL},
    {"logging", "sync", K_BOOL, OFF(log_sync), NULL},

    {"models", "max_resident", K_INT, OFF(models_max_resident), NULL},
    {"models", "max_ram_mb", K_NNI, OFF(models_max_ram_mb), NULL},
    {"models", "max_vram_mb", K_NNI, OFF(models_max_vram_mb), NULL},

    {"curator", "backend", K_ENUM, OFF(curator_backend), BACKEND_MAP},
    {"curator", "model_path", K_STR, OFF(curator_model_path), NULL},
    {"curator", "ctx", K_INT, OFF(curator_ctx), NULL},
    {"curator", "threads", K_INT, OFF(curator_threads), NULL},
    {"curator", "gpu_layers", K_GPU, OFF(curator_gpu_layers), NULL},
    {"curator", "base_url", K_NSTR, OFF(curator_base_url), NULL},
    {"curator", "remote_model", K_STR, OFF(curator_remote_model), NULL},
    {"curator", "api_key_env", K_NSTR, OFF(curator_api_key_env), NULL},
    {"curator", "api_grammar", K_STR, OFF(curator_api_grammar), NULL},
    {"curator", "ram_mb", K_NNI, OFF(curator_ram_mb), NULL},
    {"curator", "vram_mb", K_NNI, OFF(curator_vram_mb), NULL},
    {"curator", "kv_cache", K_BOOL, OFF(curator_kv_cache), NULL},
    {"curator", "instruction_path", K_NSTR, OFF(instruction_path), NULL},

    {"embedding", "backend", K_ENUM, OFF(embed_backend), BACKEND_MAP},
    {"embedding", "model_path", K_STR, OFF(embed_model_path), NULL},
    {"embedding", "dim", K_INT, OFF(embed_dim), NULL},
    {"embedding", "gpu_layers", K_GPU, OFF(embed_gpu_layers), NULL},
    {"embedding", "base_url", K_NSTR, OFF(embed_base_url), NULL},
    {"embedding", "remote_model", K_STR, OFF(embed_remote_model), NULL},
    {"embedding", "api_key_env", K_NSTR, OFF(embed_api_key_env), NULL},
    {"embedding", "ram_mb", K_NNI, OFF(embed_ram_mb), NULL},
    {"embedding", "vram_mb", K_NNI, OFF(embed_vram_mb), NULL},
    {"embedding", "query_prefix", K_STR, OFF(query_prefix), NULL},
    {"embedding", "passage_prefix", K_STR, OFF(passage_prefix), NULL},

    {"budgets", "identity_tokens", K_INT, OFF(identity_tokens), NULL},
    {"budgets", "context_tokens", K_INT, OFF(context_tokens), NULL},
    {"budgets", "project_tokens", K_INT, OFF(project_tokens), NULL},

    {"injection", "token_estimator", K_ENUM, OFF(token_estimator),
     ESTIMATOR_MAP},
    {"injection", "template_path", K_NSTR, OFF(template_path), NULL},

    {"retrieval", "k_context", K_INT, OFF(k_context), NULL},
    {"retrieval", "k_project", K_INT, OFF(k_project), NULL},
    {"retrieval", "min_similarity", K_F01, OFF(min_similarity), NULL},
    {"retrieval", "w_similarity", K_F01, OFF(w_similarity), NULL},
    {"retrieval", "w_relevance", K_F01, OFF(w_relevance), NULL},
    {"retrieval", "w_recency", K_F01, OFF(w_recency), NULL},

    {"curation", "turn_batch", K_INT, OFF(turn_batch), NULL},
    {"curation", "idle_flush", K_DUR, OFF(idle_flush_s), NULL},
    {"curation", "max_ops_per_cycle", K_INT, OFF(max_ops_per_cycle), NULL},
    {"curation", "dup_threshold", K_F01, OFF(dup_threshold), NULL},
    {"curation", "transcript_tokens", K_INT, OFF(transcript_tokens), NULL},
    {"curation", "event_queue_max", K_INT, OFF(event_queue_max), NULL},
    {"curation", "identity_confirm_window", K_DUR,
     OFF(identity_confirm_window_s), NULL},
    {"curation", "maintenance_interval", K_DUR, OFF(maintenance_interval_s),
     NULL},
    {"curation", "review_batch", K_INT, OFF(review_batch), NULL},

    {"recall", "k", K_INT, OFF(recall_k), NULL},
    {"recall", "answer_tokens", K_INT, OFF(recall_answer_tokens), NULL},
    {"recall", "timeout", K_DUR, OFF(recall_timeout_s), NULL},

    {"decay", "context_half_life", K_DUR, OFF(context_half_life_s), NULL},
    {"decay", "project_half_life", K_DUR, OFF(project_half_life_s), NULL},
    {"decay", "prune_threshold", K_F01, OFF(prune_threshold), NULL},
    {"decay", "purge_after", K_DUR, OFF(purge_after_s), NULL},

    {"limits", "identity_max_records", K_INT, OFF(identity_max_records), NULL},
    {"limits", "content_max_chars", K_INT, OFF(content_max_chars), NULL},

    {"projects", "autocreate", K_BOOL, OFF(autocreate), NULL},
};

#undef OFF

#define CFG_NKEYS (sizeof CFG_KEYS / sizeof CFG_KEYS[0])

static bool cfg_group_known(const char *group) {
  size_t i;
  for (i = 0; i < CFG_NKEYS; i++)
    if (strcmp(CFG_KEYS[i].group, group) == 0) return true;
  return false;
}

static const cfg_key *cfg_key_find(const char *group, const char *key) {
  size_t i;
  for (i = 0; i < CFG_NKEYS; i++)
    if (strcmp(CFG_KEYS[i].group, group) == 0 &&
        strcmp(CFG_KEYS[i].key, key) == 0)
      return &CFG_KEYS[i];
  return NULL;
}

/* ─────────────────────────── overlay ─────────────────────────── */

static asper_err cfg_type_err(asper_ctx *c, const char *path,
                              const char *expected, const xcdn_value_t *v) {
  return asper_seterr(c, ASPER_ERR_CONFIG, "config: %s: expected %s, got %s",
                      path, expected, xcdn_value_type_str(v->type));
}

static asper_err cfg_apply(asper_ctx *c, asper_config *cfg, const cfg_key *k,
                           const xcdn_value_t *v) {
  char path[96];
  void *dst = (char *)cfg + k->off;

  snprintf(path, sizeof path, "%s.%s", k->group, k->key);

  switch (k->kind) {
  case K_INT: {
    int64_t iv;
    if (v->type != XCDN_VAL_INT) return cfg_type_err(c, path, "integer", v);
    iv = v->data.integer;
    if (iv < 1 || iv > INT_MAX)
      return asper_seterr(c, ASPER_ERR_CONFIG,
                          "config: %s: %lld out of range (must be a positive "
                          "integer)",
                          path, (long long)iv);
    *(int *)dst = (int)iv;
    return ASPER_OK;
  }
  case K_NNI: {
    int64_t iv;
    if (v->type != XCDN_VAL_INT) return cfg_type_err(c, path, "integer", v);
    iv = v->data.integer;
    if (iv < 0 || iv > INT_MAX)
      return asper_seterr(c, ASPER_ERR_CONFIG,
                          "config: %s: %lld out of range (must be >= 0)",
                          path, (long long)iv);
    *(int *)dst = (int)iv;
    return ASPER_OK;
  }
  case K_GPU: {
    int64_t iv;
    if (v->type != XCDN_VAL_INT) return cfg_type_err(c, path, "integer", v);
    iv = v->data.integer;
    if (iv < -1 || iv > INT_MAX)
      return asper_seterr(c, ASPER_ERR_CONFIG,
                          "config: %s: %lld out of range (must be >= -1)",
                          path, (long long)iv);
    *(int *)dst = (int)iv;
    return ASPER_OK;
  }

  case K_DUR: {
    const char *s;
    int64_t secs;
    if (v->type != XCDN_VAL_DURATION && v->type != XCDN_VAL_STRING)
      return cfg_type_err(c, path, "duration", v);
    s = v->data.string;
    if (!s || !asper_duration_parse(s, &secs))
      return asper_seterr(c, ASPER_ERR_CONFIG,
                          "config: %s: invalid ISO 8601 duration \"%s\"", path,
                          s ? s : "");
    if (secs <= 0)
      return asper_seterr(c, ASPER_ERR_CONFIG,
                          "config: %s: duration must be positive", path);
    *(int64_t *)dst = secs;
    return ASPER_OK;
  }

  case K_F01: {
    double d;
    if (v->type == XCDN_VAL_FLOAT)
      d = v->data.floating;
    else if (v->type == XCDN_VAL_INT)
      d = (double)v->data.integer;
    else
      return cfg_type_err(c, path, "number", v);
    if (!(d >= 0.0 && d <= 1.0))
      return asper_seterr(c, ASPER_ERR_CONFIG,
                          "config: %s: %g out of range [0, 1]", path, d);
    *(double *)dst = d;
    return ASPER_OK;
  }

  case K_BOOL:
    if (v->type != XCDN_VAL_BOOL) return cfg_type_err(c, path, "boolean", v);
    *(bool *)dst = v->data.boolean;
    return ASPER_OK;

  case K_STR:
  case K_NSTR: {
    char *dup;
    if (k->kind == K_NSTR && v->type == XCDN_VAL_NULL) {
      free(*(char **)dst);
      *(char **)dst = NULL;
      return ASPER_OK;
    }
    if (v->type != XCDN_VAL_STRING || !v->data.string)
      return cfg_type_err(c, path,
                          k->kind == K_NSTR ? "string or null" : "string", v);
    dup = asper_strdup(v->data.string);
    if (!dup)
      return asper_seterr(c, ASPER_ERR_NOMEM, "config: out of memory");
    free(*(char **)dst);
    *(char **)dst = dup;
    return ASPER_OK;
  }

  case K_ENUM: {
    const enum_map *e;
    if (v->type != XCDN_VAL_STRING || !v->data.string)
      return cfg_type_err(c, path, "string", v);
    for (e = k->emap; e->name; e++) {
      if (strcmp(e->name, v->data.string) == 0) {
        *(int *)dst = e->value;
        return ASPER_OK;
      }
    }
    return asper_seterr(c, ASPER_ERR_CONFIG,
                        "config: %s: unknown value \"%s\"", path,
                        v->data.string);
  }
  }

  return asper_seterr(c, ASPER_ERR_INVALID, "config: %s: bad key kind", path);
}

/* asper_config_defaults cannot report allocation failure (void contract);
 * verify its owned strings here, on the load path every open takes. */
static asper_err cfg_check_defaults(asper_ctx *c, const asper_config *cfg) {
  if (!cfg->curator_model_path || !cfg->embed_model_path ||
      !cfg->curator_remote_model || !cfg->curator_api_grammar ||
      !cfg->embed_remote_model ||
      !cfg->query_prefix || !cfg->passage_prefix)
    return asper_seterr(c, ASPER_ERR_NOMEM,
                        "config: out of memory building defaults");
  return ASPER_OK;
}

asper_err asper_config_load(asper_ctx *c, asper_config *cfg,
                            const char *path) {
  char *text = NULL;
  xcdn_document_t *doc;
  xcdn_error_t xerr;
  const xcdn_node_t *root = NULL;
  const xcdn_value_t *rootv;
  size_t i, j;
  asper_err e;

  if (!cfg)
    return asper_seterr(c, ASPER_ERR_INVALID, "config: null config");
  e = cfg_check_defaults(c, cfg);
  if (e != ASPER_OK) return e;
  if (!path) return ASPER_OK;

  e = os_read_file(path, &text, NULL);
  if (e != ASPER_OK)
    return asper_seterr(c, e, "config: cannot read %s", path);

  doc = xcdn_parse(text, &xerr);
  free(text);
  if (!doc)
    return asper_seterr(c, ASPER_ERR_CONFIG,
                        "config: %s: parse error at line %zu: %s", path,
                        xerr.span.line, xerr.message);

  if (doc->values_len == 0) { /* empty file: defaults stay */
    xcdn_document_free(doc);
    return ASPER_OK;
  }

  for (i = 0; i < doc->values_len; i++) {
    const xcdn_node_t *n = doc->values[i];
    if (n && n->value && n->value->type == XCDN_VAL_OBJECT) {
      root = n;
      break;
    }
  }
  if (!root) {
    xcdn_document_free(doc);
    return asper_seterr(c, ASPER_ERR_CONFIG,
                        "config: %s: no configuration object found", path);
  }
  if (xcdn_node_tag_count(root) > 0 &&
      !xcdn_node_has_tag(root, "asper_config"))
    asper_log(c, ASPER_LOG_WARN, "config",
              "%s: unexpected tag #%s (expected #asper_config)", path,
              xcdn_node_tag_at(root, 0));

  rootv = root->value;
  for (i = 0; i < xcdn_object_len(rootv); i++) {
    const char *gname = xcdn_object_key_at(rootv, i);
    const xcdn_node_t *gnode = xcdn_object_node_at(rootv, i);
    const xcdn_value_t *gval = gnode ? gnode->value : NULL;

    if (!gname || !gval) continue;
    if (!cfg_group_known(gname)) {
      asper_log(c, ASPER_LOG_WARN, "config", "unknown key %s", gname);
      continue;
    }
    if (gval->type != XCDN_VAL_OBJECT) {
      e = cfg_type_err(c, gname, "object", gval);
      goto done;
    }

    for (j = 0; j < xcdn_object_len(gval); j++) {
      const char *kname = xcdn_object_key_at(gval, j);
      const xcdn_node_t *knode = xcdn_object_node_at(gval, j);
      const cfg_key *k;

      if (!kname || !knode || !knode->value) continue;
      k = cfg_key_find(gname, kname);
      if (!k) {
        asper_log(c, ASPER_LOG_WARN, "config", "unknown key %s.%s", gname,
                  kname);
        continue;
      }
      e = cfg_apply(c, cfg, k, knode->value);
      if (e != ASPER_OK) goto done;
    }
  }
  if ((cfg->curator_backend == ASMODEL_BACKEND_OPENAI &&
       (!cfg->curator_base_url || !cfg->curator_base_url[0])) ||
      (cfg->embed_backend == ASMODEL_BACKEND_OPENAI &&
       (!cfg->embed_base_url || !cfg->embed_base_url[0]))) {
    e = asper_seterr(c, ASPER_ERR_CONFIG,
                     "config: OpenAI backend requires base_url");
    goto done;
  }
  if (strcmp(cfg->curator_api_grammar, "none") != 0 &&
      strcmp(cfg->curator_api_grammar, "llama") != 0 &&
      strcmp(cfg->curator_api_grammar, "vllm") != 0) {
    e = asper_seterr(c, ASPER_ERR_CONFIG,
                     "config: curator.api_grammar must be none, llama or vllm");
    goto done;
  }
  e = ASPER_OK;

done:
  xcdn_document_free(doc);
  return e;
}
