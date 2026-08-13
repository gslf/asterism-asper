/*
 * fakes.h — deterministic test doubles for libasper.
 *
 * Definitions are binding per docs/IMPLEMENTATION_NOTES.md "Tests":
 *
 * Fake embedder: bag-of-words, dim 16. The text is split on ASCII
 * whitespace; each word is lowercased (ASCII A-Z only) and adds 1.0 to
 * vec[asper_fnv1a64(word) % 16]; the vector is L2-normalized (an empty
 * text stays all-zero). model_id "fake-embedder", model_hash = 32 bytes of
 * 0x11. is_query is ignored, so shared words => high cosine, disjoint
 * words => 0 (modulo bucket collisions).
 *
 * Fake curator: a queue of canned reply strings; generate() captures the
 * prompts/grammar it was given and pops the next reply (or "NOOP\n" when
 * the queue is empty). count_tokens is the byte/4 heuristic (min 1 for
 * non-empty text).
 *
 * Fake clock: settable int64 unix seconds for deterministic time travel.
 *
 * Ownership: the vtables handed to asper_open_with carry no-op destroy
 * hooks — the test owns the fake structs and may inspect them after
 * asper_close.
 */

#ifndef ASPER_FAKES_H
#define ASPER_FAKES_H

#include "asper_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_EMBED_DIM 16

asper_embedder fake_embedder_make(void);
/* The same embedding computed directly, for expected-value math. */
void fake_embed_text(const char *text, float out[FAKE_EMBED_DIM]);

typedef struct {
  char **replies;
  size_t n, cap, next;
  char *last_system;  /* copy of the most recent system prompt */
  char *last_user;    /* copy of the most recent user prompt   */
  char *last_grammar; /* copy of the most recent GBNF (or NULL) */
  int calls;          /* generate() invocations                 */
} fake_curator;

void fake_curator_init(fake_curator *fc);
void fake_curator_dispose(fake_curator *fc);
/* Queue one scripted reply (copied). 1 = ok, 0 = out of memory. */
int fake_curator_push(fake_curator *fc, const char *reply);
asper_curator_iface fake_curator_iface_make(fake_curator *fc);

typedef struct {
  int64_t now;
} fake_clock;

void fake_clock_set(fake_clock *fc, int64_t t);
asper_clock fake_clock_make(fake_clock *fc);

#ifdef __cplusplus
}
#endif

#endif /* ASPER_FAKES_H */
