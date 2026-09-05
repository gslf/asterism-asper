/* Owned xCDN builders for records and operations; allocation failures propagate. */
#include "record_codec.h"
#include <stdlib.h>
#include <string.h>
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
  if (ok) ok = xobj_put(obj, "content", asper_record_string(r->content));
  if (ok)
    ok = xobj_put(obj, "source",
                  xcdn_value_string(asper_source_name(r->source)));
  if (ok) ok = xobj_put(obj, "evidence_kind", xcdn_value_int(r->evidence.kind));
  if (ok) ok = xobj_put(obj, "confidence_kind", xcdn_value_int(r->evidence.confidence_kind));
  if (ok) ok = xobj_put(obj, "confidence", xcdn_value_float(r->evidence.confidence));
  if (ok) ok = xobj_put(obj, "observed_at", xcdn_value_int(r->evidence.observed_at));
  if (ok) ok = xobj_put(obj, "expires_at", xcdn_value_int(r->evidence.expires_at));
  if (ok) ok = xobj_put(obj, "provenance", asper_record_string(r->evidence.provenance));
  if (ok) ok = xobj_put(obj, "workspace", asper_record_string(r->evidence.workspace));
  if (ok) ok = xobj_put(obj, "commit", asper_record_string(r->evidence.commit));
  if (ok && r->source_refs_n > 0) {
    xcdn_value_t *refs = xcdn_value_array();
    if (!refs) {
      ok = false;
    } else {
      bool rok = true;
      for (size_t i = 0; rok && i < r->source_refs_n; i++)
        rok = xarr_push(refs, xcdn_value_uuid(r->source_refs[i]));
      if (!rok) {
        xcdn_value_free(refs);
        ok = false;
      } else {
        ok = xobj_put(obj, "source_refs", refs);
      }
    }
  }
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
  if (ok) {
    xcdn_value_t *tags = xcdn_value_array();
    if (!tags) {
      ok = false;
    } else {
      bool tok = true;
      for (size_t i = 0; tok && i < r->tags_n; i++)
        tok = xarr_push(tags, asper_record_string(r->tags[i]));
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
      if (ok) ok = xobj_put(obj, "content", asper_record_string(op->content));
      if (ok) ok = xobj_put(obj,"declared_update",xcdn_value_bool(op->declared_update));
      break;
    case ASPER_OP_DEPRECATE:
      if (ok) ok = xobj_put(obj, "id", xcdn_value_uuid(op->id));
      if (ok && op->reason)
        ok = xobj_put(obj, "reason", asper_record_string(op->reason));
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

