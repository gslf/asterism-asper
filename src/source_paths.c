/* Validated source names and store paths. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

int asper_source_scope_valid(const char *scope) {
  size_t n;
  if (!scope || !(n = strlen(scope)) || n > 64) return 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)scope[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) return 0;
  }
  return strcmp(scope, ".") != 0 && strcmp(scope, "..") != 0;
}

int asper_source_object_valid(const char *ref) {
  if (!ref || strncmp(ref, OBJECT_PREFIX, 7) != 0 || strlen(ref) != 71)
    return 0;
  for (size_t i = 7; i < 71; i++)
    if (!((ref[i] >= '0' && ref[i] <= '9') || (ref[i] >= 'a' && ref[i] <= 'f'))) return 0;
  return 1;
}

char *asper_source_dir(asper_ctx *c, const char *leaf) {
  char *base = os_path_join(c->store.root, leaf);
  if (!base) return NULL;
  if (os_mkdir_p(base) != ASPER_OK) {
    free(base);
    return NULL;
  }
  return base;
}

static char *scope_dir(asper_ctx *c, const char *scope) {
  char *base = asper_source_dir(c, "scopes");
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

char *asper_source_scope_path(asper_ctx *c, const char *scope, const char *name) {
  char *dir = scope_dir(c, scope);
  char *path;
  if (!dir) return NULL;
  path = os_path_join(dir, name);
  free(dir);
  return path;
}

char *asper_source_object_path(asper_ctx *c, const char *ref) {
  char *dir;
  char name[69];
  char *path;
  if (!asper_source_object_valid(ref)) return NULL;
  dir = asper_source_dir(c, "objects");
  if (!dir) return NULL;
  memcpy(name, ref + 7, 64);
  memcpy(name + 64, ".bin", 5);
  path = os_path_join(dir, name);
  free(dir);
  return path;
}

/* Metadata reads are bounded before allocation and must be complete UTF-8. */
asper_err asper_source_text_read(const char *path, size_t limit, char **out, size_t *size) {
  FILE *f = NULL; uint64_t bytes = 0;
  *out = NULL; if (size) *size = 0;
  asper_err e = os_blob_open(path, &f, &bytes);
  if (e != ASPER_OK) return e;
  char *text = NULL;
  if (bytes > limit || bytes >= SIZE_MAX) e = ASPER_ERR_LIMIT;
  else {
    text = malloc((size_t)bytes + 1);
    if (!text) e = ASPER_ERR_NOMEM;
    else if (fread(text, 1, (size_t)bytes, f) != bytes || fgetc(f) != EOF) e = ASPER_ERR_PARSE;
    else {
      text[bytes] = 0;
      if (memchr(text, 0, (size_t)bytes) || !asper_utf8_count(text, NULL)) e = ASPER_ERR_PARSE;
    }
  }
  if (ferror(f)) e = ASPER_ERR_IO;
  if (fclose(f) && e == ASPER_OK) e = ASPER_ERR_IO;
  if (e != ASPER_OK) free(text);
  else { *out = text; if (size) *size = (size_t)bytes; }
  return e;
}
