/*
 * source.c — scoped lossless event memory, content-addressed objects,
 * checkpoints, and bounded context materialization.
 *
 * The event log is deliberately independent from curated #memory records.
 * It is the immutable source of truth; every other representation is a
 * rebuildable view.  Frames are length-delimited so arbitrary UTF-8 text,
 * including newlines, round-trips byte-for-byte.  A torn final frame is
 * truncated before the next read/write, matching the main journal policy.
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"

#define EVENT_MAGIC "AEV1"
#define OBJECT_PREFIX "sha256:"

typedef struct {
  asper_event *v;
  size_t n;
  size_t cap;
  size_t good_bytes;
  unsigned long long next_sequence;
} event_scan;

static int scope_valid(const char *scope) {
  size_t n;
  if (!scope || !(n = strlen(scope)) || n > 64) return 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)scope[i];
    if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) return 0;
  }
  return strcmp(scope, ".") != 0 && strcmp(scope, "..") != 0;
}

static int object_ref_valid(const char *ref) {
  if (!ref || strncmp(ref, OBJECT_PREFIX, 7) != 0 || strlen(ref) != 71)
    return 0;
  for (size_t i = 7; i < 71; i++)
    if (!isxdigit((unsigned char)ref[i])) return 0;
  return 1;
}

static const char *event_kind_name(asper_event_kind kind) {
  switch (kind) {
    case ASPER_EVENT_USER: return "user";
    case ASPER_EVENT_ASSISTANT: return "assistant";
    case ASPER_EVENT_DECISION: return "decision";
    case ASPER_EVENT_TOOL_CALL: return "tool_call";
    case ASPER_EVENT_TOOL_RESULT: return "tool_result";
    case ASPER_EVENT_DIAGNOSTIC: return "diagnostic";
    case ASPER_EVENT_CHECKPOINT: return "checkpoint";
    case ASPER_EVENT_ARTIFACT: return "artifact";
  }
  return "event";
}

static char *source_dir(asper_ctx *c, const char *leaf) {
  char *base = os_path_join(c->store.root, leaf);
  if (!base) return NULL;
  if (os_mkdir_p(base) != ASPER_OK) {
    free(base);
    return NULL;
  }
  return base;
}

static char *scope_dir(asper_ctx *c, const char *scope) {
  char *base = source_dir(c, "scopes");
  char *dir;
  if (!base) return NULL;
  dir = os_path_join(base, scope);
  free(base);
  if (!dir) return NULL;
  if (os_mkdir_p(dir) != ASPER_OK) {
    free(dir);
    return NULL;
  }
  return dir;
}

static char *scope_path(asper_ctx *c, const char *scope, const char *name) {
  char *dir = scope_dir(c, scope);
  char *path;
  if (!dir) return NULL;
  path = os_path_join(dir, name);
  free(dir);
  return path;
}

/* The scope index makes restart discovery portable without relying on a
 * platform-specific directory enumerator. Caller holds source_mu. */
static asper_err register_scope_locked(asper_ctx *c, const char *scope) {
  char *dir = source_dir(c, "scopes");
  char *path = NULL, *data = NULL;
  size_t len = 0;
  FILE *f = NULL;
  asper_err e = ASPER_OK;
  int found = 0;
  if (!dir) return ASPER_ERR_IO;
  path = os_path_join(dir, "index.log");
  free(dir);
  if (!path) return ASPER_ERR_NOMEM;
  e = os_read_file(path, &data, &len);
  if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  if (e != ASPER_OK) goto out;
  if (data && len) {
    for (char *p = data, *end = data + len; p < end;) {
      char *nl = memchr(p, '\n', (size_t)(end - p));
      size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
      if (n == strlen(scope) && memcmp(p, scope, n) == 0) {
        found = 1;
        break;
      }
      p = nl ? nl + 1 : end;
    }
  }
  if (!found) {
    f = os_fopen(path, "ab");
    if (!f || fprintf(f, "%s\n", scope) < 0 || fflush(f) != 0 ||
        os_fsync(f) != ASPER_OK)
      e = ASPER_ERR_IO;
  }
out:
  if (f && fclose(f) != 0 && e == ASPER_OK) e = ASPER_ERR_IO;
  free(data);
  free(path);
  return e;
}

