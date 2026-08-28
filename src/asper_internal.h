/*
 * asper_internal.h — internal contracts of libasper.
 *
 * Every module implements exactly the functions declared here; api.c and
 * worker.c glue them together. Tests include this header directly.
 *
 * Module map (one .c per group, strict ownership):
 *   util.c          buffers, strings, fnv, base64, slug checks
 *   time.c          RFC 3339 + ISO 8601 durations, injectable clock helpers
 *   uuid.c          UUID v4 generation / validation
 *   sha256.c        SHA-256 (streaming + one-shot + file)
 *   log.c           leveled rotating file log + host callback fan-out
 *   config.c        defaults + config.xcdn loading (xCDN-C)
 *   manifest.c      manifest.xcdn read/write (xCDN-C)
 *   store.c         section files, table load, projects, compaction
 *   journal.c       op serialize/parse/append/replay, torn tail
 *   cache.c         embeddings.bin read/write
 *   index.c         flat cosine index
 *   decay.c         effective relevance + scoring
 *   retrieve.c      query-side selection (top-k per section)
 *   inject.c        budgets, template render, token estimation
 *   grammar.c       per-cycle GBNF generation + op-protocol parsing
 *   curator.c       curation cycles, guardrails, quorum, review, recall
 *   worker.c        event queue, triggers, worker thread / asper_tick
 *   api.c           public API, apply() funnel, open/close wiring
 *   embed_llama.c   llama.cpp embedder backend (ASPER_WITH_LLAMA)
 *   curator_llama.c llama.cpp curator backend (ASPER_WITH_LLAMA)
 *   os_common.c / os_posix.c / os_win32.c   platform shim (os.h)
 *
 * Locking protocol: c->lock guards table + index + active_project +
 * stats + pending set. c->ev_mu guards the event queue, access batch and
 * worker signaling. c->journal_mu guards journal/audit appends. Lock order:
 * lock -> journal_mu; ev_mu is never held while taking lock (copy out, then
 * apply). asper_apply_op takes the write lock itself: callers must NOT hold
 * c->lock when calling it.
 */

#ifndef ASPER_INTERNAL_H
#define ASPER_INTERNAL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "asper.h"
#include "asmodel.h"
#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════ util.c ═══════════════════════ */

typedef struct {
  char *data; /* always NUL-terminated when len > 0 or after init */
  size_t len, cap;
} asper_buf;

void      asper_buf_init(asper_buf *b);
void      asper_buf_free(asper_buf *b);
asper_err asper_buf_append(asper_buf *b, const char *data, size_t len);
asper_err asper_buf_appends(asper_buf *b, const char *s); /* strlen */
asper_err asper_buf_appendc(asper_buf *b, char ch);
asper_err asper_buf_printf(asper_buf *b, const char *fmt, ...);
/* Detach the heap string (caller frees with free()); resets b. */
char     *asper_buf_detach(asper_buf *b);

char *asper_strdup(const char *s);            /* NULL-safe: NULL -> NULL */
char *asper_strndup(const char *s, size_t n);
/* Trim leading/trailing ASCII whitespace in place; returns s. */
char *asper_str_trim(char *s);
bool  asper_str_blank(const char *s);         /* NULL/empty/whitespace  */
/* Strict RFC 3629 validation and Unicode scalar-value count. */
bool  asper_utf8_count(const char *s, size_t *out_chars);

uint64_t asper_fnv1a64(const void *data, size_t len);

/* Base64 (RFC 4648, no wrap). Encode returns malloc'd NUL-terminated. */
char     *asper_base64_encode(const uint8_t *data, size_t len);
/* Decode into malloc'd buffer; false on bad input. */
bool      asper_base64_decode(const char *s, uint8_t **out, size_t *out_len);

/* Project slug: [a-z0-9][a-z0-9-]{0,63}. */
bool asper_slug_valid(const char *s);

