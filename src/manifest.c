/*
 * manifest.c — manifest.xcdn read/write via xCDN-C.
 *
 * Load: a missing file initializes a fresh manifest (store_version 1,
 * created_at = ctx clock now, no embedding block) and persists it at once.
 * The #asper_manifest tag is optional on read; a store_version greater than
 * ours fails with ASPER_ERR_PARSE ("newer store").
 *
 * Save: the document is built programmatically, tagged #asper_manifest,
 * serialized pretty and written atomically (tmp + fsync + os_file_replace).
 * last_compaction is omitted while 0; the embedding block is present only
 * when a model id is recorded.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"
#include "xcdn.h"

/* ─────────────────────────── helpers ─────────────────────────── */

static const xcdn_node_t *first_object(const xcdn_document_t *doc) {
  size_t i;
  for (i = 0; i < doc->values_len; i++) {
    const xcdn_node_t *n = doc->values[i];
    if (n && n->value && n->value->type == XCDN_VAL_OBJECT) return n;
  }
  return NULL;
}

static bool value_time(const xcdn_value_t *v, asper_time *out) {
  if (!v || (v->type != XCDN_VAL_DATETIME && v->type != XCDN_VAL_STRING))
    return false;
  return v->data.string && asper_time_parse_rfc3339(v->data.string, out);
}

/* ─────────────────────────── load ─────────────────────────── */

static asper_err parse_embedding(asper_ctx *c, const xcdn_value_t *obj,
                                 asper_manifest *m) {
  size_t i;

  for (i = 0; i < xcdn_object_len(obj); i++) {
    const char *key = xcdn_object_key_at(obj, i);
    const xcdn_node_t *kn = xcdn_object_node_at(obj, i);
    const xcdn_value_t *v = kn ? kn->value : NULL;

    if (!key || !v) continue;

    if (strcmp(key, "model_id") == 0) {
      if (v->type != XCDN_VAL_STRING || !v->data.string)
        return asper_seterr(c, ASPER_ERR_PARSE,
                            "manifest: embedding.model_id: expected string, "
                            "got %s",
                            xcdn_value_type_str(v->type));
      if (strlen(v->data.string) >= sizeof m->embed_model_id)
        asper_log(c, ASPER_LOG_WARN, "store",
                  "manifest: embedding.model_id truncated to %zu chars",
                  sizeof m->embed_model_id - 1);
      snprintf(m->embed_model_id, sizeof m->embed_model_id, "%s",
               v->data.string);
    } else if (strcmp(key, "dim") == 0) {
      if (v->type != XCDN_VAL_INT)
        return asper_seterr(c, ASPER_ERR_PARSE,
                            "manifest: embedding.dim: expected integer, got %s",
                            xcdn_value_type_str(v->type));
      if (v->data.integer < 1 || v->data.integer > INT_MAX)
        return asper_seterr(c, ASPER_ERR_PARSE,
                            "manifest: embedding.dim: %lld out of range",
                            (long long)v->data.integer);
      m->embed_dim = (int)v->data.integer;
    } else if (strcmp(key, "model_hash") == 0) {
      if (v->type != XCDN_VAL_BYTES)
        return asper_seterr(c, ASPER_ERR_PARSE,
                            "manifest: embedding.model_hash: expected bytes, "
                            "got %s",
                            xcdn_value_type_str(v->type));
      if (v->data.bytes.len != sizeof m->embed_model_hash) {
        asper_log(c, ASPER_LOG_WARN, "store",
                  "manifest: embedding.model_hash is %zu bytes, expected %zu; "
                  "ignored",
                  v->data.bytes.len, sizeof m->embed_model_hash);
      } else {
        memcpy(m->embed_model_hash, v->data.bytes.data,
               sizeof m->embed_model_hash);
        m->has_model_hash = true;
      }
    } else {
      asper_log(c, ASPER_LOG_WARN, "store", "manifest: unknown key embedding.%s",
                key);
    }
  }
  return ASPER_OK;
}

