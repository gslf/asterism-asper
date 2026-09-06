/* Real store/queue boundaries with a deterministic curator, without weights. */
#include "asper_internal.h"
#include "fakes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static asper_err startup(void *ud, const asmodel_input *input, const char *grammar,
    const asper_output_contract *contract, const asmodel_generate_params *params,
    volatile int *cancel, char **out) {
  (void)ud; (void)input; (void)grammar; (void)contract; (void)params; (void)cancel;
  *out = NULL; return ASPER_ERR_BUSY;
}
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  fake_curator fake; fake_curator_init(&fake);
  fake_clock clock = {0}; fake_clock_set(&clock,1785319920);
  asper_curator_iface cur = fake_curator_iface_make(&fake), held = cur;
  held.generate = startup;
  asper_embedder emb = fake_embedder_make(); asper_clock clk = fake_clock_make(&clock);
  asper_open_params params = {0}; params.memory_root = argv[2];
  asper_ctx *c = NULL;
  asper_err e = asper_open_with(&params,&emb,&held,&clk,&c);
  if (e != ASPER_OK) {
    printf("{\"open\":\"%s\"}\n",asper_err_name(e)); fake_curator_dispose(&fake); return 0;
  }
  asper_worker_stop(c); c->no_threads = true; c->curator = cur;
  c->cfg.transcript_tokens = !strcmp(argv[1],"drain-large") ? 8192 : 128;
  char id[37] = "", second[37] = "";
  if (!strcmp(argv[1],"seed")) {
    char text[5001]; memset(text,'x',sizeof text-1); text[sizeof text-1] = 0;
    asper_event_input event = {0}; event.scope = "deferred"; event.kind = ASPER_EVENT_USER;
    event.text = text; e = asper_event_append(c,&event,id);
    event.kind = ASPER_EVENT_ASSISTANT;
    event.text = "a later source with Unicode: tè 🍵";
    if (e == ASPER_OK) e = asper_event_append(c,&event,second);
  } else if (!strcmp(argv[1],"drain") || !strcmp(argv[1],"drain-large")) {
    e = asper_flush(c,1);
  } else if (strcmp(argv[1],"open")) e = ASPER_ERR_INVALID;
  asper_stats stats = {0}; asper_get_stats(c,&stats);
  printf("{\"open\":\"ASPER_OK\",\"result\":\"%s\",\"source\":\"%s\",\"second\":\"%s\","
      "\"calls\":%d,\"deferred\":%zu,\"queued\":%zu}\n",asper_err_name(e),id,second,
      fake.calls,stats.curation_deferred,stats.curation_queued);
  asper_close(c); fake_curator_dispose(&fake); return 0;
}
