/* Rebuildable offset index. The log is durable before its index is updated.
 * A page allocates one frame at a time and retains only matching events. */
#include <stdlib.h>
#include <string.h>
#include "event_index.h"

#define INDEX_RECORD 48u

static void put64(uint8_t *p, uint64_t n) {
  for (int i = 0; i < 8; i++, n >>= 8) p[i] = (uint8_t)n;
}
static uint64_t get64(const uint8_t *p) {
  uint64_t n = 0;
  for (int i = 7; i >= 0; i--) n = (n << 8) | p[i];
  return n;
}
int asper_event_index_write(FILE *f, uint64_t start, uint64_t end) {
  uint8_t row[INDEX_RECORD];
  put64(row, start); put64(row+8, end);
  asper_sha256(row, 16, row+16);
  return fwrite(row, 1, sizeof row, f) == sizeof row;
}
static int index_read(asper_event_files *fs, uint64_t seq, uint64_t *a, uint64_t *b) {
  uint8_t row[INDEX_RECORD], hash[32];
  if (!seq || seq > fs->count ||
      fseek(fs->index, (long)(8 + (seq-1)*INDEX_RECORD), SEEK_SET) ||
      fread(row, 1, sizeof row, fs->index) != sizeof row) return 0;
  asper_sha256(row, 16, hash);
  *a = get64(row); *b = get64(row+8);
  return !memcmp(hash, row+16, 32) && *a < *b && *b <= fs->bytes;
}
void asper_event_files_close(asper_event_files *fs) {
  if (fs->log) fclose(fs->log);
  if (fs->index) fclose(fs->index);
  free(fs->index_path);
  memset(fs, 0, sizeof *fs);
}

/* A complete damaged frame is never discarded as a torn tail. */
static asper_err rebuild(asper_event_files *fs, const char *path) {
  size_t len = strlen(fs->index_path);
  char *tmp = malloc(len + 5);
  FILE *index;
  uint64_t good = 0, count = 0;
  asper_err err = ASPER_OK;
  if (!tmp) return ASPER_ERR_NOMEM;
  memcpy(tmp, fs->index_path, len); memcpy(tmp+len, ".tmp", 5);
  index = os_fopen(tmp, "wb");
  if (!index) { free(tmp); return ASPER_ERR_IO; }
  if (fwrite(ASPER_EVENT_INDEX_MAGIC, 1, 8, index) != 8) err = ASPER_ERR_IO;
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
      if (err == ASPER_OK) err = os_stream_stamp(fs->log, &fs->stamp);
      break;
    }
    if (err != ASPER_OK) break;
    long end = ftell(fs->log);
    if (e.sequence != count+1 || end < 0 || (uint64_t)end > fs->bytes)
      err = ASPER_ERR_PARSE;
    else if (!asper_event_index_write(index, good, (uint64_t)end)) err = ASPER_ERR_IO;
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

