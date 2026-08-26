/* Deliberately does not include asper.h: this is the exact public declaration
 * seen by clients before asper_open_at was introduced. */
#include <stddef.h>

typedef struct asper_ctx asper_ctx;
typedef struct {
  const char *memory_root;
  const char *config_path;
} legacy_asper_open_params;

#if defined(_WIN32)
#define LEGACY_IMPORT __declspec(dllimport)
#else
#define LEGACY_IMPORT
#endif

LEGACY_IMPORT int asper_open(const legacy_asper_open_params *p,
                             asper_ctx **out);
LEGACY_IMPORT void asper_close(asper_ctx *ctx);

int main(int argc, char **argv) {
  legacy_asper_open_params p;
  asper_ctx *ctx = NULL;
  if (argc != 2) return 2;
  p.memory_root = argv[1];
  p.config_path = NULL;
  if (asper_open(&p, &ctx) != 0 || ctx == NULL) return 1;
  asper_close(ctx);
  return 0;
}
