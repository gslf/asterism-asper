/* Durable append and exact cursor pages over the rebuildable offset index. */
#include "event_index.h"
#include <stdlib.h>
#include <string.h>

asper_err asper_event_log_append(const char *path, asper_event *event) {
  asper_event_files fs;
  asper_err err = asper_event_files_open(&fs, path);
  FILE *f = NULL;
  uint64_t count = fs.count, bytes = fs.bytes;
  asper_event_files_close(&fs);
  if (err != ASPER_OK && err != ASPER_ERR_NOT_FOUND) return err;
  if (strlen(event->text) > ASPER_EVENT_BYTES ||
      bytes + strlen(event->text) + 512 > ASPER_EVENT_LOG_BYTES)
    return ASPER_ERR_INVALID;
  event->sequence = count+1;
  f = os_fopen(path, "ab");
  if (!f) return ASPER_ERR_IO;
  err = asper_event_frame_write(f, event);
  if (err == ASPER_OK && (fflush(f) || os_fsync(f) != ASPER_OK))
    err = ASPER_ERR_IO;
  if (err == ASPER_OK && !bytes) err = os_sync_parent(path);
  long end = ftell(f);
  if (fclose(f) && err == ASPER_OK) err = ASPER_ERR_IO;
  if (err != ASPER_OK) return err; /* An uncertain durable append is not replayed. */
  /* Failure of this derived write cannot revoke an already durable event.
   * Size/checksum checks rebuild an incomplete index on the next access. */
  size_t len = strlen(path);
  char *idx = malloc(len+5);
  if (!idx) return ASPER_OK;
  memcpy(idx, path, len); memcpy(idx+len, ".idx", 5);
  f = os_fopen(idx, count ? "ab" : "wb");
  if (f) {
    int ok = count || fwrite(ASPER_EVENT_INDEX_MAGIC, 1, 8, f) == 8;
    if (ok && end >= 0) (void)asper_event_index_write(f, bytes, (uint64_t)end);
    fclose(f);
  }
  free(idx);
  return ASPER_OK;
}

asper_err asper_event_log_page(const char *path, const char *query,
    unsigned long long after, size_t limit, asper_event **out, size_t *n,
    unsigned long long *next) {
  asper_event_files fs;
  asper_event *page = NULL;
  size_t used = 0, capacity = 0;
  asper_err err = asper_event_files_open(&fs, path);
  *out = NULL; *n = 0; *next = after;
  if (err == ASPER_ERR_NOT_FOUND) return ASPER_OK;
  if (err != ASPER_OK) return err;
  uint64_t cursor = after;
  while (cursor < fs.count && used < limit) {
    asper_event e;
    err = asper_event_files_read(&fs, path, cursor+1, &e);
    if (err != ASPER_OK) break;
    *next = ++cursor;
    if (*query && !strstr(e.text, query)) { free(e.text); continue; }
    if (used == capacity) {
      size_t cap = capacity ? capacity*2 : 16;
      if (cap > limit) cap = limit;
      asper_event *v = realloc(page, cap * sizeof *v);
      if (!v) { free(e.text); err = ASPER_ERR_NOMEM; break; }
      page = v; capacity = cap;
    }
    page[used++] = e;
  }
  asper_event_files_close(&fs);
  if (err != ASPER_OK) { asper_events_free(page, used); *next = after; return err; }
  *out = page; *n = used;
  return ASPER_OK;
}