static asper_err files_open(asper_event_files *fs, const char *path) {
  uint64_t size = 0, a, b;
  char magic[8];
  size_t len = strlen(path);
  asper_err err;
  memset(fs, 0, sizeof *fs);
  err = os_blob_open(path, &fs->log, &fs->bytes);
  if (err == ASPER_ERR_NOT_FOUND) return ASPER_ERR_NOT_FOUND;
  if (err != ASPER_OK) return err;
  err = os_stream_stamp(fs->log, &fs->stamp);
  if (err != ASPER_OK) { asper_event_files_close(fs); return err; }
  if (fs->bytes > ASPER_EVENT_LOG_BYTES) { asper_event_files_close(fs); return ASPER_ERR_LIMIT; }
  fs->index_path = malloc(len + 5);
  if (!fs->index_path) { asper_event_files_close(fs); return ASPER_ERR_NOMEM; }
  memcpy(fs->index_path, path, len); memcpy(fs->index_path+len, ".idx", 5);
  err = os_blob_open(fs->index_path, &fs->index, &size);
  if (err != ASPER_OK && err != ASPER_ERR_NOT_FOUND) { asper_event_files_close(fs); return err; }
  if (err == ASPER_OK && size >= 8 &&
      size <= ASPER_EVENT_LOG_BYTES && (size-8)%INDEX_RECORD == 0) {
    fs->count = (size-8)/INDEX_RECORD;
    if (fs->index && fread(magic, 1, 8, fs->index) == 8 &&
        !memcmp(magic, ASPER_EVENT_INDEX_MAGIC, 8)) {
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
  if (err != ASPER_OK) asper_event_files_close(fs);
  return err;
}

asper_err asper_event_files_open(asper_event_files *fs, const char *path) {
  asper_err e = files_open(fs, path);
  if (e == ASPER_OK) {
    os_file_stamp after;
    e = os_stream_stamp(fs->log, &after);
    if (e == ASPER_OK && (after.size != fs->bytes || memcmp(&after, &fs->stamp, sizeof after)))
      e = ASPER_ERR_PARSE;
    if (e != ASPER_OK) asper_event_files_close(fs);
  }
  return e;
}

static asper_err read_event(asper_event_files *fs, const char *path,
                           uint64_t sequence, asper_event *event, bool payload) {
  uint64_t start, end;
  memset(event, 0, sizeof *event);
  if (!sequence || sequence > fs->count) return ASPER_ERR_NOT_FOUND;
  os_file_stamp current;
  asper_err e = os_stream_stamp(fs->log, &current);
  if (e != ASPER_OK) return e;
  /* Cooperating writers only append. Shrinkage, unlinking or an equal-size
   * rewrite invalidates a captured prefix, even if stdio cached old bytes. */
  if (current.size < fs->stamp.size || current.links != fs->stamp.links ||
      (current.size == fs->stamp.size &&
       (memcmp(current.modified, fs->stamp.modified, sizeof current.modified) ||
        memcmp(current.changed, fs->stamp.changed, sizeof current.changed)))) return ASPER_ERR_PARSE;
  if (!index_read(fs, sequence, &start, &end)) {
    fclose(fs->index); fs->index = NULL;
    e = rebuild(fs, path);
    if (e != ASPER_OK) return e;
    if (!index_read(fs, sequence, &start, &end)) return ASPER_ERR_PARSE;
  }
  if (fseek(fs->log, (long)start, SEEK_SET)) return ASPER_ERR_IO;
  size_t object_bytes = 0, text_bytes = 0;
  char hash[65];
  e = payload ? asper_event_frame_read(fs->log, event) :
      asper_event_frame_head(fs->log, event, &object_bytes, &text_bytes, hash);
  long position = ftell(fs->log);
  uint64_t expected_end = (uint64_t)position + (payload ? 0 : object_bytes + text_bytes + 1);
  if (e == ASPER_OK && (event->sequence != sequence || position < 0 || expected_end != end)) e = ASPER_ERR_PARSE;
  if (e == ASPER_OK) {
    os_file_stamp after;
    e = os_stream_stamp(fs->log, &after);
    if (e == ASPER_OK && memcmp(&after, &current, sizeof after)) e = ASPER_ERR_PARSE;
    if (e == ASPER_OK) fs->stamp = after;
  }
  if (e == ASPER_ERR_NOT_FOUND) e = ASPER_ERR_PARSE; /* An indexed frame must be complete. */
  if (e != ASPER_OK) { free(event->text); event->text = NULL; }
  return e;
}

asper_err asper_event_files_read(asper_event_files *fs, const char *path,
                                uint64_t sequence, asper_event *event) {
  return read_event(fs, path, sequence, event, true);
}
asper_err asper_event_files_head(asper_event_files *fs, const char *path,
                                uint64_t sequence, asper_event *event) {
  return read_event(fs, path, sequence, event, false);
}
