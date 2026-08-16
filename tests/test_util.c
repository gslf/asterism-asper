/* test_util.c — util.c: buffers, strings, fnv, base64, slug, tokens. */

#include "asper_test.h"

#include "asper_internal.h"

TEST(buf_basic) {
  asper_buf b;
  char *s;
  asper_buf_init(&b);
  ASSERT_OK(asper_buf_append(&b, "ab", 2));
  ASSERT_OK(asper_buf_appendc(&b, 'c'));
  ASSERT_OK(asper_buf_appends(&b, "def"));
  ASSERT_OK(asper_buf_printf(&b, "-%d-%s", 42, "x"));
  ASSERT_EQ_INT(b.len, 11);
  ASSERT_TRUE(b.data != NULL);
  ASSERT_EQ_INT(b.data[b.len], 0); /* NUL-terminated */
  ASSERT_EQ_STR(b.data, "abcdef-42-x");
  s = asper_buf_detach(&b);
  ASSERT_EQ_STR(s, "abcdef-42-x");
  free(s);
  /* detach resets the buffer: it must be reusable */
  ASSERT_EQ_INT(b.len, 0);
  ASSERT_OK(asper_buf_appends(&b, "z"));
  ASSERT_EQ_STR(b.data, "z");
  asper_buf_free(&b);
}

TEST(buf_grow) {
  asper_buf b;
  int i;
  asper_buf_init(&b);
  for (i = 0; i < 500; i++) ASSERT_OK(asper_buf_appends(&b, "0123456789"));
  ASSERT_EQ_INT(b.len, 5000);
  ASSERT_EQ_INT(b.data[b.len], 0);
  ASSERT_TRUE(strncmp(b.data, "0123456789", 10) == 0);
  ASSERT_TRUE(strcmp(b.data + 4990, "0123456789") == 0);
  /* printf across the growth path */
  ASSERT_OK(asper_buf_printf(&b, "%0512d", 7));
  ASSERT_EQ_INT(b.len, 5512);
  ASSERT_EQ_INT(b.data[5511], '7');
  asper_buf_free(&b);
}

TEST(strdup_strndup) {
  char *s;
  ASSERT_TRUE(asper_strdup(NULL) == NULL);
  s = asper_strdup("hello");
  ASSERT_EQ_STR(s, "hello");
  free(s);
  s = asper_strndup("hello", 3);
  ASSERT_EQ_STR(s, "hel");
  free(s);
  s = asper_strndup("hi", 10);
  ASSERT_EQ_STR(s, "hi");
  free(s);
}

TEST(trim_blank) {
  char a[] = "  a b  ";
  char b[] = "\t\n x \r ";
  char c[] = "   ";
  char d[] = "xyz";
  ASSERT_EQ_STR(asper_str_trim(a), "a b");
  ASSERT_EQ_STR(asper_str_trim(b), "x");
  ASSERT_EQ_STR(asper_str_trim(c), "");
  ASSERT_EQ_STR(asper_str_trim(d), "xyz");
  ASSERT_TRUE(asper_str_blank(NULL));
  ASSERT_TRUE(asper_str_blank(""));
  ASSERT_TRUE(asper_str_blank(" \t\r\n"));
  ASSERT_TRUE(!asper_str_blank("a"));
  ASSERT_TRUE(!asper_str_blank("  a  "));
}

TEST(fnv1a64_vectors) {
  /* Known FNV-1a 64 vectors (draft-eastlake-fnv). */
  ASSERT_TRUE(asper_fnv1a64("", 0) == 0xcbf29ce484222325ULL);
  ASSERT_TRUE(asper_fnv1a64("a", 1) == 0xaf63dc4c8601ec8cULL);
  ASSERT_TRUE(asper_fnv1a64("foobar", 6) == 0x85944171f73967e8ULL);
}

TEST(base64_encode_vectors) {
  /* RFC 4648 §10 test vectors. */
  static const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba",
                                "foobar"};
  static const char *coded[] = {"",     "Zg==", "Zm8=",     "Zm9v",
                                "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
  size_t i;
  for (i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
    char *enc = asper_base64_encode((const uint8_t *)plain[i],
                                    strlen(plain[i]));
    ASSERT_EQ_STR(enc, coded[i]);
    free(enc);
  }
}

TEST(base64_roundtrip_binary) {
  uint8_t raw[256];
  uint8_t *dec = NULL;
  size_t dec_len = 0, i;
  char *enc;
  for (i = 0; i < sizeof raw; i++) raw[i] = (uint8_t)i;
  enc = asper_base64_encode(raw, sizeof raw);
  ASSERT_TRUE(enc != NULL);
  ASSERT_TRUE(asper_base64_decode(enc, &dec, &dec_len));
  ASSERT_EQ_INT(dec_len, sizeof raw);
  ASSERT_TRUE(memcmp(dec, raw, sizeof raw) == 0);
  free(enc);
  free(dec);
}

TEST(base64_decode_vectors_and_rejects) {
  static const char *bad[] = {
      "Zg",       /* missing padding            */
      "Zg=",      /* short padding              */
      "A",        /* length % 4 == 1            */
      "AAAAA",    /* length % 4 == 1            */
      "Zm9v!",    /* invalid character          */
      "====",     /* padding only               */
      "Zg==Zg==", /* data after padding         */
      "Zm9v\n",   /* RFC 4648 without line wrap */
  };
  uint8_t *dec = NULL;
  size_t dec_len = 0, i;
  ASSERT_TRUE(asper_base64_decode("Zm9vYmFy", &dec, &dec_len));
  ASSERT_EQ_INT(dec_len, 6);
  ASSERT_TRUE(memcmp(dec, "foobar", 6) == 0);
  free(dec);
  dec = NULL;
  ASSERT_TRUE(asper_base64_decode("", &dec, &dec_len));
  ASSERT_EQ_INT(dec_len, 0);
  free(dec);
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    if (asper_base64_decode(bad[i], &out, &out_len)) {
      ASPER_FAILF("base64 decode accepted %s", bad[i]);
      free(out);
      return;
    }
  }
}

