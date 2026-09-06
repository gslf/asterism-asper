/* Context sections share explicit token and byte envelopes. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

asper_err asper_context_materialize(asper_ctx *c,
    const asper_context_request *request, asper_context_pack *out) {
  char *project = NULL;
  asper_err e = asper_project_active(c, &project);
  if (e == ASPER_OK) e = asper_context_materialize_project(c, request, project, out);
  free(project); return e;
}

static asper_err append_checkpoint(asper_ctx *c, const asper_context_request *request,
                                   asper_buf *context) {
  if (!request->checkpoint_tokens) return ASPER_OK;
  char *text = NULL;
  asper_err e = asper_checkpoint_load(c, request->scope, &text);
  if (e == ASPER_ERR_NOT_FOUND) return ASPER_OK;
  if (e != ASPER_OK) return e;
  asper_buf section; asper_buf_init(&section);
  if (text && *text && strlen(text) <= ASPER_CONTEXT_BYTES - 24) {
    e = asper_buf_printf(&section, "## Working checkpoint\n%s\n\n", text);
    if (e == ASPER_OK && asper_source_tokens(request, section.data) <= request->checkpoint_tokens)
      e = asper_buf_append(context, section.data, section.len);
  }
  asper_buf_free(&section); free(text); return e;
}

asper_err asper_context_materialize_project(asper_ctx *c,
    const asper_context_request *request, const char *project, asper_context_pack *out) {
  if (!c || !request || !out || !asper_source_scope_valid(request->scope) || !request->query)
    return ASPER_ERR_INVALID;
  memset(out, 0, sizeof *out);
  asper_buf context; asper_buf_init(&context);
  asper_err e = asper_memory_render_project(c, request->base_system_prompt ? request->base_system_prompt : "",
                                            request->query, project, &out->system_prompt);
  if (e == ASPER_OK) {
    out->system_tokens = asper_source_tokens(request, out->system_prompt);
    e = append_checkpoint(c, request, &context);
  }
  if (e == ASPER_OK) e = asper_source_history(c, request, &context, &out->events_available, &out->events_included);
  if (e == ASPER_OK) {
    out->context_text = asper_buf_detach(&context);
    if (!out->context_text) out->context_text = asper_strdup("");
    if (!out->context_text) e = ASPER_ERR_NOMEM;
    else {
      out->context_tokens = asper_source_tokens(request, out->context_text);
      size_t budget = request->checkpoint_tokens > SIZE_MAX - request->history_tokens ? SIZE_MAX :
          request->checkpoint_tokens + request->history_tokens;
      if (out->context_tokens > budget) e = ASPER_ERR_LIMIT;
    }
  }
  asper_buf_free(&context);
  if (e != ASPER_OK) asper_context_pack_free(out);
  return e;
}

void asper_context_pack_free(asper_context_pack *pack) {
  if (!pack) return;
  free(pack->system_prompt); free(pack->context_text); memset(pack, 0, sizeof *pack);
}
