/* Rebuildable offset index. The log is durable before its index is updated.
 * A page allocates one frame at a time and retains only matching events. */
#include <stdlib.h>
#include <string.h>
#include "event_log.h"

#define INDEX_MAGIC "AEIDX2\r\n"
#define INDEX_RECORD 48u

typedef struct {
  FILE *log, *index;
  char *index_path;
  uint64_t count, bytes;
} event_files;

static void put64(uint8_t *p, uint64_t n) {
  for (int i = 0; i < 8; i++, n >>= 8) p[i] = (uint8_t)n;
}
static uint64_t get64(const uint8_t *p) {
  uint64_t n = 0;
  for (int i = 7; i >= 0; i--) n = (n << 8) | p[i];
  return n;
}
static int index_write(FILE *f, uint64_t start, uint64_t end) {
  uint8_t row[INDEX_RECORD];
  put64(row, start); put64(row+8, end);
  asper_sha256(row, 16, row+16);
  return fwrite(row, 1, sizeof row, f) == sizeof row;
}
static int index_read(event_files *fs, uint64_t seq, uint64_t *a, uint64_t *b) {
  uint8_t row[INDEX_RECORD], hash[32];
  if (!seq || seq > fs->count ||
      fseek(fs->index, (long)(8 + (seq-1)*INDEX_RECORD), SEEK_SET) ||
      fread(row, 1, sizeof row, fs->index) != sizeof row) return 0;
  asper_sha256(row, 16, hash);
  *a = get64(row); *b = get64(row+8);
  return !memcmp(hash, row+16, 32) && *a < *b && *b <= fs->bytes;
}
static void files_close(event_files *fs) {
  if (fs->log) fclose(fs->log);
  if (fs->index) fclose(fs->index);
  free(fs->index_path);
  memset(fs, 0, sizeof *fs);
}

/* A complete damaged frame is never discarded as a torn tail. */
static asper_err rebuild(event_files *fs, const char *path) {
  size_t len = strlen(fs->index_path);
  char *tmp = malloc(len + 5);
  FILE *index;
  uint64_t good = 0, count = 0;
  asper_err err = ASPER_OK;
  if (!tmp) return ASPER_ERR_NOMEM;
  memcpy(tmp, fs->index_path, len); memcpy(tmp+len, ".tmp", 5);
  index = os_fopen(tmp, "wb");
  if (!index) { free(tmp); return ASPER_ERR_IO; }
  if (fwrite(INDEX_MAGIC, 1, 8, index) != 8) err = ASPER_ERR_IO;
  rewind(fs->log);
  while (err == ASPER_OK && good < fs->bytes) {
    asper_event e;
    err = asper_event_frame_read(fs->log, &e);
    if (err == ASPER_ERR_NOT_FOUND) {
      /* Close before truncation for Windows sharing semantics. */
      fclose(fs->log); fs->log = NULL;
      err = os_truncate(path, good);
      fs->log = os_fopen(path, "r+b");
      if (!fs->log || (err == ASPER_OK && os_fsync(fs->log) != ASPER_OK))
        err = ASPER_ERR_IO;
      fs->bytes = good;
      break;
    }
    if (err != ASPER_OK) break;
    long end = ftell(fs->log);
    if (e.sequence != count+1 || end < 0 || (uint64_t)end > fs->bytes)
      err = ASPER_ERR_PARSE;
    else if (!index_write(index, good, (uint64_t)end)) err = ASPER_ERR_IO;
    free(e.text);
    if (err == ASPER_OK) { good = (uint64_t)end; count++; }
  }
  if (err == ASPER_OK && (fflush(index) || os_fsync(index) != ASPER_OK))
    err = ASPER_ERR_IO;
  if (fclose(index) && err == ASPER_OK) err = ASPER_ERR_IO;
  if (err == ASPER_OK) err = os_file_replace(tmp, fs->index_path);
  if (err == ASPER_OK) {
    fs->index = os_fopen(fs->index_path, "rb");
    fs->count = count;
    if (!fs->index) err = ASPER_ERR_IO;
  }
  if (err != ASPER_OK) os_remove_file(tmp);
  free(tmp);
  return err;
}

