/* A suspended batch is reconciled by the offline operator, never regenerated. */
#ifndef ASPER_CURATION_RECEIPT_H
#define ASPER_CURATION_RECEIPT_H
#include "source_internal.h"
#include "asmodel_json.h"
#define CURATION_BATCH_MAX 256u
#define CURATION_RECEIPT_MAX (1024u * 1024u)
#define CURATION_PENDING "curation.pending"
#define CURATION_HISTORY "curation.events"

asmodel_json_value *curation_receipt_decode(const char *text);
asper_err asper_curation_recover(asper_ctx *c);
asper_err asper_curation_guard(asper_ctx *c);
asper_err asper_curation_admit(asper_ctx *c, size_t sources);
asper_err asper_curation_begin(asper_ctx *c, const asper_turn *turns, size_t n,
    asper_record *const *handles, size_t handles_n, const char *proposal);
asper_err asper_curation_finish(asper_ctx *c, size_t applied, size_t rejected, size_t pending);
/* Private fault boundaries: 1=prepared, 2=operation, 3=journal synced,
 * 4=completed receipt, 5=history, 6=acknowledged, 7=removed guard. */
asper_err asper_curation_checkpoint(asper_ctx *c, int stage);
#endif
