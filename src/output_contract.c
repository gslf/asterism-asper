/* Curation owns both wire representations and the interpretation of output. */
#include "asper_internal.h"
#include "xcdn.h"
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

static int put(asper_buf *b, const char *s) { return asper_buf_appends(b,s) != ASPER_OK; }
static int handles(asper_buf *b, size_t n) {
  if (put(b,"(")) return -1;
  for (size_t i = 1; i <= n; i++)
    if (asper_buf_printf(b,"%sM%zu",i > 1 ? "|" : "",i) != ASPER_OK) return -1;
  return put(b,")");
}
static int pattern(asper_buf *b, const asper_output_contract *c) {
  if (c->kind == ASPER_GRAMMAR_CURATION) {
    if (put(b,"^(NOOP\\n|((INSERT (identity|context|project) \\| [^|\\n\\r]+")) return -1;
    if (c->handles && (put(b,"|(UPDATE|DEPRECATE) ") || handles(b,c->handles) ||
                       put(b," \\| [^|\\n\\r]+"))) return -1;
    return put(b,")\\n)+)$");
  }
  if (c->kind == ASPER_GRAMMAR_REVIEW) {
    if (!c->handles) return put(b,"^NOOP\\n$");
    return put(b,"^(NOOP\\n|((DEPRECATE ") || handles(b,c->handles) ||
        put(b," \\| [^|\\n\\r]+|KEEP ") || handles(b,c->handles) || put(b,")\\n)+)$");
  }
  if (c->kind == ASPER_GRAMMAR_RECALL) {
    if (put(b,"^(NOMEM\\n|ANSWER \\| [^|\\n\\r]+\\n")) return -1;
    if (c->handles && (put(b,"(CITE ") || handles(b,c->handles) || put(b,"\\n)*"))) return -1;
    return put(b,")$");
  }
  return -1;
}
char *asper_output_schema(const asper_output_contract *c) {
  asper_buf b, regex;
  char *out = NULL;
  if (!c || c->handles > 10000) return NULL;
  asper_buf_init(&b); asper_buf_init(&regex);
  if (pattern(&regex,c) || put(&b,
      "{\"type\":\"object\",\"properties\":{\"output\":{\"type\":\"string\","
      "\"minLength\":5,\"maxLength\":32768,\"pattern\":\"")) goto done;
  /* This pattern contains only controlled ASCII, backslashes and handle integers. */
  for (size_t i = 0; i < regex.len; i++) {
    if ((regex.data[i] == '\\' || regex.data[i] == '"') && put(&b,"\\")) goto done;
    if (asper_buf_appendc(&b,regex.data[i]) != ASPER_OK) goto done;
  }
  if (put(&b,"\"}},\"required\":[\"output\"],\"additionalProperties\":false}")) goto done;
  out = asper_buf_detach(&b);
done:
  asper_buf_free(&b); asper_buf_free(&regex);
  return out;
}
/* The wrapper is one JSON property. Reject duplicate keys, comments, binary
 * strings and trailing data before using the existing xCDN string decoder. */
static int wrapper_valid(const char *p) {
  while (isspace((unsigned char)*p)) p++;
  if (*p != '{') return 0;
  p++;
  while (isspace((unsigned char)*p)) p++;
  if (strncmp(p, "\"output\"", 8)) return 0;
  p += 8;
  while (isspace((unsigned char)*p)) p++;
  if (*p != ':') return 0;
  p++;
  while (isspace((unsigned char)*p)) p++;
  if (*p != '"') return 0;
  p++;
  while (*p && *p != '"') {
    if ((unsigned char)*p < 32) return 0;
    if (*p++ != '\\') continue;
    if (!*p || !strchr("\"\\/bfnrtu", *p)) return 0;
    if (*p++ == 'u') {
      if (strlen(p) < 4 || !memcmp(p,"0000",4)) return 0;
      for (int i = 0; i < 4; i++) if (!isxdigit((unsigned char)*p++)) return 0;
    }
  }
  if (*p != '"') return 0;
  p++;
  while (isspace((unsigned char)*p)) p++;
  if (*p != '}') return 0;
  p++;
  while (isspace((unsigned char)*p)) p++;
  return !*p;
}

asper_err asper_output_decode(char **text) {
  xcdn_error_t error;
  xcdn_document_t *doc;
  char *decoded = NULL;
  if (!text || !*text) return ASPER_ERR_INVALID;
  if (!wrapper_valid(*text)) return ASPER_ERR_PARSE;
  doc = xcdn_parse(*text, &error);
  if (!doc) return ASPER_ERR_PARSE;
  if (doc->values_len == 1) {
    const xcdn_value_t *v = doc->values[0]->value;
    if (v && v->type == XCDN_VAL_OBJECT && v->data.object.len == 1) {
      const xcdn_object_entry_t *entry = &v->data.object.entries[0];
      const xcdn_value_t *value = entry->node ? entry->node->value : NULL;
      if (!strcmp(entry->key,"output") && value && value->type == XCDN_VAL_STRING &&
          value->data.string && strlen(value->data.string) >= 5 &&
          strlen(value->data.string) <= 32768 && asper_utf8_count(value->data.string,NULL))
        decoded = asper_strdup(value->data.string);
    }
  }
  xcdn_document_free(doc);
  if (!decoded) return ASPER_ERR_PARSE;
  free(*text); *text = decoded;
  return ASPER_OK;
}
