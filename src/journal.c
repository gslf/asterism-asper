/*
 * journal.c — xCDN mapping of records and ops (serialize + parse), the
 * append-only journal (append / replay / torn-tail repair / fsync), and
 * the audit mirror.
 *
 * Journal format (IMPLEMENTATION_NOTES.md, binding): one COMPACT #op value
 * per line; section files are written PRETTY, one #memory value per record
 * separated by a blank line (store.c drives that through
 * asper_record_serialize).
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"
#include "xcdn.h"

/* ═══════════════════════ ops ═══════════════════════ */

void asper_op_free(asper_op *op) {
  if (!op) return;
  asper_record_free_one(op->record);
  op->record = NULL;
  free(op->content);
  op->content = NULL;
  free(op->reason);
  op->reason = NULL;
  free(op->ids);
  op->ids = NULL;
  op->ids_n = 0;
  free(op->project);
  op->project = NULL;
}

const char *asper_op_kind_name(asper_op_kind k) {
  switch (k) {
    case ASPER_OP_INSERT:         return "insert";
    case ASPER_OP_UPDATE:         return "update";
    case ASPER_OP_DEPRECATE:      return "deprecate";
    case ASPER_OP_KEEP:           return "keep";
    case ASPER_OP_SET_LOCKED:     return "set_locked";
    case ASPER_OP_ACCESS:         return "access";
    case ASPER_OP_PROJECT_CREATE: return "project_create";
  }
  return "?";
}

static bool op_kind_parse(const char *s, asper_op_kind *out) {
  int k;
  if (!s) return false;
  for (k = ASPER_OP_INSERT; k <= ASPER_OP_PROJECT_CREATE; k++) {
    if (strcmp(s, asper_op_kind_name((asper_op_kind)k)) == 0) {
      *out = (asper_op_kind)k;
      return true;
    }
  }
  return false;
}

/* ═══════════════════════ AST building (OOM-safe wrappers) ═══════════════
 * xcdn-c's void mutators do not report allocation failure, so growth and
 * key duplication are done here over the public AST structs; every helper
 * consumes its value/node argument, freeing it on failure. */

static bool xobj_put_node(xcdn_value_t *obj, const char *key,
                          xcdn_node_t *node) {
  char *k;
  if (!obj || !node) {
    xcdn_node_free(node);
    return false;
  }
  if (obj->data.object.len >= obj->data.object.cap) {
    size_t ncap = obj->data.object.cap ? obj->data.object.cap * 2 : 8;
    xcdn_object_entry_t *ne = (xcdn_object_entry_t *)realloc(
        obj->data.object.entries, ncap * sizeof(*ne));
    if (!ne) {
      xcdn_node_free(node);
      return false;
    }
    obj->data.object.entries = ne;
    obj->data.object.cap = ncap;
  }
  k = asper_strdup(key);
  if (!k) {
    xcdn_node_free(node);
    return false;
  }
  obj->data.object.entries[obj->data.object.len].key = k;
  obj->data.object.entries[obj->data.object.len].node = node;
  obj->data.object.len++;
  return true;
}

static bool xobj_put(xcdn_value_t *obj, const char *key, xcdn_value_t *v) {
  xcdn_node_t *node;
  if (!v) return false;
  node = xcdn_node_new(v);
  if (!node) {
    xcdn_value_free(v);
    return false;
  }
  return xobj_put_node(obj, key, node);
}

static bool xarr_push(xcdn_value_t *arr, xcdn_value_t *v) {
  xcdn_node_t *node;
  if (!arr || !v) {
    if (v) xcdn_value_free(v);
    return false;
  }
  node = xcdn_node_new(v);
  if (!node) {
    xcdn_value_free(v);
    return false;
  }
  if (arr->data.array.len >= arr->data.array.cap) {
    size_t ncap = arr->data.array.cap ? arr->data.array.cap * 2 : 8;
    xcdn_node_t **ni =
        (xcdn_node_t **)realloc(arr->data.array.items, ncap * sizeof(*ni));
    if (!ni) {
      xcdn_node_free(node);
      return false;
    }
    arr->data.array.items = ni;
    arr->data.array.cap = ncap;
  }
  arr->data.array.items[arr->data.array.len++] = node;
  return true;
}

static bool xnode_tag(xcdn_node_t *node, const char *name) {
  char *n;
  if (node->tags_len >= node->tags_cap) {
    size_t ncap = node->tags_cap ? node->tags_cap * 2 : 2;
    xcdn_tag_t *nt = (xcdn_tag_t *)realloc(node->tags, ncap * sizeof(*nt));
    if (!nt) return false;
    node->tags = nt;
    node->tags_cap = ncap;
  }
  n = asper_strdup(name);
  if (!n) return false;
  node->tags[node->tags_len].name = n;
  node->tags_len++;
  return true;
}