static char *object_path(asper_ctx *c, const char *ref) {
  char *dir;
  char name[69];
  char *path;
  if (!object_ref_valid(ref)) return NULL;
  dir = source_dir(c, "objects");
  if (!dir) return NULL;
  memcpy(name, ref + 7, 64);
  memcpy(name + 64, ".bin", 5);
  path = os_path_join(dir, name);
  free(dir);
  return path;
}

static void event_scan_free(event_scan *scan) {
  if (!scan) return;
  asper_events_free(scan->v, scan->n);
  memset(scan, 0, sizeof *scan);
}

void asper_events_free(asper_event *events, size_t n) {
  if (!events) return;
  for (size_t i = 0; i < n; i++) free(events[i].text);
  free(events);
}

static asper_err scan_push(event_scan *scan, const asper_event *event) {
  if (scan->n == scan->cap) {
    size_t cap = scan->cap ? scan->cap * 2 : 32;
    asper_event *nv = (asper_event *)realloc(scan->v, cap * sizeof *nv);
    if (!nv) return ASPER_ERR_NOMEM;
    scan->v = nv;
    scan->cap = cap;
  }
  scan->v[scan->n++] = *event;
  return ASPER_OK;
}

static const char *find_nl(const char *p, const char *end) {
  while (p < end && *p != '\n') p++;
  return p < end ? p : NULL;
}

/* Parse every complete frame.  A malformed mid-file frame is corruption;
 * an incomplete final frame is a recoverable torn tail. */
static asper_err event_scan_file(asper_ctx *c, const char *path,
                                 event_scan *scan) {
  char *data = NULL;
  size_t len = 0, off = 0;
  asper_err e;
  memset(scan, 0, sizeof *scan);
  scan->next_sequence = 1;
  e = os_read_file(path, &data, &len);
  if (e == ASPER_ERR_NOT_FOUND) return ASPER_OK;
  if (e != ASPER_OK) return e;
  while (off < len) {
    const char *line = data + off;
    const char *end = data + len;
    const char *nl = find_nl(line, end);
    char magic[5] = {0};
    unsigned long long seq = 0;
    long long at = 0;
    int kind = -1, pinned = 0, consumed = 0;
    char id[37] = {0};
    size_t obj_len = 0, text_len = 0, header_len, frame_len;
    asper_event ev;
    if (!nl) break;
    header_len = (size_t)(nl - line) + 1;
    if (sscanf(line, "%4s %llu %lld %d %d %36s %zu %zu%n", magic, &seq,
               &at, &kind, &pinned, id, &obj_len, &text_len, &consumed) != 8 ||
        strcmp(magic, EVENT_MAGIC) != 0 || consumed <= 0 ||
        line + consumed != nl || seq == 0 || !asper_uuid_valid(id) ||
        kind < ASPER_EVENT_USER || kind > ASPER_EVENT_ARTIFACT ||
        (pinned != 0 && pinned != 1) || obj_len > 71 ||
        obj_len > SIZE_MAX - text_len - header_len - 1) {
      free(data);
      event_scan_free(scan);
      return asper_seterr(c, ASPER_ERR_PARSE,
                          "source: malformed event frame at byte %zu", off);
    }
    frame_len = header_len + obj_len + text_len + 1;
    if (frame_len > len - off) break;
    if (data[off + frame_len - 1] != '\n') {
      free(data);
      event_scan_free(scan);
      return asper_seterr(c, ASPER_ERR_PARSE,
                          "source: malformed event terminator at byte %zu",
                          off);
    }
    memset(&ev, 0, sizeof ev);
    memcpy(ev.id, id, sizeof ev.id);
    ev.sequence = seq;
    ev.at = at;
    ev.kind = (asper_event_kind)kind;
    ev.pinned = pinned;
    if (obj_len) {
      memcpy(ev.object_ref, data + off + header_len, obj_len);
      ev.object_ref[obj_len] = '\0';
      if (!object_ref_valid(ev.object_ref)) {
        free(data);
        event_scan_free(scan);
        return asper_seterr(c, ASPER_ERR_PARSE,
                            "source: invalid object ref at byte %zu", off);
      }
    }
    ev.text = (char *)malloc(text_len + 1);
    if (!ev.text) {
      free(data);
      event_scan_free(scan);
      return ASPER_ERR_NOMEM;
    }
    memcpy(ev.text, data + off + header_len + obj_len, text_len);
    ev.text[text_len] = '\0';
    if (!asper_utf8_count(ev.text, NULL)) {
      free(ev.text);
      free(data);
      event_scan_free(scan);
      return asper_seterr(c, ASPER_ERR_PARSE,
                          "source: invalid UTF-8 event at byte %zu", off);
    }
    e = scan_push(scan, &ev);
    if (e != ASPER_OK) {
      free(ev.text);
      free(data);
      event_scan_free(scan);
      return e;
    }
    if (seq >= scan->next_sequence) scan->next_sequence = seq + 1;
    off += frame_len;
    scan->good_bytes = off;
  }
  free(data);
  if (off < len) {
    e = os_truncate(path, (uint64_t)scan->good_bytes);
    if (e != ASPER_OK) {
      event_scan_free(scan);
      return asper_seterr(c, e, "source: cannot repair torn event tail");
    }
    asper_log(c, ASPER_LOG_WARN, "source",
              "repaired torn event tail: %zu byte(s) removed",
              len - scan->good_bytes);
  }
  return ASPER_OK;
}

