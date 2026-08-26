# ⁂ asper — asterism persistence

An identitarian memory system for LLMs. 

Asper is a local, self-curated long-term memory subsystem for language models, with a specific focus on the smaller ones. It gives a host model  a durable sense of **who it is** (identity), **who it is talking to** (context), and **what it is working on** (projects), by maintaining a retrieval-augmented memory that is written, reorganized and pruned autonomously by a second, micro local *curator* model.

Identity is the always-present first layer. The store is plain [xCDN](https://github.com/gslf/xCDN), so it's easy to inspect and edit. Curator inference and embeddings run in-process via llama.cpp, on a worker thread.

Full specification: [docs/SPECS.md](docs/SPECS.md).


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

CMake options: `ASPER_BUILD_MCP` (ON), `ASPER_BUILD_TESTS` (ON), `ASPER_NO_THREADS` (OFF), `ASPER_SANITIZERS` (OFF), `ASPER_WITH_LLAMA` (ON — set OFF for a fast, inference-less build, the full test suite passes either way).


## Configuration

Everything Asper needs beyond the memory root — model paths, token budgets, retrieval weights, curation timing, decay, logging — comes from one optional file: `config.xcdn`. 

```sh
asper-mcp --root ./memory --config config.xcdn
```

```c
asper_open_params p = { .memory_root = "./memory", .config_path = "config.xcdn" };
```

Every key is optional and already has a sensible default, so you only need to write the ones you want to change; unknown keys are ignored with a warning, wrong types fail `asper_open` with `ASPER_ERR_CONFIG`. A fully-documented copy with every key spelled out at its default value ships at [config.xcdn](config.xcdn) in this repo — copy it and trim it down to what you actually want to override.

### Models

The two GGUF models Asper needs are just two keys in that file: `curator.model_path` and `embedding.model_path`. Relative paths are
resolved against the **current working directory of the process**. With no config
file at all, the defaults are:

- Curator: `models/qwen2.5-1.5b-instruct-q4_k_m.gguf`
- Embeddings: `models/multilingual-e5-small-q8_0.gguf`

When a model file is missing, `asper_open` logs a warning and continues in degraded mode (identity injection still works; retrieval/curation/recall are disabled until models are available).

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

`asper_open` preserves cwd-relative resolution and the original two-pointer
`asper_open_params` ABI. A host that resolves models relative to its own root
can use the additive `asper_open_at(&p, engine_root, &ctx)` API without changing
the public struct.

## MCP server

```sh
asper-mcp --root ./memory [--config config.xcdn]
```

Tools: `memory_search`, `memory_recall`, `memory_insert`, `memory_update`, `memory_deprecate`, `memory_list`, `project_select`, `project_list`, `observe_turn`, `context_build`, `memory_stats`.

## Agent Plugin

[plugin/](plugin/) packages the MCP server and an `asper-memory` usage skill in the [Agent Plugins 1.0](https://agent-plugins.org/) format, consumable by any compatible client (VS Code, Cursor, GitHub Copilot, ChatGPT & Codex, Kiro, …). It expects `asper-mcp` on the PATH and keeps the memory store in the client-managed plugin data directory; see [plugin/README.md](plugin/README.md).

## License

MIT — see [LICENSE](LICENSE).