asper_err asper_manifest_load(asper_ctx *c, asper_manifest *m) {
  const char *path;
  char *text = NULL;
  xcdn_document_t *doc;
  xcdn_error_t xerr;
  const xcdn_node_t *root;
  const xcdn_value_t *obj;
  size_t i;
  asper_err e;

  if (!m)
    return asper_seterr(c, ASPER_ERR_INVALID, "manifest: null out param");

  memset(m, 0, sizeof *m);
  m->store_version = 1;

  path = c->store.manifest_path;
  if (!path)
    return asper_seterr(c, ASPER_ERR_INVALID, "manifest: no path configured");

  if (!os_file_exists(path)) {
    m->created_at = asper_clock_now(&c->clock);
    e = asper_manifest_save(c, m);
    if (e == ASPER_OK)
      asper_log(c, ASPER_LOG_INFO, "store", "manifest: initialized new store");
    return e;
  }

  e = os_read_file(path, &text, NULL);
  if (e != ASPER_OK)
    return asper_seterr(c, e, "manifest: cannot read %s", path);

  doc = xcdn_parse(text, &xerr);
  free(text);
  if (!doc)
    return asper_seterr(c, ASPER_ERR_PARSE,
                        "manifest: %s: parse error at line %zu: %s", path,
                        xerr.span.line, xerr.message);

  root = first_object(doc);
  if (!root) {
    e = asper_seterr(c, ASPER_ERR_PARSE, "manifest: %s: no manifest object",
                     path);
    goto done;
  }
  if (xcdn_node_tag_count(root) > 0 &&
      !xcdn_node_has_tag(root, "asper_manifest"))
    asper_log(c, ASPER_LOG_WARN, "store",
              "manifest: unexpected tag #%s (expected #asper_manifest)",
              xcdn_node_tag_at(root, 0));

  obj = root->value;
  for (i = 0; i < xcdn_object_len(obj); i++) {
    const char *key = xcdn_object_key_at(obj, i);
    const xcdn_node_t *kn = xcdn_object_node_at(obj, i);
    const xcdn_value_t *v = kn ? kn->value : NULL;

    if (!key || !v) continue;

    if (strcmp(key, "store_version") == 0) {
      if (v->type != XCDN_VAL_INT) {
        e = asper_seterr(c, ASPER_ERR_PARSE,
                         "manifest: store_version: expected integer, got %s",
                         xcdn_value_type_str(v->type));
        goto done;
      }
      if (v->data.integer > 1) {
        e = asper_seterr(c, ASPER_ERR_PARSE,
                         "manifest: newer store (store_version %lld > 1); "
                         "refusing to open",
                         (long long)v->data.integer);
        goto done;
      }
      if (v->data.integer < 1) {
        e = asper_seterr(c, ASPER_ERR_PARSE,
                         "manifest: invalid store_version %lld",
                         (long long)v->data.integer);
        goto done;
      }
      m->store_version = (int)v->data.integer;
    } else if (strcmp(key, "created_at") == 0) {
      if (!value_time(v, &m->created_at)) {
        e = asper_seterr(c, ASPER_ERR_PARSE,
                         "manifest: created_at: invalid RFC 3339 datetime");
        goto done;
      }
    } else if (strcmp(key, "last_compaction") == 0) {
      if (!value_time(v, &m->last_compaction)) {
        e = asper_seterr(c, ASPER_ERR_PARSE,
                         "manifest: last_compaction: invalid RFC 3339 "
                         "datetime");
        goto done;
      }
    } else if (strcmp(key, "embedding") == 0) {
      if (v->type != XCDN_VAL_OBJECT) {
        e = asper_seterr(c, ASPER_ERR_PARSE,
                         "manifest: embedding: expected object, got %s",
                         xcdn_value_type_str(v->type));
        goto done;
      }
      e = parse_embedding(c, v, m);
      if (e != ASPER_OK) goto done;
    } else {
      asper_log(c, ASPER_LOG_WARN, "store", "manifest: unknown key %s", key);
    }
  }
  e = ASPER_OK;

done:
  xcdn_document_free(doc);
  return e;
}

/* ─────────────────────────── save ─────────────────────────── */

/* Wrap val in a node and insert it under key. ALWAYS consumes val (frees it
 * on failure); false on allocation failure. */
static bool obj_put(xcdn_value_t *obj, const char *key, xcdn_value_t *val) {
  xcdn_node_t *n;

  if (!val) return false;
  n = xcdn_node_new(val);
  if (!n) {
    xcdn_value_free(val);
    return false;
  }
  xcdn_object_set(obj, key, n);
  return true;
}

