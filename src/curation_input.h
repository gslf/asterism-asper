/* Select complete source inputs before retrieval or generation. */
#ifndef ASPER_CURATION_INPUT_H
#define ASPER_CURATION_INPUT_H
#include "asper_internal.h"
#define CURATION_TRANSCRIPT_BYTES (64u * 1024u)
asper_err asper_curation_transcript(asper_ctx *c, const asper_turn *turns, size_t n,
                                   size_t *included, char **out);
#endif
