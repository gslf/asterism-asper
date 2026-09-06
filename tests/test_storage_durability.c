/* Corruption must preserve repair inputs; crash recovery must not double-apply. */
#include "asper_test.h"
#include "fakes.h"
#include "store_files.h"
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif
static fake_clock clk;
static fake_curator curator;
static asper_ctx *open_root(const char *root) {
  asper_open_params p = {0}; p.memory_root = root;
  asper_ctx *c = NULL; asper_embedder emb = fake_embedder_make();
  asper_curator_iface cur = fake_curator_iface_make(&curator);
  asper_clock clock = fake_clock_make(&clk);
  if (asper_open_with(&p,&emb,&cur,&clock,&c) != ASPER_OK) return NULL;
  asper_worker_stop(c); asper_set_logger(c,NULL,NULL); return c;
}
static void setup(void) { fake_curator_init(&curator); fake_clock_set(&clk,1785319920LL); }

TEST(snapshot_damage_is_fatal) {
  for (int damage = 0; damage < 2; damage++) {
    char root[256],path[512],id[37]; ASSERT_TRUE(asper_test_tmpdir(root)); setup();
    asper_ctx *c = open_root(root); ASSERT_TRUE(c != NULL);
    ASSERT_OK(asper_memory_insert(c,ASPER_SECTION_CONTEXT,NULL,"Original fact",0,id));
    asper_close(c);
    snprintf(path,sizeof path,"%s/context.xcdn",root);
    char *text = NULL; size_t n;
    ASSERT_OK(asper_store_file_read(path,false,ASPER_SECTION_MAX,&text,&n));
    if (damage) n--; else { char *fact = strstr(text,"Original"); ASSERT_TRUE(fact != NULL); fact[0] = 'o'; }
    ASSERT_OK(os_write_file(path,text,n)); free(text);
    ASSERT_TRUE(open_root(root) == NULL);
    uint64_t actual; ASSERT_OK(os_file_size(path,&actual)); ASSERT_EQ_INT(actual,n);
    fake_curator_dispose(&curator); asper_test_rmtree(root);
  }
}

TEST(all_backups_are_checked_before_restore) {
  char root[256],path[512],id[37]; ASSERT_TRUE(asper_test_tmpdir(root)); setup();
  asper_ctx *c = open_root(root); ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c,ASPER_SECTION_CONTEXT,NULL,"Original fact",0,id));
  ASSERT_OK(asper_store_compact(c));
  char *rels[] = {"context.xcdn","journal.xcdn"};
  ASSERT_OK(asper_compact_begin(c,rels,2));
  snprintf(path,sizeof path,"%s/context.xcdn",root);
  ASSERT_OK(os_write_file(path,"current target",14));
  snprintf(path,sizeof path,"%s/journal.xcdn.compact.bak",root);
  ASSERT_OK(os_write_file(path,"damaged backup",14));
  c->store.poisoned = true; asper_close(c);
  ASSERT_TRUE(open_root(root) == NULL);
  snprintf(path,sizeof path,"%s/context.xcdn",root);
  char *text = NULL; ASSERT_OK(asper_store_file_read(path,false,100,&text,NULL));
  ASSERT_EQ_STR(text,"current target"); free(text);
  snprintf(path,sizeof path,"%s/compact.pending",root); ASSERT_TRUE(os_file_exists(path));
  fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(old_store_requires_explicit_conversion) {
  char root[256],path[512]; ASSERT_TRUE(asper_test_tmpdir(root)); setup();
  snprintf(path,sizeof path,"%s/manifest.xcdn",root);
  const char *manifest = "#asper_manifest { store_version: 1 }\n";
  ASSERT_OK(os_write_file(path,manifest,strlen(manifest)));
  ASSERT_TRUE(open_root(root) == NULL);
  char *text = NULL; ASSERT_OK(asper_store_file_read(path,false,100,&text,NULL));
  ASSERT_EQ_STR(text,manifest); free(text);
  fake_curator_dispose(&curator); asper_test_rmtree(root);
}

