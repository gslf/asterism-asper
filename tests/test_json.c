/* test_json.c — mcp/json.c: strict RFC 8259 parsing, escapes, surrogate
 * pairs, depth cap, rejects, number discrimination, writer round-trips.
 * Compiled with ../mcp/json.c (see tests/CMakeLists.txt). */

#include "asper_test.h"

#include "json.h"

static jx_value *parse_ok(const char *text) {
  jx_value *v = NULL;
  if (jx_parse(text, strlen(text), &v) != 0) return NULL;
  return v;
}

static int parse_fails(const char *text) {
  jx_value *v = NULL;
  int rc = jx_parse(text, strlen(text), &v);
  if (rc == 0) {
    jx_free(v);
    return 0;
  }
  return v == NULL;
}

/* Structural equality via the public accessors. */
static int jx_equal(const jx_value *a, const jx_value *b) {
  size_t i, n;
  if (jx_typeof(a) != jx_typeof(b)) return 0;
  switch (jx_typeof(a)) {
    case JX_NULL:
      return 1;
    case JX_BOOL:
      return jx_bool_value(a) == jx_bool_value(b);
    case JX_NUMBER:
      if (jx_is_int(a) != jx_is_int(b)) return 0;
      if (jx_is_int(a)) return jx_int_value(a) == jx_int_value(b);
      return jx_double_value(a) == jx_double_value(b);
    case JX_STRING:
      if (jx_string_length(a) != jx_string_length(b)) return 0;
      return memcmp(jx_string_value(a), jx_string_value(b),
                    jx_string_length(a)) == 0;
    case JX_ARRAY:
      n = jx_array_len(a);
      if (n != jx_array_len(b)) return 0;
      for (i = 0; i < n; i++)
        if (!jx_equal(jx_array_at(a, i), jx_array_at(b, i))) return 0;
      return 1;
    case JX_OBJECT:
      n = jx_object_count(a);
      if (n != jx_object_count(b)) return 0;
      for (i = 0; i < n; i++) {
        if (strcmp(jx_object_key_at(a, i), jx_object_key_at(b, i)) != 0)
          return 0;
        if (!jx_equal(jx_object_value_at(a, i), jx_object_value_at(b, i)))
          return 0;
      }
      return 1;
  }
  return 0;
}

TEST(parse_scalars) {
  jx_value *v;
  v = parse_ok("null");
  ASSERT_EQ_INT(jx_typeof(v), JX_NULL);
  jx_free(v);
  v = parse_ok("true");
  ASSERT_EQ_INT(jx_typeof(v), JX_BOOL);
  ASSERT_EQ_INT(jx_bool_value(v), 1);
  jx_free(v);
  v = parse_ok("false");
  ASSERT_EQ_INT(jx_bool_value(v), 0);
  jx_free(v);
  v = parse_ok("  \t\n \"pad\"  "); /* RFC 8259 allows surrounding ws */
  ASSERT_EQ_INT(jx_typeof(v), JX_STRING);
  ASSERT_EQ_STR(jx_string_value(v), "pad");
  jx_free(v);
}

TEST(number_int_double_discrimination) {
  jx_value *v;
  v = parse_ok("42");
  ASSERT_EQ_INT(jx_typeof(v), JX_NUMBER);
  ASSERT_TRUE(jx_is_int(v));
  ASSERT_EQ_INT(jx_int_value(v), 42);
  ASSERT_EQ_DBL(jx_double_value(v), 42.0, 1e-12);
  jx_free(v);
  v = parse_ok("-7");
  ASSERT_TRUE(jx_is_int(v));
  ASSERT_EQ_INT(jx_int_value(v), -7);
  jx_free(v);
  v = parse_ok("0");
  ASSERT_TRUE(jx_is_int(v));
  ASSERT_EQ_INT(jx_int_value(v), 0);
  jx_free(v);
  v = parse_ok("9223372036854775807"); /* INT64_MAX */
  ASSERT_TRUE(jx_is_int(v));
  ASSERT_TRUE(jx_int_value(v) == 9223372036854775807LL);
  jx_free(v);
  v = parse_ok("3.14");
  ASSERT_TRUE(!jx_is_int(v));
  ASSERT_EQ_DBL(jx_double_value(v), 3.14, 1e-12);
  jx_free(v);
  v = parse_ok("-1.5e-3");
  ASSERT_TRUE(!jx_is_int(v));
  ASSERT_EQ_DBL(jx_double_value(v), -0.0015, 1e-15);
  jx_free(v);
  v = parse_ok("1e3");
  ASSERT_EQ_DBL(jx_double_value(v), 1000.0, 1e-9);
  jx_free(v);
  v = parse_ok("10000000000000000000"); /* > INT64_MAX: double only */
  ASSERT_EQ_INT(jx_typeof(v), JX_NUMBER);
  ASSERT_TRUE(!jx_is_int(v));
  ASSERT_EQ_DBL(jx_double_value(v), 1e19, 1e6);
  jx_free(v);
}

