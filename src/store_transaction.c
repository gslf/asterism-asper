/* Compaction is a local multi-file transaction. Validate every backup before
 * restoring any target; retain backups until marker deletion is durable. */
#include "store_files.h"
#include <stdlib.h>
#include <string.h>
#define MARKER "compact.pending"
#define MAX_FILES 4096u

typedef struct { char *rel; char hash[65]; bool existed; } entry;
static char *backup_path(const char *target) {
  size_t n = strlen(target)+13;
  char *p = malloc(n); if (p) snprintf(p,n,"%s.compact.bak",target); return p;
}
static bool relative_valid(const char *rel) {
  if (!strcmp(rel,"identity.xcdn") || !strcmp(rel,"context.xcdn") || !strcmp(rel,"journal.xcdn")) return true;
  size_t n = strlen(rel);
  if (strncmp(rel,"projects/",9) || n <= 14 || strcmp(rel+n-5,".xcdn")) return false;
  char *slug = asper_strndup(rel+9,n-14);
  bool ok = slug && asper_slug_valid(slug); free(slug); return ok;
}
static void cleanup(asper_ctx *c, const char *rel) {
  char *target = os_path_join(c->store.root,rel);
  char *backup = target ? backup_path(target) : NULL;
  if (backup) (void)os_remove_file(backup);
  free(target); free(backup);
}
static asper_err parse(char *text, entry **out, size_t *count) {
  entry *rows = calloc(MAX_FILES,sizeof *rows);
  if (!rows) return ASPER_ERR_NOMEM;
  size_t used = 0; asper_err e = ASPER_OK;
  for (char *line = text; *line;) {
    char *end = strchr(line,'\n'), *rel;
    if (!end || used == MAX_FILES) { e = ASPER_ERR_PARSE; break; }
    *end = 0;
    bool exists = line[0] == 'E';
    if (exists && strlen(line) >= 68 && line[1] == ' ' && line[66] == ' ') {
      memcpy(rows[used].hash,line+2,64);
      for (size_t i = 0; i < 64; i++) {
        char h = rows[used].hash[i];
        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f'))) e = ASPER_ERR_PARSE;
      }
      rel = line+67;
    } else if (!strncmp(line,"M - ",4)) rel = line+4;
    else { e = ASPER_ERR_PARSE; break; }
    if (!relative_valid(rel)) e = ASPER_ERR_PARSE;
    for (size_t i = 0; i < used; i++) if (!strcmp(rows[i].rel,rel)) e = ASPER_ERR_PARSE;
    if (e != ASPER_OK) break;
    rows[used].existed = exists; rows[used++].rel = rel; line = end+1;
  }
  if (!used && e == ASPER_OK) e = ASPER_ERR_PARSE;
  if (e != ASPER_OK) free(rows); else { *out = rows; *count = used; }
  return e;
}
static asper_err restore(asper_ctx *c, const entry *row, bool apply) {
  char *target = os_path_join(c->store.root,row->rel);
  char *backup = target ? backup_path(target) : NULL;
  asper_err e = target && backup ? ASPER_OK : ASPER_ERR_NOMEM;
  if (e == ASPER_OK && row->existed) {
    e = asper_store_file_copy(backup,apply ? target : NULL,row->hash,NULL);
  } else if (e == ASPER_OK && apply) {
    e = os_remove_file(target);
    if (e == ASPER_ERR_NOT_FOUND) e = ASPER_OK;
  }
  free(target); free(backup); return e;
}
asper_err asper_compact_recover(asper_ctx *c) {
  char *path = os_path_join(c->store.root,MARKER), *text = NULL;
  entry *rows = NULL; size_t count = 0;
  if (!path) return ASPER_ERR_NOMEM;
  asper_err e = asper_store_file_read(path,true,1024u*1024u,&text,NULL);
  if (e == ASPER_ERR_NOT_FOUND) { free(path); return ASPER_OK; }
  if (e == ASPER_OK) e = parse(text,&rows,&count);
  for (int pass = 0; pass < 2 && e == ASPER_OK; pass++)
    for (size_t i = 0; i < count && e == ASPER_OK; i++) e = restore(c,&rows[i],pass != 0);
  if (e == ASPER_OK) e = os_remove_file(path);
  if (e == ASPER_OK) for (size_t i = 0; i < count; i++) cleanup(c,rows[i].rel);
  free(rows); free(text); free(path); return e;
}
asper_err asper_compact_begin(asper_ctx *c, char *const *rels, size_t count) {
  if (!count || count > MAX_FILES) return ASPER_ERR_LIMIT;
  for (size_t i = 0; i < count; i++) {
    if (!relative_valid(rels[i])) return ASPER_ERR_INVALID;
    for (size_t j = 0; j < i; j++) if (!strcmp(rels[j],rels[i])) return ASPER_ERR_INVALID;
  }
  char *path = os_path_join(c->store.root,MARKER);
  if (!path) return ASPER_ERR_NOMEM;
  if (os_file_exists(path)) { free(path); c->store.poisoned = true; return ASPER_ERR_IO; }
  asper_buf marker; asper_buf_init(&marker);
  asper_err e = ASPER_OK;
  for (size_t i = 0; e == ASPER_OK && i < count; i++) {
    char *target = os_path_join(c->store.root,rels[i]);
    char *backup = target ? backup_path(target) : NULL, hash[65];
    e = target && backup ? asper_store_file_copy(target,backup,NULL,hash) : ASPER_ERR_NOMEM;
    if (e == ASPER_ERR_NOT_FOUND) e = asper_buf_printf(&marker,"M - %s\n",rels[i]);
    else if (e == ASPER_OK) e = asper_buf_printf(&marker,"E %s %s\n",hash,rels[i]);
    free(target); free(backup);
  }
  if (e == ASPER_OK) e = asper_store_file_write(c,path,marker.data,marker.len,true);
  if (e != ASPER_OK) {
    if (os_file_exists(path)) c->store.poisoned = true;
    else for (size_t i = 0; i < count; i++) cleanup(c,rels[i]);
  }
  free(path); asper_buf_free(&marker); return e;
}
asper_err asper_compact_commit(asper_ctx *c, char *const *rels, size_t count) {
  char *path = os_path_join(c->store.root,MARKER);
  if (!path) return ASPER_ERR_NOMEM;
  asper_err e = os_remove_file(path); free(path);
  if (e == ASPER_OK) for (size_t i = 0; i < count; i++) cleanup(c,rels[i]);
  return e;
}
