/* Exact historical claims and their source handles, independent of compaction. */
#include "knowledge.h"
#include "event_log.h"
#include <stdlib.h>
#include <string.h>

void asper_grounding_history_free(asper_grounding_revision *revisions, size_t n) {
  for (size_t i = 0; revisions && i < n; i++) {
    free(revisions[i].content); asper_grounding_free(revisions[i].grounding);
  }
  free(revisions);
}
asper_err asper_memory_grounding_history(asper_ctx *c, const char *id,
    unsigned long long after, size_t limit, asper_grounding_revision **out,
    size_t *out_n, unsigned long long *next) {
  if (!c || !c->knowledge || !asper_uuid_valid(id) || !limit || limit > 100 || !out || !out_n || !next)
    return ASPER_ERR_INVALID;
  *out = NULL; *out_n = 0; *next = after;
  asper_grounding_revision *rows = calloc(limit,sizeof *rows);
  if (!rows) return ASPER_ERR_NOMEM;
  char needle[48]; snprintf(needle,sizeof needle,"\"id\":\"%s\"",id);
  os_rwlock_wrlock(&c->lock);
  asper_err e = asper_knowledge_guard(c);
  size_t used = 0;
  while (e == ASPER_OK && used < limit) {
    asper_event *events = NULL; size_t n = 0;
    e = asper_event_log_page(c->knowledge->path,needle,*next,limit-used,&events,&n,next);
    for (size_t i = 0; e == ASPER_OK && i < n; i++) {
      knowledge_entry entry = {0};
      e = events[i].kind == ASPER_EVENT_DIAGNOSTIC ? knowledge_decode(events[i].text,&entry) : ASPER_ERR_PARSE;
      if (e == ASPER_OK && !strcmp(entry.id,id)) {
        asper_grounding_revision *r = &rows[used++];
        r->content = entry.content; entry.content = NULL;
        strcpy(r->content_sha256,entry.hash); r->revision = entry.revision;
        r->sequence = events[i].sequence; r->at = events[i].at;
        r->grounding = entry.grounding; entry.grounding = NULL;
      }
      knowledge_entry_free(&entry);
    }
    asper_events_free(events,n);
    if (!n) break;
  }
  if (e == ASPER_ERR_PARSE || e == ASPER_ERR_IO) c->knowledge->poisoned = true;
  os_rwlock_wrunlock(&c->lock);
  if (e != ASPER_OK) { asper_grounding_history_free(rows,used); return e; }
  *out = rows; *out_n = used; return ASPER_OK;
}