TEST(string_escapes_and_surrogates) {
  jx_value *v;
  v = parse_ok("\"a\\n\\t\\\"\\\\b\\/\"");
  ASSERT_EQ_STR(jx_string_value(v), "a\n\t\"\\b/");
  jx_free(v);
  v = parse_ok("\"\\u0041\\u00e9\"");
  ASSERT_EQ_STR(jx_string_value(v), "A\xc3\xa9"); /* é as UTF-8 */
  jx_free(v);
  /* surrogate pair: U+1F600 */
  v = parse_ok("\"\\ud83d\\ude00\"");
  ASSERT_EQ_STR(jx_string_value(v), "\xf0\x9f\x98\x80");
  ASSERT_EQ_INT(jx_string_length(v), 4);
  jx_free(v);
  /* raw multibyte UTF-8 passes through */
  v = parse_ok("\"caff\xc3\xa8\"");
  ASSERT_EQ_STR(jx_string_value(v), "caff\xc3\xa8");
  jx_free(v);
}

TEST(containers_and_lookup) {
  jx_value *v = parse_ok(
      "{\"name\":\"asper\",\"k\":6,\"weights\":[0.7,0.2,0.1],"
      "\"active\":true,\"project\":null}");
  const jx_value *w;
  ASSERT_TRUE(v != NULL);
  ASSERT_EQ_INT(jx_typeof(v), JX_OBJECT);
  ASSERT_EQ_INT(jx_object_count(v), 5);
  ASSERT_EQ_STR(jx_string_value(jx_object_get(v, "name")), "asper");
  ASSERT_TRUE(jx_is_int(jx_object_get(v, "k")));
  ASSERT_EQ_INT(jx_int_value(jx_object_get(v, "k")), 6);
  w = jx_object_get(v, "weights");
  ASSERT_EQ_INT(jx_typeof(w), JX_ARRAY);
  ASSERT_EQ_INT(jx_array_len(w), 3);
  ASSERT_EQ_DBL(jx_double_value(jx_array_at(w, 0)), 0.7, 1e-12);
  ASSERT_EQ_DBL(jx_double_value(jx_array_at(w, 2)), 0.1, 1e-12);
  ASSERT_EQ_INT(jx_bool_value(jx_object_get(v, "active")), 1);
  ASSERT_EQ_INT(jx_typeof(jx_object_get(v, "project")), JX_NULL);
  ASSERT_TRUE(jx_object_get(v, "absent") == NULL);
  /* insertion order is preserved */
  ASSERT_EQ_STR(jx_object_key_at(v, 0), "name");
  ASSERT_EQ_STR(jx_object_key_at(v, 4), "project");
  jx_free(v);
}

TEST(depth_cap) {
  char deep[300];
  size_t i, n;
  jx_value *v = NULL;
  /* 32 levels: fine */
  n = 32;
  for (i = 0; i < n; i++) deep[i] = '[';
  deep[n] = '1';
  for (i = 0; i < n; i++) deep[n + 1 + i] = ']';
  deep[2 * n + 1] = '\0';
  v = parse_ok(deep);
  ASSERT_TRUE(v != NULL);
  jx_free(v);
  /* 100 levels: beyond the documented cap of 64 */
  n = 100;
  for (i = 0; i < n; i++) deep[i] = '[';
  deep[n] = '1';
  for (i = 0; i < n; i++) deep[n + 1 + i] = ']';
  deep[2 * n + 1] = '\0';
  ASSERT_TRUE(parse_fails(deep));
}

