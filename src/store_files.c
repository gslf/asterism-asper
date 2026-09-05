/* Bounded snapshot I/O. Checked snapshots use one complete AEV2 frame;
 * snapshots and transaction markers never receive torn-tail repair. */
#include "store_files.h"
#include "event_log.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>

asper_err asper_store_file_read(const char *path, bool checked, size_t limit,
                               char **out, size_t *size) {
  if (!path || !out || limit > ASPER_STORE_FILE_MAX) return ASPER_ERR_INVALID;
  *out = NULL;
  if (size) *size = 0;
  FILE *f = os_fopen(path,"rb");
  if (!f) return errno == ENOENT ? ASPER_ERR_NOT_FOUND : ASPER_ERR_IO;
  asper_err e = ASPER_OK;
  long n;
  if (fseek(f,0,SEEK_END) || (n = ftell(f)) < 0 || fseek(f,0,SEEK_SET)) e = ASPER_ERR_IO;
  else if ((uint64_t)n > limit + (checked ? 512u : 0u)) e = ASPER_ERR_LIMIT;
  if (e == ASPER_OK && checked) {
    asper_event event;
    e = asper_event_frame_read(f,&event);
    if (e == ASPER_ERR_NOT_FOUND) e = ASPER_ERR_PARSE;
    if (e == ASPER_OK && (event.sequence != 1 || event.at || event.pinned ||
        event.kind != ASPER_EVENT_DIAGNOSTIC || event.object_ref[0] ||
        fgetc(f) != EOF || ferror(f) || strlen(event.text) > limit)) e = ASPER_ERR_PARSE;
    if (e == ASPER_OK) { *out = event.text; if (size) *size = strlen(event.text); }
    else free(event.text);
  } else if (e == ASPER_OK) {
    char *text = malloc((size_t)n+1);
    if (!text) e = ASPER_ERR_NOMEM;
    else if (fread(text,1,(size_t)n,f) != (size_t)n || fgetc(f) != EOF || ferror(f)) {
      free(text); e = ASPER_ERR_IO;
    } else { text[n] = 0; *out = text; if (size) *size = (size_t)n; }
  }
  if (fclose(f) && e == ASPER_OK) e = ASPER_ERR_IO;
  if (e != ASPER_OK) { free(*out); *out = NULL; if (size) *size = 0; }
  return e;
}
asper_err asper_store_file_write(asper_ctx *c, const char *path, const char *data,
                                size_t size, bool checked) {
  if (!path || !data) return ASPER_ERR_INVALID;
  if (size > (checked ? ASPER_SECTION_MAX : ASPER_STORE_FILE_MAX)) return ASPER_ERR_LIMIT;
  if (checked && (memchr(data,0,size) || strlen(data) != size)) return ASPER_ERR_INVALID;
  size_t n = strlen(path);
  char *tmp = malloc(n+5);
  if (!tmp) return ASPER_ERR_NOMEM;
  memcpy(tmp,path,n); memcpy(tmp+n,".tmp",5);
  FILE *f = os_fopen(tmp,"wb");
  asper_err e = f ? ASPER_OK : ASPER_ERR_IO;
  if (f) {
    if (checked) {
      asper_event event = {0}; event.sequence = 1; event.kind = ASPER_EVENT_DIAGNOSTIC;
      event.text = (char *)data; asper_uuid_v4(event.id);
      e = asper_event_frame_write(f,&event);
    } else if (fwrite(data,1,size,f) != size) e = ASPER_ERR_IO;
    if (e == ASPER_OK) e = os_fsync(f);
    if (fclose(f) && e == ASPER_OK) e = ASPER_ERR_IO;
  }
  if (e == ASPER_OK) e = os_file_replace(tmp,path);
  if (e != ASPER_OK) {
    (void)os_remove_file(tmp);
    if (c) asper_seterr(c,e,"store: cannot persist %s",path);
  }
  free(tmp); return e;
}

/* Copy or validate a backup with bounded working memory, even for a full WAL.
 * The destination becomes visible only after the complete source hash matches. */
asper_err asper_store_file_copy(const char *src, const char *dst,
                               const char *expected, char out_hash[65]) {
  FILE *in = os_fopen(src,"rb"), *out = NULL;
  if (!in) return errno == ENOENT ? ASPER_ERR_NOT_FOUND : ASPER_ERR_IO;
  char *tmp = NULL, buffer[16384], hash[65];
  uint8_t bytes[32]; asper_sha256_ctx digest; asper_sha256_init(&digest);
  asper_err e = ASPER_OK; size_t total = 0, count;
  if (dst) {
    size_t n = strlen(dst); tmp = malloc(n+5);
    if (!tmp) e = ASPER_ERR_NOMEM;
    else { memcpy(tmp,dst,n); memcpy(tmp+n,".tmp",5); out = os_fopen(tmp,"wb"); if (!out) e = ASPER_ERR_IO; }
  }
  while (e == ASPER_OK && (count = fread(buffer,1,sizeof buffer,in)) > 0) {
    if (count > ASPER_STORE_FILE_MAX-total) { e = ASPER_ERR_LIMIT; break; }
    total += count; asper_sha256_update(&digest,buffer,count);
    if (out && fwrite(buffer,1,count,out) != count) e = ASPER_ERR_IO;
  }
  if (ferror(in) && e == ASPER_OK) e = ASPER_ERR_IO;
  if (fclose(in) && e == ASPER_OK) e = ASPER_ERR_IO;
  asper_sha256_final(&digest,bytes);
  for (size_t i = 0; i < 32; i++) snprintf(hash+2*i,3,"%02x",bytes[i]);
  if (e == ASPER_OK && expected && strcmp(expected,hash)) e = ASPER_ERR_PARSE;
  if (out) {
    if (e == ASPER_OK) e = os_fsync(out);
    if (fclose(out) && e == ASPER_OK) e = ASPER_ERR_IO;
  }
  if (e == ASPER_OK && dst) e = os_file_replace(tmp,dst);
  if (e != ASPER_OK && tmp) (void)os_remove_file(tmp);
  if (e == ASPER_OK && out_hash) strcpy(out_hash,hash);
  free(tmp); return e;
}
