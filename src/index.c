/*
 * index.c — flat in-memory cosine index.
 *
 * One contiguous float32[n x dim] matrix plus a parallel asper_record*
 * array; recs[i]->emb_row == i is the maintained invariant. Rows are
 * L2-normalized on insert so similarity is a plain dot product.
 */

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"

void asper_index_init(asper_index *ix, int dim)
{
    ix->vecs = NULL;
    ix->recs = NULL;
    ix->n = 0;
    ix->cap = 0;
    ix->dim = dim;
}

void asper_index_free(asper_index *ix)
{
    if (!ix)
        return;
    free(ix->vecs);
    free(ix->recs);
    ix->vecs = NULL;
    ix->recs = NULL;
    ix->n = 0;
    ix->cap = 0;
}

/* Copy vec into row, L2-normalizing; a zero vector stays all-zero. */
static void asper_index_write_row(asper_index *ix, size_t row,
                                  const float *vec)
{
    float *dst = ix->vecs + row * (size_t)ix->dim;
    double norm = 0.0;
    int i;

    for (i = 0; i < ix->dim; i++)
        norm += (double)vec[i] * (double)vec[i];
    if (norm > 0.0) {
        double inv = 1.0 / sqrt(norm);
        for (i = 0; i < ix->dim; i++)
            dst[i] = (float)((double)vec[i] * inv);
    } else {
        memset(dst, 0, (size_t)ix->dim * sizeof(float));
    }
}

asper_err asper_index_put(asper_index *ix, asper_record *r, const float *vec)
{
    if (!ix || !r || !vec || ix->dim <= 0)
        return ASPER_ERR_INVALID;

    if (r->emb_row >= 0) {
        size_t row = (size_t)r->emb_row;
        if (row >= ix->n || ix->recs[row] != r)
            return ASPER_ERR_INVALID; /* broken emb_row invariant */
        asper_index_write_row(ix, row, vec);
        return ASPER_OK;
    }

    if (ix->n >= (size_t)INT_MAX)
        return ASPER_ERR_INVALID; /* emb_row is an int */

    if (ix->n == ix->cap) {
        size_t ncap = ix->cap ? ix->cap * 2 : 16;
        float *nv;
        asper_record **nr;

        nv = realloc(ix->vecs, ncap * (size_t)ix->dim * sizeof(float));
        if (!nv)
            return ASPER_ERR_NOMEM;
        ix->vecs = nv;
        nr = realloc(ix->recs, ncap * sizeof(asper_record *));
        if (!nr)
            return ASPER_ERR_NOMEM; /* vecs kept larger; cap unchanged */
        ix->recs = nr;
        ix->cap = ncap;
    }

    r->emb_row = (int)ix->n;
    ix->recs[ix->n] = r;
    asper_index_write_row(ix, ix->n, vec);
    ix->n++;
    return ASPER_OK;
}

void asper_index_remove(asper_index *ix, asper_record *r)
{
    size_t row, last;

    if (!ix || !r || r->emb_row < 0)
        return;
    row = (size_t)r->emb_row;
    if (row >= ix->n || ix->recs[row] != r) {
        r->emb_row = -1; /* stale reference; nothing to move */
        return;
    }
    last = ix->n - 1;
    if (row != last) {
        memcpy(ix->vecs + row * (size_t)ix->dim,
               ix->vecs + last * (size_t)ix->dim,
               (size_t)ix->dim * sizeof(float));
        ix->recs[row] = ix->recs[last];
        ix->recs[row]->emb_row = (int)row;
    }
    ix->n = last;
    r->emb_row = -1;
}

const float *asper_index_vec(const asper_index *ix, const asper_record *r)
{
    if (!ix || !r || r->emb_row < 0 || (size_t)r->emb_row >= ix->n)
        return NULL;
    return ix->vecs + (size_t)r->emb_row * (size_t)ix->dim;
}

double asper_cosine(const float *a, const float *b, int dim)
{
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    int i = 0;

    /* Four independent float accumulators expose a vector-friendly hot loop.
     * Embeddings are float32 and normalized, so double products only added
     * conversion cost without providing meaningful ranking precision. */
    for (; i + 3 < dim; i += 4) {
        a0 += a[i] * b[i];
        a1 += a[i + 1] * b[i + 1];
        a2 += a[i + 2] * b[i + 2];
        a3 += a[i + 3] * b[i + 3];
    }
    for (; i < dim; i++) a0 += a[i] * b[i];
    return (double)((a0 + a1) + (a2 + a3));
}

/* Section/project/status filter:
 * - deprecated records never match;
 * - ANY spans all sections (identity included), but a PROJECT record only
 *   matches when the project parameter is non-NULL and equal — so an
 *   ANY-scan covers identity + context + the given (active) project;
 * - PROJECT requires an exact slug match against a non-NULL parameter. */
static bool asper_scan_match(const asper_record *r, asper_section section,
                             const char *project)
{
    if (r->deprecated)
        return false;
    if (section != ASPER_SECTION_ANY && r->section != section)
        return false;
    if (r->section == ASPER_SECTION_PROJECT)
        return project != NULL && r->project != NULL &&
               strcmp(r->project, project) == 0;
    return true;
}

/* cos desc, updated_at desc, id asc — similarity order for the call sites
 * that require it (related memories, dedup winner). */
static int asper_hit_cmp_cos(const asper_hit *a, const asper_hit *b)
{
    if (a->cos > b->cos)
        return -1;
    if (a->cos < b->cos)
        return 1;
    if (a->rec->updated_at > b->rec->updated_at)
        return -1;
    if (a->rec->updated_at < b->rec->updated_at)
        return 1;
    return strcmp(a->rec->id, b->rec->id);
}

size_t asper_index_scan(const asper_index *ix, const asper_config *cfg,
                        const asper_clock *clk, const float *qvec,
                        asper_section section, const char *project,
                        size_t k, double min_sim, int rank_by_cos,
                        asper_hit *hits)
{
    asper_time now;
    size_t i, count = 0;

    if (!ix || !cfg || !clk || !qvec || !hits || k == 0)
        return 0;
    now = asper_clock_now(clk);

    for (i = 0; i < ix->n; i++) {
        asper_record *r = ix->recs[i];
        asper_hit h;
        /* Not `cos`: that would hide <math.h>'s cos() (MSVC C4459, fatal
         * under /WX). The asper_hit member keeps its name. */
        double cosine;
        size_t pos;

        if (!asper_scan_match(r, section, project))
            continue;
        cosine = asper_cosine(qvec, ix->vecs + i * (size_t)ix->dim, ix->dim);
        if (cosine < min_sim)
            continue;

        h.rec = r;
        h.cos = cosine;
        h.score = asper_score_record(cfg, r, cosine, now);

        /* Ordered insertion into the top-k array; k is small.
         * Selection and order follow the active ranking: composite score
         * (deterministic) or raw cosine when rank_by_cos != 0. */
        pos = count;
        while (pos > 0 &&
               (rank_by_cos ? asper_hit_cmp_cos(&h, &hits[pos - 1])
                            : asper_hit_cmp(&h, &hits[pos - 1])) < 0)
            pos--;
        if (pos == k)
            continue; /* ranks below a full list */
        if (count < k)
            count++;
        memmove(&hits[pos + 1], &hits[pos],
                (count - 1 - pos) * sizeof(asper_hit));
        hits[pos] = h;
    }
    return count;
}