/* UTF-8 byte-length/4 heuristic token estimate (>= 1 for non-empty). */
int asper_token_heuristic(const char *text);

/* ═══════════════════════ time.c ═══════════════════════ */

typedef int64_t asper_time; /* unix seconds, UTC */

typedef struct {
  void *ud;
  asper_time (*now)(void *ud);
} asper_clock;

asper_time asper_clock_now(const asper_clock *clk);
/* Real-time clock backed by os_now_unix. */
asper_clock asper_clock_system(void);

/* "2026-07-29T10:12:00Z" (also accepts +hh:mm offsets and .frac). */
bool asper_time_parse_rfc3339(const char *s, asper_time *out);
/* Always "YYYY-MM-DDTHH:MM:SSZ" (20 chars + NUL). */
void asper_time_format_rfc3339(asper_time t, char out[32]);
/* ISO 8601 duration: PnDTnHnMnS / PnW / PTnS ... into seconds.
 * Months/years are rejected (ambiguous). */
bool asper_duration_parse(const char *s, int64_t *out_seconds);

/* ═══════════════════════ uuid.c ═══════════════════════ */

void asper_uuid_v4(char out[37]); /* lowercase hex, RFC 4122 v4 */
bool asper_uuid_valid(const char *s);
/* "xxxxxxxx-..." -> 16 raw bytes; false on malformed input. */
bool asper_uuid_to_bytes(const char *s, uint8_t out[16]);
void asper_uuid_from_bytes(const uint8_t in[16], char out[37]);

/* ═══════════════════════ sha256.c ═══════════════════════ */

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buffer[64];
  size_t buflen;
} asper_sha256_ctx;

void asper_sha256_init(asper_sha256_ctx *ctx);
void asper_sha256_update(asper_sha256_ctx *ctx, const void *data, size_t len);
void asper_sha256_final(asper_sha256_ctx *ctx, uint8_t out[32]);
void asper_sha256(const void *data, size_t len, uint8_t out[32]);
asper_err asper_sha256_file(const char *path, uint8_t out[32]);
void asper_embedding_pipeline_hash(const uint8_t weights_hash[32],
                                   const char *query_prefix,
                                   const char *passage_prefix,
                                   uint8_t out[32]);

/* ═══════════════════════ records ═══════════════════════ */

typedef enum {
  ASPER_SRC_SEED = 0,
  ASPER_SRC_MANUAL = 1,
  ASPER_SRC_CURATOR = 2
} asper_source;

/* Shared between the in-memory table (owned by store) and API results
 * (deep clones). */
struct asper_record {
  char id[37];
  asper_section section;   /* IDENTITY / CONTEXT / PROJECT only */
  char *project;           /* owned; non-NULL iff section == PROJECT */
  char *content;           /* owned */
  asper_source source;
  asper_time created_at, updated_at, last_access;
  uint32_t access_count;
  double relevance;        /* stored base relevance [0,1] */
  bool locked;
  bool deprecated;         /* status: "deprecated" when true */
  asper_time deprecated_at; /* 0 unless deprecated */
  char supersedes[37];     /* empty string when null */
  char **tags;             /* owned array of owned strings */
  size_t tags_n;
  /* -- volatile, not persisted -- */
  double score;            /* retrieval score on search results */
  int emb_row;             /* row in asper_index, -1 if absent */
};

asper_record *asper_record_new(void);
void          asper_record_free_one(asper_record *r);
asper_record *asper_record_clone(const asper_record *r); /* score copied,
                                                            emb_row = -1 */
const char   *asper_source_name(asper_source s); /* "seed"/"manual"/"curator" */
bool          asper_source_parse(const char *s, asper_source *out);
const char   *asper_section_name(asper_section s);
bool          asper_section_parse(const char *s, asper_section *out);

/* ═══════════════════════ table (store.c) ═══════════════════════ */

typedef struct {
  asper_record **recs;
  size_t n, cap;
  /* open-addressing id -> index map, rebuilt on remove-heavy ops */
  struct asper_idmap *idmap;
} asper_table;