static bool xdoc_push(xcdn_document_t *doc, xcdn_node_t *node) {
  if (doc->values_len >= doc->values_cap) {
    size_t ncap = doc->values_cap ? doc->values_cap * 2 : 2;
    xcdn_node_t **nv =
        (xcdn_node_t **)realloc(doc->values, ncap * sizeof(*nv));
    if (!nv) return false;
    doc->values = nv;
    doc->values_cap = ncap;
  }
  doc->values[doc->values_len++] = node;
  return true;
}

static xcdn_value_t *dt_value(asper_time t) {
  char buf[32];
  asper_time_format_rfc3339(t, buf);
  return xcdn_value_datetime(buf);
}

/* ═══════════════════════ string escape shim ═══════════════════════
 * xcdn-c's lexer decodes only \" and \\ and leaves \n \r \t \b \f \/ and
 * \uXXXX escapes RAW in parsed strings, while its serializer escapes real
 * control characters — so strings holding newlines would not round-trip
 * through the dependency. The shim finishes the job locally: esc_decode
 * resolves the escapes the lexer left raw; esc_encode pre-escapes strings
 * that contain a backslash (\ LF CR TAB) so decode is their exact inverse.
 * Backslash-free strings pass through untouched and stay pretty on disk. */

static char *esc_encode(const char *s) {
  size_t n, extra = 0;
  char *out, *w;
  if (!s) s = "";
  if (strchr(s, '\\') == NULL) return asper_strdup(s);
  n = strlen(s);
  for (size_t i = 0; i < n; i++)
    if (s[i] == '\\' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')
      extra++;
  out = (char *)malloc(n + extra + 1);
  if (!out) return NULL;
  w = out;
  for (size_t i = 0; i < n; i++) {
    switch (s[i]) {
      case '\\': *w++ = '\\'; *w++ = '\\'; break;
      case '\n': *w++ = '\\'; *w++ = 'n'; break;
      case '\r': *w++ = '\\'; *w++ = 'r'; break;
      case '\t': *w++ = '\\'; *w++ = 't'; break;
      default:   *w++ = s[i]; break;
    }
  }
  *w = '\0';
  return out;
}

static int hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    unsigned d;
    if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
    else return 0;
    v = v * 16 + d;
  }
  *out = v;
  return 1;
}