TEST(rejects) {
  static const char *bad[] = {
      "",
      "   ",
      "tru",
      "nulll",
      "01",
      "+1",
      "1.",
      ".5",
      "-",
      "NaN",
      "Infinity",
      "-Infinity",
      "1e",
      "{\"a\":1,}",     /* trailing comma          */
      "[1,]",
      "[1 2]",
      "{\"a\" 1}",
      "{\"a\":}",
      "{a:1}",          /* unquoted key            */
      "\"abc",          /* unterminated            */
      "\"\\x41\"",      /* unknown escape          */
      "\"\\u12\"",      /* short unicode escape    */
      "\"\\ud800\"",    /* lone high surrogate     */
      "\"\\ude00\"",    /* lone low surrogate      */
      "'single'",
      "1 2",            /* trailing garbage        */
      "null null",
      "{} []",
      "[1] extra",
      "// comment\n1",
      "{\"a\":1} trailing",
  };
  size_t i;
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    if (!parse_fails(bad[i])) {
      ASPER_FAILF("accepted invalid JSON: %s", bad[i]);
      return;
    }
  }
  /* raw control character inside a string */
  {
    const char ctrl[] = {'"', 'a', 0x01, 'b', '"', '\0'};
    ASSERT_TRUE(parse_fails(ctrl));
  }
  {
    const char tab[] = {'"', 'a', '\t', 'b', '"', '\0'};
    ASSERT_TRUE(parse_fails(tab));
  }
  /* invalid UTF-8 byte sequences */
  {
    const char bad_utf8[] = {'"', (char)0xff, '"', '\0'};
    ASSERT_TRUE(parse_fails(bad_utf8));
  }
  {
    const char trunc_utf8[] = {'"', (char)0xc3, '"', '\0'};
    ASSERT_TRUE(parse_fails(trunc_utf8));
  }
  {
    /* overlong encoding of '/' */
    const char overlong[] = {'"', (char)0xc0, (char)0xaf, '"', '\0'};
    ASSERT_TRUE(parse_fails(overlong));
  }
}

TEST(writer_compact_exact) {
  jx_value *v = parse_ok("{ \"a\" : [ 1 , 2.5 , \"x\\ny\" , true , null ] }");
  char *out;
  ASSERT_TRUE(v != NULL);
  out = jx_write(v, 0);
  ASSERT_EQ_STR(out, "{\"a\":[1,2.5,\"x\\ny\",true,null]}");
  ASSERT_TRUE(strchr(out, '\n') == NULL); /* line-delimited transport safe */
  free(out);
  jx_free(v);
  /* integers stay integral through the writer */
  v = jx_int(42);
  ASSERT_TRUE(v != NULL);
  out = jx_write(v, 0);
  ASSERT_EQ_STR(out, "42");
  free(out);
  jx_free(v);
}

TEST(writer_roundtrip_structural) {
  static const char *texts[] = {
      "null",
      "true",
      "[-1,0,9223372036854775807,1.25]",
      "{\"s\":\"caff\xc3\xa8 \\u0041\",\"nested\":{\"a\":[{},[]]}}",
      "\"\\ud83d\\ude00 emoji\"",
      "{\"esc\":\"quote \\\" backslash \\\\ nl \\n tab \\t\"}",
  };
  size_t i;
  for (i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
    jx_value *a = parse_ok(texts[i]);
    jx_value *b = NULL;
    char *out;
    ASSERT_TRUE(a != NULL);
    out = jx_write(a, 0);
    ASSERT_TRUE(out != NULL);
    ASSERT_TRUE(jx_parse(out, strlen(out), &b) == 0);
    if (!jx_equal(a, b)) {
      ASPER_FAILF("round trip changed %s -> %s", texts[i], out);
      free(out);
      jx_free(a);
      jx_free(b);
      return;
    }
    free(out);
    jx_free(a);
    jx_free(b);
  }
}

TEST(builders_and_clone) {
  jx_value *obj = jx_object();
  jx_value *arr = jx_array();
  jx_value *copy;
  int st = 0;
  char *out;
  ASSERT_TRUE(obj && arr);
  st |= jx_array_push(arr, jx_int(1));
  st |= jx_array_push(arr, jx_string("two"));
  st |= jx_object_set(obj, "list", arr);
  st |= jx_object_set(obj, "ok", jx_bool(1));
  ASSERT_EQ_INT(st, 0);
  copy = jx_clone(obj);
  ASSERT_TRUE(copy != NULL);
  ASSERT_TRUE(jx_equal(obj, copy));
  out = jx_write(copy, 0);
  ASSERT_EQ_STR(out, "{\"list\":[1,\"two\"],\"ok\":true}");
  free(out);
  jx_free(copy);
  jx_free(obj);
}

TEST(double_rejects_non_finite) {
  /* Built at runtime: folding `1e308 * 10.0` at compile time makes MSVC
   * raise C4056 (overflow in floating-point constant arithmetic). */
  double huge = 1e308;
  jx_value *v = jx_double(huge * 10.0); /* inf */
  ASSERT_TRUE(v == NULL);
  v = jx_string("bad \xff utf8");
  ASSERT_TRUE(v == NULL);
}

TEST_LIST = {
    TEST_ENTRY(parse_scalars),
    TEST_ENTRY(number_int_double_discrimination),
    TEST_ENTRY(string_escapes_and_surrogates),
    TEST_ENTRY(containers_and_lookup),
    TEST_ENTRY(depth_cap),
    TEST_ENTRY(rejects),
    TEST_ENTRY(writer_compact_exact),
    TEST_ENTRY(writer_roundtrip_structural),
    TEST_ENTRY(builders_and_clone),
    TEST_ENTRY(double_rejects_non_finite),
};

RUN_ALL_TESTS()
