/*
 * time.c — injectable clock, RFC 3339 parse/format, ISO 8601 durations.
 *
 * Calendar conversion uses the days-from-civil algorithm (proleptic
 * Gregorian), valid over the whole int64 range used here; no timegm, no
 * struct tm, no locale.
 */

#include "asper_internal.h"

/* ---- clock -------------------------------------------------------------- */

static asper_time clock_system_now(void *ud) {
  (void)ud;
  return (asper_time)os_now_unix();
}

asper_time asper_clock_now(const asper_clock *clk) {
  if (!clk || !clk->now) return (asper_time)os_now_unix();
  return clk->now(clk->ud);
}

asper_clock asper_clock_system(void) {
  asper_clock c;
  c.ud = NULL;
  c.now = clock_system_now;
  return c;
}

/* ---- civil calendar ------------------------------------------------------ */

/* Days since 1970-01-01 for a proleptic-Gregorian civil date. */
static int64_t days_from_civil(int64_t y, int m, int d) {
  int64_t era;
  unsigned yoe, doy, doe;
  y -= (m <= 2);
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);                            /* [0, 399] */
  doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                /* [0,146096] */
  return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *py, int *pm, int *pd) {
  int64_t era, y;
  unsigned doe, yoe, doy, mp, d, m;
  z += 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = (unsigned)(z - era * 146097);                         /* [0,146096] */
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int64_t)yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              /* [0, 365] */
  mp = (5 * doy + 2) / 153;                                   /* [0, 11]  */
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  *py = y + (m <= 2);
  *pm = (int)m;
  *pd = (int)d;
}

static bool is_leap(int y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int y, int m) {
  static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && is_leap(y)) return 29;
  return dm[m - 1];
}

/* ---- RFC 3339 ------------------------------------------------------------ */

/* Exactly n ASCII digits at s into *out; stops (false) at any non-digit,
 * including NUL, so it never reads past the end of the string. */
static bool parse_digits(const char *s, int n, int *out) {
  int v = 0, i;
  for (i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  *out = v;
  return true;
}

bool asper_time_parse_rfc3339(const char *s, asper_time *out) {
  int y, mo, d, h, mi, se, offh, offm;
  int64_t off = 0;
  const char *p;
  char sign;
  if (!s || !out) return false;
  p = s;
  if (!parse_digits(p, 4, &y)) return false;
  if (p[4] != '-') return false;
  if (!parse_digits(p + 5, 2, &mo)) return false;
  if (p[7] != '-') return false;
  if (!parse_digits(p + 8, 2, &d)) return false;
  if (p[10] != 'T' && p[10] != 't') return false;
  if (!parse_digits(p + 11, 2, &h)) return false;
  if (p[13] != ':') return false;
  if (!parse_digits(p + 14, 2, &mi)) return false;
  if (p[16] != ':') return false;
  if (!parse_digits(p + 17, 2, &se)) return false;
  p += 19;
  if (*p == '.') { /* fractional seconds: validated, then truncated */
    p++;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
  }
  sign = *p;
  if (sign == 'Z' || sign == 'z') {
    p++;
  } else if (sign == '+' || sign == '-') {
    p++;
    if (!parse_digits(p, 2, &offh)) return false;
    if (p[2] != ':') return false;
    if (!parse_digits(p + 3, 2, &offm)) return false;
    p += 5;
    if (offh > 23 || offm > 59) return false;
    off = (int64_t)offh * 3600 + (int64_t)offm * 60;
    if (sign == '+') off = -off;
  } else {
    return false;
  }
  if (*p != '\0') return false;
  if (mo < 1 || mo > 12) return false;
  if (d < 1 || d > days_in_month(y, mo)) return false;
  if (h > 23 || mi > 59 || se > 60) return false; /* 60 = leap second */
  *out = days_from_civil(y, mo, d) * INT64_C(86400) + (int64_t)h * 3600 +
         (int64_t)mi * 60 + se + off;
  return true;
}

void asper_time_format_rfc3339(asper_time t, char out[32]) {
  int64_t days, sod, y;
  int m, d, h, mi, se;
  if (!out) return;
  days = t / 86400;
  sod = t % 86400;
  if (sod < 0) {
    sod += 86400;
    days -= 1;
  }
  civil_from_days(days, &y, &m, &d);
  h = (int)(sod / 3600);
  mi = (int)((sod / 60) % 60);
  se = (int)(sod % 60);
  snprintf(out, 32, "%04lld-%02d-%02dT%02d:%02d:%02dZ", (long long)y, m, d, h,
           mi, se);
}

/* ---- ISO 8601 durations -------------------------------------------------- */

/* Non-negative integer, 1..12 digits (bounds every legal total well inside
 * int64 seconds). Rejects fractional values. */
static bool dur_number(const char **pp, int64_t *out) {
  const char *p = *pp;
  int64_t v = 0;
  int nd = 0;
  while (*p >= '0' && *p <= '9') {
    if (nd >= 12) return false;
    v = v * 10 + (*p - '0');
    p++;
    nd++;
  }
  if (nd == 0) return false;
  if (*p == '.' || *p == ',') return false;
  *pp = p;
  *out = v;
  return true;
}

bool asper_duration_parse(const char *s, int64_t *out_seconds) {
  const char *p;
  int64_t acc = 0, v;
  if (!s || !out_seconds) return false;
  p = s;
  if (*p != 'P') return false;
  p++;
  if (*p == '\0') return false;

  if (*p != 'T') {
    if (!dur_number(&p, &v)) return false;
    if (*p == 'W') { /* PnW is exclusive: no other components */
      p++;
      if (*p != '\0') return false;
      *out_seconds = v * INT64_C(604800);
      return true;
    }
    if (*p != 'D') return false; /* rejects Y and M date designators */
    p++;
    acc = v * INT64_C(86400);
    if (*p == '\0') {
      *out_seconds = acc;
      return true;
    }
  }

  if (*p != 'T') return false;
  p++;
  if (*p == '\0') return false; /* "T" requires at least one time part */
  {
    int stage = 0; /* enforces H before M before S, each at most once */
    while (*p != '\0') {
      char u;
      if (!dur_number(&p, &v)) return false;
      u = *p;
      p++;
      if (u == 'H' && stage < 1) {
        acc += v * 3600;
        stage = 1;
      } else if (u == 'M' && stage < 2) {
        acc += v * 60;
        stage = 2;
      } else if (u == 'S' && stage < 3) {
        acc += v;
        stage = 3;
      } else {
        return false;
      }
    }
  }
  *out_seconds = acc;
  return true;
}
