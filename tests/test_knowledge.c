/* Evidence survives restart; validity must be re-established from current state. */
#include "asper_test.h"
#include "knowledge.h"

typedef struct {
  char root[256], id[37], hash[65]; asper_ctx *c;
  asper_source_span span; asper_dependency dependency; asper_grounding grounding;
} fixture;
static const char *claim = "β journal_commit failed";
static int open_fixture(fixture *f) {
  memset(f,0,sizeof *f);
  if (!asper_test_tmpdir(f->root)) return 0;
  asper_open_params p = {.memory_root=f->root};
  if (asper_open(&p,&f->c) != ASPER_OK) return 0;
  asper_event_input event = {.scope="task",.kind=ASPER_EVENT_TOOL_RESULT,.text=claim};
  strcpy(f->span.scope,"task"); f->span.sequence = 1;
  if (asper_event_append(f->c,&event,f->span.event_id) != ASPER_OK ||
      asper_memory_insert(f->c,ASPER_SECTION_CONTEXT,NULL,claim,0,f->id) != ASPER_OK) return 0;
  knowledge_content_hash(claim,f->hash);
  f->span.source_end = f->span.claim_end = strlen(claim);
  strcpy(f->dependency.resource,"file:repo/journal.c"); strcpy(f->dependency.version,"sha256:revision-a");
  f->grounding.sources = &f->span; f->grounding.sources_n = 1;
  f->grounding.dependencies = &f->dependency; f->grounding.dependencies_n = 1;
  return 1;
}
static int status(asper_ctx *c, const char *id) {
  asper_grounding *g = NULL; unsigned long long rev; asper_knowledge_status state;
  asper_err e = asper_memory_grounding(c,id,&g,&rev,&state);
  asper_grounding_free(g); return e == ASPER_OK ? (int)state : -1;
}
static int hits(asper_ctx *c) {
  asper_record **records = NULL; size_t n = 0;
  asper_err e = asper_memory_search(c,ASPER_SECTION_CONTEXT,NULL,"journal_commit",16,&records,&n);
  asper_records_free(records,n); return e == ASPER_OK ? (int)n : -1;
}
static asper_err observe(fixture *f, const char *version) {
  return asper_memory_observe_dependency(f->c,f->dependency.resource,version);
}
static void close_fixture(fixture *f) { asper_close(f->c); asper_test_rmtree(f->root); }

