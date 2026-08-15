/*
 * grammar.c — per-cycle GBNF generation + curator op-protocol parsing.
 *
 * Owns the built-in curator instructions (the default instruction is
 * byte-identical to docs/curator_instruction.txt) and the three
 * per-cycle grammar shapes.
 * Pure functions: no context, no locking, no I/O.
 */

#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"

const char ASPER_DEFAULT_INSTRUCTION[] =
    "You maintain the long-term memory of an AI assistant. You will see\n"
    "the recent conversation and a list of existing memories labeled "
    "[M1]...[Mn].\n"
    "\n"
    "Decide what deserves to be remembered.\n"
    "Answer ONLY with operations, one per line:\n"
    "\n"
    "INSERT identity | <fact>    the assistant's persona, values, tone, "
    "style\n"
    "INSERT context | <fact>     the user, their preferences, their "
    "environment\n"
    "INSERT project | <fact>     the currently active project\n"
    "UPDATE M<n> | <fact>        replace an existing memory with a better "
    "version\n"
    "DEPRECATE M<n> | <reason>   the memory is wrong or obsolete\n"
    "NOOP                        nothing worth remembering\n"
    "\n"
    "Rules:\n"
    "- A fact is one self-contained sentence (max 500 characters), "
    "understandable\n"
    "  without the conversation.\n"
    "- Prefer UPDATE over INSERT when an existing memory covers the same "
    "topic.\n"
    "- Store durable facts, not chit-chat. Never store secrets or one-off "
    "trivia.\n"
    "- When in doubt: NOOP.\n";

const char ASPER_REVIEW_INSTRUCTION[] =
    "You maintain the long-term memory of an AI assistant. The memories\n"
    "listed below, labeled [M1]...[Mn], have decayed and are candidates for\n"
    "removal.\n"
    "\n"
    "Review each candidate. Answer ONLY with operations, one per line:\n"
    "\n"
    "DEPRECATE M<n> | <reason>   the memory is wrong, obsolete, or useless\n"
    "KEEP M<n>                   the memory is still valuable\n"
    "NOOP                        no decision on any candidate\n"
    "\n"
    "Rules:\n"
    "- Judge only the listed candidates.\n"
    "- When unsure about a candidate, KEEP it.\n";

const char ASPER_RECALL_INSTRUCTION[] =
    "You answer questions about the long-term memory of an AI assistant,\n"
    "using ONLY the memories listed below, labeled [M1]...[Mn].\n"
    "\n"
    "Answer ONLY in this format:\n"
    "\n"
    "ANSWER | <concise answer, one or two sentences>\n"
    "CITE M<n>                   one line per memory supporting the answer\n"
    "NOMEM                       nothing in the memories answers the "
    "question\n"
    "\n"
    "Rules:\n"
    "- Never use knowledge that is not in the listed memories.\n"
    "- Cite every memory the answer relies on.\n"
    "- If the memories do not answer the question: NOMEM.\n";

/* ---- GBNF generation ---------------------------------------------------- */

static asper_err gbnf_handle_rule(asper_buf *b, size_t handle_count)
{
  asper_err e = asper_buf_appends(b, "handle ::= ");
  for (size_t i = 1; i <= handle_count && e == ASPER_OK; i++) {
    if (i > 1)
      e = asper_buf_appends(b, " | ");
    if (e == ASPER_OK)
      e = asper_buf_printf(b, "\"M%zu\"", i);
  }
  if (e == ASPER_OK)
    e = asper_buf_appendc(b, '\n');
  return e;
}

