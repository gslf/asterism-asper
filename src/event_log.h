/* Private event storage. Caller serializes operations with source_mu. */
#ifndef ASPER_EVENT_LOG_H
#define ASPER_EVENT_LOG_H
#include "asper_internal.h"

#define ASPER_EVENT_BYTES (16u * 1024u * 1024u)
#define ASPER_EVENT_LOG_BYTES (512u * 1024u * 1024u)

/* NOT_FOUND denotes EOF or an incomplete final frame; complete damage is PARSE. */
asper_err asper_event_frame_read(FILE *f, asper_event *event);
/* Selection metadata only: payload/object bytes have not been read or verified. */
asper_err asper_event_frame_head(FILE *f, asper_event *event, size_t *object_bytes,
                                size_t *text_bytes, char payload_hash[65]);
asper_err asper_event_frame_write(FILE *f, const asper_event *event);
asper_err asper_event_log_append(const char *path, asper_event *event);
asper_err asper_event_log_page(const char *path, const char *query,
    unsigned long long after, size_t limit, asper_event **out, size_t *n,
    unsigned long long *next);
#endif