TEST(slug_rules) {
  char long_ok[66];
  char long_bad[67];
  memset(long_ok, 'a', 64);
  long_ok[64] = '\0';
  memset(long_bad, 'a', 65);
  long_bad[65] = '\0';
  ASSERT_TRUE(asper_slug_valid("a"));
  ASSERT_TRUE(asper_slug_valid("thesis"));
  ASSERT_TRUE(asper_slug_valid("website-redesign"));
  ASSERT_TRUE(asper_slug_valid("a1-2b"));
  ASSERT_TRUE(asper_slug_valid("0abc"));
  ASSERT_TRUE(asper_slug_valid("a-")); /* trailing hyphen is in the grammar */
  ASSERT_TRUE(asper_slug_valid(long_ok));  /* 64 chars: 1 + 63 */
  ASSERT_TRUE(!asper_slug_valid(long_bad)); /* 65 chars */
  ASSERT_TRUE(!asper_slug_valid(NULL));
  ASSERT_TRUE(!asper_slug_valid(""));
  ASSERT_TRUE(!asper_slug_valid("-a"));
  ASSERT_TRUE(!asper_slug_valid("A"));
  ASSERT_TRUE(!asper_slug_valid("Thesis"));
  ASSERT_TRUE(!asper_slug_valid("a_b"));
  ASSERT_TRUE(!asper_slug_valid("a b"));
  ASSERT_TRUE(!asper_slug_valid("a.b"));
}

TEST(token_heuristic) {
  /* UTF-8 bytes / 4, minimum 1 for non-empty text. */
  ASSERT_EQ_INT(asper_token_heuristic(""), 0);
  ASSERT_EQ_INT(asper_token_heuristic("ab"), 1);
  ASSERT_EQ_INT(asper_token_heuristic("abcd"), 1);
  ASSERT_EQ_INT(asper_token_heuristic("abcdefgh"), 2);
  ASSERT_EQ_INT(asper_token_heuristic("0123456789012345678901234567890123456789012"),
                10); /* 43 bytes */
  ASSERT_EQ_INT(asper_token_heuristic("\xc3\xa8\xc3\xa8\xc3\xa8\xc3\xa8"),
                2); /* 4 two-byte code points = 8 bytes */
}

TEST(utf8_validation_and_character_count) {
  size_t n = 99;
  ASSERT_TRUE(asper_utf8_count("hello", &n));
  ASSERT_EQ_INT(n, 5);
  ASSERT_TRUE(asper_utf8_count("\xc3\xa8\xe2\x82\xac\xf0\x9f\x98\x80", &n));
  ASSERT_EQ_INT(n, 3);
  ASSERT_TRUE(!asper_utf8_count("\xc0\x80", &n));       /* overlong NUL */
  ASSERT_TRUE(!asper_utf8_count("\xed\xa0\x80", &n)); /* surrogate */
  ASSERT_TRUE(!asper_utf8_count("\xf4\x90\x80\x80", &n));
  ASSERT_TRUE(!asper_utf8_count("\xe2\x82", &n));      /* truncated */
}

TEST(section_source_names) {
  asper_section s;
  asper_source src;
  ASSERT_EQ_STR(asper_section_name(ASPER_SECTION_IDENTITY), "identity");
  ASSERT_EQ_STR(asper_section_name(ASPER_SECTION_CONTEXT), "context");
  ASSERT_EQ_STR(asper_section_name(ASPER_SECTION_PROJECT), "project");
  ASSERT_TRUE(asper_section_parse("identity", &s));
  ASSERT_EQ_INT(s, ASPER_SECTION_IDENTITY);
  ASSERT_TRUE(asper_section_parse("project", &s));
  ASSERT_EQ_INT(s, ASPER_SECTION_PROJECT);
  ASSERT_TRUE(!asper_section_parse("sideways", &s));
  ASSERT_EQ_STR(asper_source_name(ASPER_SRC_SEED), "seed");
  ASSERT_EQ_STR(asper_source_name(ASPER_SRC_MANUAL), "manual");
  ASSERT_EQ_STR(asper_source_name(ASPER_SRC_CURATOR), "curator");
  ASSERT_TRUE(asper_source_parse("curator", &src));
  ASSERT_EQ_INT(src, ASPER_SRC_CURATOR);
  ASSERT_TRUE(!asper_source_parse("oracle", &src));
}

TEST_LIST = {
    TEST_ENTRY(buf_basic),
    TEST_ENTRY(buf_grow),
    TEST_ENTRY(strdup_strndup),
    TEST_ENTRY(trim_blank),
    TEST_ENTRY(fnv1a64_vectors),
    TEST_ENTRY(base64_encode_vectors),
    TEST_ENTRY(base64_roundtrip_binary),
    TEST_ENTRY(base64_decode_vectors_and_rejects),
    TEST_ENTRY(slug_rules),
    TEST_ENTRY(token_heuristic),
    TEST_ENTRY(utf8_validation_and_character_count),
    TEST_ENTRY(section_source_names),
};

RUN_ALL_TESTS()