static void apply_pin_log(asper_ctx *c, const char *scope, event_scan *scan) {
  char *path = scope_path(c, scope, "pins.log");
  char *data = NULL;
  size_t len = 0;
  if (!path) return;
  if (os_read_file(path, &data, &len) == ASPER_OK) {
    char *p = data;
    char *end = data + len;
    while (p < end) {
      char id[37] = {0};
      int value = 0, used = 0;
      if (sscanf(p, "%36s %d%n", id, &value, &used) != 2 || used <= 0)
        break;
      for (size_t i = 0; i < scan->n; i++)
        if (strcmp(scan->v[i].id, id) == 0) scan->v[i].pinned = value != 0;
      while (p < end && *p != '\n') p++;
      if (p < end) p++;
    }
  }
  free(data);
  free(path);
}

asper_err asper_event_append(asper_ctx *c, const asper_event_input *event,
                             char out_id[37]) {
  char id[37];
  char *path = NULL;
  FILE *f = NULL;
  event_scan scan;
  asper_err e;
  size_t text_len, obj_len;
  long long at;
  if (!c || !event || !scope_valid(event->scope) || !event->text)
    return c ? asper_seterr(c, ASPER_ERR_INVALID,
                            "source: invalid event arguments")
             : ASPER_ERR_INVALID;
  if (event->kind < ASPER_EVENT_USER || event->kind > ASPER_EVENT_ARTIFACT ||
      !asper_utf8_count(event->text, NULL) ||
      (event->object_ref && event->object_ref[0] &&
       !object_ref_valid(event->object_ref)))
    return asper_seterr(c, ASPER_ERR_INVALID, "source: invalid event");
  path = scope_path(c, event->scope, "events.log");
  if (!path) return asper_seterr(c, ASPER_ERR_IO,
                                 "source: cannot create scope directory");
  os_mutex_lock(&c->source_mu);
  e = register_scope_locked(c, event->scope);
  if (e != ASPER_OK) goto out;
  e = event_scan_file(c, path, &scan);
  if (e != ASPER_OK) goto out;
  asper_uuid_v4(id);
  at = (long long)asper_clock_now(&c->clock);
  text_len = strlen(event->text);
  obj_len = event->object_ref ? strlen(event->object_ref) : 0;
  f = os_fopen(path, "ab");
  if (!f) {
    e = ASPER_ERR_IO;
    goto out_scan;
  }
  if (fprintf(f, EVENT_MAGIC " %llu %lld %d %d %s %zu %zu\n",
              scan.next_sequence, at, (int)event->kind,
              event->pinned ? 1 : 0, id, obj_len, text_len) < 0 ||
      (obj_len && fwrite(event->object_ref, 1, obj_len, f) != obj_len) ||
      (text_len && fwrite(event->text, 1, text_len, f) != text_len) ||
      fputc('\n', f) == EOF || fflush(f) != 0 || os_fsync(f) != ASPER_OK) {
    e = ASPER_ERR_IO;
    goto out_scan;
  }
  if (out_id) memcpy(out_id, id, 37);
  e = ASPER_OK;
out_scan:
  event_scan_free(&scan);
out:
  if (f && fclose(f) != 0 && e == ASPER_OK) e = ASPER_ERR_IO;
  os_mutex_unlock(&c->source_mu);
  free(path);
  if (e != ASPER_OK)
    return asper_seterr(c, e, "source: event append failed");
  if (event->kind == ASPER_EVENT_USER ||
      event->kind == ASPER_EVENT_ASSISTANT) {
    asper_err qe = asper_enqueue_turn(
        c, event->kind == ASPER_EVENT_ASSISTANT ? ASPER_ROLE_ASSISTANT
                                                : ASPER_ROLE_USER,
        event->text, (asper_time)at, id);
    if (qe != ASPER_OK)
      asper_log(c, ASPER_LOG_WARN, "source",
                "durable event %s awaits later curation: %s", id,
                asper_err_name(qe));
  }
  return ASPER_OK;
}