TEST(erasure_guard_blocks_recovery_and_initialization) {
  char root[256], path[512]; ASSERT_TRUE(asper_test_tmpdir(root)); setup();
  snprintf(path, sizeof path, "%s/.erase.pending", root);
  ASSERT_OK(os_write_file(path, "", 0));
  asper_open_params p = {0}; p.memory_root = root;
  asper_ctx *c = NULL;
  ASSERT_ERR(asper_open(&p, &c), ASPER_ERR_BUSY); ASSERT_TRUE(!c);
  snprintf(path, sizeof path, "%s/manifest.xcdn", root);
  ASSERT_TRUE(!os_file_exists(path));
  snprintf(path, sizeof path, "%s/journal.xcdn", root);
  ASSERT_TRUE(!os_file_exists(path));
  fake_curator_dispose(&curator); asper_test_rmtree(root);
}

static asper_err uncertain_commit(int stage) { return stage == 5 ? ASPER_ERR_IO : ASPER_OK; }
TEST(commit_sync_uncertainty_blocks_the_live_store) {
  char root[256],id[37]; ASSERT_TRUE(asper_test_tmpdir(root)); setup();
  asper_ctx *c = open_root(root); ASSERT_TRUE(c != NULL);
  ASSERT_OK(asper_memory_insert(c,ASPER_SECTION_CONTEXT,NULL,"Committed snapshot",0,id));
  c->store.compact_checkpoint = uncertain_commit;
  ASSERT_ERR(asper_store_compact(c),ASPER_ERR_IO); ASSERT_TRUE(c->store.poisoned);
  ASSERT_ERR(asper_memory_update(c,id,"Unsafe continuation"),ASPER_ERR_IO);
  asper_close(c); c = open_root(root); ASSERT_TRUE(c != NULL);
  asper_record **rows = NULL; size_t n;
  ASSERT_OK(asper_memory_list(c,ASPER_SECTION_CONTEXT,NULL,0,&rows,&n)); ASSERT_EQ_INT(n,1);
  ASSERT_EQ_STR(asper_record_content(rows[0]),"Committed snapshot");
  asper_records_free(rows,n); asper_close(c);
  fake_curator_dispose(&curator); asper_test_rmtree(root);
}

#ifndef _WIN32
static int crash_stage;
static asper_err crash_at(int stage) { if (stage == crash_stage) _exit(73); return ASPER_OK; }
#endif
TEST(crash_during_each_compaction_boundary) {
#ifndef _WIN32
  for (crash_stage = 1; crash_stage <= 5; crash_stage++) {
    char root[256],id[37]; ASSERT_TRUE(asper_test_tmpdir(root)); setup();
    asper_ctx *c = open_root(root); ASSERT_TRUE(c != NULL);
    ASSERT_OK(asper_memory_insert(c,ASPER_SECTION_CONTEXT,NULL,"Before checkpoint",0,id));
    asper_close(c);
    pid_t child = fork(); ASSERT_TRUE(child >= 0);
    if (!child) {
      c = open_root(root); if (!c) _exit(90);
      if (asper_memory_update(c,id,"After checkpoint") != ASPER_OK) _exit(91);
      asper_op access = {0}; access.kind = ASPER_OP_ACCESS;
      char ids[1][37]; strcpy(ids[0],id); access.ids = ids; access.ids_n = 1;
      if (asper_apply_op(c,&access,false) != ASPER_OK) _exit(92);
      c->store.compact_checkpoint = crash_at;
      (void)asper_store_compact(c); _exit(93);
    }
    int status; ASSERT_EQ_INT(waitpid(child,&status,0),child);
    ASSERT_TRUE(WIFEXITED(status)); ASSERT_EQ_INT(WEXITSTATUS(status),73);
    c = open_root(root); ASSERT_TRUE(c != NULL);
    asper_record **rows = NULL; size_t n;
    ASSERT_OK(asper_memory_list(c,ASPER_SECTION_CONTEXT,NULL,0,&rows,&n)); ASSERT_EQ_INT(n,1);
    ASSERT_EQ_STR(asper_record_content(rows[0]),"After checkpoint");
    ASSERT_EQ_INT(asper_record_access_count(rows[0]),1);
    asper_records_free(rows,n); asper_close(c);
    fake_curator_dispose(&curator); asper_test_rmtree(root);
  }
#endif
}
TEST_LIST = {
  TEST_ENTRY(erasure_guard_blocks_recovery_and_initialization),
  TEST_ENTRY(commit_sync_uncertainty_blocks_the_live_store),
  TEST_ENTRY(snapshot_damage_is_fatal),
  TEST_ENTRY(all_backups_are_checked_before_restore),
  TEST_ENTRY(old_store_requires_explicit_conversion),
  TEST_ENTRY(crash_during_each_compaction_boundary),
};
RUN_ALL_TESTS()
