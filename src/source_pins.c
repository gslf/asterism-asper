/* Bounded last-write-wins pin overlays shared by pages and source views. */
#include "source_internal.h"
#include <stdlib.h>
#include <string.h>

static int pin_order(const void *a, const void *b) {
  const asper_source_pin *x = a, *y = b;
  int id = strcmp(x->id, y->id);
  return id ? id : (x->order > y->order) - (x->order < y->order);
}

asper_err asper_source_pins_load(asper_ctx *c, const char *scope, asper_source_pins *pins) {
  memset(pins, 0, sizeof *pins);
  char *path = asper_source_scope_path(c, scope, "pins.log");
  if (!path) return ASPER_ERR_NOMEM;
  FILE *f = NULL; uint64_t bytes = 0;
  asper_err e = os_blob_open(path, &f, &bytes);
  free(path);
  if (e == ASPER_ERR_NOT_FOUND) return ASPER_OK;
  if (e != ASPER_OK) return e;
  if (bytes > ASPER_PIN_BYTES) e = ASPER_ERR_LIMIT;
  else if (bytes % 39) e = ASPER_ERR_PARSE;
  size_t n = (size_t)(bytes / 39);
  if (e == ASPER_OK && n) {
    pins->rows = calloc(n, sizeof *pins->rows);
    if (!pins->rows) e = ASPER_ERR_NOMEM;
  }
  for (size_t i = 0; e == ASPER_OK && i < n; i++) {
    char line[40] = {0};
    if (fread(line, 1, 39, f) != 39 || line[36] != ' ' || line[38] != '\n' ||
        (line[37] != '0' && line[37] != '1')) e = ASPER_ERR_PARSE;
    line[36] = 0;
    if (e == ASPER_OK && !asper_uuid_valid(line)) e = ASPER_ERR_PARSE;
    if (e == ASPER_OK) {
      memcpy(pins->rows[i].id, line, 37); pins->rows[i].order = (unsigned)i;
      pins->rows[i].pinned = line[37] == '1';
    }
  }
  if (e == ASPER_OK && fgetc(f) != EOF) e = ASPER_ERR_PARSE;
  if (ferror(f)) e = ASPER_ERR_IO;
  if (fclose(f) && e == ASPER_OK) e = ASPER_ERR_IO;
  if (e != ASPER_OK) return e;
  if (n > 1) qsort(pins->rows, n, sizeof *pins->rows, pin_order);
  for (size_t i = 0; i < n;) {
    size_t j = i + 1;
    while (j < n && !strcmp(pins->rows[i].id, pins->rows[j].id)) j++;
    pins->rows[pins->n++] = pins->rows[j-1]; i = j;
  }
  return ASPER_OK;
}

void asper_source_pins_apply(const asper_source_pins *pins, asper_event *event) {
  size_t lo = 0, hi = pins->n;
  while (lo < hi) {
    size_t mid = lo + (hi-lo)/2;
    if (strcmp(pins->rows[mid].id, event->id) < 0) lo = mid+1; else hi = mid;
  }
  if (lo < pins->n && !strcmp(pins->rows[lo].id, event->id)) event->pinned = pins->rows[lo].pinned;
}
