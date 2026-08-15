/*
 * cache.c — embeddings.bin read/write.
 *
 * On-disk layout, little-endian regardless of host:
 *
 *   magic       4 B   "ASPE"
 *   version     u32   1
 *   dim         u32   embedding dimension
 *   count       u64   number of entries
 *   model_hash  32 B  SHA-256 of the embedding model file
 *   entries     count x (16 B raw record UUID
 *                        + u64 FNV-1a-64 of content bytes
 *                        + dim x f32, L2-normalized)
 *
 * The cache is derived data: every failure mode short of allocation
 * failure degrades to a full rebuild (all live rows reported stale),
 * never to an error. Vectors are moved with memcpy to avoid alignment
 * UB; header integers go through explicit little-endian codecs, and the
 * float payload is byte-swapped on big-endian hosts.
 *
 * Stale lists (and full rebuilds) cover only non-deprecated records:
 * deprecated records have no index row by design (DEPRECATE removes it),
 * so embedding them at load would waste startup time for rows the scan
 * ignores anyway.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"

/* The on-disk format stores raw IEEE-754 single precision. */
typedef char asper_assert_float32[(sizeof(float) == 4) ? 1 : -1];

#define ASPER_CACHE_MAGIC "ASPE"
#define ASPER_CACHE_VERSION 1u
#define ASPER_CACHE_HEADER 52u /* 4 + 4 + 4 + 8 + 32 */

static bool asper_host_is_le(void)
{
    const union {
        uint32_t u;
        uint8_t b[4];
    } probe = { 0x01020304u };
    return probe.b[0] == 0x04;
}

