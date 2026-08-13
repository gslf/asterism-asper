/*
 * asper.h — Asterism Persistence ("Asper")
 *
 * An identitarian memory system for small LLMs.
 * Public C API of libasper. See docs/SPEC.txt §9.
 *
 * Contract notes:
 *   - All strings are UTF-8.
 *   - All functions return asper_err except destructors and accessors.
 *   - Every out-allocation is released with the matching asper_free /
 *     asper_records_free / asper_strings_free.
 *   - asper_open / asper_close are not thread-safe with respect to the same
 *     context; everything else is.
 *   - No global state: multiple contexts over different roots may coexist.
 *   - asper_last_error is per-context (not per-thread): the message refers to
 *     the most recent failing call on that context.
 *
 * License: MIT — per aspera ad astra.
 */

#ifndef ASPER_H
#define ASPER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASPER_VERSION_MAJOR 0
#define ASPER_VERSION_MINOR 1
#define ASPER_VERSION_PATCH 0

/* Returns "major.minor.patch". */
const char *asper_version(void);

typedef struct asper_ctx asper_ctx;

typedef enum {
  ASPER_OK = 0,
  ASPER_ERR_IO,        /* filesystem failure                                */
  ASPER_ERR_PARSE,     /* malformed xCDN / journal / cache                  */
  ASPER_ERR_CONFIG,    /* invalid configuration value                       */
  ASPER_ERR_MODEL,     /* model load / inference failure                    */
  ASPER_ERR_NOT_FOUND, /* unknown id / project / no recall answer           */
  ASPER_ERR_LOCKED,    /* operation rejected on a locked record             */
  ASPER_ERR_INVALID,   /* invalid argument or state                         */
  ASPER_ERR_BUSY,      /* timeout / worker unavailable                      */
  ASPER_ERR_NOMEM      /* allocation failure                                */
} asper_err;

/* Stable name of an error code, e.g. "ASPER_ERR_IO". */
const char *asper_err_name(asper_err e);

typedef enum {
  ASPER_SECTION_IDENTITY = 0,
  ASPER_SECTION_CONTEXT  = 1,
  ASPER_SECTION_PROJECT  = 2,
  /* Only valid for asper_memory_search / asper_memory_list: all sections. */
  ASPER_SECTION_ANY      = 3
} asper_section;

typedef enum {
  ASPER_ROLE_USER = 0,
  ASPER_ROLE_ASSISTANT = 1
} asper_role;

/* Log levels for asper_set_logger callbacks. */
enum {
  ASPER_LOG_ERROR = 0,
  ASPER_LOG_WARN  = 1,
  ASPER_LOG_INFO  = 2,
  ASPER_LOG_DEBUG = 3
};

typedef struct {
  const char *memory_root; /* store directory; created if missing        */
  const char *config_path; /* optional config.xcdn; NULL = defaults      */
} asper_open_params;

asper_err   asper_open(const asper_open_params *p, asper_ctx **out);
/* Flush, compact, join worker, release everything. NULL is a no-op. */
void        asper_close(asper_ctx *c);
/* UTF-8 message for the most recent error on this context; ctx-owned. */
const char *asper_last_error(const asper_ctx *c);

/* ---- hot path ---------------------------------------------------------- */

asper_err asper_observe_turn(asper_ctx *c, asper_role role,
                             const char *text_utf8);
/* Returns base_system_prompt + rendered memory block in *out_prompt
 * (asper_free). Deterministic: same store + same inputs => same bytes. */
asper_err asper_build_prompt(asper_ctx *c, const char *base_system_prompt,
                             const char *user_message, char **out_prompt);

/* ---- records ----------------------------------------------------------- */

typedef struct asper_record asper_record;

