/* Strict schema for the checked pending receipt and its historical payload. */
#include "curation_receipt.h"
#include <string.h>

static int string(const asmodel_json_value *o, const char *key, size_t max) {
  const asmodel_json_value *v = asmodel_json_object_get(o, key);
  const char *s = asmodel_json_string_value(v);
  return s && strlen(s) == asmodel_json_string_length(v) && strlen(s) <= max;
}
static int number(const asmodel_json_value *o, const char *key) {
  const asmodel_json_value *v = asmodel_json_object_get(o, key);
  return asmodel_json_is_int(v) && asmodel_json_int_value(v) >= 0;
}
static const char *value(const asmodel_json_value *o, const char *key) {
  return asmodel_json_string_value(asmodel_json_object_get(o, key));
}
static int ids(const asmodel_json_value *o, const char *key, size_t max, bool nonempty) {
  const asmodel_json_value *a = asmodel_json_object_get(o, key);
  if (asmodel_json_typeof(a) != ASMODEL_JSON_ARRAY) return 0;
  size_t n = asmodel_json_array_len(a);
  if (n > max || (nonempty && !n)) return 0;
  for (size_t i = 0; i < n; i++) {
    const asmodel_json_value *v = asmodel_json_array_at(a, i);
    const char *id = asmodel_json_string_value(v);
    if (!id || asmodel_json_string_length(v) != 36 || !asper_uuid_valid(id)) return 0;
    for (size_t j = 0; j < i; j++)
      if (!strcmp(id, asmodel_json_string_value(asmodel_json_array_at(a, j)))) return 0;
  }
  return 1;
}
asmodel_json_value *curation_receipt_decode(const char *text) {
  if (!text || strlen(text) > CURATION_RECEIPT_MAX) return NULL;
  asmodel_json_value *o = NULL;
  if (asmodel_json_parse(text, strlen(text), &o)) return NULL;
  bool valid = asmodel_json_object_count(o) == 14 && number(o, "schema") &&
      asmodel_json_int_value(asmodel_json_object_get(o, "schema")) == 1 &&
      string(o, "id", 36) && asper_uuid_valid(value(o, "id")) &&
      string(o, "scope", 64) && asper_source_scope_valid(value(o, "scope")) &&
      string(o, "project", 128) && number(o, "created_at") &&
      number(o, "journal_ops_before") && number(o, "journal_bytes_before") &&
      ids(o, "sources", CURATION_BATCH_MAX, true) && ids(o, "handles", 12, false) &&
      string(o, "proposal", 65536) && string(o, "outcome", 32) &&
      string(o, "resolution_note", 1024);
  const asmodel_json_value *counts = asmodel_json_object_get(o, "counts");
  const asmodel_json_value *end = asmodel_json_object_get(o, "journal_ops_after");
  const char *outcome = valid ? value(o, "outcome") : "";
  if (valid && !strcmp(outcome, "prepared"))
    valid = (counts && asmodel_json_typeof(counts) == ASMODEL_JSON_NULL) && (end && asmodel_json_typeof(end) == ASMODEL_JSON_NULL) && !*value(o, "resolution_note");
  else if (valid && !strcmp(outcome, "processed"))
    valid = asmodel_json_object_count(counts) == 3 && number(counts, "applied") &&
        number(counts, "rejected") && number(counts, "pending") && number(o, "journal_ops_after") &&
        asmodel_json_int_value(end) >= asmodel_json_int_value(asmodel_json_object_get(o, "journal_ops_before")) &&
        !*value(o, "resolution_note");
  else if (valid && !strcmp(outcome, "interrupted_acknowledged"))
    valid = (counts && asmodel_json_typeof(counts) == ASMODEL_JSON_NULL) && (end && asmodel_json_typeof(end) == ASMODEL_JSON_NULL) && *value(o, "resolution_note");
  else valid = false;
  if (!valid) { asmodel_json_free(o); o = NULL; }
  return o;
}
