/* Validated source names and store paths. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

int asper_source_scope_valid(const char *scope) {
  size_t n;
  if (!scope || !(n = strlen(scope)) || n > 64) return 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)scope[i];
    if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) return 0;
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
