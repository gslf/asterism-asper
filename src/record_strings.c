/* Invert the pinned xCDN serializer escapes without changing raw multiline text. */
#include "record_codec.h"
#include <stdlib.h>
#include <string.h>
static char *esc_encode(const char *s) {
  size_t n, extra = 0;
  char *out, *w;
  if (!s) s = "";
  if (strchr(s, '\\') == NULL) return asper_strdup(s);
  n = strlen(s);
  for (size_t i = 0; i < n; i++)
    if (s[i] == '\\' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')
      extra++;
  out = (char *)malloc(n + extra + 1);
  if (!out) return NULL;
  w = out;
  for (size_t i = 0; i < n; i++) {
    switch (s[i]) {
      case '\\': *w++ = '\\'; *w++ = '\\'; break;
      case '\n': *w++ = '\\'; *w++ = 'n'; break;
      case '\r': *w++ = '\\'; *w++ = 'r'; break;
      case '\t': *w++ = '\\'; *w++ = 't'; break;
      default:   *w++ = s[i]; break;
    }
  }
  *w = '\0';
  return out;
}

static int hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    unsigned d;
    if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
    else return 0;
    v = v * 16 + d;
  }
  *out = v;
  return 1;
}

static size_t utf8_put(char *w, uint32_t cp) {
  if (cp < 0x80) {
    w[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    w[0] = (char)(0xC0 | (cp >> 6));
    w[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    w[0] = (char)(0xE0 | (cp >> 12));
    w[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    w[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  w[0] = (char)(0xF0 | (cp >> 18));
  w[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  w[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  w[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static char *esc_decode(const char *s) {
  size_t n, i = 0;
  char *out, *w;
  if (!s) s = "";
  n = strlen(s);
  out = (char *)malloc(n + 1); /* decoding never grows the string */
  if (!out) return NULL;
  w = out;
  while (i < n) {
    char c = s[i];
    if (c != '\\' || i + 1 >= n) {
      *w++ = c;
      i++;
      continue;
    }
    switch (s[i + 1]) {
      case '\\': *w++ = '\\'; i += 2; break;
      case '"':  *w++ = '"';  i += 2; break;
      case '/':  *w++ = '/';  i += 2; break;
      case 'n':  *w++ = '\n'; i += 2; break;
      case 'r':  *w++ = '\r'; i += 2; break;
      case 't':  *w++ = '\t'; i += 2; break;
      case 'b':  *w++ = '\b'; i += 2; break;
      case 'f':  *w++ = '\f'; i += 2; break;
      case 'u': {
        unsigned cp;
        if (i + 6 <= n && hex4(s + i + 2, &cp)) {
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 12 <= n &&
              s[i + 6] == '\\' && s[i + 7] == 'u') {
            unsigned lo;
            if (hex4(s + i + 8, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              uint32_t full =
                  0x10000u + (((uint32_t)cp - 0xD800u) << 10) + (lo - 0xDC00u);
              w += utf8_put(w, full);
              i += 12;
              break;
            }
          }
          w += utf8_put(w, cp);
          i += 6;
        } else {
          *w++ = s[i]; /* malformed: keep raw */
          i++;
        }
        break;
      }
      default:
        *w++ = s[i]; /* unknown escape: keep raw */
        i++;
        break;
    }
  }
  *w = '\0';
  return out;
}

/* Decode escapes only when the string cannot have come from a RAW
 * (triple-quoted) xCDN literal: our own serializer always escapes control
 * characters, so a parsed string containing a raw byte < 0x20 (newline,
 * tab, ...) is hand-written triple-quoted content and must be preserved
 * verbatim (Post-review amendments). Known limitation: SINGLE-line
 * triple-quoted hand-edited content containing literal \x sequences is
 * still interpreted as escapes — the proper fix is an is_raw flag in
 * xCDN-C's AST (upstream). */
char *asper_record_unescape(const char *s) {
  if (s) {
    const unsigned char *p;
    for (p = (const unsigned char *)s; *p; p++)
      if (*p < 0x20) return asper_strdup(s);
  }
  return esc_decode(s);
}

/* String value with the encode shim applied (owned copy). */
xcdn_value_t *asper_record_string(const char *s) {
  char *enc = esc_encode(s);
  xcdn_value_t *v;
  if (!enc) return NULL;
  v = xcdn_value_string_owned(enc);
  if (!v) free(enc);
  return v;
}

