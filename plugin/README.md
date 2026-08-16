# asper — Agent Plugin

This directory is an [Agent Plugins 1.0](https://agent-plugins.org/specification)
package for Asper: it declares the `asper-mcp` memory server (`mcp.json`) and
ships the `asper-memory` skill (`skills/`), which teaches a host agent the
intended memory workflow. Any Agent Plugins-capable client (VS Code, Cursor,
GitHub Copilot, ChatGPT & Codex, Kiro, …) can consume it; each client documents
its own install command — in general, point the client at this directory.

## Prerequisite: `asper-mcp` on PATH

The plugin does not bundle binaries: it launches the bare command `asper-mcp`
and relies on platform executable search. Build and install it first, e.g.:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
install build/asper-mcp /usr/local/bin/
```

## Where things live

The client provides a writable per-plugin data directory (`PLUGIN_DATA`) that
persists across plugin updates. The server is configured to keep everything
there:

| Path                          | Purpose                                          |
|-------------------------------|--------------------------------------------------|
| `<PLUGIN_DATA>/memory/`       | the memory store (`--root`; created on first run) |
| `<PLUGIN_DATA>/models/*.gguf` | curator + embedding models (optional, see below) |

The store is plain xCDN text — you can open and hand-edit it.

## Full mode vs degraded mode

Out of the box the server starts in **degraded mode**: identity injection and
manual memory management work; semantic retrieval, curation, and recall are
disabled until the two GGUF models are present. The server's working directory
is `PLUGIN_DATA`, so the default model paths resolve there — to enable full
mode, drop the models in:

```
<PLUGIN_DATA>/models/qwen2.5-1.5b-instruct-q4_k_m.gguf
<PLUGIN_DATA>/models/multilingual-e5-small-q8_0.gguf
```

To customize further (budgets, retrieval weights, model paths, logging), place
a `config.xcdn` in `PLUGIN_DATA` and add `"--config", "config.xcdn"` to the
`args` in `mcp.json` (§11 of [SPECS.md](../docs/SPECS.md) lists every key).