void          asper_table_init(asper_table *t);
void          asper_table_free(asper_table *t); /* frees records */
asper_err     asper_table_add(asper_table *t, asper_record *r); /* owns r;
                              ASPER_ERR_INVALID on duplicate id */
asper_record *asper_table_get(const asper_table *t, const char *id);
/* Remove and free the record with id; false if absent. */
bool          asper_table_remove(asper_table *t, const char *id);

/* ═══════════════════════ config.c ═══════════════════════ */

enum { ASPER_SYNC_BATCH = 0, ASPER_SYNC_ALWAYS = 1, ASPER_SYNC_NEVER = 2 };
enum { ASPER_TOKENS_CURATOR = 0, ASPER_TOKENS_HEURISTIC = 1 };

typedef struct {
  /* storage */
  int journal_sync;            /* ASPER_SYNC_*        */
  int journal_max_ops;
  bool audit_log;
  /* logging */
  char *log_path;              /* NULL = disabled */
  int log_level;               /* ASPER_LOG_*     */
  int log_max_size_kb;
  int log_max_files;
  bool log_sync;
  /* curator */
  char *curator_model_path;
  int curator_ctx;
  int curator_threads;
  int curator_gpu_layers;      /* -1 = all in VRAM (default), 0 = CPU */
  int curator_backend;
  char *curator_base_url, *curator_remote_model, *curator_api_key_env;
  char *curator_api_grammar;
  int curator_ram_mb, curator_vram_mb;
  bool curator_kv_cache;
  char *instruction_path;      /* NULL = built-in default */
  /* embedding */
  char *embed_model_path;
  int embed_dim;
  int embed_gpu_layers;        /* -1 = all in VRAM (default), 0 = CPU */
  int embed_backend;
  char *embed_base_url, *embed_remote_model, *embed_api_key_env;
  int embed_ram_mb, embed_vram_mb;
  int models_max_resident, models_max_ram_mb, models_max_vram_mb;
  char *query_prefix;          /* "" default */
  char *passage_prefix;
  /* budgets */
  int identity_tokens, context_tokens, project_tokens;
  /* injection */
  int token_estimator;         /* ASPER_TOKENS_* */
  char *template_path;         /* NULL = built-in default */
  /* retrieval */
  int k_context, k_project;
  double min_similarity, w_similarity, w_relevance, w_recency;
  /* curation */
  int turn_batch;
  int64_t idle_flush_s;
  int max_ops_per_cycle;
  double dup_threshold;
  int transcript_tokens;
  int event_queue_max;
  int64_t identity_confirm_window_s;
  int64_t maintenance_interval_s;
  int review_batch;
  /* recall */
  int recall_k;
  int recall_answer_tokens;
  int64_t recall_timeout_s;
  /* decay */
  int64_t context_half_life_s, project_half_life_s;
  double prune_threshold;
  int64_t purge_after_s;
  /* limits */
  int identity_max_records;
  int content_max_chars;
  /* projects */
  bool autocreate;
} asper_config;

void      asper_config_defaults(asper_config *cfg);
/* Overlay path (xCDN #asper_config) onto cfg; unknown keys warn via log,
 * wrong types => ASPER_ERR_CONFIG. path NULL => defaults only. */
asper_err asper_config_load(asper_ctx *c, asper_config *cfg,
                            const char *path);
void      asper_config_free(asper_config *cfg);

/* ═══════════════════════ ops + journal.c ═══════════════════════ */

typedef enum {
  ASPER_OP_INSERT = 0,
  ASPER_OP_UPDATE,
  ASPER_OP_DEPRECATE,
  ASPER_OP_KEEP,
  ASPER_OP_SET_LOCKED,
  ASPER_OP_ACCESS,
  ASPER_OP_PROJECT_CREATE
} asper_op_kind;

