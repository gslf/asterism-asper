/*
 * retrieve.c — query-side selection.
 *
 * asper_retrieve embeds the query on the calling thread (the embedder
 * backend owns a dedicated llama context for is_query != 0, so this never
 * contends with the worker), scans the flat index under the read lock and
 * returns deep clones of the hits with .score set. The collect_* helpers
 * return deep clones for injection (in identity order) and for
 * asper_memory_list. All returned arrays are released with
 * asper_records_free.
 */

#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"

static void free_clone_array(asper_record **arr, size_t n)
{
  for (size_t i = 0; i < n; i++)
    asper_record_free_one(arr[i]);
  free(arr);
}

static asper_err retrieve_hybrid(asper_ctx *c, const char *query,
    asper_section section, const char *project, size_t k, double min_sim,
    asper_record ***out, size_t *out_n) {
  asper_search_document *docs = NULL;
  asper_record **refs = NULL, **arr = NULL;
  asper_search_hit *hits = NULL;
  float *vec = NULL; size_t n = 0, hn = 0;
  asper_err e = ASPER_OK;
  int dim = c->embedder.dim;
  if (!k) return ASPER_OK;
  if (c->has_embedder && dim > 0) {
    vec = calloc((size_t)dim, sizeof *vec);
    if (!vec) return ASPER_ERR_NOMEM;
    if (c->embedder.embed(c->embedder.ud, query, 1, vec) != ASPER_OK) {
      free(vec); vec = NULL; /* lexical fallback on model outage */
    }
  }
  if (vec) {
    double norm=0;for (int i=0;i<dim;i++) norm+=(double)vec[i]*vec[i];
    if (norm>0 && isfinite(norm)) for (int i=0;i<dim;i++) vec[i]/=(float)sqrt(norm);
    else { free(vec);vec=NULL; }
  }
  os_rwlock_rdlock(&c->lock);
  size_t cap = c->store.table.n;
  if (k > cap) k = cap;
  docs = calloc(cap ? cap : 1, sizeof *docs);
  refs = calloc(cap ? cap : 1, sizeof *refs);
  hits = calloc(k ? k : 1, sizeof *hits);
  if (!docs || !refs || !hits) { e = ASPER_ERR_NOMEM; goto done; }
  for (size_t i=0; i<cap; i++) {
    asper_record *r = c->store.table.recs[i];
    if (r->deprecated || (r->evidence.expires_at > 0 &&
        asper_clock_now(&c->clock) >= r->evidence.expires_at) ||
        (section != ASPER_SECTION_ANY && r->section != section) ||
        (r->section == ASPER_SECTION_PROJECT && (!project || !r->project ||
         strcmp(project,r->project)))) continue;
    refs[n] = r; docs[n].text = r->content;
    docs[n].path = r->evidence.provenance;
    docs[n].symbols = r->content;
    if (vec && dim == c->index.dim) {
      const float *rv=asper_index_vec(&c->index,r);
      if (rv && asper_cosine(vec,rv,dim)>=min_sim) docs[n].vector=rv;
    }
    n++;
  }
  e = asper_hybrid_search(docs,n,query,vec,(size_t)(dim>0?dim:0),k,hits,&hn);
  if (e != ASPER_OK) goto done;
  arr = calloc(hn ? hn : 1,sizeof *arr);
  if (!arr) { e=ASPER_ERR_NOMEM; goto done; }
  for (size_t i=0;i<hn;i++) {
    arr[i]=asper_record_clone(refs[hits[i].index]);
    if (!arr[i]) { free_clone_array(arr,i); arr=NULL; e=ASPER_ERR_NOMEM; goto done; }
    arr[i]->score=hits[i].score;
  }
  *out=arr; *out_n=hn;
done:
  os_rwlock_rdunlock(&c->lock);
  free(docs); free(refs); free(hits); free(vec);
  return e;
}

