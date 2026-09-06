/* Compile this same probe against each revision to compare restart behavior. */
#include "asper_internal.h"
#include "fakes.h"
#include <stdlib.h>
#include <string.h>

static int generated, embeddings;
static asper_embedder base_embedder;
static asper_curator_iface base_curator;
static asper_err embed(void *ud, const char *text, int query, const asmodel_embed_params *params, float *out) {
  if (generated && ++embeddings == 3) _Exit(82); /* First INSERT is already journaled. */
  return base_embedder.embed(ud, text, query, params, out);
}
static asper_err generate(void *ud, const asmodel_input *input, const char *grammar,
    const asper_output_contract *contract, const asmodel_generate_params *params,
    volatile int *cancel, char **out) {
  asper_err e = base_curator.generate(ud, input, grammar, contract, params, cancel, out);
  generated = 1; return e;
}
int main(int argc, char **argv) {
  if (argc != 3 || (strcmp(argv[1], "interrupt") && strcmp(argv[1], "resume"))) return 2;
  fake_curator curator; fake_curator_init(&curator);
  fake_clock clock = {0}; fake_clock_set(&clock, 1785319920);
  base_curator = fake_curator_iface_make(&curator); base_embedder = fake_embedder_make();
  asper_curator_iface cur = base_curator; cur.generate = generate;
  asper_embedder emb = base_embedder; emb.embed = embed;
  asper_clock clk = fake_clock_make(&clock);
  asper_open_params p = {0}; p.memory_root = argv[2]; asper_ctx *c = NULL;
  if (asper_open_with(&p, &emb, &cur, &clk, &c) != ASPER_OK) return 3;
  asper_worker_stop(c); c->no_threads = true;
  if (!strcmp(argv[1], "interrupt")) {
    if (!fake_curator_push(&curator, "INSERT context | Otters inhabit freshwater rivers\n"
        "INSERT context | Copper conducts electrical current\n") ||
        fake_event_append(c, "probe", ASPER_EVENT_USER, "remember both facts") != ASPER_OK ||
        fake_event_append(c, "probe", ASPER_EVENT_ASSISTANT, "observed") != ASPER_OK) return 4;
    (void)asper_flush(c, 1); return 5;
  }
  size_t pending = c->turns_n;
  asper_err e = asper_flush(c, 1);
  printf("{\"requeued_sources\":%zu,\"curator_calls\":%d,\"flush\":\"%s\",\"records\":%zu}\n",
         pending, curator.calls, asper_err_name(e), c->store.table.n);
  asper_close(c); fake_curator_dispose(&curator); return 0;
}