typedef struct {
  asper_op_kind kind;
  asper_time at;
  asper_record *record;    /* INSERT: owned full record */
  char id[37];             /* UPDATE/DEPRECATE/KEEP/SET_LOCKED */
  char *content;           /* UPDATE: owned */
  char *reason;            /* DEPRECATE: owned, may be NULL */
  bool value;              /* SET_LOCKED */
  char (*ids)[37];         /* ACCESS: owned array */
  size_t ids_n;
  char *project;           /* PROJECT_CREATE: owned slug */
} asper_op;

void asper_op_free(asper_op *op);         /* frees members, not op itself */
const char *asper_op_kind_name(asper_op_kind k);

/* Serialize one op as a single #op xCDN value (used for journal + audit). */
asper_err asper_op_serialize(const asper_op *op, asper_buf *out);
/* Serialize one record as a #memory xCDN value. */
asper_err asper_record_serialize(const asper_record *r, asper_buf *out);

/* Parse a #memory node that was already parsed by xCDN-C. Declared here as
 * a void* to keep xcdn.h out of this header; the argument is a
 * const xcdn_node_t*. */
asper_err asper_record_from_node(asper_ctx *c, const void *xcdn_node,
                                 asper_record **out);
asper_err asper_op_from_node(asper_ctx *c, const void *xcdn_node,
                             asper_op *out);

/* Replay journal at path into the table (mutating records in place).
 * Torn tail: truncate + WARN. Mid-file parse error: ASPER_ERR_PARSE.
 * Counts replayed ops into *out_ops. Missing file: OK, 0 ops. */
asper_err asper_journal_replay(asper_ctx *c, const char *path,
                               size_t *out_ops);
/* Append an op to the journal (c->journal_mu inside), honoring
 * cfg->journal_sync (`force_sync` overrides to fsync now). Also mirrors to
 * audit.xcdn when enabled. Increments c->store.journal_ops. */
asper_err asper_journal_append(asper_ctx *c, const asper_op *op,
                               bool force_sync);
/* fsync journal stream if open. */
asper_err asper_journal_sync(asper_ctx *c);

/* ═══════════════════════ manifest.c ═══════════════════════ */

typedef struct {
  int store_version;               /* 1 */
  asper_time created_at;
  asper_time last_compaction;      /* 0 = never */
  char embed_model_id[128];
  int embed_dim;
  uint8_t embed_model_hash[32];
  bool has_model_hash;
} asper_manifest;

/* Read manifest.xcdn; missing file => initialized manifest with
 * store_version 1, created_at = now, written to disk. Greater
 * store_version => ASPER_ERR_PARSE. */
asper_err asper_manifest_load(asper_ctx *c, asper_manifest *m);
asper_err asper_manifest_save(asper_ctx *c, const asper_manifest *m);

/* ═══════════════════════ store.c ═══════════════════════ */

typedef struct {
  char *root;              /* owned copy of memory_root */
  char *identity_path, *context_path, *projects_dir;
  char *journal_path, *manifest_path, *cache_dir, *cache_path;
  char *audit_path, *log_dir;
  asper_table table;
  char **projects;         /* known slugs, sorted asc */
  size_t projects_n, projects_cap;
  asper_manifest manifest;
  size_t journal_ops;      /* ops in journal since last compaction */
  FILE *journal_fp;        /* append stream, owned */
  FILE *audit_fp;          /* NULL unless cfg.audit_log */
} asper_store;

/* Create directories, load manifest + section files + projects, replay
 * journal. Does NOT touch embeddings (cache.c) or index. */
asper_err asper_store_open(asper_ctx *c);
void      asper_store_close(asper_ctx *c); /* close streams, free table */
/* Apply one op to the table only (no journal, no index): shared by replay
 * and live apply. INSERT takes ownership of op->record on success. */
asper_err asper_store_apply(asper_ctx *c, asper_op *op);
/* Rewrite section files + purge + reset journal + manifest bump.
 * Caller must hold no locks (takes them internally). */