asper_err asper_retrieve_ex(asper_ctx *c, const char *query, asper_section s,
                            const char *project, size_t k, double min_sim,
                            int score_is_cos, asper_record ***out,
                            size_t *out_n)
{
  if (!c || !out || !out_n)
    return ASPER_ERR_INVALID;
  *out = NULL;
  *out_n = 0;
  if (!query)
    return asper_seterr(c, ASPER_ERR_INVALID, "retrieve: NULL query");
  if (!score_is_cos)
    return retrieve_hybrid(c, query, s, project, k, min_sim, out, out_n);
  /* Cosine-only mode is reserved for deduplication, never hybrid scores. */
  if (!c->has_embedder || k == 0)
    return ASPER_OK;

  int dim = c->index.dim > 0 ? c->index.dim : c->embedder.dim;
  if (dim <= 0)
    return ASPER_OK;
  if (k > SIZE_MAX / sizeof(asper_hit))
    return asper_seterr(c, ASPER_ERR_INVALID, "retrieve: k too large");

  float *qvec = malloc((size_t)dim * sizeof *qvec);
  if (!qvec)
    return asper_seterr(c, ASPER_ERR_NOMEM, "retrieve: out of memory");

  asper_err e = c->embedder.embed(c->embedder.ud, query, 1, qvec);
  if (e != ASPER_OK) {
    free(qvec);
    return asper_seterr(c, e, "retrieve: query embedding failed");
  }

  asper_hit *hits = malloc(k * sizeof *hits);
  if (!hits) {
    free(qvec);
    return asper_seterr(c, ASPER_ERR_NOMEM, "retrieve: out of memory");
  }

  os_rwlock_rdlock(&c->lock);
  size_t n = asper_index_scan(&c->index, &c->cfg, &c->clock, qvec, s,
                              project, k, min_sim, score_is_cos, hits);
  asper_record **arr = NULL;
  if (n > 0) {
    arr = malloc(n * sizeof *arr);
    if (!arr) {
      os_rwlock_rdunlock(&c->lock);
      free(hits);
      free(qvec);
      return asper_seterr(c, ASPER_ERR_NOMEM, "retrieve: out of memory");
    }
    for (size_t i = 0; i < n; i++) {
      asper_record *clone = asper_record_clone(hits[i].rec);
      if (!clone) {
        free_clone_array(arr, i);
        os_rwlock_rdunlock(&c->lock);
        free(hits);
        free(qvec);
        return asper_seterr(c, ASPER_ERR_NOMEM, "retrieve: out of memory");
      }
      clone->score = score_is_cos ? hits[i].cos : hits[i].score;
      arr[i] = clone;
    }
  }
  os_rwlock_rdunlock(&c->lock);

  asper_log(c, ASPER_LOG_DEBUG, "retrieve",
            "query k=%zu hits=%zu best=%.2f floor=%.2f", k, n,
            n > 0 ? hits[0].cos : 0.0, min_sim);
  free(hits);
  free(qvec);
  *out = arr;
  *out_n = n;
  return ASPER_OK;
}

asper_err asper_retrieve(asper_ctx *c, const char *query, asper_section s,
                         const char *project, size_t k, double min_sim,
                         asper_record ***out, size_t *out_n)
{
  return asper_retrieve_ex(c, query, s, project, k, min_sim, 0, out, out_n);
}

