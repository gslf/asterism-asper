/* Produce a real interrupted batch for the offline operator integration test. */
#include "curation_receipt.h"
#include "fakes.h"
#include <stdlib.h>

static asper_err interrupt_after_mutation(int stage) {
  if (stage == 2) _Exit(82);
  return ASPER_OK;
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  fake_curator curator; fake_curator_init(&curator);
  fake_clock clock = {0}; fake_clock_set(&clock, 1785319920);
  asper_curator_iface cur = fake_curator_iface_make(&curator);
  asper_embedder emb = fake_embedder_make();
  asper_clock clk = fake_clock_make(&clock);
  asper_open_params p = {0}; p.memory_root = argv[1];
  asper_ctx *c = NULL;
  if (asper_open_with(&p, &emb, &cur, &clk, &c) != ASPER_OK) return 3;
  asper_worker_stop(c); c->no_threads = true;
  c->store.curation_checkpoint = interrupt_after_mutation;
  if (!fake_curator_push(&curator, "INSERT context | Otters inhabit freshwater rivers\n") ||
      fake_event_append(c, "receipt", ASPER_EVENT_USER, "remember the otters") != ASPER_OK ||
      fake_event_append(c, "receipt", ASPER_EVENT_ASSISTANT, "observed") != ASPER_OK) return 4;
  (void)asper_curation_cycle(c, true);
  asper_close(c); fake_curator_dispose(&curator); return 5;
}