static void asper_put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void asper_put_u64le(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint32_t asper_get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t asper_get_u64le(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | (uint64_t)p[i];
    return v;
}

/* File bytes (LE) -> aligned float buffer. */
static void asper_vec_decode(const uint8_t *src, float *dst, int dim)
{
    int i;
    if (asper_host_is_le()) {
        memcpy(dst, src, (size_t)dim * 4u);
        return;
    }
    for (i = 0; i < dim; i++) {
        uint32_t bits = asper_get_u32le(src + (size_t)i * 4u);
        memcpy(&dst[i], &bits, 4);
    }
}

/* Float row -> file bytes (LE). */
static void asper_vec_encode(const float *src, uint8_t *dst, int dim)
{
    int i;
    if (asper_host_is_le()) {
        memcpy(dst, src, (size_t)dim * 4u);
        return;
    }
    for (i = 0; i < dim; i++) {
        uint32_t bits;
        memcpy(&bits, &src[i], 4);
        asper_put_u32le(dst + (size_t)i * 4u, bits);
    }
}

static uint64_t asper_content_fnv(const asper_record *r)
{
    const char *content = r->content ? r->content : "";
    return asper_fnv1a64(content, strlen(content));
}

/* Full rebuild: every non-deprecated table row is stale. */
static asper_err asper_cache_all_stale(asper_ctx *c, size_t **out_stale,
                                       size_t *out_stale_n)
{
    const asper_table *t = &c->store.table;
    size_t *stale = NULL;
    size_t n = 0, i;

    if (t->n > 0) {
        stale = malloc(t->n * sizeof(size_t));
        if (!stale)
            return asper_seterr(c, ASPER_ERR_NOMEM,
                                "cache: out of memory building stale list");
        for (i = 0; i < t->n; i++)
            if (!t->recs[i]->deprecated)
                stale[n++] = i;
        if (n == 0) {
            free(stale);
            stale = NULL;
        }
    }
    *out_stale = stale;
    *out_stale_n = n;
    return ASPER_OK;
}

asper_err asper_cache_load(asper_ctx *c, size_t **out_stale,
                           size_t *out_stale_n)
{
    const asper_table *t;
    const asper_manifest *m;
    const char *why;
    uint8_t *data = NULL;
    float *vec = NULL;
    size_t *stale = NULL;
    size_t len = 0, stale_n = 0, entry_size, i, loaded = 0;
    uint64_t count;
    uint32_t dim;
    const uint8_t *p;
    asper_err e;

    if (!c || !out_stale || !out_stale_n || !c->store.cache_path)
        return ASPER_ERR_INVALID;
    *out_stale = NULL;
    *out_stale_n = 0;
    t = &c->store.table;
    m = &c->store.manifest;

    if (!os_file_exists(c->store.cache_path)) {
        e = asper_cache_all_stale(c, out_stale, out_stale_n);
        if (e == ASPER_OK)
            asper_log(c, ASPER_LOG_INFO, "embed",
                      "embedding cache missing; embedding %zu records",
                      *out_stale_n);
        return e;
    }

    {
        char *fbuf = NULL;
        e = os_read_file(c->store.cache_path, &fbuf, &len);
        if (e != ASPER_OK) {
            asper_log(c, ASPER_LOG_WARN, "embed",
                      "embedding cache unreadable: full rebuild");
            return asper_cache_all_stale(c, out_stale, out_stale_n);
        }
        data = (uint8_t *)fbuf;
    }

    if (len < ASPER_CACHE_HEADER ||
        memcmp(data, ASPER_CACHE_MAGIC, 4) != 0) {
        why = "bad magic";
        goto rebuild;
    }
    if (asper_get_u32le(data + 4) != ASPER_CACHE_VERSION) {
        why = "unsupported version";
        goto rebuild;
    }
    dim = asper_get_u32le(data + 8);
    if (c->index.dim <= 0 || dim != (uint32_t)c->index.dim) {
        why = "dimension mismatch";
        goto rebuild;
    }
    count = asper_get_u64le(data + 12);
    if (!m->has_model_hash ||
        memcmp(data + 20, m->embed_model_hash, 32) != 0) {
        why = "embedding model changed";
        goto rebuild;
    }
    entry_size = 16u + 8u + (size_t)dim * 4u;
    if (count > (uint64_t)(len - ASPER_CACHE_HEADER) / entry_size ||
        (uint64_t)len != ASPER_CACHE_HEADER + count * entry_size) {
        why = "truncated";
        goto rebuild;
    }

    vec = malloc((size_t)dim * sizeof(float));
    if (!vec) {
        free(data);
        return asper_seterr(c, ASPER_ERR_NOMEM,
                            "cache: out of memory loading vectors");
    }

    p = data + ASPER_CACHE_HEADER;
    for (i = 0; i < (size_t)count; i++, p += entry_size) {
        char id[37];
        asper_record *r;

        asper_uuid_from_bytes(p, id);
        r = asper_table_get(t, id);
        if (!r || r->deprecated)
            continue; /* purged, hand-deleted, or retired */
        if (asper_get_u64le(p + 16) != asper_content_fnv(r))
            continue; /* content changed: swept as stale below */
        asper_vec_decode(p + 24, vec, (int)dim);
        e = asper_index_put(&c->index, r, vec);
        if (e != ASPER_OK) {
            free(vec);
            free(data);
            return asper_seterr(c, e, "cache: index insert failed");
        }
        loaded++;
    }
    free(vec);
    vec = NULL;

    if (t->n > 0) {
        stale = malloc(t->n * sizeof(size_t));
        if (!stale) {
            free(data);
            return asper_seterr(c, ASPER_ERR_NOMEM,
                                "cache: out of memory building stale list");
        }
        for (i = 0; i < t->n; i++) {
            const asper_record *r = t->recs[i];
            if (!r->deprecated && r->emb_row < 0)
                stale[stale_n++] = i;
        }
        if (stale_n == 0) {
            free(stale);
            stale = NULL;
        }
    }
    free(data);
    asper_log(c, ASPER_LOG_INFO, "embed",
              "embedding cache: %zu loaded, %zu stale", loaded, stale_n);
    *out_stale = stale;
    *out_stale_n = stale_n;
    return ASPER_OK;

rebuild:
    asper_log(c, ASPER_LOG_WARN, "embed",
              "embedding cache invalid (%s): full rebuild", why);
    free(vec);
    free(data);
    return asper_cache_all_stale(c, out_stale, out_stale_n);
}

/* A record is written iff it has a live index row and a well-formed id;
 * the same predicate drives the count and the write pass so the header
 * count always matches the entries actually emitted. */
static bool asper_cache_savable(const asper_ctx *c, const asper_record *r,
                                uint8_t uuid[16], const float **out_row)
{
    const float *row;

    if (r->emb_row < 0)
        return false;
    row = asper_index_vec(&c->index, r);
    if (!row)
        return false;
    if (!asper_uuid_to_bytes(r->id, uuid))
        return false;
    if (out_row)
        *out_row = row;
    return true;
}

asper_err asper_cache_save(asper_ctx *c)
{
    const asper_table *t;
    uint8_t header[ASPER_CACHE_HEADER];
    uint8_t uuid[16];
    uint8_t *entry = NULL;
    char *tmp = NULL;
    FILE *fp = NULL;
    asper_buf snap;
    size_t entry_size, count = 0, i, tmp_len;
    int dim;
    asper_err e = ASPER_OK;

    if (!c || !c->store.cache_path)
        return ASPER_ERR_INVALID;

    asper_buf_init(&snap);

    /* Snapshot phase: table + index rows are guarded by c->lock, so
     * header + entries are serialized into memory under the WRITE lock.
     * cache_dirty is cleared inside the same critical section: marks set
     * by mutations that land after this point survive the save. Any
     * failure past the clear restores the flag (see `out`). */
    os_rwlock_wrlock(&c->lock);
    dim = c->index.dim;
    if (dim <= 0) {
        os_rwlock_wrunlock(&c->lock);
        asper_buf_free(&snap);
        return asper_seterr(c, ASPER_ERR_INVALID,
                            "cache: index has no dimension");
    }
    c->cache_dirty = false;
    t = &c->store.table;

    entry_size = 16u + 8u + (size_t)dim * 4u;
    entry = malloc(entry_size);
    if (!entry) {
        e = asper_seterr(c, ASPER_ERR_NOMEM,
                         "cache: out of memory saving");
        goto fail_unlock;
    }

    for (i = 0; i < t->n; i++)
        if (asper_cache_savable(c, t->recs[i], uuid, NULL))
            count++;

    memcpy(header, ASPER_CACHE_MAGIC, 4);
    asper_put_u32le(header + 4, ASPER_CACHE_VERSION);
    asper_put_u32le(header + 8, (uint32_t)dim);
    asper_put_u64le(header + 12, (uint64_t)count);
    if (c->store.manifest.has_model_hash)
        memcpy(header + 20, c->store.manifest.embed_model_hash, 32);
    else
        memset(header + 20, 0, 32); /* no known model: zeros (rebuilds) */

    e = asper_buf_append(&snap, (const char *)header, sizeof(header));
    if (e != ASPER_OK) {
        e = asper_seterr(c, e, "cache: out of memory saving");
        goto fail_unlock;
    }

    for (i = 0; i < t->n; i++) {
        const asper_record *r = t->recs[i];
        const float *row;

        if (!asper_cache_savable(c, r, entry, &row))
            continue;
        asper_put_u64le(entry + 16, asper_content_fnv(r));
        asper_vec_encode(row, entry + 24, dim);
        e = asper_buf_append(&snap, (const char *)entry, entry_size);
        if (e != ASPER_OK) {
            e = asper_seterr(c, e, "cache: out of memory saving");
            goto fail_unlock;
        }
    }
    os_rwlock_wrunlock(&c->lock);

    /* Write phase: no locks held while touching the filesystem. */
    tmp_len = strlen(c->store.cache_path) + 5;
    tmp = malloc(tmp_len);
    if (!tmp) {
        e = asper_seterr(c, ASPER_ERR_NOMEM,
                         "cache: out of memory saving");
        goto out;
    }
    snprintf(tmp, tmp_len, "%s.tmp", c->store.cache_path);

    fp = os_fopen(tmp, "wb");
    if (!fp) {
        e = asper_seterr(c, ASPER_ERR_IO, "cache: cannot open %s: %s", tmp,
                         strerror(errno));
        goto out;
    }

    if (fwrite(snap.data, 1, snap.len, fp) != snap.len) {
        e = asper_seterr(c, ASPER_ERR_IO, "cache: write failed: %s",
                         strerror(errno));
        goto out;
    }

    if (fflush(fp) != 0) {
        e = asper_seterr(c, ASPER_ERR_IO, "cache: flush failed: %s",
                         strerror(errno));
        goto out;
    }
    e = os_fsync(fp);
    if (e != ASPER_OK) {
        e = asper_seterr(c, e, "cache: fsync failed");
        goto out;
    }
    if (fclose(fp) != 0) {
        fp = NULL;
        e = asper_seterr(c, ASPER_ERR_IO, "cache: close failed: %s",
                         strerror(errno));
        goto out;
    }
    fp = NULL;

    e = os_file_replace(tmp, c->store.cache_path);
    if (e != ASPER_OK) {
        e = asper_seterr(c, e, "cache: atomic replace failed");
        goto out;
    }
    asper_log(c, ASPER_LOG_INFO, "embed",
              "embedding cache saved: %zu entries", count);
    goto out;

fail_unlock:
    os_rwlock_wrunlock(&c->lock); /* `out` restores cache_dirty below */

out:
    if (fp)
        fclose(fp);
    if (e != ASPER_OK) {
        if (tmp)
            os_remove_file(tmp); /* best effort; may not exist */
        os_rwlock_wrlock(&c->lock);
        c->cache_dirty = true; /* failed after the clear: keep the mark */
        os_rwlock_wrunlock(&c->lock);
        asper_log(c, ASPER_LOG_ERROR, "embed", "embedding cache save failed");
    }
    free(tmp);
    free(entry);
    asper_buf_free(&snap);
    return e;
}
