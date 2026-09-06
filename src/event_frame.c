/* AEV2: bounded, length-delimited UTF-8 with metadata and payload integrity. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "event_log.h"

static int prefix(char out[192], const asper_event *e, size_t obj, size_t text) {
  return snprintf(out, 192, "AEV2 %llu %lld %d %d %s %zu %zu",
      e->sequence, (long long)e->at, (int)e->kind, e->pinned, e->id,
      obj, text);
}

static void hex_digest(const uint8_t bytes[32], char hex[65]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    hex[2*i] = digits[bytes[i] >> 4];
    hex[2*i+1] = digits[bytes[i] & 15];
  }
  hex[64] = 0;
}

static void digest(const char *head, const asper_event *e, char hex[65]) {
  asper_sha256_ctx hash;
  uint8_t bytes[32];
  asper_sha256_init(&hash);
  asper_sha256_update(&hash, head, strlen(head));
  asper_sha256_update(&hash, e->object_ref, strlen(e->object_ref));
  asper_sha256_update(&hash, e->text, strlen(e->text));
  asper_sha256_final(&hash, bytes);
  hex_digest(bytes, hex);
}

asper_err asper_event_frame_write(FILE *f, const asper_event *e) {
  char head[192], hash[65], header_hash[65];
  uint8_t bytes[32];
  size_t n = strlen(e->text), obj = strlen(e->object_ref);
  int len = prefix(head, e, obj, n);
  if (n > ASPER_EVENT_BYTES || len < 0 || len >= (int)sizeof head)
    return ASPER_ERR_INVALID;
  asper_sha256(head, (size_t)len, bytes);
  hex_digest(bytes, header_hash);
  digest(head, e, hash);
  if (fprintf(f, "%s %s %s\n", head, header_hash, hash) < 0 ||
      fwrite(e->object_ref, 1, obj, f) != obj ||
      fwrite(e->text, 1, n, f) != n || fputc('\n', f) == EOF)
    return ASPER_ERR_IO;
  return ASPER_OK;
}

static int number(char **p, unsigned long long max, unsigned long long *out) {
  char *end;
  if (!isdigit((unsigned char)**p)) return 0;
  errno = 0;
  *out = strtoull(*p, &end, 10);
  if (errno == ERANGE || *out > max || *end != ' ') return 0;
  *p = end+1;
  return 1;
}

asper_err asper_event_frame_head(FILE *f, asper_event *e, size_t *object_bytes,
                                  size_t *text_bytes, char hash[65]) {
  char line[352], header_hash[65], expected[65], head[192];
  uint8_t bytes[32];
  unsigned long long seq;
  long long at;
  size_t obj, text;
  unsigned long long value;
  char *p, *end;
  int used;
  memset(e, 0, sizeof *e);
  if (!fgets(line, sizeof line, f))
    return ferror(f) ? ASPER_ERR_IO : ASPER_ERR_NOT_FOUND;
  if (!strchr(line, '\n')) {
    size_t n = strlen(line);
    bool prefix_ok = !strncmp(line,"AEV2 ",n < 5 ? n : 5);
    return ferror(f) ? ASPER_ERR_IO :
        (feof(f) && prefix_ok ? ASPER_ERR_NOT_FOUND : ASPER_ERR_PARSE);
  }
  if (strncmp(line, "AEV2 ", 5)) return ASPER_ERR_PARSE;
  p = line+5;
  if (!number(&p, ULLONG_MAX, &seq) || !seq) return ASPER_ERR_PARSE;
  if (*p != '-' && !isdigit((unsigned char)*p)) return ASPER_ERR_PARSE;
  errno = 0;
  at = strtoll(p, &end, 10);
  if (errno == ERANGE || end == p || *end != ' ') return ASPER_ERR_PARSE;
  p = end+1;
  if (!number(&p, ASPER_EVENT_ARTIFACT, &value))
    return ASPER_ERR_PARSE;
  e->kind = (asper_event_kind)value;
  if (!number(&p, 1, &value)) return ASPER_ERR_PARSE;
  e->pinned = (int)value;
  if (strlen(p) < 37 || p[36] != ' ') return ASPER_ERR_PARSE;
  memcpy(e->id, p, 36); p += 37;
  if (!asper_uuid_valid(e->id) || !number(&p, 71, &value) ||
      (value != 0 && value != 71)) return ASPER_ERR_PARSE;
  obj = (size_t)value;
  if (!number(&p, ASPER_EVENT_BYTES, &value)) return ASPER_ERR_PARSE;
  text = (size_t)value;
  if (strlen(p) != 130 || p[64] != ' ' || p[129] != '\n')
    return ASPER_ERR_PARSE;
  memcpy(header_hash, p, 64); header_hash[64] = 0;
  memcpy(hash, p+65, 64); hash[64] = 0;
  used = (int)(p-line)+129;
  e->sequence = seq; e->at = at;
  int len = prefix(head, e, obj, text);
  if (len < 0 || len >= (int)sizeof head || used != len + 130 ||
      strncmp(line, head, (size_t)len) || line[len] != ' ')
    return ASPER_ERR_PARSE;
  /* Check length integrity before deciding whether the payload is torn. */
  asper_sha256(head, (size_t)len, bytes);
  hex_digest(bytes, expected);
  if (strcmp(expected, header_hash)) return ASPER_ERR_PARSE;
  *object_bytes = obj; *text_bytes = text;
  return ASPER_OK;
}

asper_err asper_event_frame_read(FILE *f, asper_event *e) {
  size_t obj = 0, text = 0;
  char hash[65], expected[65], head[192];
  asper_err err = asper_event_frame_head(f, e, &obj, &text, hash);
  if (err != ASPER_OK) return err;
  (void)prefix(head, e, obj, text);
  err = ASPER_ERR_PARSE;
  if (fread(e->object_ref, 1, obj, f) != obj) goto torn;
  if (obj) {
    if (strncmp(e->object_ref, "sha256:", 7)) goto done;
    for (size_t i = 7; i < obj; i++)
      if (!isxdigit((unsigned char)e->object_ref[i])) goto done;
  }
  e->text = malloc(text + 1);
  if (!e->text) return ASPER_ERR_NOMEM;
  if (fread(e->text, 1, text, f) != text) goto torn;
  e->text[text] = 0;
  int term = fgetc(f);
  if (term == EOF) goto torn;
  if (term != '\n' || memchr(e->text, 0, text) ||
      !asper_utf8_count(e->text, NULL)) goto done;
  digest(head, e, expected);
  if (strcmp(hash, expected)) goto done;
  return ASPER_OK;
torn:
  err = ferror(f) ? ASPER_ERR_IO : ASPER_ERR_NOT_FOUND;
done:
  free(e->text); e->text = NULL;
  return err;
}
