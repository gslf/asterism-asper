/* Keep selected lines only. Old pins take priority; the remaining budget takes
 * the newest fitting events, then presentation restores ascending sequence. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

#define HEADING "## Memory source events\n"
typedef struct { uint64_t sequence; char *text; } selected_line;
typedef struct {
  selected_line *lines;
  size_t n, cap, bytes, tokens, byte_limit;
} selection;

size_t asper_source_tokens(const asper_context_request *request, const char *text) {
  if (!text || !*text) return 0;
  int n = request->count_tokens ? request->count_tokens(text, request->count_userdata) : -1;
  return n >= 0 ? (size_t)n : (strlen(text) + 3) / 4;
}

static const char *kind_name(asper_event_kind kind) {
  static const char *names[] = {"user", "assistant", "decision", "tool_call",
      "tool_result", "diagnostic", "checkpoint", "artifact"};
  return kind >= ASPER_EVENT_USER && kind <= ASPER_EVENT_ARTIFACT ? names[kind] : "event";
}

static asper_err select_event(selection *s, const asper_context_request *request,
                               const asper_event *event) {
  if (strlen(event->text) > s->byte_limit - s->bytes) return ASPER_OK;
  asper_buf line; asper_buf_init(&line);
  asper_err e = asper_buf_printf(&line, "[%s:%s] %s", kind_name(event->kind), event->id, event->text);
  if (e == ASPER_OK && event->object_ref[0]) e = asper_buf_printf(&line, " [source %s]", event->object_ref);
  if (e == ASPER_OK) e = asper_buf_appendc(&line, '\n');
  if (e != ASPER_OK) { asper_buf_free(&line); return e; }
  size_t cost = asper_source_tokens(request, line.data);
  if (cost > request->history_tokens - s->tokens || line.len > s->byte_limit - s->bytes) {
    asper_buf_free(&line); return ASPER_OK;
  }
  if (s->n == s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 8;
    if (cap > ASPER_CONTEXT_EVENTS) cap = ASPER_CONTEXT_EVENTS;
    selected_line *rows = realloc(s->lines, cap * sizeof *rows);
    if (!rows) { asper_buf_free(&line); return ASPER_ERR_NOMEM; }
    s->lines = rows; s->cap = cap;
  }
  s->bytes += line.len; s->tokens += cost;
  s->lines[s->n++] = (selected_line){event->sequence, asper_buf_detach(&line)};
  return ASPER_OK;
}

static int already_pinned(const selection *s, size_t pins, uint64_t sequence) {
  size_t lo = 0, hi = pins;
  while (lo < hi) {
    size_t mid = lo + (hi-lo)/2;
    if (s->lines[mid].sequence < sequence) lo = mid+1; else hi = mid;
  }
  return lo < pins && s->lines[lo].sequence == sequence;
}

static int sequence_order(const void *a, const void *b) {
  const selected_line *x = a, *y = b;
  return (x->sequence > y->sequence) - (x->sequence < y->sequence);
}

asper_err asper_source_history(asper_ctx *c, const asper_context_request *request,
                              asper_buf *context, size_t *available, size_t *included) {
  asper_source_view view;
  selection chosen = {0};
  asper_err e = asper_source_view_open(c, request->scope, &view);
  if (e != ASPER_OK) return e;
  *available = (size_t)view.files.count; *included = 0;
  chosen.tokens = asper_source_tokens(request, HEADING);
  if (!request->history_tokens || chosen.tokens > request->history_tokens ||
      context->len > ASPER_CONTEXT_BYTES - (sizeof HEADING - 1)) goto done;
  chosen.byte_limit = ASPER_CONTEXT_BYTES - context->len - (sizeof HEADING - 1);
  size_t pins = 0;
  for (int pass = 0; pass < 2 && e == ASPER_OK; pass++) {
    for (uint64_t offset = 0; offset < view.files.count && chosen.n < ASPER_CONTEXT_EVENTS; offset++) {
      uint64_t sequence = pass ? view.files.count - offset : offset + 1;
      if (pass && already_pinned(&chosen, pins, sequence)) continue;
      asper_event event;
      e = pass ? asper_source_view_read(&view, sequence, &event) :
          asper_source_view_head(&view, sequence, &event);
      if (e != ASPER_OK) break;
      bool eligible = event.kind != ASPER_EVENT_CHECKPOINT && (pass || event.pinned);
      if (eligible && !pass) e = asper_source_view_read(&view, sequence, &event);
      if (pass && (event.kind == ASPER_EVENT_USER || event.kind == ASPER_EVENT_ASSISTANT) &&
          !strcmp(event.text, request->query)) eligible = false;
      if (e == ASPER_OK && eligible) e = select_event(&chosen, request, &event);
      free(event.text);
      if (e != ASPER_OK) break;
    }
    if (!pass) pins = chosen.n;
  }
  if (e == ASPER_OK && chosen.n) {
    qsort(chosen.lines, chosen.n, sizeof *chosen.lines, sequence_order);
    size_t start = context->len;
    e = asper_buf_appends(context, HEADING);
    for (size_t i = 0; e == ASPER_OK && i < chosen.n; i++) e = asper_buf_appends(context, chosen.lines[i].text);
    /* Tokenizers need not be additive across concatenated pieces. */
    if (e == ASPER_OK && asper_source_tokens(request, context->data + start) > request->history_tokens)
      e = ASPER_ERR_LIMIT;
    if (e == ASPER_OK) *included = chosen.n;
  }
done:
  for (size_t i = 0; i < chosen.n; i++) free(chosen.lines[i].text);
  free(chosen.lines); asper_source_view_close(&view); return e;
}
