/* test_time.c — time.c: RFC 3339, ISO 8601 durations, injectable clock. */

#include "asper_test.h"

#include "asper_internal.h"

TEST(rfc3339_parse_known) {
  static const struct {
    const char *s;
    long long epoch;
  } cases[] = {
      {"1970-01-01T00:00:00Z", 0},
      {"2001-09-09T01:46:40Z", 1000000000LL},
      {"2000-02-29T12:00:00Z", 951825600LL},  /* leap day        */
      {"2024-02-29T00:00:00Z", 1709164800LL}, /* leap day        */
      {"2026-07-29T10:12:00Z", 1785319920LL},
      {"2026-12-31T23:59:59Z", 1798761599LL},
      {"2026-07-29T12:12:00+02:00", 1785319920LL}, /* offset east */
      {"2026-07-29T05:12:00-05:00", 1785319920LL}, /* offset west */
      {"2026-07-29T10:12:00.123Z", 1785319920LL},  /* fraction    */
      {"2026-07-29T10:12:00.123+02:00", 1785312720LL},
  };
  size_t i;
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    asper_time t = -1;
    if (!asper_time_parse_rfc3339(cases[i].s, &t)) {
      ASPER_FAILF("rejected valid %s", cases[i].s);
      return;
    }
    if ((long long)t != cases[i].epoch) {
      ASPER_FAILF("%s -> %lld, want %lld", cases[i].s, (long long)t,
                  cases[i].epoch);
      return;
    }
  }
}

TEST(rfc3339_rejects) {
  static const char *bad[] = {
      "",
      "garbage",
      "2026-07-29",              /* date only            */
      "10:12:00Z",               /* time only            */
      "2026-07-29 10:12:00Z",    /* space separator      */
      "2026-07-29T10:12:00",     /* missing offset       */
      "2026-13-01T00:00:00Z",    /* month 13             */
      "2026-00-01T00:00:00Z",    /* month 0              */
      "2026-02-30T00:00:00Z",    /* Feb 30               */
      "2025-02-29T00:00:00Z",    /* not a leap year      */
      "2026-07-32T00:00:00Z",    /* day 32               */
      "2026-07-00T00:00:00Z",    /* day 0                */
      "2026-07-29T24:00:00Z",    /* hour 24              */
      "2026-07-29T10:60:00Z",    /* minute 60            */
      "2026-07-29T10:12:61Z",    /* second 61            */
      "2026-07-29T10:12:00X",    /* bad zone letter      */
      "2026-07-29T10:12:00+2:00",/* short offset hours   */
      "26-07-29T10:12:00Z",      /* two-digit year       */
  };
  size_t i;
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    asper_time t;
    if (asper_time_parse_rfc3339(bad[i], &t)) {
      ASPER_FAILF("accepted invalid \"%s\"", bad[i]);
      return;
    }
  }
}

TEST(rfc3339_format) {
  char out[32];
  asper_time_format_rfc3339(0, out);
  ASSERT_EQ_STR(out, "1970-01-01T00:00:00Z");
  ASSERT_EQ_INT(strlen(out), 20);
  asper_time_format_rfc3339(1785319920LL, out);
  ASSERT_EQ_STR(out, "2026-07-29T10:12:00Z");
  asper_time_format_rfc3339(1000000000LL, out);
  ASSERT_EQ_STR(out, "2001-09-09T01:46:40Z");
  asper_time_format_rfc3339(1709164800LL, out);
  ASSERT_EQ_STR(out, "2024-02-29T00:00:00Z");
}

TEST(rfc3339_roundtrip_stability) {
  static const long long epochs[] = {0,          1,           951825600LL,
                                     1000000000LL, 1709164800LL, 1785319920LL,
                                     1798761599LL};
  size_t i;
  for (i = 0; i < sizeof(epochs) / sizeof(epochs[0]); i++) {
    char a[32], b[32];
    asper_time t = 0;
    asper_time_format_rfc3339((asper_time)epochs[i], a);
    ASSERT_TRUE(asper_time_parse_rfc3339(a, &t));
    ASSERT_EQ_INT(t, epochs[i]);
    asper_time_format_rfc3339(t, b);
    ASSERT_EQ_STR(a, b); /* format is stable */
  }
  /* Offsets canonicalize to Z on re-format. */
  {
    asper_time t = 0;
    char out[32];
    ASSERT_TRUE(asper_time_parse_rfc3339("2026-07-29T12:12:00+02:00", &t));
    asper_time_format_rfc3339(t, out);
    ASSERT_EQ_STR(out, "2026-07-29T10:12:00Z");
  }
}

TEST(duration_parse_known) {
  static const struct {
    const char *s;
    long long secs;
  } cases[] = {
      {"PT30S", 30},
      {"PT1H", 3600},
      {"PT2M", 120},
      {"PT10S", 10},
      {"P1D", 86400},
      {"P7D", 604800},
      {"P2W", 1209600},
      {"P1DT2H3M4S", 93784},
      {"P30D", 2592000},
      {"P90D", 7776000},
      {"PT0S", 0},
  };
  size_t i;
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int64_t secs = -1;
    if (!asper_duration_parse(cases[i].s, &secs)) {
      ASPER_FAILF("rejected valid %s", cases[i].s);
      return;
    }
    if ((long long)secs != cases[i].secs) {
      ASPER_FAILF("%s -> %lld, want %lld", cases[i].s, (long long)secs,
                  cases[i].secs);
      return;
    }
  }
}

TEST(duration_rejects) {
  static const char *bad[] = {
      "",     "P",    "PT",    "P1M",  /* months are ambiguous */
      "P1Y",                          /* years are ambiguous  */
      "1D",   "30S",  "PT5",          /* number without unit  */
      "PD",                           /* unit without number  */
      "P1D2H",                        /* time part without T  */
      "junk",
  };
  size_t i;
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    int64_t secs;
    if (asper_duration_parse(bad[i], &secs)) {
      ASPER_FAILF("accepted invalid \"%s\"", bad[i]);
      return;
    }
  }
}

static asper_time test_fixed_now(void *ud) { return *(const int64_t *)ud; }

TEST(clock_injectable) {
  int64_t v = 1785319920LL;
  asper_clock ck;
  ck.ud = &v;
  ck.now = test_fixed_now;
  ASSERT_EQ_INT(asper_clock_now(&ck), 1785319920LL);
  v = 42;
  ASSERT_EQ_INT(asper_clock_now(&ck), 42);
}

TEST(clock_system_sane) {
  asper_clock ck = asper_clock_system();
  asper_time now = asper_clock_now(&ck);
  /* after 2023-11-14 and before 2100 */
  ASSERT_TRUE(now > 1700000000LL);
  ASSERT_TRUE(now < 4102444800LL);
}

TEST_LIST = {
    TEST_ENTRY(rfc3339_parse_known),
    TEST_ENTRY(rfc3339_rejects),
    TEST_ENTRY(rfc3339_format),
    TEST_ENTRY(rfc3339_roundtrip_stability),
    TEST_ENTRY(duration_parse_known),
    TEST_ENTRY(duration_rejects),
    TEST_ENTRY(clock_injectable),
    TEST_ENTRY(clock_system_sane),
};

RUN_ALL_TESTS()