static asper_err files_open(event_files *fs, const char *path) {
  uint64_t size = 0, a, b;
  char magic[8];
  size_t len = strlen(path);
  asper_err err;
  memset(fs, 0, sizeof *fs);
  err = os_file_size(path, &fs->bytes);
  if (err == ASPER_ERR_NOT_FOUND) return ASPER_ERR_NOT_FOUND;
  if (err != ASPER_OK) return err;
  if (fs->bytes > ASPER_EVENT_LOG_BYTES) return ASPER_ERR_INVALID;
  fs->index_path = malloc(len + 5);
  if (!fs->index_path) return ASPER_ERR_NOMEM;
  memcpy(fs->index_path, path, len); memcpy(fs->index_path+len, ".idx", 5);
  fs->log = os_fopen(path, "rb");
  if (!fs->log) { files_close(fs); return ASPER_ERR_IO; }
  if (os_file_size(fs->index_path, &size) == ASPER_OK && size >= 8 &&
      size <= ASPER_EVENT_LOG_BYTES && (size-8)%INDEX_RECORD == 0) {
    fs->count = (size-8)/INDEX_RECORD;
    fs->index = os_fopen(fs->index_path, "rb");
    if (fs->index && fread(magic, 1, 8, fs->index) == 8 &&
        !memcmp(magic, INDEX_MAGIC, 8)) {
      if (!fs->count && !fs->bytes) return ASPER_OK;
      if (index_read(fs, fs->count, &a, &b) && b == fs->bytes &&
          !fseek(fs->log, (long)a, SEEK_SET)) {
        asper_event e;
        err = asper_event_frame_read(fs->log, &e);
        int valid = err == ASPER_OK && e.sequence == fs->count &&
            ftell(fs->log) == (long)b;
        free(e.text);
        if (valid) return ASPER_OK;
      }
    }
  }
  if (fs->index) fclose(fs->index);
  fs->index = NULL;
  err = rebuild(fs, path);
  if (err != ASPER_OK) files_close(fs);
  return err;
}

asper_err asper_event_log_append(const char *path, asper_event *event) {
  event_files fs;
  asper_err err = files_open(&fs, path);
  FILE *f = NULL;
  uint64_t count = fs.count, bytes = fs.bytes;
  files_close(&fs);
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
    int ok = count || fwrite(INDEX_MAGIC, 1, 8, f) == 8;
    if (ok && end >= 0) (void)index_write(f, bytes, (uint64_t)end);
    fclose(f);
  }
  free(idx);
  return ASPER_OK;
}

asper_err asper_event_log_page(const char *path, const char *query,
    unsigned long long after, size_t limit, asper_event **out, size_t *n,
    unsigned long long *next) {
  event_files fs;
  asper_event *page = NULL;
  size_t used = 0, capacity = 0;
  asper_err err = files_open(&fs, path);
  *out = NULL; *n = 0; *next = after;
  if (err == ASPER_ERR_NOT_FOUND) return ASPER_OK;
  if (err != ASPER_OK) return err;
  uint64_t cursor = after;
  int rebuilt = 0;
  while (cursor < fs.count && used < limit) {
    uint64_t a, b;
    asper_event e;
    if (!index_read(&fs, cursor+1, &a, &b)) {
      if (rebuilt) { err = ASPER_ERR_PARSE; break; }
      fclose(fs.index); fs.index = NULL;
      err = rebuild(&fs, path); rebuilt = 1;
      if (err != ASPER_OK) break;
      continue;
    }
    if (fseek(fs.log, (long)a, SEEK_SET)) { err = ASPER_ERR_IO; break; }
    err = asper_event_frame_read(fs.log, &e);
    if (err != ASPER_OK) break;
    if (e.sequence != cursor+1 || ftell(fs.log) != (long)b) {
      free(e.text); err = ASPER_ERR_PARSE; break;
    }
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
  files_close(&fs);
  if (err != ASPER_OK) { asper_events_free(page, used); *next = after; return err; }
  *out = page; *n = used;
  return ASPER_OK;
}
