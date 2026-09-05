/* Versioned grounding records. JSON is payload inside the checked event frame. */
#include "knowledge.h"
#include <stdlib.h>
#include <string.h>

static int put(asmodel_json_value *o, const char *key, const char *s) {
  return asmodel_json_object_set(o,key,asmodel_json_string(s));
}
static int number(asmodel_json_value *o, const char *key, unsigned long long n) {
  return n > INT64_MAX ? -1 : asmodel_json_object_set(o,key,asmodel_json_int((long long)n));
}
static asmodel_json_value *span_value(const asper_source_span *s) {
  asmodel_json_value *o = asmodel_json_object();
  int e = !o || put(o,"scope",s->scope) || put(o,"event",s->event_id) ||
      number(o,"sequence",s->sequence) || number(o,"source_begin",s->source_begin) ||
      number(o,"source_end",s->source_end) || number(o,"claim_begin",s->claim_begin) ||
      number(o,"claim_end",s->claim_end);
  if (e) { asmodel_json_free(o); return NULL; }
  return o;
}
static asmodel_json_value *dependency_value(const asper_dependency *d) {
  asmodel_json_value *o = asmodel_json_object();
  if (!o || put(o,"resource",d->resource) || put(o,"version",d->version)) {
    asmodel_json_free(o); return NULL;
  }
  return o;
}
static asmodel_json_value *link_value(const asper_knowledge_link *l) {
  asmodel_json_value *o = asmodel_json_object();
  if (!o || number(o,"kind",l->kind) || put(o,"id",l->record_id) || put(o,"hash",l->content_sha256)) {
    asmodel_json_free(o); return NULL;
  }
  return o;
}
char *knowledge_encode(const knowledge_entry *entry) {
  const asper_grounding *g = entry->grounding;
  asmodel_json_value *o = asmodel_json_object();
  int e = !o || number(o,"schema",1) || put(o,"id",entry->id) || put(o,"hash",entry->hash) ||
      number(o,"revision",entry->revision) || put(o,"content",entry->content) || put(o,"reason",g->reason) ||
      asmodel_json_object_set(o,"revoked",asmodel_json_bool(g->revoked));
  const char *names[] = {"sources","dependencies","links"};
  const size_t counts[] = {g->sources_n,g->dependencies_n,g->links_n};
  for (size_t group = 0; group < 3 && !e; group++) {
    asmodel_json_value *a = asmodel_json_array();
    if (!a) { e = 1; break; }
    for (size_t i = 0; i < counts[group] && !e; i++) {
      asmodel_json_value *v = group == 0 ? span_value(&g->sources[i]) :
          group == 1 ? dependency_value(&g->dependencies[i]) : link_value(&g->links[i]);
      e = asmodel_json_array_push(a,v);
    }
    if (e) asmodel_json_free(a);
    else e = asmodel_json_object_set(o,names[group],a);
  }
  char *text = e ? NULL : asmodel_json_write(o,0);
  asmodel_json_free(o); return text;
}
static int string_field(const asmodel_json_value *o, const char *key, char *out, size_t size) {
  const asmodel_json_value *v = asmodel_json_object_get(o,key);
  const char *s = asmodel_json_string_value(v);
  if (!s || strlen(s) != asmodel_json_string_length(v) || strlen(s) >= size) return 0;
  strcpy(out,s); return 1;
}
static int integer_field(const asmodel_json_value *o, const char *key, unsigned long long *out) {
  const asmodel_json_value *v = asmodel_json_object_get(o,key);
  if (!asmodel_json_is_int(v) || asmodel_json_int_value(v) < 0) return 0;
  *out = (unsigned long long)asmodel_json_int_value(v); return 1;
}
static int offset_field(const asmodel_json_value *o, const char *key, size_t *out) {
  unsigned long long n;
  if (!integer_field(o,key,&n) || n > SIZE_MAX) return 0;
  *out = (size_t)n; return 1;
}
static int parse_span(const asmodel_json_value *o, asper_source_span *s) {
  return asmodel_json_object_count(o) == 7 && string_field(o,"scope",s->scope,sizeof s->scope) &&
      string_field(o,"event",s->event_id,sizeof s->event_id) && integer_field(o,"sequence",&s->sequence) &&
      offset_field(o,"source_begin",&s->source_begin) && offset_field(o,"source_end",&s->source_end) &&
      offset_field(o,"claim_begin",&s->claim_begin) && offset_field(o,"claim_end",&s->claim_end);
}
static int parse_dependency(const asmodel_json_value *o, asper_dependency *d) {
  return asmodel_json_object_count(o) == 2 && string_field(o,"resource",d->resource,sizeof d->resource) &&
      string_field(o,"version",d->version,sizeof d->version);
}
static int parse_link(const asmodel_json_value *o, asper_knowledge_link *l) {
  unsigned long long n;
  if (asmodel_json_object_count(o) != 3 || !integer_field(o,"kind",&n) || n > ASPER_REL_SUPERSEDES ||
      !string_field(o,"id",l->record_id,sizeof l->record_id) ||
      !string_field(o,"hash",l->content_sha256,sizeof l->content_sha256)) return 0;
  l->kind = (asper_relation_kind)n; return 1;
}
void asper_grounding_free(asper_grounding *g) {
  if (!g) return;
  free(g->sources); free(g->dependencies); free(g->links); free(g);
}
asper_err knowledge_decode(const char *text, knowledge_entry *out) {
  asmodel_json_value *o = NULL;
  unsigned long long schema;
  memset(out,0,sizeof *out);
  if (asmodel_json_parse(text,strlen(text),&o)) return ASPER_ERR_PARSE;
  asper_grounding *g = calloc(1,sizeof *g);
  if (!g) { asmodel_json_free(o); return ASPER_ERR_NOMEM; }
  int valid = asmodel_json_object_count(o) == 10 && integer_field(o,"schema",&schema) && schema == 1 &&
      integer_field(o,"revision",&out->revision) && out->revision &&
      string_field(o,"id",out->id,sizeof out->id) && asper_uuid_valid(out->id) &&
      string_field(o,"hash",out->hash,sizeof out->hash) && knowledge_hash(out->hash) &&
      string_field(o,"reason",g->reason,sizeof g->reason) &&
      asmodel_json_typeof(asmodel_json_object_get(o,"revoked")) == ASMODEL_JSON_BOOL;
  g->revoked = asmodel_json_bool_value(asmodel_json_object_get(o,"revoked"));
  const char *names[] = {"sources","dependencies","links"};
  for (size_t group = 0; group < 3 && valid; group++) {
    const asmodel_json_value *a = asmodel_json_object_get(o,names[group]);
    size_t n = asmodel_json_array_len(a);
    if (asmodel_json_typeof(a) != ASMODEL_JSON_ARRAY || n > KNOWLEDGE_MAX_ITEMS) { valid = 0; break; }
    void *data = n ? calloc(n,group == 0 ? sizeof *g->sources : group == 1 ? sizeof *g->dependencies : sizeof *g->links) : NULL;
    if (n && !data) { asper_grounding_free(g); asmodel_json_free(o); return ASPER_ERR_NOMEM; }
    if (group == 0) { g->sources = data; g->sources_n = n; }
    if (group == 1) { g->dependencies = data; g->dependencies_n = n; }
    if (group == 2) { g->links = data; g->links_n = n; }
    for (size_t i = 0; i < n && valid; i++) {
      const asmodel_json_value *v = asmodel_json_array_at(a,i);
      valid = group == 0 ? parse_span(v,&g->sources[i]) :
          group == 1 ? parse_dependency(v,&g->dependencies[i]) : parse_link(v,&g->links[i]);
    }
  }
  const asmodel_json_value *claim = asmodel_json_object_get(o,"content");
  const char *content = asmodel_json_string_value(claim);
  char hash[65];
  if (!content || !*content || strlen(content) > 65536 || strlen(content) != asmodel_json_string_length(claim)) valid = 0;
  else {
    knowledge_content_hash(content,hash);
    if (strcmp(hash,out->hash)) valid = 0;
    if (valid) out->content = asper_strdup(content);
  }
  asmodel_json_free(o);
  if (valid && !out->content) { asper_grounding_free(g); return ASPER_ERR_NOMEM; }
  if (!valid) { asper_grounding_free(g); return ASPER_ERR_PARSE; }
  out->grounding = g; return ASPER_OK;
}

void knowledge_entry_free(knowledge_entry *e) {
  if (!e) return;
  asper_grounding_free(e->grounding); free(e->content); memset(e,0,sizeof *e);
}
