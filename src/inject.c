/*
 * inject.c — budgets, template render, token estimation.
 *
 * Rendering contract (IMPLEMENTATION_NOTES "Injection rendering"):
 *   - Each record renders as "- <content>\n"; the block substituted for a
 *     placeholder carries newlines BETWEEN records only — the template's own
 *     newline after the placeholder terminates the last record line, so no
 *     blank line is introduced beyond the template's own.
 *   - A section with zero surviving records loses its whole template
 *     segment: the placeholder line, the contiguous non-empty heading block
 *     immediately above it, and the single blank line separating that block
 *     from what precedes it.
 *   - Nothing injected at all => copy of base_system_prompt, unchanged.
 *
 * Survivor-marking contract (IMPLEMENTATION_NOTES "build_prompt"): the
 * record arrays are caller-owned CLONES used as scratch; every clone
 * dropped by budget trimming (or belonging to a section whose placeholder
 * is absent from the template) gets score = -1 so api.c can queue access
 * events for exactly the records that were injected (score != -1).
 *
 * Fully deterministic: byte operations only, no locale-dependent calls.
 */

#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"

/* Byte-identical to docs/injection_template.txt (final newline included). */
const char ASPER_DEFAULT_TEMPLATE[] =
    "{base_system_prompt}\n"
    "\n"
    "# Memory\n"
    "\n"
    "## Who you are\n"
    "{identity_block}\n"
    "\n"
    "## What you remember about the user and the environment\n"
    "{context_block}\n"
    "\n"
    "## Active project: {project_name}\n"
    "{project_block}\n";

#define PH_BASE     "{base_system_prompt}"
#define PH_IDENTITY "{identity_block}"
#define PH_CONTEXT  "{context_block}"
#define PH_PROJECT  "{project_block}"
#define PH_PNAME    "{project_name}"

int asper_identity_cmp(const void *a, const void *b)
{
  const asper_record *ra = *(asper_record *const *)a;
  const asper_record *rb = *(asper_record *const *)b;
  if (ra->locked != rb->locked)
    return ra->locked ? -1 : 1;
  if (ra->relevance != rb->relevance)
    return ra->relevance > rb->relevance ? -1 : 1;
  if (ra->created_at != rb->created_at)
    return ra->created_at < rb->created_at ? -1 : 1;
  return strcmp(ra->id, rb->id);
}

int asper_estimate_tokens(asper_ctx *c, const char *text)
{
  if (!text)
    return 0;
  if (c && c->cfg.token_estimator == ASPER_TOKENS_CURATOR && c->has_curator) {
    int n = c->curator.count_tokens(c->curator.ud, text);
    if (n >= 0)
      return n;
    /* Tokenizer failure: fall through to the heuristic. */
  }
  return asper_token_heuristic(text);
}

/* Tokens of the rendered record line "- <content>\n" (budgets count
 * record lines; headings are free). */
static int record_line_tokens(asper_ctx *c, const char *content,
                              asper_err *err)
{
  size_t n = strlen(content);
  char *line = malloc(n + 4);
  if (!line) {
    *err = ASPER_ERR_NOMEM;
    return 0;
  }
  line[0] = '-';
  line[1] = ' ';
  memcpy(line + 2, content, n);
  line[n + 2] = '\n';
  line[n + 3] = '\0';
  int t = asper_estimate_tokens(c, line);
  free(line);
  return t;
}

/* The clone arrays are caller-owned scratch (see the survivor-marking
 * contract above): writing score here never touches a table record. The
 * pointed-to records are non-const by declaration (asper_record *const *),
 * so no const needs casting away. */
static void mark_dropped(asper_record *const *recs, size_t from, size_t n)
{
  for (size_t i = from; i < n; i++)
    recs[i]->score = -1.0;
}

/* Keep the longest prefix whose cumulative line-token total fits budget
 * ("drop from the tail": arrays arrive already precedence-ordered);
 * mark everything after the cut as dropped. */
static asper_err section_trim(asper_ctx *c, asper_record *const *recs,
                              size_t n, int budget, size_t *out_kept)
{
  size_t kept = 0;
  int used = 0;
  while (kept < n) {
    const char *content = recs[kept]->content ? recs[kept]->content : "";
    asper_err err = ASPER_OK;
    int t = record_line_tokens(c, content, &err);
    if (err != ASPER_OK)
      return err;
    if (used + t > budget)
      break;
    used += t;
    kept++;
  }
  mark_dropped(recs, kept, n);
  *out_kept = kept;
  return ASPER_OK;
}

/* Block text: "- c1\n- c2\n...- ck" — newline between records, none
 * trailing (the template's newline after the placeholder supplies it). */
static asper_err section_block(asper_record *const *recs, size_t kept,
                               asper_buf *b)
{
  for (size_t i = 0; i < kept; i++) {
    const char *content = recs[i]->content ? recs[i]->content : "";
    asper_err e;
    if (i > 0 && (e = asper_buf_appendc(b, '\n')) != ASPER_OK)
      return e;
    if ((e = asper_buf_appends(b, "- ")) != ASPER_OK)
      return e;
    if ((e = asper_buf_appends(b, content)) != ASPER_OK)
      return e;
  }
  return ASPER_OK;
}