asper_err asper_store_compact(asper_ctx *c);
bool      asper_store_project_known(const asper_ctx *c, const char *slug);
/* Register slug in the in-memory list (idempotent). */
asper_err asper_store_project_add(asper_ctx *c, const char *slug);

/* ═══════════════════════ cache.c (embeddings.bin) ═══════════════════════ */

/* Load cache; entries whose UUID+content-hash match feed the index via
 * asper_index_put. Returns the ids that still need embedding in *out_stale
 * (malloc'd array of table indices). Invalid/mismatched cache => full
 * rebuild (all rows stale), WARN. */
asper_err asper_cache_load(asper_ctx *c, size_t **out_stale,
                           size_t *out_stale_n);
/* Rewrite embeddings.bin from the current index (atomic replace). */
asper_err asper_cache_save(asper_ctx *c);
bool      asper_cache_needs_save(asper_ctx *c);

/* ═══════════════════════ index.c ═══════════════════════ */

typedef struct {
  float *vecs;             /* n x dim, L2-normalized rows */
  asper_record **recs;     /* parallel; recs[i]->emb_row == i */
  size_t n, cap;
  int dim;
} asper_index;

void      asper_index_init(asper_index *ix, int dim);
void      asper_index_free(asper_index *ix);
/* Insert or overwrite the row for r (uses r->emb_row); normalizes vec. */
asper_err asper_index_put(asper_index *ix, asper_record *r,
                          const float *vec);
/* Remove r's row (swap-with-last, fixes moved record's emb_row). */
void      asper_index_remove(asper_index *ix, asper_record *r);
const float *asper_index_vec(const asper_index *ix, const asper_record *r);
/* Cosine of two normalized vectors of length dim. */
double    asper_cosine(const float *a, const float *b, int dim);

typedef struct {
  asper_record *rec;
  double cos;
  double score;
} asper_hit;

/* Scan: active records matching section (ANY = identity + context + the
 * given project; PROJECT records never match a NULL project). Fills hits
 * with cos + score (scored via decay.c), capped at k; min_sim excludes
 * below-floor cosine. rank_by_cos == 0: ordered deterministically
 * (score desc, updated_at desc, id asc); rank_by_cos != 0: ordered by cos
 * desc, updated_at desc, id asc — used where similarity order is
 * required (related memories, dedup winner). Returns
 * count. Caller holds the read lock. */
size_t asper_index_scan(const asper_index *ix, const asper_config *cfg,
                        const asper_clock *clk, const float *qvec,
                        asper_section section, const char *project,
                        size_t k, double min_sim, int rank_by_cos,
                        asper_hit *hits);

/* ═══════════════════════ decay.c ═══════════════════════ */

int64_t asper_half_life_s(const asper_config *cfg, asper_section s);
/* eff(m, t); identity: no decay. */
double  asper_eff_relevance(const asper_config *cfg, const asper_record *r,
                            asper_time now);
/* score(m, q). */
double  asper_score_record(const asper_config *cfg, const asper_record *r,
                           double cosine, asper_time now);
/* Deterministic tie-break: score desc, updated_at desc, id asc.
 * Returns <0 if a ranks before b. */
int     asper_hit_cmp(const asper_hit *a, const asper_hit *b);

/* ═══════════════════════ embedder / curator vtables ═══════════════════════ */

typedef struct {
  void *ud;
  int dim;
  char model_id[128];
  /* SHA-256 of the complete embedding pipeline (weights plus model-owned
   * query/passage preprocessing), used to invalidate persisted vectors. */
  uint8_t model_hash[32];
  /* Callable from ANY thread for both kinds; backends serialize
   * internally (separate query/passage contexts so queries never wait on
   * passage work). is_query != 0 marks hot-path query embedding. out has
   * room for dim floats; result L2-normalized. */
  asper_err (*embed)(void *ud, const char *text, int is_query, float *out);
  void (*destroy)(void *ud);
} asper_embedder;

