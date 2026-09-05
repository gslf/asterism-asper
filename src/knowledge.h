#ifndef ASPER_KNOWLEDGE_H
#define ASPER_KNOWLEDGE_H
#include "asper_internal.h"
#include "asmodel_json.h"
#define KNOWLEDGE_MAX_ITEMS 16
#define KNOWLEDGE_MAX_RECORDS 16384

typedef struct {
  char id[37], hash[65];
  char *content;
  unsigned long long revision;
  asper_grounding *grounding;
  bool sources_ok;
} knowledge_entry;
typedef struct asper_knowledge {
  knowledge_entry *entries; size_t n, cap;
  asper_dependency *observations; size_t observed_n, observed_cap;
  char *path;
  bool poisoned;
} asper_knowledge;

bool knowledge_hash(const char *s);
void knowledge_content_hash(const char *text, char out[65]);
knowledge_entry *knowledge_find(asper_knowledge *k, const char *id);
void knowledge_entry_free(knowledge_entry *entry);
char *knowledge_encode(const knowledge_entry *entry);
asper_err knowledge_decode(const char *text, knowledge_entry *out);
asper_err knowledge_validate(asper_ctx *c, const asper_record *record,
                             const asper_grounding *grounding);
asper_err knowledge_sources(asper_ctx *c, const asper_grounding *grounding);
asper_err knowledge_commit(asper_ctx *c, knowledge_entry *entry);
#endif