static size_t utf8_put(char *w, uint32_t cp) {
  if (cp < 0x80) {
    w[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    w[0] = (char)(0xC0 | (cp >> 6));
    w[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    w[0] = (char)(0xE0 | (cp >> 12));
    w[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    w[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  w[0] = (char)(0xF0 | (cp >> 18));
  w[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  w[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  w[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static char *esc_decode(const char *s) {
  size_t n, i = 0;
  char *out, *w;
  if (!s) s = "";
  n = strlen(s);
  out = (char *)malloc(n + 1); /* decoding never grows the string */
  if (!out) return NULL;
  w = out;
  while (i < n) {
    char c = s[i];
    if (c != '\\' || i + 1 >= n) {
      *w++ = c;
      i++;
      continue;
    }
    switch (s[i + 1]) {
      case '\\': *w++ = '\\'; i += 2; break;
      case '"':  *w++ = '"';  i += 2; break;
      case '/':  *w++ = '/';  i += 2; break;
      case 'n':  *w++ = '\n'; i += 2; break;
      case 'r':  *w++ = '\r'; i += 2; break;
      case 't':  *w++ = '\t'; i += 2; break;
      case 'b':  *w++ = '\b'; i += 2; break;
      case 'f':  *w++ = '\f'; i += 2; break;
      case 'u': {
        unsigned cp;
        if (i + 6 <= n && hex4(s + i + 2, &cp)) {
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 12 <= n &&
              s[i + 6] == '\\' && s[i + 7] == 'u') {
            unsigned lo;
            if (hex4(s + i + 8, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              uint32_t full =
                  0x10000u + (((uint32_t)cp - 0xD800u) << 10) + (lo - 0xDC00u);
              w += utf8_put(w, full);
              i += 12;
              break;
            }
          }
          w += utf8_put(w, cp);
          i += 6;
        } else {
          *w++ = s[i]; /* malformed: keep raw */
          i++;
        }
        break;
      }
      default:
        *w++ = s[i]; /* unknown escape: keep raw */
        i++;
        break;
    }
  }
  *w = '\0';
  return out;
}

/* Decode escapes only when the string cannot have come from a RAW
 * (triple-quoted) xCDN literal: our own serializer always escapes control
 * characters, so a parsed string containing a raw byte < 0x20 (newline,
 * tab, ...) is hand-written triple-quoted content and must be preserved
 * verbatim (Post-review amendments). Known limitation: SINGLE-line
 * triple-quoted hand-edited content containing literal \x sequences is
 * still interpreted as escapes — the proper fix is an is_raw flag in
 * xCDN-C's AST (upstream). */
static char *esc_decode_checked(const char *s) {
  if (s) {
    const unsigned char *p;
    for (p = (const unsigned char *)s; *p; p++)
      if (*p < 0x20) return asper_strdup(s);
  }
  return esc_decode(s);
}

/* String value with the encode shim applied (owned copy). */
static xcdn_value_t *xstr_esc(const char *s) {
  char *enc = esc_encode(s);
  xcdn_value_t *v;
  if (!enc) return NULL;
  v = xcdn_value_string_owned(enc);
  if (!v) free(enc);
  return v;
}

/* ═══════════════════════ record -> node ═══════════════════════ */

/* Build a #memory-tagged node with every field in order (project and
 * supersedes serialize as null when absent). deprecated_at is emitted after
 * status for deprecated records only: it is required to preserve the purge
 * clock across compaction and reload; active records keep the exact
 * shape. */
static xcdn_node_t *record_build_node(const asper_record *r) {
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_node_t *node;
  bool ok = obj != NULL;

  if (ok) ok = xobj_put(obj, "id", xcdn_value_uuid(r->id));
  if (ok)
    ok = xobj_put(obj, "section",
                  xcdn_value_string(asper_section_name(r->section)));
  if (ok)
    ok = xobj_put(obj, "project",
                  r->project ? xcdn_value_string(r->project)
                             : xcdn_value_null());
  if (ok) ok = xobj_put(obj, "content", xstr_esc(r->content));
  if (ok)
    ok = xobj_put(obj, "source",
                  xcdn_value_string(asper_source_name(r->source)));
  if (ok) ok = xobj_put(obj, "created_at", dt_value(r->created_at));
  if (ok) ok = xobj_put(obj, "updated_at", dt_value(r->updated_at));
  if (ok) ok = xobj_put(obj, "last_access", dt_value(r->last_access));
  if (ok)
    ok = xobj_put(obj, "access_count",
                  xcdn_value_int((int64_t)r->access_count));
  if (ok) ok = xobj_put(obj, "relevance", xcdn_value_float(r->relevance));
  if (ok) ok = xobj_put(obj, "locked", xcdn_value_bool(r->locked));
  if (ok)
    ok = xobj_put(obj, "status",
                  xcdn_value_string(r->deprecated ? "deprecated" : "active"));
  if (ok && r->deprecated)
    ok = xobj_put(obj, "deprecated_at", dt_value(r->deprecated_at));
  if (ok)
    ok = xobj_put(obj, "supersedes",
                  r->supersedes[0] ? xcdn_value_uuid(r->supersedes)
                                   : xcdn_value_null());
  if (ok) {
    xcdn_value_t *tags = xcdn_value_array();
    if (!tags) {
      ok = false;
    } else {
      bool tok = true;
      for (size_t i = 0; tok && i < r->tags_n; i++)
        tok = xarr_push(tags, xstr_esc(r->tags[i]));
      if (!tok) {
        xcdn_value_free(tags);
        ok = false;
      } else {
        ok = xobj_put(obj, "tags", tags);
      }
    }
  }
  if (!ok) {
    if (obj) xcdn_value_free(obj);
    return NULL;
  }
  node = xcdn_node_new(obj);
  if (!node) {
    xcdn_value_free(obj);
    return NULL;
  }
  if (!xnode_tag(node, "memory")) {
    xcdn_node_free(node);
    return NULL;
  }
  return node;
}

/* Serialize a single-value document holding node; appends to out. */
static asper_err serialize_node(xcdn_node_t *node, bool pretty,
                                asper_buf *out) {
  xcdn_document_t *doc;
  char *s;
  asper_err e;
  doc = xcdn_document_new();
  if (!doc) {
    xcdn_node_free(node);
    return ASPER_ERR_NOMEM;
  }
  if (!xdoc_push(doc, node)) {
    xcdn_node_free(node);
    xcdn_document_free(doc);
    return ASPER_ERR_NOMEM;
  }
  s = pretty ? xcdn_to_string_pretty(doc) : xcdn_to_string_compact(doc);
  xcdn_document_free(doc);
  if (!s) return ASPER_ERR_NOMEM;
  e = asper_buf_appends(out, s);
  free(s);
  return e;
}

asper_err asper_record_serialize(const asper_record *r, asper_buf *out) {
  xcdn_node_t *node;
  if (!r || !out) return ASPER_ERR_INVALID;
  node = record_build_node(r);
  if (!node) return ASPER_ERR_NOMEM;
  return serialize_node(node, true, out);
}

/* ═══════════════════════ op -> node ═══════════════════════ */

asper_err asper_op_serialize(const asper_op *op, asper_buf *out) {
  xcdn_value_t *obj;
  xcdn_node_t *node;
  bool ok;

  if (!op || !out) return ASPER_ERR_INVALID;
  obj = xcdn_value_object();
  ok = obj != NULL;
  if (ok)
    ok = xobj_put(obj, "kind",
                  xcdn_value_string(asper_op_kind_name(op->kind)));
  if (ok) ok = xobj_put(obj, "at", dt_value(op->at));

  switch (op->kind) {
    case ASPER_OP_INSERT:
      if (ok) {
        xcdn_node_t *rn = op->record ? record_build_node(op->record) : NULL;
        ok = rn != NULL && xobj_put_node(obj, "record", rn);
      }
      break;
    case ASPER_OP_UPDATE:
      if (ok) ok = xobj_put(obj, "id", xcdn_value_uuid(op->id));
      if (ok) ok = xobj_put(obj, "content", xstr_esc(op->content));
      break;
    case ASPER_OP_DEPRECATE:
      if (ok) ok = xobj_put(obj, "id", xcdn_value_uuid(op->id));
      if (ok && op->reason)
        ok = xobj_put(obj, "reason", xstr_esc(op->reason));
      break;
    case ASPER_OP_KEEP:
      if (ok) ok = xobj_put(obj, "id", xcdn_value_uuid(op->id));
      break;
    case ASPER_OP_SET_LOCKED:
      if (ok) ok = xobj_put(obj, "id", xcdn_value_uuid(op->id));
      if (ok) ok = xobj_put(obj, "value", xcdn_value_bool(op->value));
      break;
    case ASPER_OP_ACCESS:
      if (ok) {
        xcdn_value_t *ids = xcdn_value_array();
        bool tok = ids != NULL;
        for (size_t i = 0; tok && i < op->ids_n; i++)
          tok = xarr_push(ids, xcdn_value_uuid(op->ids[i]));
        if (!tok) {
          if (ids) xcdn_value_free(ids);
          ok = false;
        } else {
          ok = xobj_put(obj, "ids", ids);
        }
      }
      break;
    case ASPER_OP_PROJECT_CREATE:
      if (ok)
        ok = xobj_put(obj, "project",
                      xcdn_value_string(op->project ? op->project : ""));
      break;
  }

  if (!ok) {
    if (obj) xcdn_value_free(obj);
    return ASPER_ERR_NOMEM;
  }
  node = xcdn_node_new(obj);
  if (!node) {
    xcdn_value_free(obj);
    return ASPER_ERR_NOMEM;
  }
  if (!xnode_tag(node, "op")) {
    xcdn_node_free(node);
    return ASPER_ERR_NOMEM;
  }
  return serialize_node(node, false, out);
}

/* ═══════════════════════ node -> record ═══════════════════════ */

static const xcdn_value_t *obj_field(const xcdn_value_t *obj,
                                     const char *key) {
  xcdn_node_t *n = xcdn_object_get(obj, key);
  return n ? n->value : NULL;
}

static const char *val_str(const xcdn_value_t *v) {
  return (v && v->type == XCDN_VAL_STRING) ? v->data.string : NULL;
}

static bool val_uuid37(const xcdn_value_t *v, char out[37]) {
  const char *s;
  if (!v || v->type != XCDN_VAL_UUID) return false;
  s = v->data.string;
  if (!s || strlen(s) != 36 || !asper_uuid_valid(s)) return false;
  for (int i = 0; i < 36; i++) out[i] = (char)tolower((unsigned char)s[i]);
  out[36] = '\0';
  return true;
}

static bool val_time(const xcdn_value_t *v, asper_time *out) {
  if (!v || v->type != XCDN_VAL_DATETIME || !v->data.string) return false;
  return asper_time_parse_rfc3339(v->data.string, out);
}

/* Format the message BEFORE freeing r: the arguments may reference r->id. */
#define REC_FAIL(...)                                              \
  do {                                                             \
    asper_err fail_e_ = asper_seterr(c, ASPER_ERR_PARSE, __VA_ARGS__); \
    asper_record_free_one(r);                                      \
    return fail_e_;                                                \
  } while (0)

#define REC_OOM()                                                          \
  do {                                                                     \
    asper_err fail_e_ =                                                    \
        asper_seterr(c, ASPER_ERR_NOMEM, "record: out of memory");         \
    asper_record_free_one(r);                                              \
    return fail_e_;                                                        \
  } while (0)

asper_err asper_record_from_node(asper_ctx *c, const void *xcdn_node,
                                 asper_record **out) {
  const xcdn_node_t *node = (const xcdn_node_t *)xcdn_node;
  const xcdn_value_t *obj, *v;
  const char *s;
  asper_record *r;
  asper_time now;

  if (!out) return asper_seterr(c, ASPER_ERR_INVALID, "record: null out");
  *out = NULL;
  if (!node || !node->value || node->value->type != XCDN_VAL_OBJECT)
    return asper_seterr(c, ASPER_ERR_PARSE, "record: value is not an object");
  obj = node->value;

  r = asper_record_new();
  if (!r) return asper_seterr(c, ASPER_ERR_NOMEM, "record: out of memory");

  /* required: id, section, content */
  if (!val_uuid37(obj_field(obj, "id"), r->id))
    REC_FAIL("record: missing or invalid id");
  s = val_str(obj_field(obj, "section"));
  if (!s || !asper_section_parse(s, &r->section))
    REC_FAIL("record %s: missing or invalid section", r->id);

  v = obj_field(obj, "project");
  if (v && v->type == XCDN_VAL_STRING) {
    if (!v->data.string || !asper_slug_valid(v->data.string))
      REC_FAIL("record %s: invalid project slug", r->id);
    r->project = asper_strdup(v->data.string);
    if (!r->project) REC_OOM();
  } else if (v && v->type != XCDN_VAL_NULL) {
    REC_FAIL("record %s: project must be a string or null", r->id);
  }
  if ((r->section == ASPER_SECTION_PROJECT) != (r->project != NULL))
    REC_FAIL("record %s: project required iff section is \"project\"", r->id);

  s = val_str(obj_field(obj, "content"));
  if (!s) REC_FAIL("record %s: missing content", r->id);
  r->content = esc_decode_checked(s);
  if (!r->content) REC_OOM();
  if (asper_str_blank(r->content))
    REC_FAIL("record %s: empty content", r->id);

  /* optional fields with their defaults */
  v = obj_field(obj, "source");
  if (v) {
    s = val_str(v);
    if (!s || !asper_source_parse(s, &r->source))
      REC_FAIL("record %s: invalid source", r->id);
  } else {
    r->source = ASPER_SRC_MANUAL; /* hand-added records are user records */
  }

  now = asper_clock_now(&c->clock);
  v = obj_field(obj, "created_at");
  if (v) {
    if (!val_time(v, &r->created_at))
      REC_FAIL("record %s: invalid created_at", r->id);
  } else {
    r->created_at = now;
  }
  v = obj_field(obj, "updated_at");
  if (v) {
    if (!val_time(v, &r->updated_at))
      REC_FAIL("record %s: invalid updated_at", r->id);
  } else {
    r->updated_at = r->created_at;
  }
  v = obj_field(obj, "last_access");
  if (v) {
    if (!val_time(v, &r->last_access))
      REC_FAIL("record %s: invalid last_access", r->id);
  } else {
    r->last_access = r->created_at;
  }

  v = obj_field(obj, "access_count");
  if (v) {
    if (v->type != XCDN_VAL_INT)
      REC_FAIL("record %s: access_count must be an integer", r->id);
    if (v->data.integer < 0)
      r->access_count = 0;
    else if (v->data.integer > (int64_t)UINT32_MAX)
      r->access_count = UINT32_MAX;
    else
      r->access_count = (uint32_t)v->data.integer;
  }

  v = obj_field(obj, "relevance");
  if (v) {
    double rel;
    if (v->type == XCDN_VAL_FLOAT)
      rel = v->data.floating;
    else if (v->type == XCDN_VAL_INT)
      rel = (double)v->data.integer;
    else
      REC_FAIL("record %s: relevance must be a number", r->id);
    r->relevance = rel < 0.0 ? 0.0 : rel > 1.0 ? 1.0 : rel;
  } else {
    r->relevance = r->source == ASPER_SRC_SEED
                       ? 1.00
                       : r->source == ASPER_SRC_MANUAL ? 0.80 : 0.60;
  }

  v = obj_field(obj, "locked");
  if (v) {
    if (v->type != XCDN_VAL_BOOL)
      REC_FAIL("record %s: locked must be a boolean", r->id);
    r->locked = v->data.boolean;
  }

  v = obj_field(obj, "status");
  if (v) {
    s = val_str(v);
    if (s && strcmp(s, "active") == 0)
      r->deprecated = false;
    else if (s && strcmp(s, "deprecated") == 0)
      r->deprecated = true;
    else
      REC_FAIL("record %s: status must be \"active\" or \"deprecated\"",
               r->id);
  }

  v = obj_field(obj, "supersedes");
  if (v && v->type != XCDN_VAL_NULL) {
    if (!val_uuid37(v, r->supersedes))
      REC_FAIL("record %s: supersedes must be a UUID or null", r->id);
  }

  v = obj_field(obj, "deprecated_at");
  if (v && !val_time(v, &r->deprecated_at))
    REC_FAIL("record %s: invalid deprecated_at", r->id);
  if (r->deprecated) {
    if (r->deprecated_at == 0) r->deprecated_at = r->updated_at;
  } else {
    r->deprecated_at = 0;
  }

  v = obj_field(obj, "tags");
  if (v && v->type != XCDN_VAL_NULL) {
    if (v->type != XCDN_VAL_ARRAY)
      REC_FAIL("record %s: tags must be an array", r->id);
    if (v->data.array.len > 0) {
      r->tags = (char **)calloc(v->data.array.len, sizeof(char *));
      if (!r->tags) REC_OOM();
      for (size_t i = 0; i < v->data.array.len; i++) {
        const xcdn_node_t *tn = v->data.array.items[i];
        const char *ts = tn ? val_str(tn->value) : NULL;
        if (!ts) REC_FAIL("record %s: tags must contain strings", r->id);
        r->tags[i] = esc_decode_checked(ts);
        if (!r->tags[i]) REC_OOM();
        r->tags_n = i + 1;
      }
    }
  }

  *out = r;
  return ASPER_OK;
}

/* ═══════════════════════ node -> op ═══════════════════════ */

/* Format the message BEFORE clearing out: arguments may reference it. */
#define OP_FAIL(...)                                               \
  do {                                                             \
    asper_err fail_e_ = asper_seterr(c, ASPER_ERR_PARSE, __VA_ARGS__); \
    asper_op_free(out);                                            \
    memset(out, 0, sizeof(*out));                                  \
    return fail_e_;                                                \
  } while (0)

#define OP_OOM()                                                       \
  do {                                                                 \
    asper_err fail_e_ =                                                \
        asper_seterr(c, ASPER_ERR_NOMEM, "op: out of memory");         \
    asper_op_free(out);                                                \
    memset(out, 0, sizeof(*out));                                      \
    return fail_e_;                                                    \
  } while (0)

asper_err asper_op_from_node(asper_ctx *c, const void *xcdn_node,
                             asper_op *out) {
  const xcdn_node_t *node = (const xcdn_node_t *)xcdn_node;
  const xcdn_value_t *obj, *v;
  const char *s;
  asper_err e;

  if (!out) return asper_seterr(c, ASPER_ERR_INVALID, "op: null out");
  memset(out, 0, sizeof(*out));
  if (!node || !node->value || node->value->type != XCDN_VAL_OBJECT)
    return asper_seterr(c, ASPER_ERR_PARSE, "op: value is not an object");
  obj = node->value;

  s = val_str(obj_field(obj, "kind"));
  if (!s || !op_kind_parse(s, &out->kind))
    OP_FAIL("op: missing or unknown kind");
  if (!val_time(obj_field(obj, "at"), &out->at))
    OP_FAIL("op %s: missing or invalid at", asper_op_kind_name(out->kind));

  switch (out->kind) {
    case ASPER_OP_INSERT: {
      xcdn_node_t *rn = xcdn_object_get(obj, "record");
      if (!rn) OP_FAIL("op insert: missing record");
      e = asper_record_from_node(c, rn, &out->record);
      if (e != ASPER_OK) {
        memset(out, 0, sizeof(*out));
        return e; /* message already set */
      }
      break;
    }
    case ASPER_OP_UPDATE:
      if (!val_uuid37(obj_field(obj, "id"), out->id))
        OP_FAIL("op update: missing or invalid id");
      s = val_str(obj_field(obj, "content"));
      if (!s) OP_FAIL("op update: missing content");
      out->content = esc_decode_checked(s);
      if (!out->content) OP_OOM();
      break;
    case ASPER_OP_DEPRECATE:
      if (!val_uuid37(obj_field(obj, "id"), out->id))
        OP_FAIL("op deprecate: missing or invalid id");
      v = obj_field(obj, "reason");
      if (v && v->type == XCDN_VAL_STRING) {
        out->reason = esc_decode_checked(v->data.string);
        if (!out->reason) OP_OOM();
      } else if (v && v->type != XCDN_VAL_NULL) {
        OP_FAIL("op deprecate: reason must be a string");
      }
      break;
    case ASPER_OP_KEEP:
      if (!val_uuid37(obj_field(obj, "id"), out->id))
        OP_FAIL("op keep: missing or invalid id");
      break;
    case ASPER_OP_SET_LOCKED:
      if (!val_uuid37(obj_field(obj, "id"), out->id))
        OP_FAIL("op set_locked: missing or invalid id");
      v = obj_field(obj, "value");
      if (!v || v->type != XCDN_VAL_BOOL)
        OP_FAIL("op set_locked: missing or invalid value");
      out->value = v->data.boolean;
      break;
    case ASPER_OP_ACCESS:
      v = obj_field(obj, "ids");
      if (!v || v->type != XCDN_VAL_ARRAY)
        OP_FAIL("op access: missing ids array");
      if (v->data.array.len > 0) {
        out->ids = (char (*)[37])malloc(v->data.array.len * sizeof(*out->ids));
        if (!out->ids) OP_OOM();
        out->ids_n = v->data.array.len;
        for (size_t i = 0; i < v->data.array.len; i++) {
          const xcdn_node_t *idn = v->data.array.items[i];
          if (!idn || !val_uuid37(idn->value, out->ids[i]))
            OP_FAIL("op access: ids must contain UUIDs");
        }
      }
      break;
    case ASPER_OP_PROJECT_CREATE:
      s = val_str(obj_field(obj, "project"));
      if (!s || !asper_slug_valid(s))
        OP_FAIL("op project_create: missing or invalid project slug");
      out->project = asper_strdup(s);
      if (!out->project) OP_OOM();
      break;
  }
  return ASPER_OK;
}

/* ═══════════════════════ replay ═══════════════════════ */

/* Torn-tail rule (IMPLEMENTATION_NOTES.md): on parse failure retry with the
 * buffer truncated after the last '\n' that is followed by a non-empty
 * remainder (drops exactly the final line); success => truncate the file
 * there and WARN. Anything else is a mid-file error: ASPER_ERR_PARSE, file
 * untouched. */
asper_err asper_journal_replay(asper_ctx *c, const char *path,
                               size_t *out_ops) {
  char *text = NULL;
  size_t len = 0;
  size_t count = 0;
  xcdn_error_t xe;
  xcdn_document_t *doc;
  asper_err e;

  if (out_ops) *out_ops = 0;
  if (!os_file_exists(path)) return ASPER_OK;
  e = os_read_file(path, &text, &len);
  if (e != ASPER_OK)
    return asper_seterr(c, e, "journal: cannot read %s", path);
  if (len == 0) {
    free(text);
    return ASPER_OK;
  }

  doc = xcdn_parse_str(text, len, &xe);
  if (!doc) {
    size_t off = 0;
    xcdn_error_t xe2;
    for (size_t i = len; i-- > 0;) {
      if (text[i] != '\n') continue;
      bool nonempty = false;
      for (size_t j = i + 1; j < len; j++) {
        char ch = text[j];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
          nonempty = true;
          break;
        }
      }
      if (nonempty) {
        off = i + 1;
        break;
      }
    }
    doc = off > 0 ? xcdn_parse_str(text, off, &xe2) : xcdn_document_new();
    if (!doc) {
      free(text);
      if (off == 0)
        return asper_seterr(c, ASPER_ERR_NOMEM, "journal: out of memory");
      return asper_seterr(c, ASPER_ERR_PARSE,
                          "journal: parse error before the tail: %s (line "
                          "%zu)",
                          xe.message, xe.span.line);
    }
    e = os_truncate(path, (uint64_t)off);
    if (e != ASPER_OK) {
      xcdn_document_free(doc);
      free(text);
      return asper_seterr(c, e, "journal: cannot truncate torn tail of %s",
                          path);
    }
    asper_log(c, ASPER_LOG_WARN, "store",
              "journal torn tail: 1 value discarded (%zu byte(s) dropped)",
              len - off);
  }
  free(text);

  for (size_t i = 0; i < doc->values_len; i++) {
    xcdn_node_t *n = doc->values[i];
    asper_op op;
    if (!xcdn_node_has_tag(n, "op")) {
      asper_log(c, ASPER_LOG_WARN, "store",
                "journal: value %zu is not tagged #op; skipped", i + 1);
      continue;
    }
    e = asper_op_from_node(c, n, &op);
    if (e == ASPER_ERR_NOMEM) {
      xcdn_document_free(doc);
      return e;
    }
    if (e != ASPER_OK) {
      asper_log(c, ASPER_LOG_WARN, "store",
                "journal: bad op %zu skipped: %s", i + 1,
                asper_last_error(c));
      continue;
    }
    count++; /* the op occupies the journal whether or not it applies */
    e = asper_store_apply(c, &op);
    if (e == ASPER_ERR_NOMEM) {
      asper_op_free(&op);
      xcdn_document_free(doc);
      return e;
    }
    if (e != ASPER_OK)
      asper_log(c, ASPER_LOG_WARN, "store",
                "journal: op %zu (%s) not applied: %s", i + 1,
                asper_op_kind_name(op.kind), asper_last_error(c));
    asper_op_free(&op);
  }
  xcdn_document_free(doc);

  c->store.journal_ops += count;
  if (out_ops) *out_ops = count;
  return ASPER_OK;
}

/* ═══════════════════════ append / sync ═══════════════════════ */

asper_err asper_journal_append(asper_ctx *c, const asper_op *op,
                               bool force_sync) {
  asper_buf line;
  asper_err e;
  long good_off;

  if (!c || !op) return ASPER_ERR_INVALID;
  asper_buf_init(&line);
  e = asper_op_serialize(op, &line);
  if (e == ASPER_OK) e = asper_buf_appendc(&line, '\n');
  if (e != ASPER_OK) {
    asper_buf_free(&line);
    return asper_seterr(c, e, "journal: op serialization failed");
  }

  os_mutex_lock(&c->journal_mu);
  if (!c->store.journal_fp) {
    os_mutex_unlock(&c->journal_mu);
    asper_buf_free(&line);
    return asper_seterr(c, ASPER_ERR_IO, "journal: stream not open");
  }
  /* Capture the end offset of the last fully-written line: every prior
   * append flushed, so file size == logical end. A failed write below is
   * rolled back to this offset (Post-review amendments) — otherwise the
   * partial line would be buried mid-file by later appends, where the
   * torn-tail rule cannot repair it and the store refuses to open. */
  good_off = -1;
  if (fseek(c->store.journal_fp, 0, SEEK_END) == 0)
    good_off = ftell(c->store.journal_fp);
  if (fwrite(line.data, 1, line.len, c->store.journal_fp) != line.len ||
      fflush(c->store.journal_fp) != 0) {
    bool recovered = false;
    clearerr(c->store.journal_fp);
    fclose(c->store.journal_fp);
    c->store.journal_fp = NULL;
    if (good_off >= 0 &&
        os_truncate(c->store.journal_path, (uint64_t)good_off) == ASPER_OK) {
      c->store.journal_fp = os_fopen(c->store.journal_path, "ab");
      recovered = c->store.journal_fp != NULL;
    }
    if (!recovered)
      asper_log(c, ASPER_LOG_ERROR, "store",
                "journal: append failed and rollback to offset %ld failed; "
                "journal closed, further mutations will fail", good_off);
    os_mutex_unlock(&c->journal_mu);
    asper_buf_free(&line);
    return asper_seterr(c, ASPER_ERR_IO, "journal: append failed");
  }
  e = ASPER_OK;
  if (force_sync || c->cfg.journal_sync == ASPER_SYNC_ALWAYS)
    e = os_fsync(c->store.journal_fp);
  c->store.journal_ops++;

  if (c->store.audit_fp) {
    if (fwrite(line.data, 1, line.len, c->store.audit_fp) != line.len ||
        fflush(c->store.audit_fp) != 0)
      asper_log(c, ASPER_LOG_WARN, "store", "audit append failed");
  }
  os_mutex_unlock(&c->journal_mu);
  asper_buf_free(&line);

  if (e != ASPER_OK)
    return asper_seterr(c, e, "journal: fsync failed");
  return ASPER_OK;
}

asper_err asper_journal_sync(asper_ctx *c) {
  asper_err e = ASPER_OK;
  if (!c) return ASPER_ERR_INVALID;
  os_mutex_lock(&c->journal_mu);
  if (c->store.journal_fp) {
    if (fflush(c->store.journal_fp) != 0)
      e = ASPER_ERR_IO;
    else
      e = os_fsync(c->store.journal_fp);
  }
  os_mutex_unlock(&c->journal_mu);
  if (e != ASPER_OK) return asper_seterr(c, e, "journal: sync failed");
  return ASPER_OK;
}