typedef struct {
  void *ud;
  /* Greedy generation constrained by gbnf (NULL = unconstrained).
   * *out_text malloc'd. Never called concurrently: every call site holds
   * the cycle slot (see the protocol below) — but the calling THREAD varies
   * (worker for cycles, host threads for recall/flush), so backends must
   * not assume thread affinity. */
  asper_err (*generate)(void *ud, const char *system_prompt,
                        const char *user_prompt, const char *gbnf,
                        int max_tokens, int64_t deadline_ms,
                        char **out_text);
  /* Token count with the curator tokenizer; <0 on error. Any thread. */
  int (*count_tokens)(void *ud, const char *text);
  void (*destroy)(void *ud);
} asper_curator_iface;

/* llama.cpp backends (embed_llama.c / curator_llama.c). Without
 * ASPER_WITH_LLAMA both return ASPER_ERR_MODEL ("built without llama"). */
asper_err asper_embedder_llama_create(asper_ctx *c, asper_embedder *out);
asper_err asper_curator_llama_create(asper_ctx *c, asper_curator_iface *out);
asper_err asper_models_bind(asper_ctx *c,
                            const asper_model_binding *binding,
                            asper_embedder *out_embedder,
                            asper_curator_iface *out_curator);
void asper_models_shutdown(asper_ctx *c);

/* ═══════════════════════ retrieve.c ═══════════════════════ */

/* Embed the query (is_query=1, caller thread), scan under the read lock,
 * return CLONES with .score set, ordered deterministically. No embedder available =>
 * 0 results, ASPER_OK. s may be ASPER_SECTION_ANY (recall/search). */
asper_err asper_retrieve(asper_ctx *c, const char *query, asper_section s,
                         const char *project, size_t k, double min_sim,
                         asper_record ***out, size_t *out_n);
/* As asper_retrieve, but when score_is_cos != 0 selection and ordering use
 * the raw cosine (asper_index_scan rank_by_cos) and the clones' .score
 * carries that cosine — for call sites that require similarity
 * order. */
asper_err asper_retrieve_ex(asper_ctx *c, const char *query, asper_section s,
                            const char *project, size_t k, double min_sim,
                            int score_is_cos, asper_record ***out,
                            size_t *out_n);
/* Clones of all active identity records, sorted per asper_identity_cmp. */
asper_err asper_collect_identity(asper_ctx *c, asper_record ***out,
                                 size_t *out_n);
/* Clones for asper_memory_list (s may be ANY; project filter when not
 * NULL), ordered by created_at asc then id asc. */
asper_err asper_collect_list(asper_ctx *c, asper_section s,
                             const char *project, bool include_deprecated,
                             asper_record ***out, size_t *out_n);

/* ═══════════════════════ inject.c ═══════════════════════ */

/* Token estimate per cfg.token_estimator (curator tokenizer or heuristic;
 * falls back to heuristic when no curator is available). */
int asper_estimate_tokens(asper_ctx *c, const char *text);

/* Render base + memory block. identity/ctx/proj arrays are already
 * retrieval-ordered clones owned by the caller; inject trims whole records
 * to the budgets with precedence, drops empty sections, returns
 * base unchanged (copy) when all empty. Deterministic. */
asper_err asper_inject_render(asper_ctx *c, const char *base_system_prompt,
                              asper_record *const *identity, size_t id_n,
                              asper_record *const *ctxr, size_t ctx_n,
                              asper_record *const *proj, size_t proj_n,
                              const char *project_name, char **out_prompt);

/* Identity injection order: locked first, then relevance desc,
 * created_at asc, id asc. qsort comparator over asper_record*. */
int asper_identity_cmp(const void *a, const void *b);

/* Built-in template as a C string. */
extern const char ASPER_DEFAULT_TEMPLATE[];

/* ═══════════════════════ grammar.c ═══════════════════════ */

