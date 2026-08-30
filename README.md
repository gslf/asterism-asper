# ⁂ asper — asterism persistence

An identitarian memory system for LLMs. 

Asper is a local, self-curated long-term memory subsystem for language models, with a specific focus on the smaller ones. It gives a host model  a durable sense of **who it is** (identity), **who it is talking to** (context), and **what it is working on** (projects), by maintaining a retrieval-augmented memory that is written, reorganized and pruned autonomously by a second, micro local *curator* model.

Identity is the always-present first layer. The store is plain [xCDN](https://github.com/gslf/xCDN), so it's easy to inspect and edit. Curator inference and embeddings run in-process via llama.cpp, on a worker thread. They are owned by the shared `asmodel` runtime: standalone Asper creates its own manager, while an embedding host can lend a process-wide manager through `asper_open_at_with_models`.

Exact scoped events are the source of truth. Asper also owns atomic working
checkpoints and content-addressed objects for large payloads. Semantic records
are bounded derivatives with source-event UUID provenance; unfinished curation
is replayed after restart, so compaction saves context tokens without deleting
information.

Full specification: [docs/SPECS.md](docs/SPECS.md).


## Deliverables

| Artifact    | Description                                                        |
|-------------|--------------------------------------------------------------------|
| `libasper`  | C99 library (static + shared): store, retrieval, injection, curation |
| `asper-mcp` | MCP server (stdio, JSON-RPC 2.0) exposing the memory as tools      |

## Build

```sh
mkdir asterism && cd asterism
git clone --recursive https://github.com/gslf/asterism-asper.git
git clone https://github.com/gslf/asterism-asmodel.git
cd asterism-asper
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`asterism-asmodel` is expected next to this repository; use
`-DASPER_ASMODEL_DIR=/path/to/asterism-asmodel` for another layout. CMake
options: `ASPER_BUILD_MCP` (ON), `ASPER_BUILD_TESTS` (ON),
`ASPER_NO_THREADS` (OFF), `ASPER_SANITIZERS` (OFF), `ASPER_WITH_LLAMA`
(ON — set OFF for a fast, inference-less build, the full test suite passes
either way).

**Upstream llama.cpp compatibility.** The pinned `deps/llama.cpp`
submodule is built unmodified: Asper does not apply or require a custom
fork. Calls that model-controlled data reaches go through the small C++
adapter in `src/llama_guard.cpp`; exceptions are converted into the error
returns the C callers already handle. Its grammar sampler adapter also
contains malformed grammar state at the public sampler boundary and ends
generation cleanly, without changing llama.cpp sources.


## Configuration

Everything Asper needs beyond the memory root — model paths, token budgets, retrieval weights, curation timing, decay, logging — comes from one optional file: `config.xcdn`. 

```sh
asper-mcp --root ./memory --config config.xcdn
```

```c
asper_open_params p = { .memory_root = "./memory", .config_path = "config.xcdn" };
```

Every key is optional and already has a sensible default, so you only need to write the ones you want to change; unknown keys are ignored with a warning, wrong types fail `asper_open` with `ASPER_ERR_CONFIG`. A fully-documented copy with every key spelled out at its default value ships at [examples/config.xcdn](examples/config.xcdn) in this repo — copy it and trim it down to what you actually want to override.

### Models

The two GGUF models Asper needs are just two keys in that file: `curator.model_path` and `embedding.model_path`. Relative paths are
resolved against the **current working directory of the process**. With no config
file at all, the defaults are:

- Curator: `models/qwen2.5-1.5b-instruct-q4_k_m.gguf`
- Embeddings: `models/multilingual-e5-small-q8_0.gguf`

When a model file is missing, `asper_open` logs a warning and continues in degraded mode (identity injection still works; retrieval/curation/recall are disabled until models are available).

Both roles can instead use an OpenAI-compatible API (LM Studio, vLLM,
Unsloth Studio, or another compatible server):

```xcdn
#asper_config {
  models: { max_resident: 2, max_ram_mb: 12000, max_vram_mb: 8000 },
  curator: {
    backend: "openai",
    base_url: "http://127.0.0.1:1234/v1",
    remote_model: "qwen2.5-1.5b-instruct",
    api_key_env: "LOCAL_LLM_API_KEY",
    provider: "llama-server",
  },
  embedding: {
    backend: "openai",
    base_url: "http://127.0.0.1:1234/v1",
    remote_model: "text-embedding-nomic-embed-text-v1.5",
    dim: 768,
  },
}
```

`api_key_env` names an environment variable; credentials are never stored
in the xCDN file. `provider` is explicit: `"llama-server"`, `"lmstudio"`,
`"vllm"`, or `"generic"`.

## Quick start (C API)

```c
#include "asper.h"

asper_open_params p = { .memory_root = "./memory", .config_path = NULL };
asper_ctx *ctx;
asper_open(&p, &ctx);

asper_event_input event = {
  .scope = "chat-1", .kind = ASPER_EVENT_USER, .text = "Ciao!"
};
asper_event_append(ctx, &event, NULL);

asper_context_request request = {
  .scope = "chat-1", .query = "Ciao!",
  .base_system_prompt = "You are a helpful assistant.",
  .history_tokens = 2048, .checkpoint_tokens = 1024
};
asper_context_pack context;
asper_context_materialize(ctx, &request, &context);
/* ... run your model with `context.system_prompt` + `context.context_text` ... */
asper_context_pack_free(&context);
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

Tools: `memory_search`, `memory_recall`, `memory_insert`, `memory_update`, `memory_deprecate`, `memory_list`, `project_select`, `project_list`, `source_append`, `context_materialize`, `memory_stats`.

## Agent Plugin

[plugin/](plugin/) packages the MCP server and an `asper-memory` usage skill in the [Agent Plugins 1.0](https://agent-plugins.org/) format, consumable by any compatible client (VS Code, Cursor, GitHub Copilot, ChatGPT & Codex, Kiro, …). It expects `asper-mcp` on the PATH and keeps the memory store in the client-managed plugin data directory; see [plugin/README.md](plugin/README.md).

## License

MIT — see [LICENSE](LICENSE).
