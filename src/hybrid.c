/* Deterministic, bounded hybrid retrieval. No generative model is trusted
 * with candidate IDs: exact identifiers, BM25 and vector ranks are fused,
 * then reranked by query coverage and exact path/phrase evidence. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "asper.h"

#define TERMS 96
#define TERM_BYTES 192
static int word(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c >= 128 || c == '_';
}
static size_t length(const char *s) {
  size_t n = 0;
  int prev = 0;
  for (; *s; s++) {
    int w = word((unsigned char)*s);
    if (w && !prev)
      n++;
    prev = w;
  }
  return n ? n : 1;
}
static size_t terms(const char *s, char out[TERMS][TERM_BYTES]) {
  size_t n = 0;
  while (s && *s && n < TERMS) {
    const char *a;
    size_t len, i;
    while (*s && !word((unsigned char)*s))
      s++;
    a = s;
    while (word((unsigned char)*s))
      s++;
    len = (size_t)(s - a);
    if (len < 2 || len >= TERM_BYTES)
      continue;
    for (i = 0; i < n; i++)
      if (strlen(out[i]) == len && strncmp(out[i], a, len) == 0)
        break;
    if (i < n)
      continue;
    static const char *const stop[] = {
        "the",    "and",  "to",        "of",      "in",          "it",
        "is",     "an",   "on",        "message", "active_file", "objective",
        "recent", "user", "assistant", NULL};
    int ignore = 0;
    for (size_t j = 0; stop[j]; j++)
      if (strlen(stop[j]) == len && !strncmp(a, stop[j], len)) {
        ignore = 1;
        break;
      }
    if (!ignore) {
      memcpy(out[n], a, len);
      out[n++][len] = 0;
    }
  }
  return n;
}
static size_t frequency(const char *s, const char *term) {
  size_t n = 0, len = strlen(term);
  const char *p = s;
  if (!s)
    return 0;
  while ((p = strstr(p, term)) != NULL) {
    if ((p == s || !word((unsigned char)p[-1])) && !word((unsigned char)p[len]))
      n++;
    p += len;
  }
  return n;
}
static double similarity(const float *a, const float *b, size_t dim) {
  double dot = 0, aa = 0, bb = 0;
  if (!a || !b)
    return -1;
  for (size_t i = 0; i < dim; i++) {
    dot += a[i] * b[i];
    aa += a[i] * a[i];
    bb += b[i] * b[i];
  }
  return aa > 0 && bb > 0 && isfinite(aa) && isfinite(bb) && isfinite(dot)
             ? dot / sqrt(aa * bb)
             : -1;
}
static int ranked(const void *a, const void *b) {
  const asper_search_hit *x = a, *y = b;
  if (x->score != y->score)
    return x->score > y->score ? -1 : 1;
  return x->index < y->index ? -1 : x->index > y->index;
}
asper_err asper_hybrid_search(const asper_search_document *docs, size_t n,
                              const char *query, const float *query_vector,
                              size_t dim, size_t k, asper_search_hit *out,
                              size_t *out_n) {
  char ts[TERMS][TERM_BYTES];
  size_t nt, df[TERMS] = {0};
  asper_search_hit *h;
  double avg = 0;
  if (!out_n || !query || (n && !docs) || (k && !out) || n > 1000000)
    return ASPER_ERR_INVALID;
  *out_n = 0;
  if (!n || !k)
    return ASPER_OK;
  h = calloc(n, sizeof *h);
  if (!h)
    return ASPER_ERR_NOMEM;
  nt = terms(query, ts);
  for (size_t i = 0; i < n; i++) {
    const char *s = docs[i].text ? docs[i].text : "";
    avg += (double)length(s);
    for (size_t j = 0; j < nt; j++)
      if (frequency(s, ts[j]))
        df[j]++;
  }
  avg /= n;
  for (size_t i = 0; i < n; i++) {
    const char *s = docs[i].text ? docs[i].text : "";
    double len = (double)length(s);
    size_t cover = 0;
    h[i].index = i;
    h[i].vector = similarity(query_vector, docs[i].vector, dim);
    for (size_t j = 0; j < nt; j++) {
      double tf = (double)frequency(s, ts[j]);
      if (tf) {
        cover++;
        h[i].bm25 += log(1 + ((double)n - df[j] + 0.5) / (df[j] + 0.5)) * tf *
                     2.2 / (tf + 1.2 * (0.25 + 0.75 * len / avg));
      }
      /* Symbols are case-sensitive whole tokens. */
      if (frequency(docs[i].symbols, ts[j]))
        h[i].exact += 1;
    }
    if (docs[i].path && docs[i].path[0] && strstr(query, docs[i].path))
      h[i].exact += 4;
    if (strlen(query) > 3 && strstr(s, query))
      h[i].exact += 2;
    h[i].coverage = nt ? (double)cover / nt : 0;
  }
  /* Three stable O(n log n) rankings, then reciprocal rank fusion. */
  asper_search_hit *rank = malloc(n * sizeof *rank);
  if (!rank) {
    free(h);
    return ASPER_ERR_NOMEM;
  }
  for (int channel = 0; channel < 3; channel++) {
    for (size_t i = 0; i < n; i++) {
      rank[i].index = i;
      rank[i].score = channel == 0   ? h[i].bm25
                      : channel == 1 ? h[i].vector
                                     : h[i].exact;
    }
    qsort(rank, n, sizeof *rank, ranked);
    for (size_t i = 0; i < n; i++) {
      if (rank[i].score > (channel == 1 ? 0.25 : 0))
        h[rank[i].index].score += 1.0 / (61 + i);
    }
  }
  free(rank);
  for (size_t i = 0; i < n; i++)
    if (h[i].score > 0)
      h[i].score += 0.03 * h[i].coverage + 0.02 * h[i].exact;
  qsort(h, n, sizeof *h, ranked);
  while (*out_n < n && *out_n < k && h[*out_n].score > 0) {
    out[*out_n] = h[*out_n];
    (*out_n)++;
  }
  free(h);
  return ASPER_OK;
}
