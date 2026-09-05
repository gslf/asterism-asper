/* Checked operation frames. Acknowledged writes precede projection; uncertain
 * durability closes the writer so compaction cannot erase unobserved changes. */
#include "event_log.h"
#include "xcdn.h"
#include <stdlib.h>
#include <string.h>

static void poison_locked(asper_store *st) {
  st->poisoned = true;
  if (st->journal_fp) fclose(st->journal_fp);
  st->journal_fp = NULL;
}
void asper_store_poison(asper_ctx *c) {
  os_mutex_lock(&c->journal_mu);
  poison_locked(&c->store);
  os_mutex_unlock(&c->journal_mu);
}

static asper_err replay_frame(asper_ctx *c, const asper_event *event, size_t sequence) {
  if (event->sequence != sequence || event->kind != ASPER_EVENT_DIAGNOSTIC ||
      event->pinned || event->object_ref[0]) return ASPER_ERR_PARSE;
  xcdn_document_t *doc = xcdn_parse(event->text,NULL);
  asper_err e = ASPER_ERR_PARSE;
  asper_op op = {0};
  if (doc && doc->values_len == 1 && xcdn_node_tag_count(doc->values[0]) == 1 &&
      xcdn_node_has_tag(doc->values[0],"op")) {
    e = asper_op_from_node(c,doc->values[0],&op);
    if (e == ASPER_OK && op.at != event->at) e = ASPER_ERR_PARSE;
    if (e == ASPER_OK) e = asper_store_apply(c,&op);
  }
  asper_op_free(&op); xcdn_document_free(doc);
  return e == ASPER_ERR_NOMEM ? e : e == ASPER_OK ? e : ASPER_ERR_PARSE;
}

asper_err asper_journal_replay(asper_ctx *c, const char *path, size_t *out_ops) {
  uint64_t bytes = 0;
  size_t count = 0;
  long good = 0;
  bool torn = false;
  if (out_ops) *out_ops = 0;
  asper_err e = os_file_size(path,&bytes);
  if (e == ASPER_ERR_NOT_FOUND) return ASPER_OK;
  if (e != ASPER_OK) return e;
  if (bytes > ASPER_EVENT_LOG_BYTES) return ASPER_ERR_LIMIT;
  FILE *f = os_fopen(path,"rb");
  if (!f) return ASPER_ERR_IO;
  while ((uint64_t)good < bytes) {
    asper_event event;
    e = asper_event_frame_read(f,&event);
    if (e == ASPER_ERR_NOT_FOUND) { torn = true; e = ASPER_OK; break; }
    if (e != ASPER_OK) break;
    e = replay_frame(c,&event,count+1);
    free(event.text);
    if (e != ASPER_OK) break;
    count++; good = ftell(f);
    if (good < 0) { e = ASPER_ERR_IO; break; }
  }
  if (fclose(f) && e == ASPER_OK) e = ASPER_ERR_IO;
  if (e == ASPER_OK && torn) {
    e = os_truncate(path,(uint64_t)good);
    if (e == ASPER_OK) {
      f = os_fopen(path,"ab");
      if (!f) e = ASPER_ERR_IO;
      else { e = os_fsync(f); if (fclose(f) && e == ASPER_OK) e = ASPER_ERR_IO; }
    }
    if (e == ASPER_OK) asper_log(c,ASPER_LOG_WARN,"store","discarded incomplete journal tail");
  }
  if (e != ASPER_OK) return asper_seterr(c,e,"journal: invalid or unreadable operation at %ld",good);
  c->store.journal_ops = count;
  if (out_ops) *out_ops = count;
  return ASPER_OK;
}

/* A failed buffered write is retractable only after truncation is durable.
 * A failed fsync is uncertain: keep the bytes and require recovery on reopen. */
static void rollback(asper_ctx *c, long offset) {
  asper_store *st = &c->store;
  if (st->journal_fp) fclose(st->journal_fp);
  st->journal_fp = NULL;
  if (offset >= 0 && os_truncate(st->journal_path,(uint64_t)offset) == ASPER_OK) {
    st->journal_fp = os_fopen(st->journal_path,"ab");
    if (st->journal_fp && os_fsync(st->journal_fp) == ASPER_OK) return;
  }
  poison_locked(st);
}

asper_err asper_journal_append(asper_ctx *c, const asper_op *op, bool force_sync) {
  if (!c || !op) return ASPER_ERR_INVALID;
  asper_buf text; asper_buf_init(&text);
  asper_err e = asper_op_serialize(op,&text);
  if (e != ASPER_OK) { asper_buf_free(&text); return e; }
  os_mutex_lock(&c->journal_mu);
  asper_store *st = &c->store;
  long offset = -1;
  if (st->poisoned || !st->journal_fp) e = ASPER_ERR_IO;
  else if (fseek(st->journal_fp,0,SEEK_END) || (offset = ftell(st->journal_fp)) < 0) e = ASPER_ERR_IO;
  else if (text.len > ASPER_EVENT_BYTES || (uint64_t)offset + text.len + 512 > ASPER_EVENT_LOG_BYTES)
    e = ASPER_ERR_LIMIT;
  if (e == ASPER_OK) {
    asper_event event = {0};
    event.text = text.data; event.kind = ASPER_EVENT_DIAGNOSTIC;
    event.sequence = st->journal_ops+1; event.at = op->at; asper_uuid_v4(event.id);
    int fault = st->journal_fault; st->journal_fault = 0;
    if (fault == 1) { (void)fputs("AEV2 ",st->journal_fp); e = ASPER_ERR_IO; }
    else e = asper_event_frame_write(st->journal_fp,&event);
    if (e == ASPER_OK && (fflush(st->journal_fp) || fault == 2)) e = ASPER_ERR_IO;
    if (e != ASPER_OK) rollback(c,offset);
    else {
      if (force_sync || c->cfg.journal_sync == ASPER_SYNC_ALWAYS)
        e = fault == 3 ? ASPER_ERR_IO : os_fsync(st->journal_fp);
      if (e != ASPER_OK) poison_locked(st);
      else {
        st->journal_ops++;
        if (st->audit_fp && (fwrite(text.data,1,text.len,st->audit_fp) != text.len ||
            fputc('\n',st->audit_fp) == EOF || fflush(st->audit_fp)))
          asper_log(c,ASPER_LOG_WARN,"store","audit append failed");
      }
    }
  }
  os_mutex_unlock(&c->journal_mu); asper_buf_free(&text);
  return e;
}

asper_err asper_journal_sync(asper_ctx *c) {
  if (!c) return ASPER_ERR_INVALID;
  os_rwlock_wrlock(&c->lock);
  os_mutex_lock(&c->journal_mu);
  asper_store *st = &c->store;
  asper_err e = st->poisoned || !st->journal_fp ? ASPER_ERR_IO :
      st->journal_fault == 3 ? ASPER_ERR_IO : os_fsync(st->journal_fp);
  st->journal_fault = 0;
  if (e != ASPER_OK) poison_locked(st);
  os_mutex_unlock(&c->journal_mu);
  os_rwlock_wrunlock(&c->lock);
  return e;
}