typedef enum {
  ASPER_COP_INSERT = 0,
  ASPER_COP_UPDATE,
  ASPER_COP_DEPRECATE,
  ASPER_COP_KEEP,
  ASPER_COP_NOOP,
  ASPER_COP_ANSWER,
  ASPER_COP_CITE,
  ASPER_COP_NOMEM
} asper_cop_kind;

typedef struct {
  asper_cop_kind kind;
  asper_section section;   /* INSERT only */
  int handle;              /* 1-based M<n>; 0 when absent */
  char *text;              /* owned: fact / reason / answer; may be NULL */
} asper_cop;

typedef enum {
  ASPER_GRAMMAR_CURATION = 0,  /* INSERT/UPDATE/DEPRECATE/NOOP (+KEEP off) */
  ASPER_GRAMMAR_REVIEW,        /* DEPRECATE/KEEP/NOOP */
  ASPER_GRAMMAR_RECALL         /* ANSWER/CITE/NOMEM */
} asper_grammar_kind;

/* Emit llama.cpp GBNF for the cycle; handle_count = number of M<n> handles
 * (0 => no handle-bearing productions). malloc'd string. */
char *asper_gbnf_build(asper_grammar_kind kind, size_t handle_count,
                       int content_max_chars);
/* Parse curator output lines into malloc'd ops array. Tolerant: skips
 * malformed lines (counts them into *out_bad). handle_count bounds M<n>. */
asper_err asper_protocol_parse(const char *text, size_t handle_count,
                               asper_cop **out_ops, size_t *out_n,
                               size_t *out_bad);
void asper_cops_free(asper_cop *ops, size_t n);

/* Built-in curator instruction as a C string. */
extern const char ASPER_DEFAULT_INSTRUCTION[];
extern const char ASPER_REVIEW_INSTRUCTION[];
extern const char ASPER_RECALL_INSTRUCTION[];

/* ═══════════════════════ events / worker.c ═══════════════════════ */

typedef struct {
  asper_role role;
  char *text;      /* owned */
  asper_time at;
} asper_turn;

/* Curator-execution scheduling: every curator generation —
 * curation cycle, maintenance review, recall — runs holding the "cycle
 * slot": under ev_mu, wait until !cycle_busy, set it, run WITHOUT ev_mu,
 * clear it, broadcast done_cv. Recall increments recall_waiting while
 * waiting; the worker defers starting NEW cycles while recall_waiting > 0
 * (priority). Recall waits at most cfg.recall_timeout_s => ASPER_ERR_BUSY.
 * In ASPER_NO_THREADS builds all of this degenerates to direct calls. */

asper_err asper_worker_start(asper_ctx *c);
void      asper_worker_stop(asper_ctx *c); /* signal + join, drains work */
void      asper_worker_kick(asper_ctx *c); /* wake the worker */
/* Run everything due now on the calling thread: pending recall request,
 * curation cycle when triggered (force_cycle runs even a partial batch),
 * access-batch flush, maintenance review + compaction when due or `full`.
 * Shared by the worker loop, asper_tick and asper_flush. */
asper_err asper_run_due_work(asper_ctx *c, bool force_cycle, bool full);
/* Queue an access boost for id (ev_mu inside). */
void      asper_access_note(asper_ctx *c, const char *id);
/* Flush the pending access batch as one ACCESS op (no-op when empty). */
asper_err asper_access_flush(asper_ctx *c);

/* ═══════════════════════ curator.c ═══════════════════════ */

typedef struct {
  char id[37];
  asper_op_kind kind;      /* UPDATE or DEPRECATE */
  uint64_t content_hash;   /* fnv of proposed content ("" for DEPRECATE) */
  asper_time first_at;
} asper_pending;

/* One curation cycle over up to cfg.turn_batch queued turns (all of them
 * when force). Builds prompt, runs curator, validates,
 * applies via asper_apply_op. No-op when no turns queued. */
