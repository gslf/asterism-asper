/*
 * main.c — asper-mcp: MCP stdio server over libasper.
 *
 * JSON-RPC 2.0, newline-delimited compact JSON on stdin/stdout, protocol
 * revision 2025-06-18. Uses the PUBLIC libasper API exclusively (asper.h).
 *
 * Wire conventions:
 *   - One request per line; responses are single-line compact JSON,
 *     flushed after each write.
 *   - Batch arrays are not supported (MCP does not use them): -32600.
 *   - Requests without an "id" are notifications: processed, no response.
 *     Methods under "notifications/" are ignored only when the id is
 *     absent; with an id they dispatch normally (unknown => -32601).
 *   - Tool results: {content:[{type:"text",text:<compact JSON payload>}],
 *     isError:bool}. Asper failures set isError:true with payload
 *     {"error":"ASPER_ERR_...","message":"..."}.
 *   - Record timestamps are integer unix seconds (UTC) in the fields
 *     created_at_unix / updated_at_unix / last_access_unix (the library
 *     exposes unix seconds; RFC 3339 formatting is left to clients).
 *   - stderr carries nothing but a startup failure message; the library's
 *     default stderr log callback is disabled at startup.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asper.h"
#include "json.h"

/* ═══════════════════════ usage / help ═══════════════════════ */

static const char USAGE[] =
    "usage: asper-mcp --root <dir> [--config <file>]\n"
    "       asper-mcp --help | --version\n";

static const char HELP[] =
    "asper-mcp - MCP stdio server for Asper (Asterism Persistence)\n"
    "\n"
    "usage: asper-mcp --root <dir> [--config <file>]\n"
    "       asper-mcp --help | --version\n"
    "\n"
    "options:\n"
    "  --root <dir>     memory store directory (created if missing);"
    " required\n"
    "  --config <file>  optional xCDN configuration file (#asper_config)\n"
    "  --help           print this help and exit\n"
    "  --version        print the version and exit\n"
    "\n"
    "The server speaks JSON-RPC 2.0 over stdio (legacy MCP 2025-06-18\n"
    "and modern MCP 2026-07-28), one compact JSON message per line.\n"
    "Memory records in\n"
    "tool results are JSON objects; their timestamps are integer unix\n"
    "seconds (UTC) in the fields created_at_unix, updated_at_unix and\n"
    "last_access_unix.\n";

/* Emitted verbatim when even the error response cannot be allocated. */
static const char OOM_RESPONSE[] =
    "{\"jsonrpc\":\"2.0\",\"id\":null,"
    "\"error\":{\"code\":-32603,\"message\":\"out of memory\"}}";

#define MCP_LEGACY_VERSION "2025-06-18"
#define MCP_MODERN_VERSION "2026-07-28"

/* ═══════════════════════ small helpers ═══════════════════════ */