/* ---- template line surgery ---------------------------------------------- */

typedef struct {
  const char *start;
  size_t len;   /* includes the terminating '\n' when present */
  bool blank;   /* spaces/tabs/CR only before the terminator */
  bool barrier; /* holds a block placeholder or {base_system_prompt}:
                   a heading walk never crosses it */
  bool drop;
} tmpl_line;

static bool span_contains(const char *s, size_t n, const char *needle)
{
  size_t m = strlen(needle);
  if (m == 0 || m > n)
    return false;
  for (size_t i = 0; i + m <= n; i++) {
    if (memcmp(s + i, needle, m) == 0)
      return true;
  }
  return false;
}

static asper_err template_lines(const char *tmpl, tmpl_line **out,
                                size_t *out_n)
{
  *out = NULL;
  *out_n = 0;
  size_t nlines = 0;
  for (const char *p = tmpl; *p;) {
    const char *nl = strchr(p, '\n');
    nlines++;
    if (!nl)
      break;
    p = nl + 1;
  }
  if (nlines == 0)
    return ASPER_OK;

  tmpl_line *ls = calloc(nlines, sizeof *ls);
  if (!ls)
    return ASPER_ERR_NOMEM;
  size_t i = 0;
  for (const char *p = tmpl; *p && i < nlines; i++) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);
    size_t body = nl ? len - 1 : len;
    ls[i].start = p;
    ls[i].len = len;
    ls[i].blank = true;
    for (size_t j = 0; j < body; j++) {
      char ch = p[j];
      if (ch != ' ' && ch != '\t' && ch != '\r') {
        ls[i].blank = false;
        break;
      }
    }
    ls[i].barrier = span_contains(p, body, PH_IDENTITY) ||
                    span_contains(p, body, PH_CONTEXT) ||
                    span_contains(p, body, PH_PROJECT) ||
                    span_contains(p, body, PH_BASE);
    p += len;
  }
  *out = ls;
  *out_n = nlines;
  return ASPER_OK;
}

static size_t line_of(const tmpl_line *ls, size_t nlines, const char *pos)
{
  for (size_t i = 0; i < nlines; i++) {
    if (pos >= ls[i].start && pos < ls[i].start + ls[i].len)
      return i;
  }
  return nlines;
}

/* Empty-section segment removal: the placeholder line, the contiguous
 * non-empty heading block immediately above it, and the one blank line
 * separating that block from the rest of the template. */
static void drop_segment(tmpl_line *ls, size_t pl)
{
  ls[pl].drop = true;
  size_t i = pl;
  while (i > 0 && !ls[i - 1].blank && !ls[i - 1].drop && !ls[i - 1].barrier)
    ls[--i].drop = true;
  if (i > 0 && ls[i - 1].blank && !ls[i - 1].drop)
    ls[i - 1].drop = true;
}

/* ---- placeholder substitution ------------------------------------------- */

static asper_err substitute(asper_buf *out, const char *text,
                            const char *base, const char *idb,
                            const char *ctb, const char *prb,
                            const char *pname)
{
  const char *p = text;
  asper_err e;
  while (*p) {
    const char *br = strchr(p, '{');
    if (!br) {
      return asper_buf_appends(out, p);
    }
    if (br > p && (e = asper_buf_append(out, p, (size_t)(br - p))) != ASPER_OK)
      return e;
    p = br;
    const char *rep = NULL;
    size_t adv = 0;
    if (strncmp(p, PH_BASE, sizeof PH_BASE - 1) == 0) {
      rep = base;
      adv = sizeof PH_BASE - 1;
    } else if (strncmp(p, PH_IDENTITY, sizeof PH_IDENTITY - 1) == 0) {
      rep = idb;
      adv = sizeof PH_IDENTITY - 1;
    } else if (strncmp(p, PH_CONTEXT, sizeof PH_CONTEXT - 1) == 0) {
      rep = ctb;
      adv = sizeof PH_CONTEXT - 1;
    } else if (strncmp(p, PH_PROJECT, sizeof PH_PROJECT - 1) == 0) {
      rep = prb;
      adv = sizeof PH_PROJECT - 1;
    } else if (strncmp(p, PH_PNAME, sizeof PH_PNAME - 1) == 0) {
      rep = pname;
      adv = sizeof PH_PNAME - 1;
    }
    if (rep) {
      if ((e = asper_buf_appends(out, rep)) != ASPER_OK)
        return e;
      p += adv;
    } else {
      if ((e = asper_buf_appendc(out, '{')) != ASPER_OK)
        return e;
      p++;
    }
  }
  return ASPER_OK;
}

/* ---- render -------------------------------------------------------------- */