asper_err asper_curation_cycle(asper_ctx *c, bool force);
/* Maintenance review. `force` ignores the interval gate. */
asper_err asper_maintenance_review(asper_ctx *c, bool force);
/* Recall, executed on the worker (or synchronously without
 * threads). */
asper_err asper_recall_run(asper_ctx *c, const char *question,
                           int64_t deadline_ms,
                           char **out_answer, asper_record ***out_cited,
                           size_t *out_cited_n);

/* ═══════════════════════ api.c ═══════════════════════ */

/* Internal open used by asper_open and by tests: emb/cur/clk NULL mean
 * "default" (llama backends when built with ASPER_WITH_LLAMA and the model
 * files load — otherwise degraded mode with a WARN — and the system
 * clock). Non-NULL vtables are copied; ownership of their ud transfers to
 * the context (destroy called at close). */
asper_err asper_open_with(const asper_open_params *p,
                          const asper_embedder *emb,
                          const asper_curator_iface *cur,
                          const asper_clock *clk, asper_ctx **out);

/* THE single mutation funnel: validate, journal-append, apply to
 * table, maintain index/embeddings (INSERT/UPDATE re-embed via the
 * embedder; DEPRECATE removes the row), update stats. Takes the write lock
 * internally. `from_curator` selects guardrail behavior for
 * quorum/caps accounting. Frees/owns op members per asper_store_apply. */
asper_err asper_apply_op(asper_ctx *c, asper_op *op, bool from_curator);

/* Set the per-context error message (returns e for tail-calls). */
asper_err asper_seterr(asper_ctx *c, asper_err e, const char *fmt, ...);

/* ═══════════════════════ log.c ═══════════════════════ */

/* Open file sink per cfg (no-op when log_path NULL). */
asper_err asper_log_open(asper_ctx *c);
void      asper_log_close(asper_ctx *c);
/* Format + fan out to file sink (with rotation) and host callback.
 * subsys: "store","index","embed","curator","inject","recall","mcp",
 * "config","worker". Never fails the caller. */
void asper_log(asper_ctx *c, int level, const char *subsys,
               const char *fmt, ...);

/* ═══════════════════════ the context ═══════════════════════ */

struct asper_ctx {
  asper_config cfg;
  asper_store store;
  asper_index index;
  asper_clock clock;

  asper_embedder embedder;      bool has_embedder;
  asper_curator_iface curator;  bool has_curator;
  asmodel_manager *model_manager;
  bool owns_model_manager;

  char *active_project;         /* owned, NULL = none */

  /* concurrency */
  os_rwlock lock;               /* table + index + active_project + stats */
  os_mutex journal_mu;
  os_mutex cache_mu;             /* serializes cache snapshots/replaces */
  os_mutex err_mu;
  char err_buf[512];

  /* worker + events */
  os_thread worker;
  bool worker_running;
  bool stop_worker;
  os_mutex ev_mu;
  os_cond ev_cv;                /* worker wakeup */
  os_cond done_cv;              /* recall completion / drain */
  asper_turn *turns;            /* FIFO of unprocessed turns */
  size_t turns_n, turns_cap;
  size_t turns_dropped;
  asper_time last_turn_at;      /* 0 = none */
  char (*access_batch)[37];
  size_t access_n, access_cap;
  bool cycle_busy;              /* a curator generation is running */
  int  recall_waiting;          /* recalls waiting for the cycle slot */

  /* curator state */
  asper_pending *pending;       /* identity quorum set */
  size_t pending_n, pending_cap;
  asper_time last_maintenance;  /* 0 = never */

  /* stats (guarded by lock) */
  asper_stats stats;

  /* logging */
  FILE *log_fp;
  uint64_t log_size;
  os_mutex log_mu;
  asper_log_fn log_cb;
  void *log_ud;

  bool cache_dirty;             /* embeddings changed since last save */
  bool no_threads;              /* built/opened without worker */
};

#ifdef __cplusplus
}
#endif

#endif /* ASPER_INTERNAL_H */
