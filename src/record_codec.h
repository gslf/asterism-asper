#ifndef ASPER_RECORD_CODEC_H
#define ASPER_RECORD_CODEC_H
#include "asper_internal.h"
#include "xcdn.h"
xcdn_value_t *asper_record_string(const char *text);
char *asper_record_unescape(const char *text);
#endif
