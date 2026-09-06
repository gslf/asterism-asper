/* Recover uncurated exact events without replaying tool effects. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

static int source_id_cmp(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static char (*curated_ids_parse(const char *data, size_t len,
                                size_t *out_n))[37] {
  char (*ids)[37] = NULL;
  size_t n = 0, cap = len / 37 + 1;
  const char *p, *end;
  *out_n = 0;
  if (!data || !len) return NULL;
  ids = (char (*)[37])calloc(cap, sizeof *ids);
  if (!ids) return NULL;
  p = data;
  end = data + len;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    size_t line_n = nl ? (size_t)(nl - p) : (size_t)(end - p);
    if (line_n == 36) {
      memcpy(ids[n], p, 36);
      ids[n][36] = '\0';
      if (asper_uuid_valid(ids[n])) n++;
    }
    p = nl ? nl + 1 : end;
  }
  if (n > 1) qsort(ids, n, sizeof *ids, source_id_cmp);
  *out_n = n;
  return ids;
}

static int curated_id_has(char (*ids)[37], size_t n, const char *id) {
  return ids && bsearch(id, ids, n, sizeof *ids, source_id_cmp) != NULL;
}

asper_err asper_source_mark_curated(asper_ctx *c,
                                    const asper_turn *turns, size_t n) {
  char *path;
  FILE *f = NULL;
  asper_err e = ASPER_OK;
  if (!c || (!turns && n)) return ASPER_ERR_INVALID;
  path = os_path_join(c->store.root, "curated-events.log");
  if (!path) return ASPER_ERR_NOMEM;
  os_mutex_lock(&c->source_mu);
  f = os_fopen(path, "ab");
  if (!f) e = ASPER_ERR_IO;
  for (size_t i = 0; e == ASPER_OK && i < n; i++)
    if (asper_uuid_valid(turns[i].source_id) &&
        fprintf(f, "%s\n", turns[i].source_id) < 0)
      e = ASPER_ERR_IO;
  if (e == ASPER_OK && (fflush(f) != 0 || os_fsync(f) != ASPER_OK))
    e = ASPER_ERR_IO;
  if (f && fclose(f) != 0 && e == ASPER_OK) e = ASPER_ERR_IO;
  os_mutex_unlock(&c->source_mu);
  free(path);
  return e;
}

asper_err asper_source_replay_pending(asper_ctx *c) {
  char *scopes_dir = NULL, *index_path = NULL, *curated_path = NULL;
  char *index = NULL, *curated = NULL;
  size_t index_len = 0, curated_len = 0, queued = 0;
  char (*curated_ids)[37] = NULL;
  size_t curated_n = 0;
  asper_err e;
  if (!c) return ASPER_ERR_INVALID;
  scopes_dir = asper_source_dir(c, "scopes");
  if (!scopes_dir) return ASPER_ERR_IO;
  index_path = os_path_join(scopes_dir, "index.log");
  curated_path = os_path_join(c->store.root, "curated-events.log");
  if (!index_path || !curated_path) {
    e = ASPER_ERR_NOMEM;
    goto out;
  }
  e = os_read_file(index_path, &index, &index_len);
  if (e == ASPER_ERR_NOT_FOUND) {
    e = ASPER_OK;
    goto out;
  }
  if (e != ASPER_OK) goto out;
  if (!index || index_len == 0) goto out;
  e = os_read_file(curated_path, &curated, &curated_len);
  if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  if (e != ASPER_OK) goto out;
  curated_ids = curated_ids_parse(curated, curated_len, &curated_n);
  if (curated_len && !curated_ids) {
    e = ASPER_ERR_NOMEM;
    goto out;
  }
  for (char *p = index, *end = index + index_len; p < end;) {
    char *nl = memchr(p, '\n', (size_t)(end - p));
    size_t sn = nl ? (size_t)(nl - p) : (size_t)(end - p);
    char scope[65];
    asper_event *events = NULL;
    size_t events_n = 0;
    if (sn == 0 || sn >= sizeof scope) {
      p = nl ? nl + 1 : end;
      continue;
    }
    memcpy(scope, p, sn);
    scope[sn] = '\0';
    if (!asper_source_scope_valid(scope)) {
      e = asper_seterr(c, ASPER_ERR_PARSE,
                       "source: invalid scope in durable index");
      goto out;
    }
    e = asper_event_list(c, scope, &events, &events_n);
    if (e != ASPER_OK) goto out;
    for (size_t i = 0; i < events_n; i++) {
      asper_role role;
      if (events[i].kind != ASPER_EVENT_USER &&
          events[i].kind != ASPER_EVENT_ASSISTANT)
        continue;
      if (curated_id_has(curated_ids, curated_n, events[i].id)) continue;
      role = events[i].kind == ASPER_EVENT_ASSISTANT ? ASPER_ROLE_ASSISTANT
                                                      : ASPER_ROLE_USER;
      e = asper_enqueue_turn(c, role, events[i].text,
                             (asper_time)events[i].at, events[i].id, scope, events[i].object_ref);
      if (e != ASPER_OK) {
        asper_events_free(events, events_n);
        goto out;
      }
      queued++;
    }
    asper_events_free(events, events_n);
    p = nl ? nl + 1 : end;
  }
  if (queued)
    asper_log(c, ASPER_LOG_INFO, "source",
              "replayed %zu durable event(s) awaiting curation", queued);
out:
  free(curated_ids);
  free(curated);
  free(index);
  free(curated_path);
  free(index_path);
  free(scopes_dir);
  return e;
}