asper_err asper_event_list(asper_ctx *c, const char *scope,
                           asper_event **out, size_t *out_n) {
  char *path;
  event_scan scan;
  asper_err e;
  if (!c || !out || !out_n || !scope_valid(scope)) return ASPER_ERR_INVALID;
  *out = NULL;
  *out_n = 0;
  path = scope_path(c, scope, "events.log");
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  e = event_scan_file(c, path, &scan);
  if (e == ASPER_OK) apply_pin_log(c, scope, &scan);
  os_mutex_unlock(&c->source_mu);
  free(path);
  if (e != ASPER_OK) return e;
  *out = scan.v;
  *out_n = scan.n;
  return ASPER_OK;
}

asper_err asper_event_set_pinned(asper_ctx *c, const char *scope,
                                 const char *event_id, int pinned) {
  asper_event *events = NULL;
  size_t n = 0;
  char *path = NULL;
  FILE *f = NULL;
  asper_err e;
  int found = 0;
  if (!c || !scope_valid(scope) || !asper_uuid_valid(event_id))
    return ASPER_ERR_INVALID;
  e = asper_event_list(c, scope, &events, &n);
  if (e != ASPER_OK) return e;
  for (size_t i = 0; i < n; i++)
    if (strcmp(events[i].id, event_id) == 0) found = 1;
  asper_events_free(events, n);
  if (!found) return ASPER_ERR_NOT_FOUND;
  path = scope_path(c, scope, "pins.log");
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  f = os_fopen(path, "ab");
  if (!f || fprintf(f, "%s %d\n", event_id, pinned ? 1 : 0) < 0 ||
      fflush(f) != 0 || os_fsync(f) != ASPER_OK)
    e = ASPER_ERR_IO;
  else
    e = ASPER_OK;
  if (f && fclose(f) != 0 && e == ASPER_OK) e = ASPER_ERR_IO;
  os_mutex_unlock(&c->source_mu);
  free(path);
  return e;
}