char *asper_gbnf_build(asper_grammar_kind kind, size_t handle_count,
                       int content_max_chars)
{
  /* content_max_chars is enforced by the engine guardrails, never by
   * the grammar; the parameter is kept for a future length-bounded rule. */
  (void)content_max_chars;

  asper_buf b;
  asper_buf_init(&b);

#define GB(s)                                                                 \
  do {                                                                        \
    if (asper_buf_appends(&b, (s)) != ASPER_OK)                               \
      goto oom;                                                               \
  } while (0)

  switch (kind) {
  case ASPER_GRAMMAR_CURATION:
    GB("root ::= noop | opline+\n");
    if (handle_count > 0)
      GB("opline ::= insert | update | deprecate\n");
    else
      GB("opline ::= insert\n");
    GB("noop ::= \"NOOP\" \"\\n\"\n");
    GB("insert ::= \"INSERT \" (\"identity\" | \"context\" | \"project\") "
       "\" | \" text \"\\n\"\n");
    if (handle_count > 0) {
      GB("update ::= \"UPDATE \" handle \" | \" text \"\\n\"\n");
      GB("deprecate ::= \"DEPRECATE \" handle \" | \" text \"\\n\"\n");
      if (gbnf_handle_rule(&b, handle_count) != ASPER_OK)
        goto oom;
    }
    GB("text ::= [^|\\n\\r]+\n");
    break;

  case ASPER_GRAMMAR_REVIEW:
    if (handle_count == 0) {
      /* Nothing to review: the only legal output is NOOP. */
      GB("root ::= noop\n");
      GB("noop ::= \"NOOP\" \"\\n\"\n");
    } else {
      GB("root ::= noop | revline+\n");
      GB("revline ::= deprecate | keep\n");
      GB("noop ::= \"NOOP\" \"\\n\"\n");
      GB("deprecate ::= \"DEPRECATE \" handle \" | \" text \"\\n\"\n");
      GB("keep ::= \"KEEP \" handle \"\\n\"\n");
      if (gbnf_handle_rule(&b, handle_count) != ASPER_OK)
        goto oom;
      GB("text ::= [^|\\n\\r]+\n");
    }
    break;

  case ASPER_GRAMMAR_RECALL:
    if (handle_count == 0)
      GB("root ::= nomem | answer\n");
    else
      GB("root ::= nomem | (answer citeline*)\n");
    GB("nomem ::= \"NOMEM\" \"\\n\"\n");
    GB("answer ::= \"ANSWER | \" text \"\\n\"\n");
    if (handle_count > 0) {
      GB("citeline ::= \"CITE \" handle \"\\n\"\n");
      if (gbnf_handle_rule(&b, handle_count) != ASPER_OK)
        goto oom;
    }
    GB("text ::= [^|\\n\\r]+\n");
    break;

  default:
    asper_buf_free(&b);
    return NULL;
  }
#undef GB

  return asper_buf_detach(&b);

oom:
  asper_buf_free(&b);
  return NULL;
}

/* ---- protocol parsing --------------------------------------------------- */

/* Match word `w` at the start of `s`, followed by whitespace or the end of
 * the line; on match *rest points past the word and any following blanks. */
static bool word_prefix(char *s, const char *w, char **rest)
{
  size_t l = strlen(w);
  if (strncmp(s, w, l) != 0)
    return false;
  if (s[l] != ' ' && s[l] != '\t' && s[l] != '\0')
    return false;
  char *r = s + l;
  while (*r == ' ' || *r == '\t')
    r++;
  *rest = r;
  return true;
}

/* First " | " separator of a trimmed line. Line-level trimming may have
 * eaten the trailing blank of a separator with empty text ("INSERT x | "),
 * so a trailing " |" also counts. Returns the position of the space before
 * '|', or NULL; the text starts at sep + 2 (then trimmed). */
static char *find_sep(char *s)
{
  char *sep = strstr(s, " | ");
  size_t len = strlen(s);
  if (len >= 2 && s[len - 2] == ' ' && s[len - 1] == '|') {
    char *tail = s + len - 2;
    if (!sep || tail < sep)
      sep = tail;
  }
  return sep;
}

/* "M<n>" with n in [1, handle_count]; the whole token must be consumed. */
static bool parse_handle(const char *tok, size_t handle_count, int *out)
{
  if (tok[0] != 'M')
    return false;
  const char *d = tok + 1;
  if (*d < '0' || *d > '9')
    return false;
  long v = 0;
  while (*d >= '0' && *d <= '9') {
    v = v * 10 + (*d - '0');
    if (v > 1000000)
      return false;
    d++;
  }
  if (*d != '\0')
    return false;
  if (v < 1 || (size_t)v > handle_count)
    return false;
  *out = (int)v;
  return true;
}

/* First whitespace-delimited token of `s`, parsed as a handle. Trailing
 * content after the token is tolerated and ignored. */
static bool parse_handle_token(char *s, size_t handle_count, int *out)
{
  size_t e = strcspn(s, " \t");
  char saved = s[e];
  s[e] = '\0';
  bool ok = parse_handle(s, handle_count, out);
  s[e] = saved;
  return ok;
}

/* Parse one trimmed, mutable line. Returns 1 on success (op filled, text
 * owned by op), 0 on a malformed line, -1 on allocation failure. */