asper_err asper_inject_render(asper_ctx *c, const char *base_system_prompt,
                              asper_record *const *identity, size_t id_n,
                              asper_record *const *ctxr, size_t ctx_n,
                              asper_record *const *proj, size_t proj_n,
                              const char *project_name, char **out_prompt)
{
  if (!c || !out_prompt)
    return ASPER_ERR_INVALID;
  *out_prompt = NULL;

  const char *base = base_system_prompt ? base_system_prompt : "";
  const char *pname = project_name ? project_name : "";

  char *owned_tmpl = NULL;
  const char *tmpl = ASPER_DEFAULT_TEMPLATE;
  if (c->cfg.template_path) {
    asper_err re = os_read_file(c->cfg.template_path, &owned_tmpl, NULL);
    if (re != ASPER_OK)
      return asper_seterr(c, re, "inject: cannot read template '%s'",
                          c->cfg.template_path);
    tmpl = owned_tmpl;
  }

  const char *ph_id = strstr(tmpl, PH_IDENTITY);
  const char *ph_ctx = strstr(tmpl, PH_CONTEXT);
  const char *ph_proj = strstr(tmpl, PH_PROJECT);

  asper_err e = ASPER_OK;
  size_t id_kept = 0, ctx_kept = 0, proj_kept = 0;

  /* A section whose placeholder is absent from a custom template cannot be
   * injected: all of its records count as dropped. */
  if (!ph_id)
    mark_dropped(identity, 0, id_n);
  else
    e = section_trim(c, identity, id_n, c->cfg.identity_tokens, &id_kept);
  if (e == ASPER_OK) {
    if (!ph_ctx)
      mark_dropped(ctxr, 0, ctx_n);
    else
      e = section_trim(c, ctxr, ctx_n, c->cfg.context_tokens, &ctx_kept);
  }
  if (e == ASPER_OK) {
    if (!ph_proj)
      mark_dropped(proj, 0, proj_n);
    else
      e = section_trim(c, proj, proj_n, c->cfg.project_tokens, &proj_kept);
  }
  if (e != ASPER_OK) {
    free(owned_tmpl);
    return asper_seterr(c, e, "inject: out of memory");
  }

  asper_log(c, ASPER_LOG_DEBUG, "inject",
            "render identity=%zu/%zu context=%zu/%zu project=%zu/%zu",
            id_kept, id_n, ctx_kept, ctx_n, proj_kept, proj_n);

  if (id_kept + ctx_kept + proj_kept == 0) {
    free(owned_tmpl);
    char *copy = asper_strdup(base);
    if (!copy)
      return asper_seterr(c, ASPER_ERR_NOMEM, "inject: out of memory");
    *out_prompt = copy;
    return ASPER_OK;
  }

  asper_buf idb, ctb, prb, joined, outb;
  asper_buf_init(&idb);
  asper_buf_init(&ctb);
  asper_buf_init(&prb);
  asper_buf_init(&joined);
  asper_buf_init(&outb);
  tmpl_line *lines = NULL;
  size_t nlines = 0;
  char *result = NULL;

  e = section_block(identity, id_kept, &idb);
  if (e == ASPER_OK)
    e = section_block(ctxr, ctx_kept, &ctb);
  if (e == ASPER_OK)
    e = section_block(proj, proj_kept, &prb);
  if (e != ASPER_OK)
    goto cleanup;

  e = template_lines(tmpl, &lines, &nlines);
  if (e != ASPER_OK)
    goto cleanup;

  if (id_kept == 0 && ph_id) {
    size_t li = line_of(lines, nlines, ph_id);
    if (li < nlines)
      drop_segment(lines, li);
  }
  if (ctx_kept == 0 && ph_ctx) {
    size_t li = line_of(lines, nlines, ph_ctx);
    if (li < nlines)
      drop_segment(lines, li);
  }
  if (proj_kept == 0 && ph_proj) {
    size_t li = line_of(lines, nlines, ph_proj);
    if (li < nlines)
      drop_segment(lines, li);
  }

  for (size_t i = 0; i < nlines; i++) {
    if (lines[i].drop)
      continue;
    if ((e = asper_buf_append(&joined, lines[i].start, lines[i].len)) !=
        ASPER_OK)
      goto cleanup;
  }

  e = substitute(&outb, joined.len > 0 ? joined.data : "", base,
                 idb.len > 0 ? idb.data : "", ctb.len > 0 ? ctb.data : "",
                 prb.len > 0 ? prb.data : "", pname);
  if (e != ASPER_OK)
    goto cleanup;

  result = asper_buf_detach(&outb);
  if (!result)
    result = asper_strdup("");
  if (!result) {
    e = ASPER_ERR_NOMEM;
    goto cleanup;
  }
  *out_prompt = result;

cleanup:
  asper_buf_free(&idb);
  asper_buf_free(&ctb);
  asper_buf_free(&prb);
  asper_buf_free(&joined);
  asper_buf_free(&outb);
  free(lines);
  free(owned_tmpl);
  if (e != ASPER_OK)
    return asper_seterr(c, e, "inject: render failed");
  return ASPER_OK;
}
