/* Content-addressed binary objects and exact range reads. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

static void hash_hex(const unsigned char hash[32], char out[65]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    out[i * 2] = digits[hash[i] >> 4];
    out[i * 2 + 1] = digits[hash[i] & 15];
  }
  out[64] = '\0';
}

/* Hash the entire object while retaining only the requested slice. A damaged
 * byte outside the slice still invalidates the content-addressed reference. */
static asper_err read_slice(FILE *f, size_t total, const char *expected, size_t offset,
                            size_t take, unsigned char *copy) {
  asper_sha256_ctx hash; asper_sha256_init(&hash);
  unsigned char buffer[8192], digest[32]; char hex[65];
  size_t position = 0;
  for (;;) {
    size_t n = fread(buffer, 1, sizeof buffer, f);
    if (n > total - position) return ASPER_ERR_PARSE;
    if (n) {
      size_t begin = offset > position ? offset : position;
      size_t end = position + n < offset + take ? position + n : offset + take;
      if (end > begin) memcpy(copy + begin - offset, buffer + begin - position, end - begin);
      asper_sha256_update(&hash, buffer, n); position += n;
    }
    if (n < sizeof buffer) {
      if (ferror(f)) return ASPER_ERR_IO;
      break;
    }
  }
  asper_sha256_final(&hash, digest); hash_hex(digest, hex);
  return position == total && !strcmp(hex, expected) ? ASPER_OK : ASPER_ERR_PARSE;
}

asper_err asper_object_put(asper_ctx *c, const void *data, size_t size,
                           char out_ref[72]) {
  unsigned char hash[32];
  char hex[65], ref[72], tmp_name[112];
  char *path = NULL, *dir = NULL, *tmp = NULL;
  asper_err e = ASPER_OK;
  if (!out_ref) return ASPER_ERR_INVALID;
  if (!c || (!data && size)) { out_ref[0] = 0; return ASPER_ERR_INVALID; }
  if (size > ASPER_OBJECT_BYTES) { out_ref[0] = 0; return ASPER_ERR_LIMIT; }
  asper_sha256(data, size, hash);
  hash_hex(hash, hex);
  snprintf(ref, sizeof ref, OBJECT_PREFIX "%s", hex);
  path = asper_source_object_path(c, ref);
  if (!path) { out_ref[0] = 0; return ASPER_ERR_IO; }
  os_mutex_lock(&c->source_mu);
  if (os_file_exists(path)) {
    FILE *f = NULL; uint64_t bytes = 0;
    e = os_blob_open(path, &f, &bytes);
    if (e == ASPER_OK && bytes > ASPER_OBJECT_BYTES) e = ASPER_ERR_LIMIT;
    if (e == ASPER_OK) e = read_slice(f, (size_t)bytes, hex, 0, 0, NULL);
    if (f && fclose(f) && e == ASPER_OK) e = ASPER_ERR_IO;
    goto out;
  }
  dir = asper_source_dir(c, "objects");
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
  if (e != ASPER_OK) {
    out_ref[0] = 0;
    return asper_seterr(c, e, "source: object write failed");
  }
  memcpy(out_ref, ref, sizeof ref);
  return ASPER_OK;
}

asper_err asper_object_read(asper_ctx *c, const char *object_ref,
                            size_t offset, size_t max_bytes,
                            void **out_data, size_t *out_size) {
  if (!c || !out_data || !out_size || !asper_source_object_valid(object_ref)) return ASPER_ERR_INVALID;
  *out_data = NULL; *out_size = 0;
  char *path = asper_source_object_path(c, object_ref);
  if (!path) return ASPER_ERR_IO;
  FILE *f = NULL; uint64_t bytes = 0; void *copy = NULL; size_t take = 0;
  os_mutex_lock(&c->source_mu);
  asper_err e = os_blob_open(path, &f, &bytes);
  if (e == ASPER_OK && bytes > ASPER_OBJECT_BYTES) e = ASPER_ERR_LIMIT;
  if (e == ASPER_OK && offset > bytes) e = ASPER_ERR_INVALID;
  if (e == ASPER_OK) {
    take = (size_t)bytes - offset;
    if (max_bytes && take > max_bytes) take = max_bytes;
    copy = malloc(take ? take : 1);
    e = copy ? read_slice(f, (size_t)bytes, object_ref + 7, offset, take, copy) : ASPER_ERR_NOMEM;
  }
  if (f && fclose(f) && e == ASPER_OK) e = ASPER_ERR_IO;
  os_mutex_unlock(&c->source_mu); free(path);
  if (e != ASPER_OK) free(copy);
  else { *out_data = copy; *out_size = take; }
  return e;
}