static int parse_line(char *t, size_t handle_count, asper_cop *op)
{
  char *rest;

  if (strcmp(t, "NOOP") == 0) {
    op->kind = ASPER_COP_NOOP;
    return 1;
  }
  if (strcmp(t, "NOMEM") == 0) {
    op->kind = ASPER_COP_NOMEM;
    return 1;
  }

  if (word_prefix(t, "INSERT", &rest)) {
    char *sep = find_sep(rest);
    if (!sep)
      return 0;
    *sep = '\0';
    asper_section sec;
    if (!asper_section_parse(asper_str_trim(rest), &sec))
      return 0;
    if (sec != ASPER_SECTION_IDENTITY && sec != ASPER_SECTION_CONTEXT &&
        sec != ASPER_SECTION_PROJECT)
      return 0;
    op->kind = ASPER_COP_INSERT;
    op->section = sec;
    op->text = asper_strdup(asper_str_trim(sep + 2));
    return op->text ? 1 : -1;
  }

  {
    bool is_upd = word_prefix(t, "UPDATE", &rest);
    bool is_dep = !is_upd && word_prefix(t, "DEPRECATE", &rest);
    if (is_upd || is_dep) {
      char *sep = find_sep(rest);
      if (!sep)
        return 0;
      *sep = '\0';
      int h;
      if (!parse_handle_token(asper_str_trim(rest), handle_count, &h))
        return 0;
      op->kind = is_upd ? ASPER_COP_UPDATE : ASPER_COP_DEPRECATE;
      op->handle = h;
      op->text = asper_strdup(asper_str_trim(sep + 2));
      return op->text ? 1 : -1;
    }
  }

  if (word_prefix(t, "KEEP", &rest) || word_prefix(t, "CITE", &rest)) {
    int h;
    if (!parse_handle_token(rest, handle_count, &h))
      return 0;
    op->kind = (t[0] == 'K') ? ASPER_COP_KEEP : ASPER_COP_CITE;
    op->handle = h;
    return 1;
  }

  if (word_prefix(t, "ANSWER", &rest)) {
    if (*rest != '|')
      return 0;
    op->kind = ASPER_COP_ANSWER;
    op->text = asper_strdup(asper_str_trim(rest + 1));
    return op->text ? 1 : -1;
  }

  return 0;
}

static asper_err cop_push(asper_cop **ops, size_t *n, size_t *cap,
                          const asper_cop *op)
{
  if (*n == *cap) {
    size_t nc = *cap ? *cap * 2 : 8;
    asper_cop *na = realloc(*ops, nc * sizeof *na);
    if (!na)
      return ASPER_ERR_NOMEM;
    *ops = na;
    *cap = nc;
  }
  (*ops)[(*n)++] = *op;
  return ASPER_OK;
}

asper_err asper_protocol_parse(const char *text, size_t handle_count,
                               asper_cop **out_ops, size_t *out_n,
                               size_t *out_bad)
{
  if (out_ops)
    *out_ops = NULL;
  if (out_n)
    *out_n = 0;
  if (out_bad)
    *out_bad = 0;
  if (!text || !out_ops || !out_n)
    return ASPER_ERR_INVALID;

  asper_cop *ops = NULL;
  size_t n = 0, cap = 0, bad = 0;
  const char *p = text;

  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    char *line = asper_strndup(p, len);
    p = nl ? nl + 1 : p + len;
    if (!line)
      goto oom;

    char *t = asper_str_trim(line);
    if (*t == '\0') { /* blank lines are ignored, not counted as bad */
      free(line);
      continue;
    }

    asper_cop op;
    memset(&op, 0, sizeof op);
    int prc = parse_line(t, handle_count, &op);
    free(line);
    if (prc < 0)
      goto oom;
    if (prc == 0) {
      bad++;
      continue;
    }
    if (cop_push(&ops, &n, &cap, &op) != ASPER_OK) {
      free(op.text);
      goto oom;
    }
  }

  *out_ops = ops;
  *out_n = n;
  if (out_bad)
    *out_bad = bad;
  return ASPER_OK;

oom:
  asper_cops_free(ops, n);
  return ASPER_ERR_NOMEM;
}

void asper_cops_free(asper_cop *ops, size_t n)
{
  if (!ops)
    return;
  for (size_t i = 0; i < n; i++)
    free(ops[i].text);
  free(ops);
}
