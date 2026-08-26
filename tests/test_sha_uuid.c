/* test_sha_uuid.c — sha256.c (NIST vectors) + uuid.c (v4 shape). */

#include "asper_test.h"

#include "asper_internal.h"

static void hex_to_bytes32(const char *hex, uint8_t out[32]) {
  size_t i;
  for (i = 0; i < 32; i++) {
    unsigned v = 0;
    sscanf(hex + 2 * i, "%2x", &v);
    out[i] = (uint8_t)v;
  }
}

static int digest_is(const uint8_t got[32], const char *hex) {
  uint8_t want[32];
  hex_to_bytes32(hex, want);
  return memcmp(got, want, 32) == 0;
}

/* NIST FIPS 180-2 test vectors. */
#define SHA_ABC \
  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define SHA_EMPTY \
  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define SHA_448 \
  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
#define SHA_MILLION_A \
  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"

TEST(sha256_nist_vectors) {
  uint8_t d[32];
  asper_sha256("abc", 3, d);
  ASSERT_TRUE(digest_is(d, SHA_ABC));
  asper_sha256("", 0, d);
  ASSERT_TRUE(digest_is(d, SHA_EMPTY));
  asper_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
               d);
  ASSERT_TRUE(digest_is(d, SHA_448));
}

TEST(sha256_streaming_chunks) {
  /* Byte-by-byte streaming must equal the one-shot digest. */
  const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  asper_sha256_ctx ctx;
  uint8_t d[32];
  size_t i, n = strlen(msg);
  asper_sha256_init(&ctx);
  for (i = 0; i < n; i++) asper_sha256_update(&ctx, msg + i, 1);
  asper_sha256_final(&ctx, d);
  ASSERT_TRUE(digest_is(d, SHA_448));
}

TEST(sha256_million_a) {
  asper_sha256_ctx ctx;
  uint8_t d[32];
  char block[1000];
  int i;
  memset(block, 'a', sizeof block);
  asper_sha256_init(&ctx);
  for (i = 0; i < 1000; i++) asper_sha256_update(&ctx, block, sizeof block);
  asper_sha256_final(&ctx, d);
  ASSERT_TRUE(digest_is(d, SHA_MILLION_A));
}

TEST(sha256_file) {
  char root[256], path[512];
  uint8_t d[32];
  FILE *f;
  ASSERT_TRUE(asper_test_tmpdir(root));
  snprintf(path, sizeof path, "%s/abc.txt", root);
  f = fopen(path, "wb");
  ASSERT_TRUE(f != NULL);
  fwrite("abc", 1, 3, f);
  fclose(f);
  ASSERT_OK(asper_sha256_file(path, d));
  ASSERT_TRUE(digest_is(d, SHA_ABC));
  snprintf(path, sizeof path, "%s/missing.txt", root);
  ASSERT_TRUE(asper_sha256_file(path, d) != ASPER_OK);
  asper_test_rmtree(root);
}

TEST(uuid_v4_shape_and_uniqueness) {
  enum { N = 64 };
  char ids[N][37];
  int i, j;
  for (i = 0; i < N; i++) {
    char *u = ids[i];
    asper_uuid_v4(u);
    ASSERT_EQ_INT(strlen(u), 36);
    ASSERT_EQ_INT(u[8], '-');
    ASSERT_EQ_INT(u[13], '-');
    ASSERT_EQ_INT(u[18], '-');
    ASSERT_EQ_INT(u[23], '-');
    ASSERT_EQ_INT(u[14], '4'); /* version nibble */
    ASSERT_TRUE(u[19] == '8' || u[19] == '9' || u[19] == 'a' ||
                u[19] == 'b'); /* RFC 4122 variant */
    for (j = 0; j < 36; j++) {
      if (j == 8 || j == 13 || j == 18 || j == 23) continue;
      ASSERT_TRUE((u[j] >= '0' && u[j] <= '9') ||
                  (u[j] >= 'a' && u[j] <= 'f')); /* lowercase hex */
    }
    ASSERT_TRUE(asper_uuid_valid(u));
  }
  for (i = 0; i < N; i++)
    for (j = i + 1; j < N; j++)
      ASSERT_TRUE(strcmp(ids[i], ids[j]) != 0);
}

TEST(uuid_bytes_roundtrip) {
  static const uint8_t want[16] = {0x7c, 0x9e, 0x66, 0x79, 0x74, 0x25,
                                   0x40, 0xde, 0x94, 0x4f, 0xe0, 0x7f,
                                   0xc1, 0xf9, 0x0a, 0xe7};
  uint8_t bytes[16];
  char out[37];
  char gen[37];
  ASSERT_TRUE(
      asper_uuid_to_bytes("7c9e6679-7425-40de-944f-e07fc1f90ae7", bytes));
  ASSERT_TRUE(memcmp(bytes, want, 16) == 0);
  asper_uuid_from_bytes(bytes, out);
  ASSERT_EQ_STR(out, "7c9e6679-7425-40de-944f-e07fc1f90ae7");
  /* generated uuid round-trips too */
  asper_uuid_v4(gen);
  ASSERT_TRUE(asper_uuid_to_bytes(gen, bytes));
  asper_uuid_from_bytes(bytes, out);
  ASSERT_EQ_STR(out, gen);
}

TEST(uuid_rejects) {
  static const char *bad[] = {
      "",
      "7c9e6679742540de944fe07fc1f90ae7",       /* no dashes      */
      "7c9e6679-7425-40de-944f-e07fc1f90ae",    /* 35 chars       */
      "7c9e6679-7425-40de-944f-e07fc1f90ae77",  /* 37 chars       */
      "7c9e6679-7425-40de-944f-e07fc1f90aeg",   /* bad hex        */
      "7c9e6679_7425_40de_944f_e07fc1f90ae7",   /* wrong dashes   */
      "7c9e-66797425-40de-944f-e07fc1f90ae7",   /* dash misplaced */
  };
  uint8_t bytes[16];
  size_t i;
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    if (asper_uuid_valid(bad[i])) {
      ASPER_FAILF("uuid_valid accepted \"%s\"", bad[i]);
      return;
    }
    if (asper_uuid_to_bytes(bad[i], bytes)) {
      ASPER_FAILF("uuid_to_bytes accepted \"%s\"", bad[i]);
      return;
    }
  }
}

TEST(embedding_pipeline_hash_versions_prefixes) {
  uint8_t weights[32] = {0};
  uint8_t a[32], b[32], c[32], again[32];
  asper_embedding_pipeline_hash(weights, "query: ", "passage: ", a);
  asper_embedding_pipeline_hash(weights, "query: ", "passage: ", again);
  asper_embedding_pipeline_hash(weights, "", "passage: ", b);
  asper_embedding_pipeline_hash(weights, "query: ", "", c);
  ASSERT_TRUE(memcmp(a, again, 32) == 0);
  ASSERT_TRUE(memcmp(a, b, 32) != 0);
  ASSERT_TRUE(memcmp(a, c, 32) != 0);
  ASSERT_TRUE(memcmp(b, c, 32) != 0);
}

TEST_LIST = {
    TEST_ENTRY(sha256_nist_vectors),
    TEST_ENTRY(sha256_streaming_chunks),
    TEST_ENTRY(sha256_million_a),
    TEST_ENTRY(sha256_file),
    TEST_ENTRY(uuid_v4_shape_and_uniqueness),
    TEST_ENTRY(uuid_bytes_roundtrip),
    TEST_ENTRY(uuid_rejects),
    TEST_ENTRY(embedding_pipeline_hash_versions_prefixes),
};

RUN_ALL_TESTS()
