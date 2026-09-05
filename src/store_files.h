#ifndef ASPER_STORE_FILES_H
#define ASPER_STORE_FILES_H
#include "asper_internal.h"
#define ASPER_STORE_FILE_MAX (512u * 1024u * 1024u)
#define ASPER_SECTION_MAX (16u * 1024u * 1024u)
asper_err asper_store_file_read(const char *path, bool checked, size_t limit,
                               char **out, size_t *size);
asper_err asper_store_file_write(asper_ctx *c, const char *path, const char *data,
                                size_t size, bool checked);
asper_err asper_store_file_copy(const char *src, const char *dst,
                               const char *expected, char out_hash[65]);
asper_err asper_compact_recover(asper_ctx *c);
asper_err asper_compact_begin(asper_ctx *c, char *const *rels, size_t n);
asper_err asper_compact_commit(asper_ctx *c, char *const *rels, size_t n);
size_t asper_store_project_index(const asper_store *st, const char *slug, bool *found);
#endif