const char   *asper_record_id(const asper_record *r);          /* UUID */
asper_section asper_record_section(const asper_record *r);
/* Project slug, or NULL when the record is not in a project section. */
const char   *asper_record_project(const asper_record *r);
const char   *asper_record_content(const asper_record *r);
/* "seed" | "manual" | "curator" */
const char   *asper_record_source(const asper_record *r);
/* Unix seconds, UTC. */
long long     asper_record_created_at(const asper_record *r);
long long     asper_record_updated_at(const asper_record *r);
long long     asper_record_last_access(const asper_record *r);
unsigned      asper_record_access_count(const asper_record *r);
double        asper_record_relevance(const asper_record *r);   /* stored */
int           asper_record_locked(const asper_record *r);
int           asper_record_deprecated(const asper_record *r);
/* UUID of the replaced record, or NULL. */
const char   *asper_record_supersedes(const asper_record *r);
size_t        asper_record_tag_count(const asper_record *r);
const char   *asper_record_tag(const asper_record *r, size_t i);
/* Retrieval score when the record came from asper_memory_search; else 0. */
double        asper_record_score(const asper_record *r);

/* ---- recall: pull channel (spec §7.7) ---------------------------------- */

/* Blocking, up to recall.timeout. out_cited/out_cited_n may be NULL if the
 * citations are unwanted. NOMEM from the curator maps to ASPER_ERR_NOT_FOUND
 * with *out_answer = NULL. */
asper_err asper_recall(asper_ctx *c, const char *question,
                       char **out_answer,
                       asper_record ***out_cited, size_t *out_cited_n);

/* ---- projects ---------------------------------------------------------- */

/* slug NULL deselects the active project. Unknown slug: creates the project
 * when projects.autocreate (default), else ASPER_ERR_NOT_FOUND. */
asper_err asper_project_select(asper_ctx *c, const char *slug);
asper_err asper_project_list(asper_ctx *c, char ***out_slugs, size_t *out_n);
/* Active project slug into *out_slug (asper_free), or NULL if none. */
asper_err asper_project_active(asper_ctx *c, char **out_slug);

/* ---- direct memory access (asper-mcp and host tooling) ----------------- */

/* project: required iff s == ASPER_SECTION_PROJECT (must be the active
 * project or an existing one); ignored otherwise. out_id: 36 chars + NUL. */
asper_err asper_memory_insert(asper_ctx *c, asper_section s,
                              const char *project, const char *content,
                              int locked, char out_id[37]);
asper_err asper_memory_update(asper_ctx *c, const char *id,
                              const char *content);
asper_err asper_memory_deprecate(asper_ctx *c, const char *id,
                                 const char *reason);
asper_err asper_memory_set_locked(asper_ctx *c, const char *id, int locked);
/* s may be ASPER_SECTION_ANY. project selects which project's records are
 * searched; NULL means the active project (with no active project, project
 * records are absent from the results). */
asper_err asper_memory_search(asper_ctx *c, asper_section s,
                              const char *project, const char *query,
                              size_t k, asper_record ***out, size_t *out_n);
asper_err asper_memory_list(asper_ctx *c, asper_section s,
                            const char *project, int include_deprecated,
                            asper_record ***out, size_t *out_n);

/* ---- lifecycle / maintenance ------------------------------------------- */

/* full != 0: run pending curation + maintenance review + compaction.
 * full == 0: flush access batch + journal to disk. */
asper_err asper_flush(asper_ctx *c, int full);
/* ASPER_NO_THREADS builds: run due background work on the caller.
 * Threaded builds: no-op returning ASPER_OK. */
asper_err asper_tick(asper_ctx *c);

typedef struct {
  size_t records_identity, records_context, records_project;
  size_t records_deprecated, journal_ops, cycles_run;
  size_t ops_applied, ops_rejected, recalls_served;
  long long last_cycle_at;      /* unix seconds UTC; 0 = never */
  long long last_compaction_at; /* unix seconds UTC; 0 = never */
} asper_stats;
asper_err asper_get_stats(asper_ctx *c, asper_stats *out);

/* Callback sink, independent of the file log. level: ASPER_LOG_*. The
 * default callback writes WARN and ERROR to stderr. fn NULL disables. */
typedef void (*asper_log_fn)(int level, const char *msg, void *userdata);
void asper_set_logger(asper_ctx *c, asper_log_fn fn, void *userdata);

/* ---- memory management -------------------------------------------------- */

void asper_free(void *p);                          /* strings from the API */
void asper_records_free(asper_record **r, size_t n);
void asper_strings_free(char **s, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ASPER_H */
