/* Read the checked operator policy once per open; live edits require restart. */
#include "source_deferred.h"
#include "store_files.h"
#include "asmodel_json.h"
#include <stdlib.h>
#include <string.h>

static const char *string(const asmodel_json_value *o, const char *key, size_t max) {
  const asmodel_json_value *v = asmodel_json_object_get(o,key);
  const char *s = asmodel_json_string_value(v);
  return s && *s && strlen(s) <= max && strlen(s) == asmodel_json_string_length(v) ? s : NULL;
}
static bool hash_valid(const char *s) {
  return s && strlen(s) == 64 && strspn(s,"0123456789abcdef") == 64;
}
static int compare(const void *left, const void *right) {
  const asper_source_deferral *a = left, *b = right;
  int scope = strcmp(a->scope,b->scope);
  return scope ? scope : strcmp(a->id,b->id);
}
const asper_source_deferral *asper_source_deferred_find(const asper_source_deferral *rows,
    size_t n, const char *scope, const char *id) {
  if (!n) return NULL;
  asper_source_deferral key = {0}; strcpy(key.scope,scope); strcpy(key.id,id);
  return bsearch(&key,rows,n,sizeof *rows,compare);
}
bool asper_source_deferred_matches(const asper_source_deferral *row, const asper_event *event) {
  if (row->sequence != event->sequence || !event->text ||
      (event->kind != ASPER_EVENT_USER && event->kind != ASPER_EVENT_ASSISTANT)) return false;
  asper_sha256_ctx hash; uint8_t digest[32], kind = (uint8_t)event->kind; char hex[65];
  asper_sha256_init(&hash); asper_sha256_update(&hash,&kind,1);
  asper_sha256_update(&hash,event->object_ref,strlen(event->object_ref)+1);
  asper_sha256_update(&hash,event->text,strlen(event->text));
  asper_sha256_final(&hash,digest);
  for (size_t i = 0; i < 32; i++) snprintf(hex+2*i,3,"%02x",digest[i]);
  return !strcmp(hex,row->hash);
}
asper_err asper_source_deferred_load(asper_ctx *c, asper_source_deferral **out, size_t *n) {
  *out = NULL; *n = 0;
  char *path = os_path_join(c->store.root,ASPER_DEFERRED_FILE), *text = NULL;
  if (!path) return ASPER_ERR_NOMEM;
  asper_err e = asper_store_file_read(path,true,ASPER_DEFERRED_BYTES,&text,NULL);
  free(path);
  if (e == ASPER_ERR_NOT_FOUND) return ASPER_OK;
  asmodel_json_value *doc = NULL;
  if (e == ASPER_OK && asmodel_json_parse(text,strlen(text),&doc)) e = ASPER_ERR_PARSE;
  const asmodel_json_value *schema = asmodel_json_object_get(doc,"schema");
  const asmodel_json_value *events = asmodel_json_object_get(doc,"events");
  size_t count = asmodel_json_array_len(events);
  if (e == ASPER_OK && (asmodel_json_object_count(doc) != 2 || !asmodel_json_is_int(schema) ||
      asmodel_json_int_value(schema) != 1 || asmodel_json_typeof(events) != ASMODEL_JSON_ARRAY ||
      count > ASPER_DEFERRED_MAX)) e = ASPER_ERR_PARSE;
  asper_source_deferral *rows = e == ASPER_OK && count ? calloc(count,sizeof *rows) : NULL;
  if (e == ASPER_OK && count && !rows) e = ASPER_ERR_NOMEM;
  for (size_t i = 0; e == ASPER_OK && i < count; i++) {
    const asmodel_json_value *item = asmodel_json_array_at(events,i);
    const char *scope = string(item,"scope",64), *id = string(item,"id",36);
    const char *hash = string(item,"sha256",64), *note = string(item,"note",1024);
    const asmodel_json_value *seq = asmodel_json_object_get(item,"sequence");
    if (asmodel_json_object_count(item) != 5 || !scope || !asper_source_scope_valid(scope) ||
        !id || !asper_uuid_valid(id) || !hash_valid(hash) || !note || asper_str_blank(note) ||
        !asmodel_json_is_int(seq) || asmodel_json_int_value(seq) <= 0) { e = ASPER_ERR_PARSE; break; }
    strcpy(rows[i].scope,scope); strcpy(rows[i].id,id); strcpy(rows[i].hash,hash);
    rows[i].sequence = (uint64_t)asmodel_json_int_value(seq);
    asper_source_view view; asper_event event = {0};
    e = asper_source_view_open(c,scope,&view);
    if (e == ASPER_OK) {
      e = asper_source_view_read(&view,rows[i].sequence,&event);
      if (e == ASPER_OK && (strcmp(event.id,id) || !asper_source_deferred_matches(&rows[i],&event)))
        e = ASPER_ERR_PARSE;
      asper_source_view_close(&view);
    }
    free(event.text);
  }
  if (e == ASPER_OK && count) {
    qsort(rows,count,sizeof *rows,compare);
    for (size_t i = 1; i < count; i++) if (!compare(&rows[i-1],&rows[i])) e = ASPER_ERR_PARSE;
  }
  asmodel_json_free(doc); free(text);
  if (e == ASPER_OK) { *out = rows; *n = count; }
  else {
    free(rows);
    asper_log(c,ASPER_LOG_ERROR,"curator","cannot load %s (%s); inspect the offline source decisions",
        ASPER_DEFERRED_FILE,asper_err_name(e));
  }
  return e;
}
