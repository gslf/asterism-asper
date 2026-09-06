/* Opt-in component probe: identical source history, no model generation. */
#include "asper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static asper_err append(asper_ctx *c, const char *text, int pinned) {
  asper_event_input input = {0}; input.scope = "bench";
  input.kind = ASPER_EVENT_DIAGNOSTIC; input.text = text; input.pinned = pinned;
  return asper_event_append(c, &input, NULL);
}
int main(int argc, char **argv) {
  if (argc != 3 && argc != 5) return 2;
  int create = !strcmp(argv[1], "create");
  if ((!create && strcmp(argv[1], "measure")) || argc != (create ? 5 : 3)) return 2;
  size_t count = 0, bytes = 0;
  if (create) {
    char *end;
    count = (size_t)strtoul(argv[3], &end, 10); if (*end || !count) return 2;
    bytes = (size_t)strtoul(argv[4], &end, 10);
    if (*end || bytes < 4096 || bytes > 1024u * 1024u || count > 256u * 1024u * 1024u / bytes) return 2;
  }
  asper_open_params params = {0}; params.memory_root = argv[2];
  asper_ctx *c = NULL;
  asper_err e = asper_open(&params, &c);
  if (e != ASPER_OK) return 1;
  asper_set_logger(c, NULL, NULL);
  if (create) {
    char *text = malloc(bytes + 1);
    if (!text) e = ASPER_ERR_NOMEM;
    else {
      memset(text, 'x', bytes); text[bytes] = 0;
      e = append(c, "The public API uses C99.", 1);
      for (size_t i = 0; e == ASPER_OK && i < count; i++) e = append(c, text, 0);
      free(text);
      for (int i = 0; e == ASPER_OK && i < 8; i++) {
        char line[96]; snprintf(line, sizeof line, "Recent diagnostic %d: build output is available.", i);
        e = append(c, line, 0);
      }
    }
  } else {
    asper_context_request req = {0}; req.scope = "bench"; req.query = "continue"; req.history_tokens = 512;
    asper_context_pack pack;
    e = asper_context_materialize(c, &req, &pack);
    if (e == ASPER_OK) {
      if (pack.events_included != 9) e = ASPER_ERR_PARSE;
      else printf("%zu\n%s", pack.events_included, pack.context_text);
      asper_context_pack_free(&pack);
    }
  }
  asper_close(c);
  if (e != ASPER_OK) fprintf(stderr, "%s\n", asper_err_name(e));
  return e == ASPER_OK ? 0 : 1;
}