static void emit_line(const char *s) {
  fputs(s, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

/* Serialize + emit + free resp; OOM falls back to the static response. */
static void send_value(jx_value *resp) {
  char *s = jx_write(resp, 0);
  jx_free(resp);
  if (!s) {
    emit_line(OOM_RESPONSE);
    return;
  }
  emit_line(s);
  free(s);
}

/* want == 0: notification — consume the owned arguments, emit nothing. */
static int decorate_modern_result(jx_value *result) {
  jx_value *meta, *info;
  int ok;
  if (!result || jx_typeof(result) != JX_OBJECT) return 0;
  meta = jx_object();
  info = jx_object();
  ok = (meta != NULL && info != NULL);
  ok &= jx_object_set(info, "name", jx_string("asper-mcp")) == 0;
  ok &= jx_object_set(info, "version", jx_string(asper_version())) == 0;
  ok &= jx_object_set(meta, "io.modelcontextprotocol/serverInfo", info) == 0;
  ok &= jx_object_set(result, "resultType", jx_string("complete")) == 0;
  ok &= jx_object_set(result, "_meta", meta) == 0;
  return ok;
}

static void send_result(int want, jx_value *id, jx_value *result, int modern) {
  jx_value *resp;
  int ok;
  if (!want) {
    jx_free(id);
    jx_free(result);
    return;
  }
  if (modern && !decorate_modern_result(result)) {
    jx_free(id);
    jx_free(result);
    emit_line(OOM_RESPONSE);
    return;
  }
  resp = jx_object();
  ok = (resp != NULL);
  ok &= jx_object_set(resp, "jsonrpc", jx_string("2.0")) == 0;
  ok &= jx_object_set(resp, "id", id ? id : jx_null()) == 0;
  ok &= jx_object_set(resp, "result", result) == 0;
  if (!ok) {
    jx_free(resp);
    emit_line(OOM_RESPONSE);
    return;
  }
  send_value(resp);
}

static void send_error(int want, jx_value *id, int code, const char *message,
                       const char *asper_name) {
  jx_value *err, *resp;
  int ok;
  if (!want) {
    jx_free(id);
    return;
  }
  err = jx_object();
  ok = (err != NULL);
  ok &= jx_object_set(err, "code", jx_int(code)) == 0;
  ok &= jx_object_set(err, "message", jx_string(message)) == 0;
  if (asper_name) {
    jx_value *data = jx_object();
    ok &= jx_object_set(data, "asper", jx_string(asper_name)) == 0;
    ok &= jx_object_set(err, "data", data) == 0;
  }
  resp = jx_object();
  ok &= (resp != NULL);
  ok &= jx_object_set(resp, "jsonrpc", jx_string("2.0")) == 0;
  ok &= jx_object_set(resp, "id", id ? id : jx_null()) == 0;
  ok &= jx_object_set(resp, "error", err) == 0;
  if (!ok) {
    jx_free(resp);
    emit_line(OOM_RESPONSE);
    return;
  }
  send_value(resp);
}

/* Wrap a tool payload in the MCP content envelope and send it. */
static void send_tool_result(int want, jx_value *id, jx_value *payload,
                             int is_error, int modern) {
  char *txt;
  jx_value *item, *content, *res;
  int ok;
  if (!want) {
    jx_free(id);
    jx_free(payload);
    return;
  }
  txt = jx_write(payload, 0);
  jx_free(payload);
  if (!txt) {
    jx_free(id);
    emit_line(OOM_RESPONSE);
    return;
  }
  item = jx_object();
  ok = (item != NULL);
  ok &= jx_object_set(item, "type", jx_string("text")) == 0;
  ok &= jx_object_set(item, "text", jx_string(txt)) == 0;
  free(txt);
  content = jx_array();
  ok &= jx_array_push(content, item) == 0;
  res = jx_object();
  ok &= jx_object_set(res, "content", content) == 0;
  ok &= jx_object_set(res, "isError", jx_bool(is_error)) == 0;
  if (!ok) {
    jx_free(res);
    jx_free(id);
    emit_line(OOM_RESPONSE);
    return;
  }
  send_result(1, id, res, modern);
}

/* ═══════════════════════ name maps ═══════════════════════ */

static const char *section_to_name(asper_section s) {
  switch (s) {
  case ASPER_SECTION_IDENTITY: return "identity";
  case ASPER_SECTION_CONTEXT:  return "context";
  case ASPER_SECTION_PROJECT:  return "project";
  case ASPER_SECTION_ANY:      return "any";
  }
  return "?";
}

static int section_from_name(const char *s, asper_section *out) {
  if (strcmp(s, "identity") == 0) { *out = ASPER_SECTION_IDENTITY; return 1; }
  if (strcmp(s, "context") == 0)  { *out = ASPER_SECTION_CONTEXT;  return 1; }
  if (strcmp(s, "project") == 0)  { *out = ASPER_SECTION_PROJECT;  return 1; }
  if (strcmp(s, "any") == 0)      { *out = ASPER_SECTION_ANY;      return 1; }
  return 0;
}

static int event_kind_from_name(const char *s, asper_event_kind *out) {
  if (strcmp(s, "user") == 0) { *out = ASPER_EVENT_USER; return 1; }
  if (strcmp(s, "assistant") == 0) {
    *out = ASPER_EVENT_ASSISTANT;
    return 1;
  }
  if (strcmp(s, "decision") == 0) { *out = ASPER_EVENT_DECISION; return 1; }
  if (strcmp(s, "tool_call") == 0) { *out = ASPER_EVENT_TOOL_CALL; return 1; }
  if (strcmp(s, "tool_result") == 0) {
    *out = ASPER_EVENT_TOOL_RESULT;
    return 1;
  }
  if (strcmp(s, "diagnostic") == 0) {
    *out = ASPER_EVENT_DIAGNOSTIC;
    return 1;
  }
  if (strcmp(s, "checkpoint") == 0) {
    *out = ASPER_EVENT_CHECKPOINT;
    return 1;
  }
  if (strcmp(s, "artifact") == 0) { *out = ASPER_EVENT_ARTIFACT; return 1; }
  return 0;
}

/* ═══════════════════════ argument extraction ═══════════════════════ */

/* 1 = present and well-typed, 0 = absent, -1 = wrong type. A string with
 * an embedded NUL is wrong-typed (it would be silently truncated by C
 * string consumers); *msg then carries the specific reason. args may be
 * NULL ("arguments" omitted). */
static int arg_str(const jx_value *args, const char *key, const char **out,
                   const char **msg) {
  const jx_value *v = jx_object_get(args, key);
  if (!v) return 0;
  if (jx_typeof(v) != JX_STRING) return -1;
  if (jx_string_length(v) != strlen(jx_string_value(v))) {
    *msg = "string must not contain NUL";
    return -1;
  }
  *out = jx_string_value(v);
  return 1;
}

static int arg_int(const jx_value *args, const char *key, long long *out) {
  const jx_value *v = jx_object_get(args, key);
  double d;
  if (!v) return 0;
  if (jx_typeof(v) != JX_NUMBER) return -1;
  if (jx_is_int(v)) {
    *out = jx_int_value(v);
    return 1;
  }
  /* Schema-valid integral spellings (5.0, 1e2): accept when the double
   * is finite, integral, and exactly representable as long long. */
  d = jx_double_value(v);
  if (!isfinite(d) || d != floor(d) || d < -0x1p63 || d >= 0x1p63)
    return -1;
  *out = (long long)d;
  return 1;
}

static int arg_bool(const jx_value *args, const char *key, int *out) {
  const jx_value *v = jx_object_get(args, key);
  if (!v) return 0;
  if (jx_typeof(v) != JX_BOOL) return -1;
  *out = jx_bool_value(v);
  return 1;
}

/* ═══════════════════════ payload builders ═══════════════════════ */

static jx_value *record_json(const asper_record *r) {
  jx_value *o = jx_object();
  jx_value *tags;
  const char *proj = asper_record_project(r);
  const char *sup = asper_record_supersedes(r);
  size_t i, tn = asper_record_tag_count(r);
  int ok = (o != NULL);

  ok &= jx_object_set(o, "id", jx_string(asper_record_id(r))) == 0;
  ok &= jx_object_set(o, "section",
                      jx_string(section_to_name(asper_record_section(r)))) == 0;
  ok &= jx_object_set(o, "project", proj ? jx_string(proj) : jx_null()) == 0;
  ok &= jx_object_set(o, "content",
                      jx_string(asper_record_content(r))) == 0;
  ok &= jx_object_set(o, "source", jx_string(asper_record_source(r))) == 0;
  { const asper_evidence *ev=asper_record_evidence(r);
    static const char *const kinds[]={"declared","observed","inferred"};
    ok &= jx_object_set(o,"evidence_kind",jx_string(kinds[ev->kind]))==0;
    ok &= jx_object_set(o,"confidence",jx_double(ev->confidence))==0;
    ok &= jx_object_set(o,"provenance",jx_string(ev->provenance))==0;
    ok &= jx_object_set(o,"workspace",jx_string(ev->workspace))==0;
    ok &= jx_object_set(o,"commit",jx_string(ev->commit))==0;
    ok &= jx_object_set(o,"observed_at_unix",jx_int(ev->observed_at))==0;
    ok &= jx_object_set(o,"expires_at_unix",jx_int(ev->expires_at))==0;
    jx_value *refs=jx_array();
    for (size_t j=0;j<asper_record_source_ref_count(r);j++)
      if (jx_array_push(refs,jx_string(asper_record_source_ref(r,j)))!=0) ok=0;
    ok &= jx_object_set(o,"source_refs",refs)==0;
  }
  ok &= jx_object_set(o, "created_at_unix",
                      jx_int(asper_record_created_at(r))) == 0;
  ok &= jx_object_set(o, "updated_at_unix",
                      jx_int(asper_record_updated_at(r))) == 0;
  ok &= jx_object_set(o, "last_access_unix",
                      jx_int(asper_record_last_access(r))) == 0;
  ok &= jx_object_set(o, "access_count",
                      jx_int((long long)asper_record_access_count(r))) == 0;
  ok &= jx_object_set(o, "relevance",
                      jx_double(asper_record_relevance(r))) == 0;
  ok &= jx_object_set(o, "locked",
                      jx_bool(asper_record_locked(r))) == 0;
  ok &= jx_object_set(o, "deprecated",
                      jx_bool(asper_record_deprecated(r))) == 0;
  ok &= jx_object_set(o, "supersedes", sup ? jx_string(sup) : jx_null()) == 0;
  tags = jx_array();
  for (i = 0; i < tn; i++) {
    if (jx_array_push(tags, jx_string(asper_record_tag(r, i))) != 0) ok = 0;
  }
  ok &= jx_object_set(o, "tags", tags) == 0;
  ok &= jx_object_set(o, "score", jx_double(asper_record_score(r))) == 0;
  if (!ok) {
    jx_free(o);
    return NULL;
  }
  return o;
}

static jx_value *records_json(asper_record **recs, size_t n) {
  jx_value *arr = jx_array();
  size_t i;
  if (!arr) return NULL;
  for (i = 0; i < n; i++) {
    if (jx_array_push(arr, record_json(recs[i])) != 0) {
      jx_free(arr);
      return NULL;
    }
  }
  return arr;
}

static jx_value *ok_payload(void) {
  jx_value *o = jx_object();
  if (jx_object_set(o, "ok", jx_bool(1)) != 0) {
    jx_free(o);
    return NULL;
  }
  return o;
}

static jx_value *asper_error_payload(asper_ctx *c, asper_err e) {
  jx_value *o = jx_object();
  const char *msg = asper_last_error(c);
  jx_value *m;
  int ok = (o != NULL);
  ok &= jx_object_set(o, "error", jx_string(asper_err_name(e))) == 0;
  m = jx_string(msg ? msg : "");
  if (!m) m = jx_string(""); /* message not valid UTF-8: drop it */
  ok &= jx_object_set(o, "message", m) == 0;
  if (!ok) {
    jx_free(o);
    return NULL;
  }
  return o;
}

/* ═══════════════════════ tools ═══════════════════════ */

enum { TOOL_OK = 0, TOOL_ASPER, TOOL_PARAM, TOOL_OOM };

/* First message wins: arg_str may already have set a more specific one
 * (*msg is NULL on tool entry). */
#define BADP(m)                                                              \
  do {                                                                       \
    if (!*msg) *msg = (m);                                                   \
    return TOOL_PARAM;                                                       \
  } while (0)

typedef int (*tool_fn)(asper_ctx *c, const jx_value *args, jx_value **out,
                       asper_err *aerr, const char **msg);

static int tool_memory_search(asper_ctx *c, const jx_value *args,
                              jx_value **out, asper_err *aerr,
                              const char **msg) {
  const char *query = NULL, *sect = NULL, *project = NULL;
  long long k = 0;
  asper_section s = ASPER_SECTION_ANY;
  asper_record **recs = NULL;
  size_t n = 0;
  asper_err e;
  jx_value *arr, *o;
  int rc;

  if (arg_str(args, "query", &query, msg) != 1)
    BADP("memory_search: \"query\" must be a string");
  rc = arg_str(args, "section", &sect, msg);
  if (rc < 0 || (rc == 1 && !section_from_name(sect, &s)))
    BADP("memory_search: \"section\" must be one of "
         "identity|context|project|any");
  if (arg_str(args, "project", &project, msg) < 0)
    BADP("memory_search: \"project\" must be a string");
  rc = arg_int(args, "k", &k);
  if (rc < 0 || k < 0)
    BADP("memory_search: \"k\" must be a non-negative integer");

  e = asper_memory_search(c, s, project, query, (size_t)k, &recs, &n);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  arr = records_json(recs, n);
  asper_records_free(recs, n);
  o = jx_object();
  if (jx_object_set(o, "matches", arr) != 0) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_memory_recall(asper_ctx *c, const jx_value *args,
                              jx_value **out, asper_err *aerr,
                              const char **msg) {
  const char *question = NULL;
  char *answer = NULL;
  asper_record **cited = NULL;
  size_t cn = 0;
  asper_err e;
  jx_value *o;
  int ok;

  if (arg_str(args, "question", &question, msg) != 1)
    BADP("memory_recall: \"question\" must be a string");
  e = asper_recall(c, question, &answer, &cited, &cn);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  o = jx_object();
  ok = (o != NULL);
  ok &= jx_object_set(o, "answer", jx_string(answer ? answer : "")) == 0;
  ok &= jx_object_set(o, "cited", records_json(cited, cn)) == 0;
  asper_free(answer);
  asper_records_free(cited, cn);
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_memory_insert(asper_ctx *c, const jx_value *args,
                              jx_value **out, asper_err *aerr,
                              const char **msg) {
  const char *sect = NULL, *content = NULL, *project = NULL;
  int locked = 0;
  asper_section s = ASPER_SECTION_CONTEXT;
  char id[37];
  asper_err e;
  jx_value *o;

  if (arg_str(args, "section", &sect, msg) != 1 ||
      !section_from_name(sect, &s) || s == ASPER_SECTION_ANY)
    BADP("memory_insert: \"section\" must be one of "
         "identity|context|project");
  if (arg_str(args, "content", &content, msg) != 1)
    BADP("memory_insert: \"content\" must be a string");
  if (arg_str(args, "project", &project, msg) < 0)
    BADP("memory_insert: \"project\" must be a string");
  if (arg_bool(args, "locked", &locked) < 0)
    BADP("memory_insert: \"locked\" must be a boolean");

  e = asper_memory_insert(c, s, project, content, locked, id);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  o = jx_object();
  if (jx_object_set(o, "id", jx_string(id)) != 0) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_memory_update(asper_ctx *c, const jx_value *args,
                              jx_value **out, asper_err *aerr,
                              const char **msg) {
  const char *id = NULL, *content = NULL;
  asper_err e;
  if (arg_str(args, "id", &id, msg) != 1)
    BADP("memory_update: \"id\" must be a string");
  if (arg_str(args, "content", &content, msg) != 1)
    BADP("memory_update: \"content\" must be a string");
  e = asper_memory_update(c, id, content);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  *out = ok_payload();
  return *out ? TOOL_OK : TOOL_OOM;
}

static int tool_memory_deprecate(asper_ctx *c, const jx_value *args,
                                 jx_value **out, asper_err *aerr,
                                 const char **msg) {
  const char *id = NULL, *reason = NULL;
  asper_err e;
  if (arg_str(args, "id", &id, msg) != 1)
    BADP("memory_deprecate: \"id\" must be a string");
  if (arg_str(args, "reason", &reason, msg) < 0)
    BADP("memory_deprecate: \"reason\" must be a string");
  e = asper_memory_deprecate(c, id, reason);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  *out = ok_payload();
  return *out ? TOOL_OK : TOOL_OOM;
}

static int tool_memory_list(asper_ctx *c, const jx_value *args,
                            jx_value **out, asper_err *aerr,
                            const char **msg) {
  const char *sect = NULL, *project = NULL;
  int incl = 0, rc;
  asper_section s = ASPER_SECTION_ANY;
  asper_record **recs = NULL;
  size_t n = 0;
  asper_err e;
  jx_value *arr, *o;

  rc = arg_str(args, "section", &sect, msg);
  if (rc < 0 || (rc == 1 && !section_from_name(sect, &s)))
    BADP("memory_list: \"section\" must be one of "
         "identity|context|project|any");
  if (arg_str(args, "project", &project, msg) < 0)
    BADP("memory_list: \"project\" must be a string");
  if (arg_bool(args, "include_deprecated", &incl) < 0)
    BADP("memory_list: \"include_deprecated\" must be a boolean");

  e = asper_memory_list(c, s, project, incl, &recs, &n);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  arr = records_json(recs, n);
  asper_records_free(recs, n);
  o = jx_object();
  if (jx_object_set(o, "records", arr) != 0) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_project_select(asper_ctx *c, const jx_value *args,
                               jx_value **out, asper_err *aerr,
                               const char **msg) {
  const jx_value *sv = jx_object_get(args, "slug");
  const char *slug = NULL;
  asper_err e;
  jx_value *o;
  int ok;

  if (!sv)
    BADP("project_select: \"slug\" is required (string or null)");
  if (jx_typeof(sv) == JX_STRING) {
    if (jx_string_length(sv) != strlen(jx_string_value(sv)))
      BADP("string must not contain NUL");
    slug = jx_string_value(sv);
  } else if (jx_typeof(sv) != JX_NULL) {
    BADP("project_select: \"slug\" must be a string or null");
  }

  e = asper_project_select(c, slug);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  o = jx_object();
  ok = (o != NULL);
  ok &= jx_object_set(o, "ok", jx_bool(1)) == 0;
  ok &= jx_object_set(o, "active", slug ? jx_string(slug) : jx_null()) == 0;
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_project_list(asper_ctx *c, const jx_value *args,
                             jx_value **out, asper_err *aerr,
                             const char **msg) {
  char **slugs = NULL;
  size_t n = 0, i;
  asper_err e;
  jx_value *arr, *o;
  int ok;
  (void)args;
  (void)msg;

  e = asper_project_list(c, &slugs, &n);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  arr = jx_array();
  ok = (arr != NULL);
  for (i = 0; i < n; i++) {
    if (jx_array_push(arr, jx_string(slugs[i])) != 0) ok = 0;
  }
  asper_strings_free(slugs, n);
  o = jx_object();
  ok &= jx_object_set(o, "projects", arr) == 0;
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_source_append(asper_ctx *c, const jx_value *args,
                              jx_value **out, asper_err *aerr,
                              const char **msg) {
  const char *scope = NULL, *kind_s = NULL, *text = NULL;
  asper_event_kind kind = ASPER_EVENT_USER;
  asper_event_input event;
  asper_err e;
  if (arg_str(args, "scope", &scope, msg) != 1)
    BADP("source_append: \"scope\" must be a string");
  if (arg_str(args, "kind", &kind_s, msg) != 1 ||
      !event_kind_from_name(kind_s, &kind))
    BADP("source_append: invalid \"kind\"");
  if (arg_str(args, "text", &text, msg) != 1)
    BADP("source_append: \"text\" must be a string");
  memset(&event, 0, sizeof event);
  event.scope = scope;
  event.kind = kind;
  event.text = text;
  e = asper_event_append(c, &event, NULL);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  *out = ok_payload();
  return *out ? TOOL_OK : TOOL_OOM;
}

static int tool_context_materialize(asper_ctx *c, const jx_value *args,
                                    jx_value **out, asper_err *aerr,
                                    const char **msg) {
  const char *scope = NULL, *query = NULL, *base = NULL;
  long long history_tokens = 0, checkpoint_tokens = 0;
  asper_context_request request;
  asper_context_pack pack;
  asper_err e;
  jx_value *o;
  int ok;

  if (arg_str(args, "scope", &scope, msg) != 1)
    BADP("context_materialize: \"scope\" must be a string");
  if (arg_str(args, "query", &query, msg) != 1)
    BADP("context_materialize: \"query\" must be a string");
  if (arg_str(args, "base_system_prompt", &base, msg) < 0)
    BADP("context_materialize: \"base_system_prompt\" must be a string");
  if (arg_int(args, "history_tokens", &history_tokens) != 1 ||
      history_tokens < 0)
    BADP("context_materialize: \"history_tokens\" must be >= 0");
  if (arg_int(args, "checkpoint_tokens", &checkpoint_tokens) != 1 ||
      checkpoint_tokens < 0)
    BADP("context_materialize: \"checkpoint_tokens\" must be >= 0");
  memset(&request, 0, sizeof request);
  memset(&pack, 0, sizeof pack);
  request.scope = scope;
  request.query = query;
  request.base_system_prompt = base ? base : "";
  request.history_tokens = (size_t)history_tokens;
  request.checkpoint_tokens = (size_t)checkpoint_tokens;
  e = asper_context_materialize(c, &request, &pack);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  o = jx_object();
  ok = jx_object_set(o, "system_prompt",
                     jx_string(pack.system_prompt ? pack.system_prompt : "")) == 0;
  ok &= jx_object_set(o, "context",
                      jx_string(pack.context_text ? pack.context_text : "")) == 0;
  ok &= jx_object_set(o, "system_tokens",
                      jx_int((long long)pack.system_tokens)) == 0;
  ok &= jx_object_set(o, "context_tokens",
                      jx_int((long long)pack.context_tokens)) == 0;
  asper_context_pack_free(&pack);
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_memory_stats(asper_ctx *c, const jx_value *args,
                             jx_value **out, asper_err *aerr,
                             const char **msg) {
  asper_stats st;
  asper_err e;
  jx_value *o;
  int ok;
  (void)args;
  (void)msg;

  memset(&st, 0, sizeof st);
  e = asper_get_stats(c, &st);
  if (e != ASPER_OK) {
    *aerr = e;
    return TOOL_ASPER;
  }
  o = jx_object();
  ok = (o != NULL);
  ok &= jx_object_set(o, "records_identity",
                      jx_int((long long)st.records_identity)) == 0;
  ok &= jx_object_set(o, "records_context",
                      jx_int((long long)st.records_context)) == 0;
  ok &= jx_object_set(o, "records_project",
                      jx_int((long long)st.records_project)) == 0;
  ok &= jx_object_set(o, "records_deprecated",
                      jx_int((long long)st.records_deprecated)) == 0;
  ok &= jx_object_set(o, "journal_ops",
                      jx_int((long long)st.journal_ops)) == 0;
  ok &= jx_object_set(o, "cycles_run",
                      jx_int((long long)st.cycles_run)) == 0;
  ok &= jx_object_set(o, "ops_applied",
                      jx_int((long long)st.ops_applied)) == 0;
  ok &= jx_object_set(o, "ops_rejected",
                      jx_int((long long)st.ops_rejected)) == 0;
  ok &= jx_object_set(o, "recalls_served",
                      jx_int((long long)st.recalls_served)) == 0;
  ok &= jx_object_set(o, "last_cycle_at_unix",
                      jx_int(st.last_cycle_at)) == 0;
  ok &= jx_object_set(o, "last_compaction_at_unix",
                      jx_int(st.last_compaction_at)) == 0;
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

/* ═══════════════════════════ tool table ══════════════════════════ */

typedef struct {
  const char *name;
  const char *desc;
  const char *schema; /* JSON Schema literal, parsed on tools/list */
  tool_fn fn;
} tool_def;

static const tool_def TOOLS[] = {
    {"memory_search",
     "Semantic search over stored memories; returns ranked matching "
     "records.",
     "{\"type\":\"object\",\"properties\":{"
     "\"query\":{\"type\":\"string\",\"description\":\"Natural-language "
     "search query\"},"
     "\"section\":{\"type\":\"string\",\"enum\":[\"identity\",\"context\","
     "\"project\",\"any\"],\"description\":\"Restrict the search to one "
     "section (default any)\"},"
     "\"project\":{\"type\":\"string\",\"description\":\"Project slug "
     "filter for project records\"},"
     "\"k\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"Maximum "
     "results; 0 or absent uses the library default\"}},"
     "\"required\":[\"query\"]}",
     tool_memory_search},
    {"memory_recall",
     "Ask the curator a natural-language question about the memory; "
     "returns an answer with cited records.",
     "{\"type\":\"object\",\"properties\":{"
     "\"question\":{\"type\":\"string\",\"description\":\"Natural-language "
     "question about the stored memory\"}},"
     "\"required\":[\"question\"]}",
     tool_memory_recall},
    {"memory_insert",
     "Insert a manual memory record; returns its id.",
     "{\"type\":\"object\",\"properties\":{"
     "\"section\":{\"type\":\"string\",\"enum\":[\"identity\",\"context\","
     "\"project\"],\"description\":\"Target memory section\"},"
     "\"content\":{\"type\":\"string\",\"description\":\"Self-contained "
     "memory statement\"},"
     "\"project\":{\"type\":\"string\",\"description\":\"Project slug; "
     "required when section is project\"},"
     "\"locked\":{\"type\":\"boolean\",\"description\":\"Protect the "
     "record from the curator (default false)\"}},"
     "\"required\":[\"section\",\"content\"]}",
     tool_memory_insert},
    {"memory_update",
     "Rewrite the content of an existing record.",
     "{\"type\":\"object\",\"properties\":{"
     "\"id\":{\"type\":\"string\",\"description\":\"Record UUID\"},"
     "\"content\":{\"type\":\"string\",\"description\":\"Replacement "
     "content\"}},"
     "\"required\":[\"id\",\"content\"]}",
     tool_memory_update},
    {"memory_deprecate",
     "Soft-deprecate a record (recoverable until purged at compaction).",
     "{\"type\":\"object\",\"properties\":{"
     "\"id\":{\"type\":\"string\",\"description\":\"Record UUID\"},"
     "\"reason\":{\"type\":\"string\",\"description\":\"Short reason for "
     "the deprecation\"}},"
     "\"required\":[\"id\"]}",
     tool_memory_deprecate},
    {"memory_list",
     "List memory records, optionally filtered by section and project.",
     "{\"type\":\"object\",\"properties\":{"
     "\"section\":{\"type\":\"string\",\"enum\":[\"identity\",\"context\","
     "\"project\",\"any\"],\"description\":\"Restrict to one section "
     "(default any)\"},"
     "\"project\":{\"type\":\"string\",\"description\":\"Project slug "
     "filter\"},"
     "\"include_deprecated\":{\"type\":\"boolean\",\"description\":"
     "\"Include deprecated records (default false)\"}}}",
     tool_memory_list},
    {"project_select",
     "Select the active project by slug, or null to deselect.",
     "{\"type\":\"object\",\"properties\":{"
     "\"slug\":{\"type\":[\"string\",\"null\"],\"description\":\"Project "
     "slug to activate; null deselects the active project\"}},"
     "\"required\":[\"slug\"]}",
     tool_project_select},
    {"project_list",
     "List known project slugs.",
     "{\"type\":\"object\",\"properties\":{}}",
     tool_project_list},
    {"source_append",
     "Append one immutable scoped source-memory event.",
     "{\"type\":\"object\",\"properties\":{"
     "\"scope\":{\"type\":\"string\"},"
     "\"kind\":{\"type\":\"string\",\"enum\":[\"user\",\"assistant\","
     "\"decision\",\"tool_call\",\"tool_result\",\"diagnostic\","
     "\"checkpoint\",\"artifact\"]},"
     "\"text\":{\"type\":\"string\",\"description\":\"Turn text\"}},"
     "\"required\":[\"scope\",\"kind\",\"text\"]}",
     tool_source_append},
    {"context_materialize",
     "Materialize semantic and exact scoped context under token budgets.",
     "{\"type\":\"object\",\"properties\":{"
     "\"scope\":{\"type\":\"string\"},"
     "\"query\":{\"type\":\"string\"},"
     "\"base_system_prompt\":{\"type\":\"string\",\"description\":\"Host "
     "system prompt to prepend (default empty)\"},"
     "\"history_tokens\":{\"type\":\"integer\",\"minimum\":0},"
     "\"checkpoint_tokens\":{\"type\":\"integer\",\"minimum\":0}},"
     "\"required\":[\"scope\",\"query\",\"history_tokens\","
     "\"checkpoint_tokens\"]}",
     tool_context_materialize},
    {"memory_stats",
     "Store and curation counters.",
     "{\"type\":\"object\",\"properties\":{}}",
     tool_memory_stats},
};

#define TOOLS_N (sizeof TOOLS / sizeof TOOLS[0])

/* ═══════════════════════ method handlers ═══════════════════════ */

static jx_value *initialize_result(void) {
  jx_value *res = jx_object();
  jx_value *caps, *si;
  int ok = (res != NULL);
  ok &= jx_object_set(res, "protocolVersion", jx_string(MCP_LEGACY_VERSION)) == 0;
  caps = jx_object();
  ok &= jx_object_set(caps, "tools", jx_object()) == 0;
  ok &= jx_object_set(res, "capabilities", caps) == 0;
  si = jx_object();
  ok &= jx_object_set(si, "name", jx_string("asper-mcp")) == 0;
  ok &= jx_object_set(si, "version", jx_string(asper_version())) == 0;
  ok &= jx_object_set(res, "serverInfo", si) == 0;
  if (!ok) {
    jx_free(res);
    return NULL;
  }
  return res;
}

static jx_value *discover_result(void) {
  jx_value *res = jx_object();
  jx_value *versions = jx_array();
  jx_value *caps = jx_object();
  int ok = (res != NULL && versions != NULL && caps != NULL);
  ok &= jx_array_push(versions, jx_string(MCP_MODERN_VERSION)) == 0;
  ok &= jx_array_push(versions, jx_string(MCP_LEGACY_VERSION)) == 0;
  ok &= jx_object_set(caps, "tools", jx_object()) == 0;
  ok &= jx_object_set(res, "supportedVersions", versions) == 0;
  ok &= jx_object_set(res, "capabilities", caps) == 0;
  ok &= jx_object_set(res, "instructions",
                      jx_string("Persistent memory tools for Asper.")) == 0;
  ok &= jx_object_set(res, "ttlMs", jx_int(3600000)) == 0;
  ok &= jx_object_set(res, "cacheScope", jx_string("private")) == 0;
  if (!ok) {
    jx_free(res);
    return NULL;
  }
  return res;
}

static void handle_tools_list(int want, jx_value *rid, int modern) {
  jx_value *arr = jx_array();
  jx_value *res;
  int ok = (arr != NULL);
  size_t i;
  for (i = 0; ok && i < TOOLS_N; i++) {
    jx_value *sch = NULL, *t;
    int tok;
    if (jx_parse(TOOLS[i].schema, strlen(TOOLS[i].schema), &sch) != 0) {
      ok = 0;
      break;
    }
    t = jx_object();
    tok = (t != NULL);
    tok &= jx_object_set(t, "name", jx_string(TOOLS[i].name)) == 0;
    tok &= jx_object_set(t, "description", jx_string(TOOLS[i].desc)) == 0;
    tok &= jx_object_set(t, "inputSchema", sch) == 0;
    if (!tok) {
      jx_free(t);
      ok = 0;
      break;
    }
    if (jx_array_push(arr, t) != 0) {
      ok = 0;
      break;
    }
  }
  if (!ok) {
    jx_free(arr);
    send_error(want, rid, -32603, "out of memory", NULL);
    return;
  }
  res = jx_object();
  if (jx_object_set(res, "tools", arr) != 0) {
    jx_free(res);
    send_error(want, rid, -32603, "out of memory", NULL);
    return;
  }
  send_result(want, rid, res, modern);
}

static void handle_tools_call(asper_ctx *c, int want, jx_value *rid,
                              const jx_value *params, int modern) {
  const char *name = NULL;
  const jx_value *args;
  const tool_def *tool = NULL;
  jx_value *payload = NULL;
  asper_err aerr = ASPER_OK;
  const char *pmsg = NULL;
  size_t i;
  int rc;

  if (!params || jx_typeof(params) != JX_OBJECT) {
    send_error(want, rid, -32602, "params must be an object", NULL);
    return;
  }
  if (arg_str(params, "name", &name, &pmsg) != 1) {
    send_error(want, rid, -32602,
               pmsg ? pmsg : "params.name must be a string", NULL);
    return;
  }
  args = jx_object_get(params, "arguments");
  if (args && jx_typeof(args) != JX_OBJECT) {
    send_error(want, rid, -32602, "params.arguments must be an object", NULL);
    return;
  }
  for (i = 0; i < TOOLS_N; i++) {
    if (strcmp(TOOLS[i].name, name) == 0) {
      tool = &TOOLS[i];
      break;
    }
  }
  if (!tool) {
    char buf[128];
    snprintf(buf, sizeof buf, "unknown tool: %.80s", name);
    send_error(want, rid, -32602, buf, NULL);
    return;
  }

  rc = tool->fn(c, args, &payload, &aerr, &pmsg);
  switch (rc) {
  case TOOL_OK:
    send_tool_result(want, rid, payload, 0, modern);
    break;
  case TOOL_ASPER: {
    jx_value *ep = asper_error_payload(c, aerr);
    if (!ep) {
      send_error(want, rid, -32603, "out of memory",
                 asper_err_name(aerr));
      break;
    }
    send_tool_result(want, rid, ep, 1, modern);
    break;
  }
  case TOOL_PARAM:
    send_error(want, rid, -32602, pmsg ? pmsg : "invalid params", NULL);
    break;
  default:
    send_error(want, rid, -32603, "out of memory", NULL);
    break;
  }
}

static void handle_request(asper_ctx *c, jx_value *req) {
  const jx_value *idv, *ver, *methv, *protocolv = NULL;
  const char *method;
  jx_value *rid;
  int has_id;
  int modern = 0;

  if (jx_typeof(req) == JX_ARRAY) {
    send_error(1, NULL, -32600, "batch requests are not supported", NULL);
    return;
  }
  if (jx_typeof(req) != JX_OBJECT) {
    send_error(1, NULL, -32600, "request must be a JSON object", NULL);
    return;
  }
  idv = jx_object_get(req, "id");
  has_id = (idv != NULL);
  ver = jx_object_get(req, "jsonrpc");
  methv = jx_object_get(req, "method");
  if (has_id && jx_typeof(idv) != JX_STRING &&
      !(jx_typeof(idv) == JX_NUMBER && jx_is_int(idv))) {
    send_error(1, NULL, -32600, "invalid JSON-RPC request id", NULL);
    return;
  }
  if (!ver || jx_typeof(ver) != JX_STRING || jx_string_length(ver) != 3 ||
      memcmp(jx_string_value(ver), "2.0", 3) != 0 || !methv ||
      jx_typeof(methv) != JX_STRING ||
      jx_string_length(methv) != strlen(jx_string_value(methv))) {
    send_error(has_id, jx_clone(idv), -32600,
               "invalid JSON-RPC 2.0 request", NULL);
    return;
  }
  method = jx_string_value(methv);
  {
    const jx_value *params = jx_object_get(req, "params");
    const jx_value *meta = params && jx_typeof(params) == JX_OBJECT
                               ? jx_object_get(params, "_meta")
                               : NULL;
    protocolv = meta && jx_typeof(meta) == JX_OBJECT
                    ? jx_object_get(meta,
                        "io.modelcontextprotocol/protocolVersion")
                    : NULL;
    modern = protocolv && jx_typeof(protocolv) == JX_STRING &&
             jx_string_length(protocolv) == 10 &&
              memcmp(jx_string_value(protocolv), MCP_MODERN_VERSION, 10) == 0;
  }
  /* "notifications/..." methods are ignored only as true notifications
   * (no id); an id-carrying request must get a response, so it falls
   * through to the normal dispatch (=> -32601 when unknown). */
  if (!has_id && strncmp(method, "notifications/", 14) == 0) return;

  rid = jx_clone(idv); /* NULL (=> null id) when absent or on OOM */
  if (has_id && !rid) {
    send_error(1, NULL, -32603, "out of memory", NULL);
    return;
  }
  if (protocolv && !modern && strcmp(method, "server/discover") != 0) {
    send_error(has_id, rid, -32022, "unsupported MCP protocol version", NULL);
    return;
  }
  if (strcmp(method, "initialize") == 0) {
    const jx_value *params = jx_object_get(req, "params");
    const jx_value *pv = params && jx_typeof(params) == JX_OBJECT
                             ? jx_object_get(params, "protocolVersion")
                             : NULL;
    if (pv && (jx_typeof(pv) != JX_STRING ||
               strcmp(jx_string_value(pv), MCP_LEGACY_VERSION) != 0)) {
      send_error(has_id, rid, -32022, "unsupported MCP protocol version", NULL);
      return;
    }
    jx_value *res = initialize_result();
    if (!res) send_error(has_id, rid, -32603, "out of memory", NULL);
    else send_result(has_id, rid, res, 0);
    return;
  }
  if (strcmp(method, "server/discover") == 0) {
    jx_value *res = discover_result();
    if (!res) send_error(has_id, rid, -32603, "out of memory", NULL);
    else send_result(has_id, rid, res, 1);
    return;
  }
  if (strcmp(method, "ping") == 0) {
    send_result(has_id, rid, jx_object(), modern);
    return;
  }
  if (strcmp(method, "tools/list") == 0) {
    handle_tools_list(has_id, rid, modern);
    return;
  }
  if (strcmp(method, "tools/call") == 0) {
    handle_tools_call(c, has_id, rid, jx_object_get(req, "params"), modern);
    return;
  }
  send_error(has_id, rid, -32601, "method not found", NULL);
}

/* ═══════════════════════ stdin line reader ═══════════════════════ */

/* 1 = line read (*out malloc'd, trailing \n/\r stripped), 0 = EOF with no
 * data, -1 = allocation failure (rest of the line drained). */
static int read_line(FILE *f, char **out, size_t *out_len) {
  size_t cap = 256, n = 0;
  char *buf = malloc(cap);
  int ch;
  if (!buf) {
    while ((ch = getc(f)) != EOF && ch != '\n') { /* drain */ }
    return -1;
  }
  while ((ch = getc(f)) != EOF && ch != '\n') {
    if (n + 2 > cap) {
      char *nb;
      if (cap > (size_t)-1 / 2) {
        free(buf);
        while ((ch = getc(f)) != EOF && ch != '\n') { /* drain */ }
        return -1;
      }
      cap *= 2;
      nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        while ((ch = getc(f)) != EOF && ch != '\n') { /* drain */ }
        return -1;
      }
      buf = nb;
    }
    buf[n++] = (char)ch;
  }
  if (ch == EOF && n == 0) {
    free(buf);
    return 0;
  }
  while (n > 0 && buf[n - 1] == '\r') n--;
  buf[n] = '\0';
  *out = buf;
  *out_len = n;
  return 1;
}

static int line_blank(const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (s[i] != ' ' && s[i] != '\t') return 0;
  }
  return 1;
}

/* ═══════════════════════ main ═══════════════════════ */

int main(int argc, char **argv) {
  const char *root = NULL, *config = NULL;
  asper_open_params params;
  asper_ctx *ctx = NULL;
  asper_err e;
  int i;

  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--help") == 0) {
      fputs(HELP, stdout);
      return 0;
    }
    if (strcmp(a, "--version") == 0) {
      printf("asper-mcp %s\n", asper_version());
      return 0;
    }
    if (strcmp(a, "--root") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "asper-mcp: --root requires a value\n%s", USAGE);
        return 2;
      }
      root = argv[i];
    } else if (strncmp(a, "--root=", 7) == 0) {
      root = a + 7;
    } else if (strcmp(a, "--config") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "asper-mcp: --config requires a value\n%s", USAGE);
        return 2;
      }
      config = argv[i];
    } else if (strncmp(a, "--config=", 9) == 0) {
      config = a + 9;
    } else {
      fprintf(stderr, "asper-mcp: unknown argument \"%s\"\n%s", a, USAGE);
      return 2;
    }
  }
  if (!root || !*root) {
    fprintf(stderr, "asper-mcp: --root is required\n%s", USAGE);
    return 2;
  }

  memset(&params, 0, sizeof params);
  params.memory_root = root;
  params.config_path = config;
  e = asper_open(&params, &ctx);
  if (e != ASPER_OK || !ctx) {
    fprintf(stderr, "asper-mcp: cannot open memory store at \"%s\": %s\n",
            root, asper_err_name(e));
    return 1;
  }
  /* stdio belongs to the protocol: silence the default stderr log sink. */
  asper_set_logger(ctx, NULL, NULL);

  for (;;) {
    char *line = NULL;
    size_t llen = 0;
    int st = read_line(stdin, &line, &llen);
    if (st == 0) break;
    if (st < 0) {
      emit_line(OOM_RESPONSE);
      continue;
    }
    if (line_blank(line, llen)) {
      free(line);
      continue;
    }
    {
      jx_value *req = NULL;
      if (jx_parse(line, llen, &req) != 0) {
        send_error(1, NULL, -32700, "parse error", NULL);
      } else {
        handle_request(ctx, req);
        jx_free(req);
      }
    }
    free(line);
  }

  asper_close(ctx);
  return 0;
}