static void hash_hex(const unsigned char hash[32], char out[65]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    out[i * 2] = digits[hash[i] >> 4];
    out[i * 2 + 1] = digits[hash[i] & 15];
  }
  out[64] = '\0';
}

asper_err asper_object_put(asper_ctx *c, const void *data, size_t size,
                           char out_ref[72]) {
  unsigned char hash[32], existing[32];
  char hex[65], ref[72], tmp_name[112];
  char *path = NULL, *dir = NULL, *tmp = NULL;
  asper_err e = ASPER_OK;
  if (!c || (!data && size) || !out_ref) return ASPER_ERR_INVALID;
  asper_sha256(data, size, hash);
  hash_hex(hash, hex);
  snprintf(ref, sizeof ref, OBJECT_PREFIX "%s", hex);
  path = object_path(c, ref);
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  if (os_file_exists(path)) {
    e = asper_sha256_file(path, existing);
    if (e == ASPER_OK && memcmp(hash, existing, sizeof hash) != 0)
      e = ASPER_ERR_PARSE;
    goto out;
  }
  dir = source_dir(c, "objects");
  if (!dir) {
    e = ASPER_ERR_IO;
    goto out;
  }
  {
    char uuid[37];
    asper_uuid_v4(uuid);
    snprintf(tmp_name, sizeof tmp_name, ".%s.tmp.%s", hex, uuid);
  }
  tmp = os_path_join(dir, tmp_name);
  if (!tmp) {
    e = ASPER_ERR_NOMEM;
    goto out;
  }
  e = os_write_file(tmp, data, size);
  if (e == ASPER_OK) e = os_file_replace(tmp, path);
  if (e != ASPER_OK) (void)os_remove_file(tmp);
out:
  os_mutex_unlock(&c->source_mu);
  free(tmp);
  free(dir);
  free(path);
  if (e != ASPER_OK)
    return asper_seterr(c, e, "source: object write failed");
  memcpy(out_ref, ref, sizeof ref);
  return ASPER_OK;
}

asper_err asper_object_read(asper_ctx *c, const char *object_ref,
                            size_t offset, size_t max_bytes,
                            void **out_data, size_t *out_size) {
  char *path, *all = NULL;
  size_t len = 0, take;
  void *copy;
  asper_err e;
  if (!c || !out_data || !out_size || !object_ref_valid(object_ref))
    return ASPER_ERR_INVALID;
  *out_data = NULL;
  *out_size = 0;
  path = object_path(c, object_ref);
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  e = os_read_file(path, &all, &len);
  os_mutex_unlock(&c->source_mu);
  free(path);
  if (e != ASPER_OK) return e;
  if (offset > len) {
    free(all);
    return ASPER_ERR_INVALID;
  }
  take = len - offset;
  if (max_bytes && take > max_bytes) take = max_bytes;
  copy = malloc(take ? take : 1);
  if (!copy) {
    free(all);
    return ASPER_ERR_NOMEM;
  }
  if (take) memcpy(copy, all + offset, take);
  free(all);
  *out_data = copy;
  *out_size = take;
  return ASPER_OK;
}

static asper_err atomic_text(asper_ctx *c, const char *path,
                             const char *text) {
  char *tmp;
  size_t n = strlen(path);
  asper_err e;
  tmp = (char *)malloc(n + 5);
  if (!tmp) return ASPER_ERR_NOMEM;
  memcpy(tmp, path, n);
  memcpy(tmp + n, ".tmp", 5);
  e = os_write_file(tmp, text, strlen(text));
  if (e == ASPER_OK) e = os_file_replace(tmp, path);
  if (e != ASPER_OK) (void)os_remove_file(tmp);
  free(tmp);
  if (e != ASPER_OK) return asper_seterr(c, e, "source: atomic write failed");
  return ASPER_OK;
}

