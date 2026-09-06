/* Private readers retain an initial event count; callers serialize index repair. */
#ifndef ASPER_EVENT_INDEX_H
#define ASPER_EVENT_INDEX_H
#include "event_log.h"
#define ASPER_EVENT_INDEX_MAGIC "AEIDX2\r\n"
typedef struct {
  FILE *log, *index;
  char *index_path;
  uint64_t count, bytes;
  os_file_stamp stamp;
} asper_event_files;
asper_err asper_event_files_open(asper_event_files *files, const char *path);
void asper_event_files_close(asper_event_files *files);
asper_err asper_event_files_read(asper_event_files *files, const char *path,
                                uint64_t sequence, asper_event *event);
asper_err asper_event_files_head(asper_event_files *files, const char *path,
                                uint64_t sequence, asper_event *event);
int asper_event_index_write(FILE *f, uint64_t start, uint64_t end);
#endif
