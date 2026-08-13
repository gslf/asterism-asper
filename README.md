# ⁂ asper — asterism persistence

An identitarian memory system for small LLMs. *Per aspera ad astra.*

Asper is a local, self-curated long-term memory subsystem for small language
models (1–8 B). It gives a host model a durable sense of **who it is** (identity),
**who it is talking to** (context), and **what it is working on** (projects), by
maintaining a retrieval-augmented memory that is written, reorganized and pruned
autonomously by a second, even smaller local *curator* model.

- **Identitarian**: identity is the always-present first layer — persona, values,
  tone and style are injected into every prompt.
- **Human-inspectable**: the store is plain [xCDN](https://github.com/gslf/xCDN)
  text you can open and hand-edit.
- **Crash-safe**: append-only journal + atomic compaction.
- **Fully offline**: curator inference and embeddings run in-process via
  llama.cpp; no network, ever.
- **Non-blocking**: curation runs on a worker thread; the host loop never waits.

Full specification: [docs/SPEC.txt](docs/SPEC.txt).

## Deliverables

| Artifact    | Description                                                        |
|-------------|--------------------------------------------------------------------|
| `libasper`  | C99 library (static + shared): store, retrieval, injection, curation |
| `asper-mcp` | MCP server (stdio, JSON-RPC 2.0) exposing the memory as tools      |

## Build

```sh
git clone --recursive https://github.com/gslf/asterism-asper.git
cd asterism-asper
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CMake options: `ASPER_BUILD_MCP` (ON), `ASPER_BUILD_TESTS` (ON),
`ASPER_NO_THREADS` (OFF), `ASPER_SANITIZERS` (OFF), `ASPER_WITH_LLAMA` (ON —
set OFF for a fast, inference-less build; the full test suite passes either way).

## Models

Asper needs two GGUF models at the paths set in `config.xcdn` (§11 of the spec):

- Curator (default): `models/qwen2.5-1.5b-instruct-q4_k_m.gguf`
- Embeddings (default): `models/multilingual-e5-small-q8_0.gguf`

When a model file is missing, `asper_open` logs a warning and continues in
degraded mode (identity injection still works; retrieval/curation/recall are
disabled until models are available).

## Quick start (C API)

```c
#include "asper.h"

asper_open_params p = { .memory_root = "./memory", .config_path = NULL };
asper_ctx *ctx;
asper_open(&p, &ctx);

char *prompt;
asper_build_prompt(ctx, "You are a helpful assistant.", "Ciao!", &prompt);
/* ... run your model with `prompt` ... */
asper_observe_turn(ctx, ASPER_ROLE_USER, "Ciao!");
asper_observe_turn(ctx, ASPER_ROLE_ASSISTANT, "Ciao! Come posso aiutarti?");

asper_free(prompt);
asper_close(ctx);
```

## MCP server

```sh
asper-mcp --root ./memory [--config config.xcdn]
```

Tools: `memory_search`, `memory_recall`, `memory_insert`, `memory_update`,
`memory_deprecate`, `memory_list`, `project_select`, `project_list`,
`observe_turn`, `context_build`, `memory_stats`.

## License

MIT — see [LICENSE](LICENSE).