TEST(source_ranges_dependencies_and_restart) {
  fixture f; ASSERT_TRUE(open_fixture(&f));
  ASSERT_OK(asper_memory_ground(f.c,f.id,f.hash,0,&f.grounding));
  ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_UNAVAILABLE); ASSERT_EQ_INT(hits(f.c),0);
  ASSERT_OK(observe(&f,"sha256:revision-a")); ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_CURRENT);
  ASSERT_EQ_INT(hits(f.c),1);
  asper_grounding *g = NULL; unsigned long long revision; asper_knowledge_status state;
  ASSERT_OK(asper_memory_grounding(f.c,f.id,&g,&revision,&state));
  ASSERT_EQ_INT(revision,1); ASSERT_EQ_STR(g->sources[0].event_id,f.span.event_id);
  ASSERT_EQ_INT(g->sources[0].claim_end,strlen(claim)); asper_grounding_free(g);
  ASSERT_OK(observe(&f,"sha256:revision-b")); ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_STALE);
  ASSERT_EQ_INT(hits(f.c),0);
  asper_close(f.c); f.c = NULL;
  asper_open_params p = {.memory_root=f.root}; ASSERT_OK(asper_open(&p,&f.c));
  ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_UNAVAILABLE);
  ASSERT_OK(observe(&f,"sha256:revision-a")); ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_CURRENT);
  ASSERT_ERR(asper_memory_ground(f.c,f.id,f.hash,0,&f.grounding),ASPER_ERR_BUSY);
  ASSERT_OK(asper_memory_update(f.c,f.id,"journal_commit has been repaired"));
  ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_STALE); ASSERT_EQ_INT(hits(f.c),0);
  asper_close(f.c); f.c = NULL; ASSERT_OK(asper_open(&p,&f.c));
  asper_grounding_revision *history = NULL; unsigned long long cursor; size_t n;
  ASSERT_OK(asper_memory_grounding_history(f.c,f.id,0,10,&history,&n,&cursor));
  ASSERT_EQ_INT(n,1); ASSERT_EQ_STR(history[0].content,claim);
  asper_grounding_history_free(history,n);
  close_fixture(&f);
}
TEST(reject_bad_ranges_hashes_and_cycles) {
  fixture f; ASSERT_TRUE(open_fixture(&f));
  f.span.source_begin = 1;
  ASSERT_ERR(asper_memory_ground(f.c,f.id,f.hash,0,&f.grounding),ASPER_ERR_INVALID);
  f.span.source_begin = 0; f.span.claim_end++;
  ASSERT_ERR(asper_memory_ground(f.c,f.id,f.hash,0,&f.grounding),ASPER_ERR_INVALID);
  f.span.claim_end--; f.span.sequence++;
  ASSERT_ERR(asper_memory_ground(f.c,f.id,f.hash,0,&f.grounding),ASPER_ERR_INVALID);
  f.span.sequence--;
  char wrong[65]; memset(wrong,'0',64); wrong[64] = 0;
  ASSERT_ERR(asper_memory_ground(f.c,f.id,wrong,0,&f.grounding),ASPER_ERR_BUSY);
  ASSERT_OK(asper_memory_ground(f.c,f.id,f.hash,0,&f.grounding));
  char child[37], hash[65]; const char *text = "journal_commit requires repair";
  ASSERT_OK(asper_memory_insert(f.c,ASPER_SECTION_CONTEXT,NULL,text,0,child)); knowledge_content_hash(text,hash);
  asper_knowledge_link link = {.kind=ASPER_REL_SUPPORTS}; strcpy(link.record_id,f.id); strcpy(link.content_sha256,f.hash);
  asper_grounding derived = {.links=&link,.links_n=1};
  ASSERT_OK(asper_memory_ground(f.c,child,hash,0,&derived));
  strcpy(link.record_id,child); strcpy(link.content_sha256,hash);
  f.grounding.links = &link; f.grounding.links_n = 1;
  ASSERT_ERR(asper_memory_ground(f.c,f.id,f.hash,1,&f.grounding),ASPER_ERR_INVALID);
  ASSERT_OK(observe(&f,"sha256:revision-a")); ASSERT_EQ_INT(status(f.c,child),ASPER_KNOWLEDGE_CURRENT);
  ASSERT_OK(observe(&f,"sha256:revision-b")); ASSERT_EQ_INT(status(f.c,child),ASPER_KNOWLEDGE_STALE);
  close_fixture(&f);
}
TEST(conflicts_corrections_and_revocation) {
  fixture f; ASSERT_TRUE(open_fixture(&f));
  ASSERT_OK(asper_memory_ground(f.c,f.id,f.hash,0,&f.grounding)); ASSERT_OK(observe(&f,"sha256:revision-a"));
  char id[37], hash[65]; const char *text = "journal_commit passed";
  ASSERT_OK(asper_memory_insert(f.c,ASPER_SECTION_CONTEXT,NULL,text,0,id)); knowledge_content_hash(text,hash);
  asper_knowledge_link link = {.kind=ASPER_REL_CONTRADICTS}; strcpy(link.record_id,f.id); strcpy(link.content_sha256,f.hash);
  asper_grounding correction = {.links=&link,.links_n=1};
  ASSERT_OK(asper_memory_ground(f.c,id,hash,0,&correction));
  ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_CONTESTED); ASSERT_EQ_INT(status(f.c,id),ASPER_KNOWLEDGE_CONTESTED);
  ASSERT_EQ_INT(hits(f.c),0);
  link.kind = ASPER_REL_SUPERSEDES;
  ASSERT_OK(asper_memory_ground(f.c,id,hash,1,&correction));
  ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_STALE);
  ASSERT_EQ_INT(status(f.c,id),ASPER_KNOWLEDGE_UNVERIFIED);
  correction.revoked = 1; strcpy(correction.reason,"Observation used the wrong checkout");
  ASSERT_OK(asper_memory_ground(f.c,id,hash,2,&correction));
  ASSERT_EQ_INT(status(f.c,id),ASPER_KNOWLEDGE_REVOKED); ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_STALE);
  ASSERT_EQ_INT(hits(f.c),0);
  asper_close(f.c); f.c = NULL;
  asper_open_params p = {.memory_root=f.root}; ASSERT_OK(asper_open(&p,&f.c));
  asper_grounding_revision *history = NULL; size_t n; unsigned long long cursor = 0;
  for (unsigned long long expected = 1; expected <= 3; expected++) {
    unsigned long long before = cursor;
    ASSERT_OK(asper_memory_grounding_history(f.c,id,cursor,1,&history,&n,&cursor));
    ASSERT_EQ_INT(n,1); ASSERT_TRUE(cursor > before); ASSERT_EQ_INT(history[0].revision,expected);
    ASSERT_EQ_STR(history[0].content,text); ASSERT_EQ_STR(history[0].content_sha256,hash);
    ASSERT_EQ_INT(history[0].grounding->revoked,expected == 3);
    asper_grounding_history_free(history,n);
  }
  ASSERT_OK(asper_memory_grounding_history(f.c,id,cursor,1,&history,&n,&cursor));
  ASSERT_EQ_INT(n,0); asper_grounding_history_free(history,n);
  close_fixture(&f);
}
TEST(partial_coverage_and_missing_source) {
  fixture f; ASSERT_TRUE(open_fixture(&f));
  f.span.claim_end = 2;
  ASSERT_OK(asper_memory_ground(f.c,f.id,f.hash,0,&f.grounding)); ASSERT_OK(observe(&f,"sha256:revision-a"));
  ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_UNVERIFIED);
  f.span.claim_end = strlen(claim);
  ASSERT_OK(asper_memory_ground(f.c,f.id,f.hash,1,&f.grounding));
  ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_CURRENT);
  asper_close(f.c); f.c = NULL;
  char path[512]; snprintf(path,sizeof path,"%s/scopes/task/events.log",f.root);
  ASSERT_EQ_INT(remove(path),0);
  asper_open_params p = {.memory_root=f.root}; ASSERT_OK(asper_open(&p,&f.c));
  ASSERT_OK(observe(&f,"sha256:revision-a")); ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_UNAVAILABLE);
  ASSERT_EQ_INT(hits(f.c),0); close_fixture(&f);
}
TEST(corrections_are_independent_of_uuid_order) {
  for (int reversed = 0; reversed < 2; reversed++) {
    fixture f; ASSERT_TRUE(open_fixture(&f));
    ASSERT_OK(asper_memory_ground(f.c,f.id,f.hash,0,&f.grounding)); ASSERT_OK(observe(&f,"sha256:revision-a"));
    char ids[2][37], hash[65]; const char *text = "journal_commit correction";
    for (int i = 0; i < 2; i++) ASSERT_OK(asper_memory_insert(f.c,ASPER_SECTION_CONTEXT,NULL,text,0,ids[i]));
    knowledge_content_hash(text,hash);
    int first = strcmp(ids[0],ids[1]) < 0 ? 0 : 1;
    int conflict = reversed ? 1-first : first, correction = 1-conflict;
    asper_knowledge_link link = {.kind=ASPER_REL_CONTRADICTS};
    strcpy(link.record_id,f.id); strcpy(link.content_sha256,f.hash);
    asper_grounding g = {.links=&link,.links_n=1};
    ASSERT_OK(asper_memory_ground(f.c,ids[conflict],hash,0,&g));
    ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_CONTESTED);
    link.kind = ASPER_REL_SUPERSEDES; strcpy(link.record_id,ids[conflict]); strcpy(link.content_sha256,hash);
    ASSERT_OK(asper_memory_ground(f.c,ids[correction],hash,0,&g));
    ASSERT_EQ_INT(status(f.c,f.id),ASPER_KNOWLEDGE_CURRENT);
    ASSERT_EQ_INT(status(f.c,ids[conflict]),ASPER_KNOWLEDGE_STALE);
    close_fixture(&f);
  }
}
static asper_time test_now(void *ud) { return *(asper_time *)ud; }
static asper_err expire_during_embed(void *ud, const char *text, int query,
    const asmodel_embed_params *params, float *out) {
  (void)text; (void)query; (void)params; (void)out;
  *(asper_time *)ud += 2;
  return ASPER_ERR_MODEL; /* Time advances even when the provider fails. */
}
TEST(expired_support_invalidates_descendants) {
  fixture f; ASSERT_TRUE(open_fixture(&f));
  asper_time tick = 1788670000; f.c->clock = (asper_clock){.ud=&tick,.now=test_now};
  asper_evidence ev = {.kind=ASPER_EVIDENCE_OBSERVED,.observed_at=tick,.expires_at=tick+1};
  strcpy(ev.provenance,"test:execution"); char parent[37], child[37];
  ASSERT_OK(asper_memory_insert_evidenced(f.c,ASPER_SECTION_CONTEXT,NULL,claim,0,&ev,parent));
  ASSERT_OK(asper_memory_insert(f.c,ASPER_SECTION_CONTEXT,NULL,claim,0,child));
  f.grounding.dependencies_n = 0;
  ASSERT_OK(asper_memory_ground(f.c,parent,f.hash,0,&f.grounding));
  asper_knowledge_link link = {.kind=ASPER_REL_SUPPORTS}; strcpy(link.record_id,parent); strcpy(link.content_sha256,f.hash);
  asper_grounding g = {.links=&link,.links_n=1};
  ASSERT_OK(asper_memory_ground(f.c,child,f.hash,0,&g)); ASSERT_EQ_INT(status(f.c,child),ASPER_KNOWLEDGE_CURRENT);
  asper_embedder original = f.c->embedder; bool had_embedder = f.c->has_embedder;
  f.c->embedder = (asper_embedder){.ud=&tick,.dim=1,.embed=expire_during_embed}; f.c->has_embedder = true;
  asper_record **rows = NULL; size_t n;
  ASSERT_OK(asper_memory_search(f.c,ASPER_SECTION_CONTEXT,NULL,claim,10,&rows,&n));
  for (size_t i = 0; i < n; i++) ASSERT_TRUE(strcmp(asper_record_id(rows[i]),child));
  asper_records_free(rows,n);
  f.c->embedder = original; f.c->has_embedder = had_embedder;
  ASSERT_EQ_INT(status(f.c,child),ASPER_KNOWLEDGE_STALE); close_fixture(&f);
}
TEST(uncertain_write_poison_and_corrupt_frames) {
  fixture f; ASSERT_TRUE(open_fixture(&f));
  ASSERT_OK(asper_memory_ground(f.c,f.id,f.hash,0,&f.grounding));
  char *actual_path = f.c->knowledge->path; f.c->knowledge->path = asper_strdup(f.root);
  ASSERT_TRUE(asper_memory_ground(f.c,f.id,f.hash,1,&f.grounding) != ASPER_OK);
  free(f.c->knowledge->path); f.c->knowledge->path = actual_path;
  ASSERT_EQ_INT(hits(f.c),-1);
  ASSERT_ERR(asper_memory_update(f.c,f.id,"journal_commit later"),ASPER_ERR_IO);
  char *path = asper_strdup(actual_path); asper_close(f.c); f.c = NULL;
  FILE *file = os_fopen(path,"r+b"); ASSERT_TRUE(file != NULL); ASSERT_EQ_INT(fseek(file,-1,SEEK_END),0);
  int byte = fgetc(file); ASSERT_EQ_INT(fseek(file,-1,SEEK_END),0); ASSERT_TRUE(fputc(byte^1,file) != EOF);
  ASSERT_EQ_INT(fclose(file),0); free(path);
  asper_open_params p = {.memory_root=f.root}; ASSERT_ERR(asper_open(&p,&f.c),ASPER_ERR_PARSE);
  ASSERT_TRUE(f.c == NULL); asper_test_rmtree(f.root);
}
TEST_LIST = {TEST_ENTRY(corrections_are_independent_of_uuid_order),TEST_ENTRY(expired_support_invalidates_descendants),
  TEST_ENTRY(partial_coverage_and_missing_source),TEST_ENTRY(source_ranges_dependencies_and_restart),TEST_ENTRY(reject_bad_ranges_hashes_and_cycles),
  TEST_ENTRY(conflicts_corrections_and_revocation),TEST_ENTRY(uncertain_write_poison_and_corrupt_frames)};
RUN_ALL_TESTS()
