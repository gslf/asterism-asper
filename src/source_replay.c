/* Recover uncurated exact events without replaying tool effects. */
#include "source_internal.h"
#include "store_files.h"
#include <stdlib.h>
#include <string.h>

static int source_id_cmp(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static asper_err curated_ids_parse(const char *data, size_t len,
                                    char (**out)[37], size_t *out_n) {
  *out = NULL; *out_n = 0;
  if (!len) return ASPER_OK;
  if (len % 37) return ASPER_ERR_PARSE;
  size_t count = len / 37;
  char (*ids)[37] = calloc(count, sizeof *ids);
  if (!ids) return ASPER_ERR_NOMEM;
  for (size_t i = 0; i < count; i++) {
    memcpy(ids[i], data + i * 37, 36);
    if (data[i * 37 + 36] != '\n' || !asper_uuid_valid(ids[i])) {
      free(ids); return ASPER_ERR_PARSE;
    }
  }
  qsort(ids, count, sizeof *ids, source_id_cmp);
  for (size_t i = 0; i < count; i++)
    if (!*out_n || strcmp(ids[*out_n - 1], ids[i])) {
      if (*out_n != i) memcpy(ids[*out_n], ids[i], 37);
      (*out_n)++;
    }
  *out = ids; return ASPER_OK;
}

int asper_source_curated_has(char (*ids)[37], size_t n, const char *id) {
  return ids && bsearch(id, ids, n, sizeof *ids, source_id_cmp) != NULL;
}

/* Curation cycles are serialized. Check before inference, and recheck before
 * append; an external I/O failure can still make the final acknowledgement uncertain. */
static asper_err curated_space(const char *path, size_t n) {
  if (n > ASPER_CURATED_BYTES / 37) return ASPER_ERR_LIMIT;
  FILE *f = NULL; uint64_t bytes = 0;
  asper_err e = os_blob_open(path, &f, &bytes);
  if (e == ASPER_ERR_NOT_FOUND) return ASPER_OK;
  if (e != ASPER_OK) return e;
  if (bytes > ASPER_CURATED_BYTES - n * 37) e = ASPER_ERR_LIMIT;
  else if (bytes % 37) e = ASPER_ERR_PARSE;
  if (fclose(f) && e == ASPER_OK) e = ASPER_ERR_IO;
  return e;
}

asper_err asper_source_curated_admit(asper_ctx *c, size_t n) {
  if (!c) return ASPER_ERR_INVALID;
  char *path = os_path_join(c->store.root, "curated-events.log");
  if (!path) return ASPER_ERR_NOMEM;
  os_mutex_lock(&c->source_mu);
  asper_err e = curated_space(path, n);
  os_mutex_unlock(&c->source_mu); free(path); return e;
}

/* Replace the bounded acknowledgement set atomically. A retained completion
 * receipt makes a repeated acknowledgement harmless, even at the quota. */
asper_err asper_source_mark_curated(asper_ctx *c, const asper_turn *turns, size_t n) {
  if (!c || (!turns && n) || n > ASPER_CURATED_BYTES / 37) return ASPER_ERR_INVALID;
  for (size_t i = 0; i < n; i++) if (!asper_uuid_valid(turns[i].source_id)) return ASPER_ERR_INVALID;
  char *path = os_path_join(c->store.root, "curated-events.log"), *data = NULL;
  if (!path) return ASPER_ERR_NOMEM;
  char (*ids)[37] = NULL; size_t bytes = 0, count = 0;
  asper_buf output; asper_buf_init(&output);
  os_mutex_lock(&c->source_mu);
  asper_err e = asper_source_text_read(path, ASPER_CURATED_BYTES, &data, &bytes);
  if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  if (e == ASPER_OK) e = curated_ids_parse(data, bytes, &ids, &count);
  free(data);
  if (e == ASPER_OK && n) {
    char (*grown)[37] = realloc(ids, (count + n) * sizeof *ids);
    if (!grown) e = ASPER_ERR_NOMEM;
    else {
      ids = grown;
      for (size_t i = 0; i < n; i++) memcpy(ids[count++], turns[i].source_id, 37);
    }
  }
  if (e == ASPER_OK) {
    if (count) qsort(ids, count, sizeof *ids, source_id_cmp);
    for (size_t i = 0; e == ASPER_OK && i < count; i++) {
      if (i && !strcmp(ids[i-1], ids[i])) continue;
      if (output.len > ASPER_CURATED_BYTES - 37) e = ASPER_ERR_LIMIT;
      else e = asper_buf_printf(&output, "%s\n", ids[i]);
    }
    if (e == ASPER_OK) e = asper_store_file_write(c, path, output.data ? output.data : "", output.len, false);
  }
  os_mutex_unlock(&c->source_mu);
  asper_buf_free(&output); free(ids); free(path); return e;
}

/* Load once for a replay lifetime. New completions are behind admission cursors. */
asper_err asper_source_curated_load(asper_ctx *c, char (**ids)[37], size_t *count) {
  char *path = os_path_join(c->store.root, "curated-events.log"), *text = NULL;
  size_t bytes = 0;
  if (!path) return ASPER_ERR_NOMEM;
  os_mutex_lock(&c->source_mu);
  asper_err e = asper_source_text_read(path, ASPER_CURATED_BYTES, &text, &bytes);
  os_mutex_unlock(&c->source_mu);
  if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  if (e == ASPER_OK) e = curated_ids_parse(text, bytes, ids, count);
  free(text); free(path); return e;
}
