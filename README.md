# ⁂ asper — asterism persistence

### Durable memory and context subsystem for small and large language models.

>⁂ asterism is a modular agent harness that turns language models into tools for creating and completing real-world workflows and automations. **SLM** and **local inference** friendly. Read the central [architecture decisions and system value](https://github.com/gslf/asterism-asngn/blob/main/docs/ARCHITECTURE.md).

⁂ asper preserves identity, user context, project knowledge, workflow state and execution evidence beyond a single model context window. A separate curator
model derives compact semantic records from exact events, while the host retains control over memory validity and permissions. 

The local store uses inspectable [xCDN](https://github.com/gslf/xCDN). Curation runs on a worker thread. Curator inference and embeddings use the shared ⁂ asmodel runtime through embedded llama.cpp or
supported remote adapters. Standalone ⁂ asper creates its own manager, an embedding host can lend a process-wide manager through `asper_open_at_with_models`.
Local persistence does not require local inference.

⁂ asper supplies memory to **⁂ asngn**, retains evidence from **⁂ astools** actions, and uses **⁂ asmodel** for inference. It can also serve independent hosts through C or MCP. 

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

`asterism-asmodel` is expected next to this repository, use
`-DASPER_ASMODEL_DIR=/path/to/asterism-asmodel` for another layout. 


CMake options: 
- `ASPER_BUILD_MCP` (ON)
- `ASPER_BUILD_TESTS` (ON)
- `ASPER_NO_THREADS` (OFF) 
- `ASPER_SANITIZERS` (OFF)
- `ASPER_WITH_LLAMA` (ON. Set OFF for a fast, inference-less build, the full test suite passes
either way).

**Upstream llama.cpp compatibility.** The pinned `deps/llama.cpp`
submodule is built unmodified: ⁂ asper does not apply or require a custom
fork.

## Configuration

Everything ⁂ asper needs beyond the memory root (model paths, token budgets, retrieval weights, curation timing, decay, logging) comes from one optional file `config.xcdn`.

```sh
asper-mcp --root ./memory --config config.xcdn
```

Every key is optional and already has a sensible default, so you only need to write the ones you want to change/. Unknown keys are ignored with a warning, wrong types fail `asper_open` with `ASPER_ERR_CONFIG`. 

A fully-documented copy with every key spelled out at its default value ships at [examples/config.xcdn](examples/config.xcdn) in this repo.

### Models

The two GGUF models ⁂ asper needs are just two keys in that file: `curator.model_path` and `embedding.model_path`. Relative paths are
resolved against the **current working directory of the process**. With no config
file at all, the defaults are:

- Curator: `models/qwen2.5-1.5b-instruct-q4_k_m.gguf`
- Embeddings: `models/multilingual-e5-small-q8_0.gguf`

When a model file is missing, `asper_open` logs a warning and continues in degraded mode (identity injection still works, retrieval/curation/recall are disabled until models are available).

Both roles can instead use an OpenAI-compatible API (LM Studio, vLLM, Unsloth Studio, or another compatible server):

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

`api_key_env` names an environment variable, credentials are never stored in the xCDN file. 

`provider` is explicit: `
- "llama-server"`
- `"lmstudio"`
- `"vllm"`
- `"generic"`

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


## MCP server

```sh
asper-mcp --root ./memory [--config config.xcdn]
```

Tools: `memory_search`, `memory_recall`, `memory_insert`, `memory_update`, `memory_deprecate`, `memory_list`, `project_select`, `project_list`, `source_append`, `context_materialize`, `memory_stats`.

## Agent Plugin

[plugin/](plugin/) packages the MCP server and an `asper-memory` usage skill in the [Agent Plugins 1.0](https://agent-plugins.org/) format, consumable by any compatible client (VS Code, Cursor, GitHub Copilot, ChatGPT & Codex, Kiro, …). It expects `asper-mcp` on the PATH and keeps the memory store in the client-managed plugin data directory; see [plugin/README.md](plugin/README.md).

## License

MIT — see [LICENSE](LICENSE).