asper_err asper_checkpoint_commit(asper_ctx *c, const char *scope,
                                   const char *text_utf8,
                                   char out_event_id[37]) {
  asper_event_input event;
  char object_ref[72];
  char event_id[37];
  char *path;
  asper_err e;
  if (!c || !scope_valid(scope) || !text_utf8 ||
      !asper_utf8_count(text_utf8, NULL)) return ASPER_ERR_INVALID;
  e = asper_object_put(c, text_utf8, strlen(text_utf8), object_ref);
  if (e != ASPER_OK) return e;
  memset(&event, 0, sizeof event);
  event.scope = scope;
  event.kind = ASPER_EVENT_CHECKPOINT;
  event.text = text_utf8;
  event.object_ref = object_ref;
  e = asper_event_append(c, &event, event_id);
  if (e != ASPER_OK) return e;
  path = scope_path(c, scope, "checkpoint.txt");
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  e = atomic_text(c, path, text_utf8);
  os_mutex_unlock(&c->source_mu);
  free(path);
  if (e == ASPER_OK && out_event_id) memcpy(out_event_id, event_id, 37);
  return e;
}

asper_err asper_checkpoint_load(asper_ctx *c, const char *scope,
                                 char **out_text) {
  char *path;
  asper_err e;
  if (!c || !out_text || !scope_valid(scope)) return ASPER_ERR_INVALID;
  *out_text = NULL;
  path = scope_path(c, scope, "checkpoint.txt");
  if (!path) return ASPER_ERR_IO;
  os_mutex_lock(&c->source_mu);
  e = os_read_file(path, out_text, NULL);
  os_mutex_unlock(&c->source_mu);
  free(path);
  if (e == ASPER_ERR_NOT_FOUND) {
    asper_event *events = NULL;
    size_t n = 0;
    e = asper_event_list(c, scope, &events, &n);
    if (e != ASPER_OK) return e;
    for (size_t i = n; i > 0; i--)
      if (events[i - 1].kind == ASPER_EVENT_CHECKPOINT) {
        *out_text = asper_strdup(events[i - 1].text);
        break;
      }
    asper_events_free(events, n);
    if (!*out_text) return ASPER_ERR_NOT_FOUND;
    return ASPER_OK;
  }
  return e;
}

static size_t count_tokens(const asper_context_request *req,
                           const char *text) {
  int n;
  if (!text || !text[0]) return 0;
  if (req->count_tokens) {
    n = req->count_tokens(text, req->count_userdata);
    if (n >= 0) return (size_t)n;
  }
  return (strlen(text) + 3) / 4;
}

static asper_err append_event_text(asper_buf *b, const asper_event *event) {
  const char *name = event_kind_name(event->kind);
  asper_err e = asper_buf_printf(b, "[%s:%s] ", name, event->id);
  if (e == ASPER_OK && event->text && event->text[0])
    e = asper_buf_appends(b, event->text);
  if (e == ASPER_OK && event->object_ref[0])
    e = asper_buf_printf(b, " [source %s]", event->object_ref);
  if (e == ASPER_OK) e = asper_buf_appendc(b, '\n');
  return e;
}