asper_err asper_collect_identity(asper_ctx *c, asper_record ***out,
                                 size_t *out_n)
{
  if (!c || !out || !out_n)
    return ASPER_ERR_INVALID;
  *out = NULL;
  *out_n = 0;

  os_rwlock_rdlock(&c->lock);
  const asper_table *t = &c->store.table;
  size_t cnt = 0;
  for (size_t i = 0; i < t->n; i++) {
    const asper_record *r = t->recs[i];
    if (r->section == ASPER_SECTION_IDENTITY && !r->deprecated &&
        r->evidence.kind != ASPER_EVIDENCE_INFERRED &&
        (!r->evidence.expires_at || asper_clock_now(&c->clock)<r->evidence.expires_at))
      cnt++;
  }
  if (cnt == 0) {
    os_rwlock_rdunlock(&c->lock);
    return ASPER_OK;
  }

  asper_record **arr = malloc(cnt * sizeof *arr);
  if (!arr) {
    os_rwlock_rdunlock(&c->lock);
    return asper_seterr(c, ASPER_ERR_NOMEM, "collect_identity: out of memory");
  }
  size_t n = 0;
  for (size_t i = 0; i < t->n && n < cnt; i++) {
    const asper_record *r = t->recs[i];
    if (r->section != ASPER_SECTION_IDENTITY || r->deprecated ||
        r->evidence.kind == ASPER_EVIDENCE_INFERRED ||
        (r->evidence.expires_at && asper_clock_now(&c->clock)>=r->evidence.expires_at))
      continue;
    asper_record *clone = asper_record_clone(r);
    if (!clone) {
      free_clone_array(arr, n);
      os_rwlock_rdunlock(&c->lock);
      return asper_seterr(c, ASPER_ERR_NOMEM,
                          "collect_identity: out of memory");
    }
    arr[n++] = clone;
  }
  qsort(arr, n, sizeof *arr, asper_identity_cmp);
  os_rwlock_rdunlock(&c->lock);

  *out = arr;
  *out_n = n;
  return ASPER_OK;
}

static bool list_match(const asper_record *r, asper_section s,
                       const char *project, bool include_deprecated)
{
  if (r->deprecated && !include_deprecated)
    return false;
  if (s != ASPER_SECTION_ANY && r->section != s)
    return false;
  /* The project filter constrains PROJECT records only; identity and
   * context records carry no project and always pass it (see asper.h). */
  if (project && r->section == ASPER_SECTION_PROJECT) {
    if (!r->project || strcmp(r->project, project) != 0)
      return false;
  }
  return true;
}

/* asper_memory_list order: created_at asc, then id asc (deterministic). */
static int list_cmp(const void *a, const void *b)
{
  const asper_record *ra = *(asper_record *const *)a;
  const asper_record *rb = *(asper_record *const *)b;
  if (ra->created_at != rb->created_at)
    return ra->created_at < rb->created_at ? -1 : 1;
  return strcmp(ra->id, rb->id);
}

asper_err asper_collect_list(asper_ctx *c, asper_section s,
                             const char *project, bool include_deprecated,
                             asper_record ***out, size_t *out_n)
{
  if (!c || !out || !out_n)
    return ASPER_ERR_INVALID;
  *out = NULL;
  *out_n = 0;

  os_rwlock_rdlock(&c->lock);
  const asper_table *t = &c->store.table;
  size_t cnt = 0;
  for (size_t i = 0; i < t->n; i++) {
    if (list_match(t->recs[i], s, project, include_deprecated))
      cnt++;
  }
  if (cnt == 0) {
    os_rwlock_rdunlock(&c->lock);
    return ASPER_OK;
  }

  asper_record **arr = malloc(cnt * sizeof *arr);
  if (!arr) {
    os_rwlock_rdunlock(&c->lock);
    return asper_seterr(c, ASPER_ERR_NOMEM, "collect_list: out of memory");
  }
  size_t n = 0;
  for (size_t i = 0; i < t->n && n < cnt; i++) {
    const asper_record *r = t->recs[i];
    if (!list_match(r, s, project, include_deprecated))
      continue;
    asper_record *clone = asper_record_clone(r);
    if (!clone) {
      free_clone_array(arr, n);
      os_rwlock_rdunlock(&c->lock);
      return asper_seterr(c, ASPER_ERR_NOMEM, "collect_list: out of memory");
    }
    arr[n++] = clone;
  }
  qsort(arr, n, sizeof *arr, list_cmp);
  os_rwlock_rdunlock(&c->lock);

  *out = arr;
  *out_n = n;
  return ASPER_OK;
}
