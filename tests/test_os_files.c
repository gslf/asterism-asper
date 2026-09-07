/* Test roots must work with the same alias-rejecting I/O used by the store. */
#include "asper_test.h"
#include "store_files.h"

TEST(temporary_root_supports_protected_io) {
  char root[256], path[512], copy[512];
  ASSERT_TRUE(asper_test_tmpdir(root));
  snprintf(path, sizeof path, "%s/snapshot", root);
  snprintf(copy, sizeof copy, "%s/backup", root);
  ASSERT_OK(asper_store_file_write(NULL, path, "snapshot", 8, true));
  ASSERT_OK(asper_store_file_copy(path, copy, NULL, NULL));
  char *text = NULL;
  ASSERT_OK(asper_store_file_read(copy, true, 100, &text, NULL));
  ASSERT_EQ_STR(text, "snapshot"); free(text);
  asper_test_rmtree(root);
}

TEST(read_stream_can_be_synced_without_losing_its_position) {
  char root[256], path[512];
  ASSERT_TRUE(asper_test_tmpdir(root));
  snprintf(path, sizeof path, "%s/receipt", root);
  ASSERT_OK(os_write_file(path, "receipt", 7));
  FILE *stream = NULL; uint64_t size = 0;
  ASSERT_OK(os_blob_open(path, &stream, &size));
  ASSERT_EQ_INT(size, 7); ASSERT_EQ_INT(fgetc(stream), 'r');
  asper_err e = os_fsync(stream);
  int next = fgetc(stream);
  fclose(stream); asper_test_rmtree(root);
  ASSERT_OK(e); ASSERT_EQ_INT(next, 'e');
}

TEST(read_stream_stamp_detects_deletion_before_close) {
  char root[256], path[512];
  ASSERT_TRUE(asper_test_tmpdir(root));
  snprintf(path, sizeof path, "%s/events", root);
  ASSERT_OK(os_write_file(path, "event", 5));
  FILE *stream = NULL; uint64_t size = 0;
  ASSERT_OK(os_blob_open(path, &stream, &size));
  os_file_stamp before, after;
  ASSERT_OK(os_stream_stamp(stream, &before));
  ASSERT_TRUE(before.links > 0);
  ASSERT_OK(os_remove_file(path));
  asper_err e = os_stream_stamp(stream, &after);
  fclose(stream); asper_test_rmtree(root);
  ASSERT_OK(e); ASSERT_EQ_INT(after.links, 0);
}

#ifndef _WIN32
TEST(temporary_root_resolves_parent_alias_without_relaxing_blob_checks) {
  char parent[256], root[256], alias[512], path[512], aliased[1024];
  ASSERT_TRUE(asper_test_tmpdir(parent));
  snprintf(alias, sizeof alias, "%s/alias", parent);
  ASSERT_EQ_INT(symlink(parent, alias), 0);
  const char *previous = getenv("TMPDIR");
  char *saved = previous ? strdup(previous) : NULL;
  ASSERT_TRUE(!previous || saved);
  ASSERT_EQ_INT(setenv("TMPDIR", alias, 1), 0);
  int made = asper_test_tmpdir(root);
  int restored = saved ? setenv("TMPDIR", saved, 1) : unsetenv("TMPDIR");
  free(saved);
  ASSERT_EQ_INT(restored, 0); ASSERT_TRUE(made);
  ASSERT_TRUE(strstr(root, "/alias/") == NULL);
  snprintf(path, sizeof path, "%s/data", root);
  ASSERT_OK(asper_store_file_write(NULL, path, "safe", 4, false));
  char *text = NULL;
  ASSERT_OK(asper_store_file_read(path, false, 100, &text, NULL));
  ASSERT_EQ_STR(text, "safe"); free(text);
  snprintf(aliased, sizeof aliased, "%s/%s/data", alias, strrchr(root, '/') + 1);
  ASSERT_ERR(asper_store_file_read(aliased, false, 100, &text, NULL), ASPER_ERR_INVALID);
  ASSERT_TRUE(text == NULL);
  ASSERT_EQ_INT(unlink(alias), 0);
  asper_test_rmtree(parent);
}
#endif

TEST_LIST = {
  TEST_ENTRY(temporary_root_supports_protected_io),
  TEST_ENTRY(read_stream_can_be_synced_without_losing_its_position),
  TEST_ENTRY(read_stream_stamp_detects_deletion_before_close),
#ifndef _WIN32
  TEST_ENTRY(temporary_root_resolves_parent_alias_without_relaxing_blob_checks),
#endif
};
RUN_ALL_TESTS()