asper_err asper_context_materialize(asper_ctx *c,
                                    const asper_context_request *request,
                                    asper_context_pack *out) {
  asper_event *events = NULL;
  size_t n = 0, history_used = 0, included = 0;
  unsigned char *take = NULL;
  char *checkpoint = NULL;
  asper_buf context;
  asper_err e;
  if (!c || !request || !out || !scope_valid(request->scope) ||
      !request->query) return ASPER_ERR_INVALID;
  memset(out, 0, sizeof *out);
  e = asper_memory_render(c,
                          request->base_system_prompt
                              ? request->base_system_prompt : "",
                          request->query, &out->system_prompt);
  if (e != ASPER_OK) goto fail;
  out->system_tokens = count_tokens(request, out->system_prompt);
  e = asper_event_list(c, request->scope, &events, &n);
  if (e != ASPER_OK) goto fail;
  out->events_available = n;
  take = (unsigned char *)calloc(n ? n : 1, 1);
  if (!take) {
    e = ASPER_ERR_NOMEM;
    goto fail;
  }
  asper_buf_init(&context);
  if (request->checkpoint_tokens > 0 &&
      asper_checkpoint_load(c, request->scope, &checkpoint) == ASPER_OK &&
      checkpoint && checkpoint[0]) {
    size_t cost = count_tokens(request, checkpoint) + 6;
    if (cost <= request->checkpoint_tokens) {
      e = asper_buf_appends(&context, "## Working checkpoint\n");
      if (e == ASPER_OK) e = asper_buf_appends(&context, checkpoint);
      if (e == ASPER_OK) e = asper_buf_appends(&context, "\n\n");
      if (e != ASPER_OK) goto fail_context;
    }
  }
  /* Pinned source events have priority.  Within each class preserve original
   * order; then fill the remaining envelope from the newest exact tail. */
  for (size_t i = 0; i < n; i++) {
    asper_buf line;
    size_t cost;
    if (!events[i].pinned || events[i].kind == ASPER_EVENT_CHECKPOINT)
      continue;
    asper_buf_init(&line);
    e = append_event_text(&line, &events[i]);
    if (e != ASPER_OK) {
      asper_buf_free(&line);
      goto fail_context;
    }
    cost = count_tokens(request, line.data);
    asper_buf_free(&line);
    if (history_used + cost <= request->history_tokens) {
      take[i] = 1;
      history_used += cost;
    }
  }
  for (size_t i = n; i > 0; i--) {
    size_t j = i - 1;
    asper_buf line;
    size_t cost;
    if (take[j] || events[j].kind == ASPER_EVENT_CHECKPOINT) continue;
    if ((events[j].kind == ASPER_EVENT_USER ||
         events[j].kind == ASPER_EVENT_ASSISTANT) &&
        strcmp(events[j].text, request->query) == 0)
      continue;
    asper_buf_init(&line);
    e = append_event_text(&line, &events[j]);
    if (e != ASPER_OK) {
      asper_buf_free(&line);
      goto fail_context;
    }
    cost = count_tokens(request, line.data);
    asper_buf_free(&line);
    if (history_used + cost <= request->history_tokens) {
      take[j] = 1;
      history_used += cost;
    }
  }
  if (n > 0) {
    int heading = 0;
    for (size_t i = 0; i < n; i++) {
      if (!take[i]) continue;
      if (!heading) {
        e = asper_buf_appends(&context, "## Memory source events\n");
        if (e != ASPER_OK) goto fail_context;
        heading = 1;
      }
      e = append_event_text(&context, &events[i]);
      if (e != ASPER_OK) goto fail_context;
      included++;
    }
  }
  out->context_text = asper_buf_detach(&context);
  if (!out->context_text) out->context_text = asper_strdup("");
  if (!out->context_text) {
    e = ASPER_ERR_NOMEM;
    goto fail_context_detached;
  }
  out->context_tokens = count_tokens(request, out->context_text);
  out->events_included = included;
  free(checkpoint);
  free(take);
  asper_events_free(events, n);
  return ASPER_OK;

fail_context:
  asper_buf_free(&context);
fail_context_detached:
  free(checkpoint);
fail:
  free(take);
  asper_events_free(events, n);
  asper_context_pack_free(out);
  return e;
}

void asper_context_pack_free(asper_context_pack *pack) {
  if (!pack) return;
  free(pack->system_prompt);
  free(pack->context_text);
  memset(pack, 0, sizeof *pack);
}

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
  scopes_dir = source_dir(c, "scopes");
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
    if (!scope_valid(scope)) {
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
                             (asper_time)events[i].at, events[i].id);
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
