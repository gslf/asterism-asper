/* Validate serialized records and operations before they enter the live store. */
#include "record_codec.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
  r->content = asper_record_unescape(s);
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

  v = obj_field(obj, "source_refs");
  if (v && v->type != XCDN_VAL_NULL) {
    if (v->type != XCDN_VAL_ARRAY)
      REC_FAIL("record %s: source_refs must be an array", r->id);
    if (v->data.array.len > 0) {
      r->source_refs =
          (char **)calloc(v->data.array.len, sizeof(char *));
      if (!r->source_refs) REC_OOM();
      for (size_t i = 0; i < v->data.array.len; i++) {
        const xcdn_node_t *rn = v->data.array.items[i];
        char uuid[37];
        if (!rn || !val_uuid37(rn->value, uuid))
          REC_FAIL("record %s: source_refs must contain UUIDs", r->id);
        r->source_refs[i] = asper_strdup(uuid);
        if (!r->source_refs[i]) REC_OOM();
        r->source_refs_n = i + 1;
      }
    }
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

  /* Old records retain explicit uncertainty; curator records get a fixed
   * expiry anchored to creation, never refreshed by retrieval or review. */
  r->evidence.kind = r->source == ASPER_SRC_CURATOR ?
      ASPER_EVIDENCE_INFERRED : ASPER_EVIDENCE_DECLARED;
  r->evidence.confidence = r->source == ASPER_SRC_CURATOR ? 0.5 : 1.0;
  r->evidence.observed_at = r->created_at;
  snprintf(r->evidence.provenance, sizeof r->evidence.provenance,
           "legacy:%s", asper_source_name(r->source));
  v = obj_field(obj, "evidence_kind");
  if (v) {
    if (v->type != XCDN_VAL_INT || v->data.integer < 0 || v->data.integer > 2)
      REC_FAIL("record %s: invalid evidence kind", r->id);
    r->evidence.kind = (asper_evidence_kind)v->data.integer;
  }
  v = obj_field(obj, "confidence");
  if (v) {
    if (v->type != XCDN_VAL_FLOAT && v->type != XCDN_VAL_INT)
      REC_FAIL("record %s: invalid confidence", r->id);
    r->evidence.confidence = v->type == XCDN_VAL_FLOAT ?
        v->data.floating : (double)v->data.integer;
    if (!(r->evidence.confidence >= 0 && r->evidence.confidence <= 1))
      REC_FAIL("record %s: invalid confidence", r->id);
  }
  v = obj_field(obj, "confidence_kind");
  if (v) {
    if (v->type != XCDN_VAL_INT || v->data.integer < ASPER_CONFIDENCE_UNKNOWN ||
        v->data.integer > ASPER_CONFIDENCE_MEASURED) REC_FAIL("invalid confidence kind");
    r->evidence.confidence_kind = (asper_confidence_kind)v->data.integer;
  }
  { const char *keys[] = {"provenance", "workspace", "commit"};
    char *dst[] = {r->evidence.provenance, r->evidence.workspace, r->evidence.commit};
    size_t caps[] = {sizeof r->evidence.provenance, sizeof r->evidence.workspace,
                     sizeof r->evidence.commit};
    for (size_t i = 0; i < 3; i++) {
      v = obj_field(obj, keys[i]);
      if (v) {
        char *decoded;
        s = val_str(v);
        if (!s) REC_FAIL("record %s: invalid evidence string", r->id);
        decoded = asper_record_unescape(s);
        if (!decoded) REC_OOM();
        if (strlen(decoded) >= caps[i]) { free(decoded); REC_FAIL("evidence too long"); }
        strcpy(dst[i], decoded); free(decoded);
      }
    }
  }
  { const char *keys[] = {"observed_at", "expires_at"};
    long long *dst[] = {&r->evidence.observed_at, &r->evidence.expires_at};
    for (size_t i = 0; i < 2; i++) {
      v = obj_field(obj, keys[i]);
      if (v) {
        if (v->type != XCDN_VAL_INT || v->data.integer < 0)
          REC_FAIL("record %s: invalid evidence timestamp", r->id);
        *dst[i] = v->data.integer;
      }
    }
  }
  if (r->evidence.kind == ASPER_EVIDENCE_INFERRED && !r->evidence.expires_at)
    r->evidence.expires_at = r->created_at + 30 * 86400;

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
        r->tags[i] = asper_record_unescape(ts);
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
      v=obj_field(obj,"declared_update");
      if (v) {
        if (v->type!=XCDN_VAL_BOOL) OP_FAIL("invalid declared_update");
        out->declared_update=v->data.boolean;
      }
      out->content = asper_record_unescape(s);
      if (!out->content) OP_OOM();
      break;
    case ASPER_OP_DEPRECATE:
      if (!val_uuid37(obj_field(obj, "id"), out->id))
        OP_FAIL("op deprecate: missing or invalid id");
      v = obj_field(obj, "reason");
      if (v && v->type == XCDN_VAL_STRING) {
        out->reason = asper_record_unescape(v->data.string);
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