static asper_err write_atomic(asper_ctx *c, const char *path,
                              const char *text) {
  size_t plen = strlen(path);
  size_t tlen = strlen(text);
  char *tmp;
  FILE *f;
  bool ok;
  asper_err e;

  tmp = malloc(plen + sizeof ".tmp");
  if (!tmp)
    return asper_seterr(c, ASPER_ERR_NOMEM, "manifest: out of memory");
  memcpy(tmp, path, plen);
  memcpy(tmp + plen, ".tmp", sizeof ".tmp");

  f = os_fopen(tmp, "wb");
  if (!f) {
    free(tmp);
    return asper_seterr(c, ASPER_ERR_IO, "manifest: cannot open %s.tmp", path);
  }

  ok = fwrite(text, 1, tlen, f) == tlen;
  if (ok && (tlen == 0 || text[tlen - 1] != '\n'))
    ok = fputc('\n', f) != EOF;
  if (ok) ok = fflush(f) == 0;
  if (ok) ok = os_fsync(f) == ASPER_OK;
  if (fclose(f) != 0) ok = false;
  if (!ok) {
    (void)os_remove_file(tmp);
    free(tmp);
    return asper_seterr(c, ASPER_ERR_IO, "manifest: write failed for %s.tmp",
                        path);
  }

  e = os_file_replace(tmp, path);
  if (e != ASPER_OK) {
    (void)os_remove_file(tmp);
    free(tmp);
    return asper_seterr(c, e, "manifest: atomic replace failed for %s", path);
  }
  free(tmp);
  return ASPER_OK;
}

asper_err asper_manifest_save(asper_ctx *c, const asper_manifest *m) {
  xcdn_document_t *doc = NULL;
  xcdn_value_t *obj = NULL;
  xcdn_value_t *emb = NULL;
  xcdn_node_t *node;
  char *text;
  char ts[32];
  asper_err e;

  if (!m)
    return asper_seterr(c, ASPER_ERR_INVALID, "manifest: null manifest");
  if (!c->store.manifest_path)
    return asper_seterr(c, ASPER_ERR_INVALID, "manifest: no path configured");

  doc = xcdn_document_new();
  obj = xcdn_value_object();
  if (!doc || !obj) goto nomem;

  if (!obj_put(obj, "store_version", xcdn_value_int(m->store_version)))
    goto nomem;

  asper_time_format_rfc3339(m->created_at, ts);
  if (!obj_put(obj, "created_at", xcdn_value_datetime(ts))) goto nomem;

  if (m->last_compaction != 0) {
    asper_time_format_rfc3339(m->last_compaction, ts);
    if (!obj_put(obj, "last_compaction", xcdn_value_datetime(ts))) goto nomem;
  }

  if (m->embed_model_id[0] != '\0') {
    bool put_ok;

    emb = xcdn_value_object();
    if (!emb) goto nomem;
    if (!obj_put(emb, "model_id", xcdn_value_string(m->embed_model_id)))
      goto nomem;
    if (!obj_put(emb, "dim", xcdn_value_int(m->embed_dim))) goto nomem;
    if (m->has_model_hash &&
        !obj_put(emb, "model_hash",
                 xcdn_value_bytes(m->embed_model_hash,
                                  sizeof m->embed_model_hash)))
      goto nomem;

    put_ok = obj_put(obj, "embedding", emb); /* consumes emb either way */
    emb = NULL;
    if (!put_ok) goto nomem;
  }

  node = xcdn_node_new(obj);
  if (!node) goto nomem;
  obj = NULL; /* owned by node */
  xcdn_node_add_tag(node, "asper_manifest");
  xcdn_document_push_value(doc, node); /* doc owns node */

  text = xcdn_to_string_pretty(doc);
  xcdn_document_free(doc);
  if (!text)
    return asper_seterr(c, ASPER_ERR_NOMEM, "manifest: out of memory");

  e = write_atomic(c, c->store.manifest_path, text);
  free(text);
  return e;

nomem:
  xcdn_value_free(emb);
  xcdn_value_free(obj);
  xcdn_document_free(doc);
  return asper_seterr(c, ASPER_ERR_NOMEM, "manifest: out of memory");
}
