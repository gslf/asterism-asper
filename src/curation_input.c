/* Every acknowledged source must be present in the actual model transcript. */
#include "curation_input.h"
#include <stdlib.h>
#include <string.h>

asper_err asper_curation_transcript(asper_ctx *c, const asper_turn *turns, size_t n,
                                   size_t *included, char **out) {
  *included = 0; *out = NULL;
  if (!c || !turns || !n) return ASPER_ERR_INVALID;
  asper_buf transcript; asper_buf_init(&transcript);
  asper_err e = asper_buf_appends(&transcript, "Conversation:\n");
  for (size_t i = 0; e == ASPER_OK && i < n; i++) {
    const char *role = turns[i].role == ASPER_ROLE_USER ? "USER" : "ASSISTANT";
    size_t length = strlen(turns[i].text), overhead = strlen(role) + 3;
    if (length > CURATION_TRANSCRIPT_BYTES - overhead ||
        transcript.len > CURATION_TRANSCRIPT_BYTES - length - overhead) break;
    size_t before = transcript.len;
    e = asper_buf_printf(&transcript, "%s: %s\n", role, turns[i].text);
    if (e != ASPER_OK) break;
    /* Count the joined prefix: tokenization need not be additive. The counter
     * runs outside the queue lock and may reenter source append safely. */
    if (c->cfg.transcript_tokens <= 0 ||
        asper_estimate_tokens(c, transcript.data) > c->cfg.transcript_tokens) {
      transcript.len = before; transcript.data[before] = 0; break;
    }
    (*included)++;
  }
  if (e == ASPER_OK && !*included)
    e = asper_seterr(c, ASPER_ERR_LIMIT,
        "curation source %s does not fit the transcript budget (%d tokens, %u bytes)",
        turns[0].source_id, c->cfg.transcript_tokens, CURATION_TRANSCRIPT_BYTES);
  if (e == ASPER_OK) *out = asper_buf_detach(&transcript);
  else *included = 0;
  asper_buf_free(&transcript); return e;
}
