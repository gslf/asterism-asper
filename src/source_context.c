/* Bounded context materialization over reopenable sources. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

static const char *event_kind_name(asper_event_kind kind) {
  switch (kind) {
    case ASPER_EVENT_USER: return "user";
    case ASPER_EVENT_ASSISTANT: return "assistant";
    case ASPER_EVENT_DECISION: return "decision";
    case ASPER_EVENT_TOOL_CALL: return "tool_call";
    case ASPER_EVENT_TOOL_RESULT: return "tool_result";
    case ASPER_EVENT_DIAGNOSTIC: return "diagnostic";
    case ASPER_EVENT_CHECKPOINT: return "checkpoint";
    case ASPER_EVENT_ARTIFACT: return "artifact";
  }
  return "event";
}

static size_t count_tokens(const asper_context_request *req,
                           const char *text) {
  int n;
  if (!text || !text[0]) return 0;
  if (req->count_tokens) {
    n = req->count_tokens(text, req->count_userdata);
    if (n >= 0) return (size_t)n;
  }
  return (strlen(text) + 3) / 4;
}

static asper_err append_event_text(asper_buf *b, const asper_event *event) {
  const char *name = event_kind_name(event->kind);
  asper_err e = asper_buf_printf(b, "[%s:%s] ", name, event->id);
  if (e == ASPER_OK && event->text && event->text[0])
    e = asper_buf_appends(b, event->text);
  if (e == ASPER_OK && event->object_ref[0])
    e = asper_buf_printf(b, " [source %s]", event->object_ref);
  if (e == ASPER_OK) e = asper_buf_appendc(b, '\n');
  return e;
}

asper_err asper_context_materialize(asper_ctx *c,
    const asper_context_request *request, asper_context_pack *out) {
  char *project=NULL; asper_err e=asper_project_active(c,&project);
  if (e==ASPER_OK) e=asper_context_materialize_project(c,request,project,out);
  free(project);return e;
}
asper_err asper_context_materialize_project(asper_ctx *c,
                                    const asper_context_request *request,
                                    const char *project, asper_context_pack *out) {
  asper_event *events = NULL;
  size_t n = 0, history_used = 0, included = 0;
  unsigned char *take = NULL;
  char *checkpoint = NULL;
  asper_buf context;
  asper_err e;
  if (!c || !request || !out || !asper_source_scope_valid(request->scope) ||
      !request->query) return ASPER_ERR_INVALID;
  memset(out, 0, sizeof *out);
  e = asper_memory_render_project(c,
                          request->base_system_prompt
                              ? request->base_system_prompt : "",
                          request->query, project, &out->system_prompt);
  if (e != ASPER_OK) goto fail;
  out->system_tokens = count_tokens(request, out->system_prompt);
  e = asper_event_list(c, request->scope, &events, &n);
  if (e != ASPER_OK) goto fail;
  out->events_available = n;
  take = (unsigned char *)calloc(n ? n : 1, 1);
  if (!take) {
    e = ASPER_ERR_NOMEM;
    goto fail;
  }
  asper_buf_init(&context);
  if (request->checkpoint_tokens > 0 &&
      asper_checkpoint_load(c, request->scope, &checkpoint) == ASPER_OK &&
      checkpoint && checkpoint[0]) {
    size_t cost = count_tokens(request, checkpoint) + 6;
    if (cost <= request->checkpoint_tokens) {
      e = asper_buf_appends(&context, "## Working checkpoint\n");
      if (e == ASPER_OK) e = asper_buf_appends(&context, checkpoint);
      if (e == ASPER_OK) e = asper_buf_appends(&context, "\n\n");
      if (e != ASPER_OK) goto fail_context;
    }
  }
  /* Pinned source events have priority.  Within each class preserve original
   * order; then fill the remaining envelope from the newest exact tail. */
  for (size_t i = 0; i < n; i++) {
    asper_buf line;
    size_t cost;
    if (!events[i].pinned || events[i].kind == ASPER_EVENT_CHECKPOINT)
      continue;
    asper_buf_init(&line);
    e = append_event_text(&line, &events[i]);
    if (e != ASPER_OK) {
      asper_buf_free(&line);
      goto fail_context;
    }
    cost = count_tokens(request, line.data);
    asper_buf_free(&line);
    if (history_used + cost <= request->history_tokens) {
      take[i] = 1;
      history_used += cost;
    }
  }
  for (size_t i = n; i > 0; i--) {
    size_t j = i - 1;
    asper_buf line;
    size_t cost;
    if (take[j] || events[j].kind == ASPER_EVENT_CHECKPOINT) continue;
    if ((events[j].kind == ASPER_EVENT_USER ||
         events[j].kind == ASPER_EVENT_ASSISTANT) &&
        strcmp(events[j].text, request->query) == 0)
      continue;
    asper_buf_init(&line);
    e = append_event_text(&line, &events[j]);
    if (e != ASPER_OK) {
      asper_buf_free(&line);
      goto fail_context;
    }
    cost = count_tokens(request, line.data);
    asper_buf_free(&line);
    if (history_used + cost <= request->history_tokens) {
      take[j] = 1;
      history_used += cost;
    }
  }
  if (n > 0) {
    int heading = 0;
    for (size_t i = 0; i < n; i++) {
      if (!take[i]) continue;
      if (!heading) {
        e = asper_buf_appends(&context, "## Memory source events\n");
        if (e != ASPER_OK) goto fail_context;
        heading = 1;
      }
      e = append_event_text(&context, &events[i]);
      if (e != ASPER_OK) goto fail_context;
      included++;
    }
  }
  out->context_text = asper_buf_detach(&context);
  if (!out->context_text) out->context_text = asper_strdup("");
  if (!out->context_text) {
    e = ASPER_ERR_NOMEM;
    goto fail_context_detached;
  }
  out->context_tokens = count_tokens(request, out->context_text);
  out->events_included = included;
  free(checkpoint);
  free(take);
  asper_events_free(events, n);
  return ASPER_OK;

fail_context:
  asper_buf_free(&context);
fail_context_detached:
  free(checkpoint);
fail:
  free(take);
  asper_events_free(events, n);
  asper_context_pack_free(out);
  return e;
}

void asper_context_pack_free(asper_context_pack *pack) {
  if (!pack) return;
  free(pack->system_prompt);
  free(pack->context_text);
  memset(pack, 0, sizeof *pack);
}
