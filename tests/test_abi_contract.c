/* Compile the public contract separately and exercise the shared library. */
#include "asper.h"
int main(int argc, char **argv) {
  if (argc != 2 || asper_abi_version() != ASPER_ABI_VERSION) return 2;
  asper_open_params p = {0};
  asper_ctx *ctx = NULL;
  p.memory_root = argv[1];
  if (asper_open(&p,&ctx) != ASPER_OK || !ctx) return 1;
  asper_close(ctx);
  return 0;
}
