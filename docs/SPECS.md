# Asper — Asterism Persistence

**An identitarian memory system for LLMs**

Software Specification · v0.3 · 2026-07-29

| | |
|---|---|
| Project | Asterism Persistence — short name "Asper" |
| Status | Accepted — all open points resolved |
| Date | 2026-07-29 |
| Implementation language | C (C99) |
| Platforms | Linux, macOS, Windows |
| License | MIT (D2) |



## 1. Introduction

### 1.1 Purpose

Asper (Asterism Persistence) is a local, self-curated long-term memory subsystem for small language models (roughly 1-8 B parameters). Small models have short effective context windows, weak long-range recall, and no persistence between sessions. Asper gives a host model a durable sense of who it is, who it is talking to, and what it is working on, by maintaining a retrieval-augmented (RAG) memory that is written, reorganized, and pruned autonomously by a second, even smaller local curator model.

The system is **identitarian** in the sense that identity is the first-class, always-present layer of memory: whatever else retrieval finds, the model's persona, values, tone, and style are injected into every prompt, giving a small model a stable character across sessions.

The system is **fully offline**: inference (curator + embeddings) and storage run on the local machine. No network access is performed.

### 1.2 Scope and deliverables

Deliverables:

- **libasper** — C99 library (static + shared) implementing the whole system: store, retrieval, injection, curation.
- **asper-mcp** — MCP (Model Context Protocol) server executable; a thin wrapper over libasper exposing the memory as MCP tools over stdio.

Out of scope for v1: GUI, remote or multi-user server, cloud sync, model fine-tuning, non-text memories (images, audio), vector-database scale (design ceiling ~100 k records).

### 1.3 Definitions

| Term | Definition |
|---|---|
| Host model | The main small LLM served by the host application. Asper never runs it; Asper only prepares its prompt. |
| Host application | The program (chat UI, agent loop, ...) that links libasper and runs the host model. |
| Curator | A local small instruct model (~1-3 B, GGUF) executed in-process via llama.cpp. It decides what to insert, where to place it, what to update, and what to deprecate. |
| Memory record | One atomic unit of memory: a short, self-contained text plus metadata (§3.1). |
| Section | One of the three memory areas: identity, context, project (§3.2). |
| Deprecation | Soft retirement of a record, driven by relevance decay and confirmed by the curator; recoverable until purged at compaction. |
| Injection | Deterministic assembly of memories into the host model's system prompt (§6). |
| Store | The on-disk representation: xCDN flat files + append-only journal + derived caches (§4). |
| Cycle | One asynchronous run of the curator over a batch of unprocessed conversation turns. |

---

## 2. System overview

### 2.1 Design goals

**G1 — Small-model first.**

**G2 — Minimal dependencies.** libasper is strict C99 over the standard library plus a thin platform-threads shim. External code is admitted only for modules that are very large and impractical to reimplement.

**G3 — Human-inspectable store.** 

**G4 — Crash safety.** A crash at any instant loses at most the in-flight batch of operations, never corrupts the store.

**G5 — Multiplatform.** Linux, macOS, Windows from a single C99 codebase. Platform differences isolated in a small OS shim (threads, atomic file replace).

**G6 — Non-blocking.** Curation runs asynchronously on a worker thread.


### 2.2 Architecture

```
  Host application (chat loop + host model)    MCP clients (agents, IDEs)
                |                                     |
                | C API (asper.h)                     | JSON-RPC 2.0 / stdio
                |                                     v
                |                              +-------------+
                |            +-----------------|  asper-mcp  |
                v            v                 +-------------+
  +--------------------------------------------------------------+
  |                        libasper  (C99)                       |
  |                                                              |
  |   Injection engine <-- Retrieval engine <-- Vector index     |
  |                              ^                   ^           |
  |                              | query embedding   | vectors   |
  |                        Embedding engine ---------+           |
  |                         (llama.cpp)                          |
  |                                                              |
  |   Event queue --> Curator engine --> ops --> Memory store    |
  |   (turns)         (llama.cpp + GBNF)         xCDN + journal  |
  +--------------------------------------------------------------+
```

Components:

- **Public API** (`asper.h`): host-facing C API; §9.
- **Injection engine**: renders the memory block of the host system prompt within per-section token budgets; §6.
- **Retrieval engine**: embeds queries, scores records, returns per-section top-k; §5.
- **Vector index**: in-memory flat cosine index over L2-normalized embeddings; §5.2.
- **Embedding engine**: llama.cpp embedding session running a small dedicated embedding model; §5.1.
- **Event queue**: buffers conversation turns until a curation trigger fires; §7.2.
- **Curator engine**: llama.cpp completion session running the curator model with grammar-constrained output; validates and applies ops; answers recall queries; §7.
- **Memory store**: xCDN section files, append-only journal, manifest, embedding cache; §4.
- **MCP server**: stdio JSON-RPC 2.0 executable exposing Asper tools; §10.

### 2.3 Data flow

**Inference path** (synchronous, hot):

1. The host calls `asper_build_prompt(base_system_prompt, user_message)`.
2. The retrieval engine embeds `user_message`.
3. **identity**: all active records are selected in deterministic order (§6.1) and trimmed to the identity token budget.
4. **context** and (if a project is active) **project**: top-k records by score above the similarity floor, trimmed to their budgets.
5. The injection engine renders the template (§6.2) and returns the fully assembled system prompt: `base_system_prompt` + memory block. The host prepends nothing and computes nothing — it passes the returned string to the host model verbatim. This realizes the chosen auto-injection integration: Asper builds the prompt, the host just uses it.
6. The ids of injected records are queued as access events (batched, asynchronous — they feed `access_count`, `last_access`, and the relevance boost).

**Curation path** (asynchronous, cold):

1. The host calls `asper_observe_turn(role, text)` after every user and assistant message.
2. When a trigger fires (§7.2), the worker thread builds the curator prompt: unprocessed turns + the most similar existing memories, each labeled with a short handle `M1..Mn`.
3. The curator emits operations in the line protocol (§7.4), constrained by a GBNF grammar so the output always parses.
4. The engine validates each op against the guardrails (§7.5), applies the surviving ops to the in-memory state, appends them to the journal, and updates the vector index and embedding cache.
5. Periodic maintenance: relevance decay evaluation, deprecation review (§7.6), compaction (§4.6).

**Recall path** (synchronous, on demand):

1. The host — typically wiring a tool call for its model — or an MCP client calls `asper_recall(question)`.
2. The retrieval engine embeds the question and gathers the top `recall.k` candidates across all sections, active project included.
3. The worker runs a dedicated curator cycle over question + candidates, under the reduced ANSWER/CITE/NOMEM grammar (§7.7).
4. Asper returns the answer plus the cited records; cited records receive the standard access boost.

---

## 3. Memory model

### 3.1 Memory record

Canonical representation (xCDN, semantic tag `#memory`):

```
#memory {
  id: u"7c9e6679-7425-40de-944f-e07fc1f90ae7",
  section: "context",  // "identity" | "context" | "project"
  project: null,  // project slug when section == "project", else null
  content: """The user prefers concise answers, written in Italian.""",
  source: "curator",  // "curator" | "manual" | "seed"
  created_at: t"2026-07-29T10:12:00Z",
  updated_at: t"2026-07-29T10:12:00Z",
  last_access: t"2026-07-29T10:12:00Z",
  access_count: 3,
  relevance: 0.62,  // stored relevance, range [0, 1]
  locked: false,  // curator may never modify or deprecate when true
  status: "active",  // "active" | "deprecated"
  supersedes: null,  // UUID of the record this one replaced, or null
  tags: ["preference", "language"],  // free-form lowercase labels, optional
}
```

Field rules:

- **id**: UUID `u"..."`. Version 4, generated by libasper. Immutable.
- **section**: string. One of the three section names. Immutable (a memory never migrates; it is deprecated and re-inserted).
- **project**: string | null. Required and non-null iff `section == "project"`.
- **content**: valid UTF-8 string. Self-contained natural-language statement, understandable without the conversation that produced it. Max `content_max_chars` Unicode scalar values (default 500).
- **source**: string. `seed` = shipped/initial identity, `manual` = inserted via API/MCP by the user, `curator` = inserted by the curator.
- **created_at, updated_at, last_access**: DateTime `t"..."`. RFC 3339, UTC.
- **access_count**: number. Times the record was injected into a prompt.
- **relevance**: number. Stored base relevance in [0,1]; effective relevance derives from it via decay (§3.4).
- **locked**: boolean. Protects seed identity (and anything the user marks) from the curator.
- **status**: string. `deprecated` records are excluded from retrieval and injection but kept on disk until purge.
- **supersedes**: UUID | null. Set by the engine when a deduplication merge replaces an older record.
- **tags**: array of strings. Optional; not used by scoring in v1, reserved for filtering.

`UPDATE` operations rewrite content of the same id in place, bump `updated_at`, and trigger re-embedding. Replacement with a new id (`supersedes`) is only produced by the engine's dedup merge, never requested by the curator.

### 3.2 The three sections

| Section | Holds | Injection | Decay |
|---|---|---|---|
| **identity** | Persona, values, tone, style of the host model. | All active records, every prompt, in deterministic order, up to the identity budget. Never subject to retrieval filtering. | None. Identity does not decay. |
| **context** | User context, user preferences, environment details. | Top-k semantic retrieval against the current user message. | Half-life 30 d (default). |
| **project** | Knowledge scoped to one named project. | Top-k semantic retrieval, restricted to the active project. | Half-life 90 d (default). |

Additional identity rules:

1. **Bounded size**: at most `identity_max_records` (default 64) active records; curator `INSERT identity` beyond the cap is rejected.
2. **Seeding**: the developer or user seeds initial identity records (`source: "seed"`), typically with `locked: true`.
3. **Asymmetric trust**: the curator may freely `INSERT` and `UPDATE` unlocked identity records, but destructive or mutating ops on identity require double confirmation across cycles (§7.5), and are always rejected on locked records.

### 3.3 Projects

A project is a namespace identified by a slug matching `[a-z0-9][a-z0-9-]{0,63}`, stored as one xCDN file (§4.1). At most one project is active per Asper context at any time, selected explicitly by the host (`asper_project_select`) or an MCP client (`project_select`). While no project is active: the project block is omitted from injection and curator `INSERT project` ops are rejected.

The curator never invents project names: `INSERT project` always targets the currently active project. Creating and choosing projects is a host/user decision: selecting an unknown slug creates the project when `projects.autocreate` is enabled (the default); with autocreation disabled, `asper_project_select` fails with `ASPER_ERR_NOT_FOUND` (D1).

### 3.4 Lifecycle and relevance

```
           insert                eff < prune_threshold         compaction
  (curator | manual | seed)      + curator confirmation      after purge_after
     --------->  ACTIVE  ---------------------->  DEPRECATED ------>  purged
                   ^                                   |
                   +--- curator KEEP / manual restore--+
```

Relevance dynamics:

- Initial stored relevance: 0.60 for curator insertions, 0.80 for manual insertions, 1.00 for seeds.
- **Access boost**: each injection adds +0.05 to stored relevance (capped at 1.0) and refreshes `last_access`. Boosts are batched (§4.3).
- **Effective relevance** at evaluation time `t`:

  ```
  eff(m, t) = relevance(m) * 0.5 ^ (dt_last_access / half_life(section))
  ```

  where identity has no decay (`eff = relevance`).
- When `eff < prune_threshold` (default 0.15) the record becomes a deprecation candidate; candidates are presented to the curator during the maintenance review (§7.6), which confirms (`DEPRECATE`) or rescues (`KEEP`). Locked records and the whole identity section are exempt.
- Deprecated records older than `purge_after` (default 60 d in that state) are physically removed at the next compaction.

---

## 4. Storage layer

The store is a directory of UTF-8 xCDN text files plus derived binary caches. xCDN ("eXtensible Cognitive Data Notation", https://github.com/gslf/xCDN) is used for every persistent text artifact: its typed literals (`u"..."` UUID, `t"..."` RFC 3339 DateTime, `r"..."` ISO 8601 Duration, `b"..."` Bytes), semantic tags (`#memory`, `#op`), comments, and native streaming (a file is a sequence of top-level values) map directly onto Asper needs. Parsing uses the official C implementation, xCDN-C (https://github.com/gslf/xCDN-C); see §12; verifying xCDN-C's streaming and serialization coverage is the opening task of milestone M1 (D3).

### 4.1 On-disk layout

```
<memory_root>/
├── manifest.xcdn          # store version, embedding model id, counters
├── identity.xcdn          # stream of #memory values, section "identity"
├── context.xcdn           # stream of #memory values, section "context"
├── projects/
│   ├── website-redesign.xcdn
│   └── thesis.xcdn        # one file per project slug
├── journal.xcdn           # stream of #op values since the last compaction
└── cache/
    └── embeddings.bin     # derived, rebuildable embedding cache
```

### 4.2 Section files

Each section file is an xCDN stream: zero or more top-level `#memory` values (§3.1) separated by whitespace, in no significant order. Files are rewritten only at compaction; between compactions all changes live in the journal.

Hand editing is a supported workflow while the store is closed: on the next load, any record whose content no longer matches the cached content hash is re-embedded automatically. Comments added by hand are legal xCDN but are not preserved across compaction (documented limitation).

### 4.3 Journal

`journal.xcdn` is an append-only xCDN stream of `#op` values. Every mutation — from the curator, from the API, or from an MCP tool — is appended here first, then applied to the in-memory state.

```
#op { kind: "insert",    at: t"2026-07-29T10:12:00Z",
      record: #memory { /* full record, §3.1 */ } }
#op { kind: "update",    at: t"2026-07-29T10:14:07Z",
      id: u"7c9e6679-7425-40de-944f-e07fc1f90ae7",
      content: """Prefers concise answers, in Italian, with C samples.""" }
#op { kind: "deprecate", at: t"2026-07-29T10:20:00Z",
      id: u"...", reason: "decayed" }
#op { kind: "keep",      at: t"2026-07-29T10:20:00Z",
      id: u"..." }  // rescue from review
#op { kind: "set_locked", at: t"2026-07-29T10:21:00Z",
      id: u"...", value: true }
#op { kind: "access",    at: t"2026-07-29T10:25:00Z",
      ids: [u"...", u"..."] }  // batched boosts
#op { kind: "project_create", at: t"2026-07-29T10:26:00Z",
      project: "thesis" }
```

Access events are batched in memory and flushed as a single access op per curation cycle (and at `asper_flush`/`asper_close`) to bound journal growth.

Durability policy `storage.journal_sync`: `"batch"` (default — fsync after each applied curation batch and after every manual op), `"always"` (after every op), `"never"` (leave it to the OS).

**Torn-tail rule**: if the last value in the journal fails to parse (crash mid-append), it is discarded, the file is truncated to the last good offset, and a warning is logged. A parse error anywhere before the tail is fatal (`ASPER_ERR_PARSE`): the store is left untouched for manual repair — which is always possible, because everything is text.

### 4.4 Manifest

```
#asper_manifest {
  store_version: 1,
  created_at: t"2026-07-29T09:00:00Z",
  last_compaction: t"2026-07-29T10:00:00Z",
  embedding: {
    model_id: "multilingual-e5-small-q8_0",
    dim: 384,
    model_hash: b"7Jw1...",  // SHA-256 of the embedding model file, Base64
  },
}
```

`store_version` gates forward-compatibility: a library refuses to open a store with a greater version. A change of embedding model (`model_hash` or `dim` mismatch) invalidates the whole embedding cache, never the text store.

### 4.5 Embedding cache

Embeddings are derived data: they never appear in the xCDN files and can always be recomputed from content. The cache avoids re-embedding the whole store at startup.

Binary little-endian layout of `cache/embeddings.bin`:

| Field | Size | Description |
|---|---|---|
| magic | 4 bytes | `"ASPE"` |
| version | u32 | 1 |
| dim | u32 | embedding dimension |
| count | u64 | number of entries |
| model_hash | 32 bytes | SHA-256 of the embedding model file |
| entry × count | 16 + 8 + 4×dim | record UUID (raw 16 B) + FNV-1a-64 hash of content + float32[dim], L2-normalized |

Rebuild/refresh triggers: file missing, bad magic/version, dim or model_hash mismatch with the manifest (full rebuild); per-record content hash mismatch or missing entry (single re-embed). Missing embeddings are computed at load before the context becomes ready; progress is reported through the log callback.

### 4.6 Load sequence and compaction

**Load**: read `manifest.xcdn` → stream-parse section files into the in-memory table → replay `journal.xcdn` in order → load `embeddings.bin`, re-embed stale/missing entries → build the vector index → ready.

**Compaction** rewrites the text store to absorb the journal:

1. Acquire the write lock and journal mutex; flush pending access beforehand.
2. Create rollback copies of every affected section and the journal, then publish a transaction marker.
3. Serialize the current table to `<file>.tmp` per section, fsync, and atomically replace every target.
4. Reset the journal and remove the marker to commit. On startup, a remaining marker restores every rollback copy before replay.
5. Purge deprecated records past `purge_after`, update `manifest.last_compaction`, and release the locks.

Triggers: journal exceeds `storage.journal_max_ops` (default 2048), or `asper_flush(full=1)`, or `asper_close`. A crash between any two steps leaves either the old consistent state (journal intact) or the new consistent state.

---

## 5. Retrieval engine

### 5.1 Embeddings

A small dedicated embedding model in GGUF format runs through llama.cpp's embedding API, separate from the curator model. Output is mean-pooled and L2-normalized. The model is configurable; the reference default is multilingual-e5-small (384 dimensions, quantized q8_0) for mixed Italian/English usage (D4); the M7 quality harness confirms or amends it. Prefix strings required by some model families (e5: `"query: "` / `"passage: "`) are configuration values (`embedding.query_prefix`, `embedding.passage_prefix`), empty by default.

Records are embedded at insert/update time on the worker thread. Queries are embedded on the calling thread using a dedicated llama context that shares the model weights with the worker's context (llama.cpp supports multiple contexts per loaded model), so the hot path never blocks behind background embedding.

### 5.2 Vector index

A flat, in-memory index: one contiguous float32[N × dim] matrix plus parallel arrays for id, section, project, and status. Similarity is cosine, computed as a dot product over normalized vectors. Scans filter by section/project/status inline.

Brute force is a deliberate choice: at the design ceiling (100 k records × 384 dims ~ 38 M multiply-adds) a scan is a few milliseconds scalar and far less with the compiler's autovectorization; memory cost is N × dim × 4 bytes (~15 MB at 10 k records). No ANN structure, no external vector library.

Persistence and index are decoupled on purpose: the xCDN files are the source of truth and are never touched by queries, so the human-readable store costs nothing on the hot path. A dedicated vector database would accelerate the cheapest stage of that path — the scan — while the dominant cost is the query embedding itself (§14); and it would pay with a heavy dependency, approximate recall, and an opaque on-disk format that breaks G3. The decision and its revisit trigger are recorded as D8.

### 5.3 Scoring and selection

For sections context and project (identity bypasses retrieval — §3.2):

```
score(m, q) = w_sim * cos(e_m, e_q)
            + w_rel * eff(m, now)  // effective relevance, §3.4
            + w_rec * 0.5 ^ (dt_updated / half_life(section))
```

Defaults: `w_sim = 0.70`, `w_rel = 0.20`, `w_rec = 0.10`. Records with `cos < retrieval.min_similarity` (default 0.25) are excluded regardless of the rest — silence is better than irrelevant memories in a small context window. Top-k defaults: `k_context = 6`, `k_project = 8`. Determinism: ties break by score desc, then `updated_at` desc, then id ascending.

The query text is the current `user_message` verbatim. (Enriching the query with a rolling conversation summary is a candidate v2 refinement, deferred by D7.)

---

## 6. Injection engine

### 6.1 Token budgets

| Key | Default | Scope |
|---|---|---|
| `budgets.identity_tokens` | 400 | whole identity section |
| `budgets.context_tokens` | 600 | retrieved context records |
| `budgets.project_tokens` | 800 | retrieved project records |

Defaults assume a ≥4 k-token host window, leaving ≥55% of it to conversation. Token counting uses the curator model's tokenizer by default (`injection.token_estimator = "curator"`) as a close proxy for the unknown host tokenizer; `"heuristic"` (UTF-8 bytes / 4) is available for hosts that want zero model involvement in the hot path.

Trimming operates on whole records only — a memory is either fully present or absent, never truncated mid-sentence. Order of precedence when trimming:

- **identity**: locked first, then stored relevance desc, then `created_at` asc, then id. Drop from the tail.
- **context / project**: retrieval score order. Drop from the tail.

### 6.2 Prompt template

`asper_build_prompt` returns one string: the host's own base system prompt followed by the rendered memory block. Default template (overridable via `injection.template_path`, plain UTF-8 text with `{placeholder}` substitution):

```
{base_system_prompt}

# Memory

## Who you are
{identity_block}

## What you remember about the user and the environment
{context_block}

## Active project: {project_name}
{project_block}
```

Each record renders as a `"- "` data line. ASCII control characters and Unicode line separators are folded to spaces so content cannot create new prompt structure; the built-in template explicitly labels memories as untrusted data, never instructions. A section that contributes zero records is removed together with its heading; if nothing at all is injected, the base prompt is returned unchanged. Rendering is fully deterministic: identical store + identical inputs ⇒ byte-identical prompt.

---

## 7. Curator engine

### 7.1 Runtime

The curator is a small instruct model (GGUF) executed in-process through llama.cpp with greedy, temperature-0 sampling and a per-cycle GBNF grammar that makes non-conforming output unrepresentable. Defaults: context 4096, `n_threads = min(4, hardware)`. Reference default (D4): Qwen2.5-1.5B-Instruct (Q4_K_M), with Llama-3.2-1B-Instruct as the lighter alternative; the M7 quality harness confirms or amends the choice.

**Model swap.** The curator is stateless with respect to the store: no persistent artifact depends on it (unlike the embedding model, §4.4-§4.5). Replacing it is therefore a pure configuration change — point `curator.model_path` at a different instruct GGUF and reopen the context. Prompt assembly goes through the chat template embedded in the GGUF metadata (`llama_chat_apply_template`), and the output side is the per-cycle GBNF grammar, which is model-independent: any instruct GGUF works with no code change, no store migration, no re-embedding (D9).

### 7.2 Triggers and batching

Turns observed via `asper_observe_turn` accumulate in the event queue (cap `curation.event_queue_max = 256`; overflow drops oldest with a warning). A curation cycle starts when either:

- `curation.turn_batch` unprocessed turns are queued (default 4, i.e. two exchanges), or
- `curation.idle_flush` (default `r"PT30S"`) has elapsed since the last unprocessed turn, or
- the host calls `asper_flush`.

Cycles are serialized on the single worker thread; a trigger during a running cycle queues the next one.

### 7.3 Curator input

The prompt is assembled deterministically from three parts:

1. **Instruction** — a fixed system text (Appendix C), embedded in the library, overridable via `curation.instruction_path`.
2. **Related memories** — for each unprocessed turn, the top 3 most similar existing records; union capped at 12, similarity-ordered. Each is labeled with a short ordinal handle, because small models mangle UUIDs:

   ```
   [M1] (context) The user prefers concise answers, written in Italian.
   [M2] (project:thesis) Chapter 2 deadline is 2026-09-01.
   ```

   The engine keeps the `M<n>` → UUID map for the cycle.
3. **Transcript** — the unprocessed turns as `USER:` / `ASSISTANT:` lines, oldest trimmed to `curation.transcript_tokens` (default 1200).

### 7.4 Operation protocol

The curator answers only in this line protocol (one op per line, `|` separator):

```
INSERT identity | <text>
INSERT context | <text>
INSERT project | <text>          # targets the active project only
UPDATE M<n> | <new text>
DEPRECATE M<n> | <short reason>
KEEP M<n>                        # maintenance review only (§7.6)
NOOP                             # nothing worth remembering
```

A per-cycle GBNF grammar (Appendix A) restricts output to exactly these productions and restricts `M<n>` to the handles actually listed in this cycle's input — an op on a hallucinated handle cannot be generated. `NOOP` must be the only line when present.

### 7.5 Guardrails

Ops that survive the grammar are still validated by the engine; a rejected op is dropped, counted, and logged — never fatal.

- `curation.max_ops_per_cycle` (default 8): excess ops beyond the cap are dropped.
- Target is locked: rejected (`ASPER_ERR_LOCKED` counter). Applies to `UPDATE`, `DEPRECATE`, and `KEEP`.
- **Identity mutation quorum**: `UPDATE` / `DEPRECATE` on an (unlocked) identity record is not applied immediately: it enters a pending set, and is executed only if the curator proposes the same op for the same record again in a later cycle within `curation.identity_confirm_window` (default `r"P7D"`). Unconfirmed pendings expire silently. `INSERT identity` is immediate but subject to the record cap.
- `identity_max_records` (default 64): `INSERT identity` beyond the cap is rejected.
- **Deduplication**: before applying `INSERT`, the engine embeds the text and compares it with active records in the same scope; if `cos >= curation.dup_threshold` (default 0.92) the insert is converted into an access boost on the existing record.
- `content_max_chars` (default 500): longer `INSERT`/`UPDATE` text is rejected by Unicode scalar-value count, not UTF-8 byte count (the instruction states the cap; rejection keeps memories atomic).
- `INSERT project` with no active project: rejected.
- Empty / whitespace-only text: rejected.

### 7.6 Maintenance review

At most once per `curation.maintenance_interval` (default `r"P1D"`) per opened store — and always during `asper_flush(full=1)` — the worker collects up to `curation.review_batch` (default 16) deprecation candidates (§3.4), lowest effective relevance first, and runs a dedicated curator cycle whose grammar admits only `DEPRECATE`, `KEEP`, and `NOOP` over the listed handles. `KEEP` refreshes `last_access` and adds the standard boost, pulling the record out of candidacy. Candidates left unmentioned stay active until a later review.

### 7.7 Recall queries

Injection (§6) is the push channel; recall is the pull channel: the host model — or any MCP client — can interrogate the curator about the memory on demand, with a natural-language question.

`asper_recall` embeds the question, collects the top `recall.k` candidates (default 12) across identity, context, and the active project — same scoring as §5.3, no per-section split — and runs a dedicated curator cycle whose grammar admits only:

```
ANSWER | <concise answer, one or two sentences>
CITE M<n>                   # zero or more supporting memories
NOMEM                       # nothing relevant in memory
```

The engine returns the answer plus the cited records; cited records receive the standard access boost (§3.4), so recall reinforces what proves useful. `NOMEM` maps to `ASPER_ERR_NOT_FOUND` with an empty answer. Generation is capped at `recall.answer_tokens` (default 160).

**Scheduling**: recall requests are queued to the worker with priority over pending curation cycles (a running cycle is never interrupted). The caller blocks up to `recall.timeout` (default `r"PT10S"`); on timeout the call fails with `ASPER_ERR_BUSY` and the request is dropped. Expected latency is one curator generation (§14).

**Wiring**: hosts whose model supports tool calling should expose recall as a tool (suggested name `memory_recall`); MCP clients get it natively (§10). Models that cannot call tools simply don't wire it — recall is strictly optional, and injection remains the baseline channel.

---

## 8. Concurrency model

- libasper creates one worker thread per open context (curation, background embedding, maintenance, compaction). The host may call the API from any number of threads.
- One readers-writer lock guards the in-memory table + vector index. Read-only calls (`asper_build_prompt`, `asper_memory_search`, `asper_memory_list`, `asper_get_stats`) take the read lock; every mutation — curator op, API op, MCP op — funnels through a single internal `apply()` that prepares passage embeddings before taking the write lock, then holds the journal mutex only for the append.
- Query embedding runs on the caller's thread with its own llama context (§5.1), so `asper_build_prompt` does not contend with the worker for the embedding model.
- Threading shim: pthreads on POSIX, Win32 threads/SRW locks on Windows; C99 sources, no C11 `<threads.h>` dependency.
- `ASPER_NO_THREADS` compile option removes the worker for embedded/single-threaded hosts: background work runs inside `asper_tick()`, which the host calls periodically (and which is a no-op in threaded builds).
- Recall (§7.7) shares the curator context and cycle slot with the worker: generation runs on the caller with a deadline that covers both slot waiting and token generation. In `ASPER_NO_THREADS` builds it remains synchronous, with the same generation deadline.
- Undefined behavior after `fork()` with an open context (llama.cpp state); documented.

---

## 9. Public C API

See `include/asper.h` — the header is the authoritative, complete version of the spec's §9 sketch (same names, same semantics, plus record accessors and `asper_strings_free`/`asper_project_active` helpers).

---

## 10. MCP server (asper-mcp)

A thin executable: `asper-mcp --root <dir> [--config <file>]`. Transport is stdio; protocol is JSON-RPC 2.0 implementing the MCP lifecycle (`initialize`, `tools/list`, `tools/call`) against the MCP specification revision pinned at implementation time.

Tools:

| Tool | Parameters | Returns |
|---|---|---|
| `memory_search` | query, section?, project?, k? | ranked matches: id, section, project, content, score |
| `memory_recall` | question | curator answer + cited ids (§7.7) |
| `memory_insert` | section, content, project?, locked? | insert source "manual"; → id |
| `memory_update` | id, content | rewrite content |
| `memory_deprecate` | id, reason? | soft-deprecate |
| `memory_list` | section?, project?, include_deprecated? | full listing |
| `project_select` | slug (nullable) | switch project |
| `project_list` | — | known slugs |
| `observe_turn` | role, text | feed curation |
| `context_build` | user_message, base_system_prompt? | assembled prompt |
| `memory_stats` | — | counters |

Design constraints:

- JSON exists only here. It is handled by an in-house strict RFC 8259 parser/writer (target ~600 LOC, UTF-8 only, depth-capped) in line with the minimal-dependency policy; the rest of the system speaks xCDN.
- Errors map to JSON-RPC error objects carrying the `asper_err` name in `data`.
- Trust model: the MCP client is a local process spawned by the user; no network listener exists. All guardrails (locked, identity quorum, caps) apply identically to MCP calls.

---

## 11. Configuration

All configuration lives in one xCDN file passed to `asper_open` (or `asper-mcp --config`). Precedence: built-in defaults ← `config.xcdn`. Every key is optional; the file below shows the complete key set with the default values used when absent.

```
#asper_config {
  storage: {
    journal_sync: "batch",  // "batch" | "always" | "never"     (§4.3)
    journal_max_ops: 2048,  // compaction trigger               (§4.6)
    audit_log: false,  // mirror ops to audit.xcdn              (§15)
  },
  logging: {
    path: null,  // file sink; null = disabled                  (§15)
    level: "info",  // "error"|"warn"|"info"|"debug"
    max_size_kb: 5120,  // rotate above this size
    max_files: 3,  // asper.log, .1, .2, ...
    sync: false,  // fsync after each line
  },
  curator: {
    model_path: "models/qwen2.5-1.5b-instruct-q4_k_m.gguf",
    ctx: 4096,
    threads: 4,
    instruction_path: null,  // override Appendix C
  },
  embedding: {
    model_path: "models/multilingual-e5-small-q8_0.gguf",
    dim: 384,
    query_prefix: "",  // e.g. "query: " for e5 models          (§5.1)
    passage_prefix: "",  // e.g. "passage: "
  },
  budgets: {
    identity_tokens: 400,
    context_tokens: 600,
    project_tokens: 800,
  },
  injection: {
    token_estimator: "curator",  // "curator" | "heuristic"     (§6.1)
    template_path: null,  // override the default template      (§6.2)
  },
  retrieval: {
    k_context: 6,
    k_project: 8,
    min_similarity: 0.25,
    w_similarity: 0.70,
    w_relevance: 0.20,
    w_recency: 0.10,
  },
  curation: {
    turn_batch: 4,
    idle_flush: r"PT30S",
    max_ops_per_cycle: 8,
    dup_threshold: 0.92,
    transcript_tokens: 1200,
    event_queue_max: 256,
    identity_confirm_window: r"P7D",
    maintenance_interval: r"P1D",
    review_batch: 16,
  },
  recall: {
    k: 12,  // candidates shown to the curator
    answer_tokens: 160,  // generation cap
    timeout: r"PT10S",
  },
  decay: {
    context_half_life: r"P30D",
    project_half_life: r"P90D",
    prune_threshold: 0.15,
    purge_after: r"P60D",
  },
  limits: {
    identity_max_records: 64,
    content_max_chars: 500,
  },
  projects: {
    autocreate: true,  // create on first select (§3.3, D1)
  },
}
```

---

## 12. Dependencies and build

Project rule: as few libraries as possible; external code only for storage-class components (e.g. SQLite) and very big modules that are impractical to replicate.

- **llama.cpp** (pinned git submodule): curator inference, embeddings, tokenizer, GBNF-constrained sampling. C++ internally, consumed strictly through its C API (`llama.h`); no C++ type or header leaks into Asper sources. Building the dependency requires a C++17 toolchain; libasper itself stays C99.
- **xCDN-C** (pinned git submodule): parsing and serialization of all persistent text. Official C implementation of the chosen notation. Coverage verification opened milestone M1 (D3) — verified complete: streaming, all typed literals, tags, triple-quoted strings, pretty/compact serialization.
- **OS threads** (pthreads / Win32): platform facility, wrapped in a small shim.
- **C standard library**: everything else.

Explicitly not used: SQLite, libcurl or any networking, third-party JSON libraries (the MCP-only JSON codec is in-house, §10), any C++ in Asper code.

Repository layout:

```
asper/
├── include/asper.h          # the public API (§9)
├── src/                     # modules; see src/asper_internal.h header
├── mcp/                     # asper-mcp entry point + JSON-RPC loop + json codec
├── deps/                    # llama.cpp, xcdn-c (pinned submodules)
├── tests/                   # unit/, integration/, golden/, quality/
├── docs/                    # this spec, curator instruction, template
├── LICENSE                  # MIT (D2)
└── CMakeLists.txt
```

Build: CMake ≥3.16. Options: `ASPER_BUILD_MCP` (ON), `ASPER_BUILD_TESTS` (ON), `ASPER_NO_THREADS` (OFF), `ASPER_SANITIZERS` (OFF), `ASPER_WITH_LLAMA` (ON). Asper translation units compile with `-std=c99 -Wall -Wextra -Wpedantic -Werror` (MSVC: `/W4 /WX`); dependencies build with their own settings. Artifacts: `libasper.a`, `libasper.so`/`.dylib`/`.dll`, `asper-mcp`.

---

## 13. Platform support

- **Linux** x86_64 / aarch64: gcc ≥9, clang ≥11. Primary development target.
- **macOS** 12+ (arm64, x86_64): Apple clang. Metal off by default for llama.cpp; CPU-only baseline.
- **Windows** 10+ x86_64: MSVC 2019+, clang-cl. Paths handled as UTF-8 internally, converted to UTF-16 at the API boundary; atomic replace via `ReplaceFileW`.

Cross-platform rules: files are written with LF endings (parsers accept CRLF); all on-disk names are ASCII lowercase (project slugs enforce this), avoiding case-sensitivity mismatches; atomic file replacement is the only rename primitive used.

---

## 14. Performance and resource targets

Non-binding targets that guide implementation choices. Baseline hardware: 4-core x86_64 laptop, no GPU. Baseline store: 10 k records, dim 384. Curator: 1.5 B Q4.

| Target | Value |
|---|---|
| `asper_open`, warm embedding cache (excl. model weight load) | ≤1.5 s |
| `asper_build_prompt` end-to-end | ≤60 ms (query embedding dominates at 30-50 ms; scan + render ≤5 ms) |
| Pure index scan, 10 k × 384 | ≤2 ms |
| Curation cycle, 4 turns | ≤15 s wall — asynchronous, never blocks the host |
| `asper_recall` end-to-end | ≤3 s — one curator generation over ≤12 candidates |
| RAM | index ~15 MB + models (~1.2 GB curator Q4 + ~130 MB embedder) |
| Disk | text store ~1-5 MB; embedding cache ~15 MB |

---

## 15. Errors, logging, observability

Error handling is return-code based (`asper_err`, §9) with a per-context UTF-8 message from `asper_last_error`. The library never aborts and never writes to stdout or stderr on its own initiative.

File logging is built in — in-house code, no dependency, off by default. With `logging.path` set, Asper appends single-line UTF-8 records:

```
2026-07-29T10:12:00Z INFO  curator  cycle #42: applied=3 rejected=1 (2.4s)
2026-07-29T10:12:31Z WARN  store    journal torn tail: 1 value discarded
2026-07-29T10:13:02Z DEBUG retrieve query k=6 best=0.81 floor=0.25
```

Each record carries an RFC 3339 UTC timestamp, a level, a subsystem tag (store, index, embed, curator, inject, recall, mcp) and the message. Levels: **error** — an operation failed; **warn** — anomaly recovered automatically (torn tail, cache rebuild, dropped op); **info** — lifecycle events: model loads, curation cycles with counts and timing, every curator op applied or rejected with its reason, compactions, recall requests; **debug** — per-query detail (scores, budgets, trimming decisions). The active threshold is `logging.level` (default `info`). Rotation is size-based: when the file exceeds `logging.max_size_kb` it rotates to `.1`, `.2`, ... keeping `logging.max_files`; `logging.sync` forces an fsync per line (default off). A logging failure never fails the operation that emitted the record.

The host callback (`asper_set_logger`) is independent of the file sink and receives every record above its own threshold; it may be invoked concurrently from host and worker threads and may re-enter libasper, so the host synchronizes mutable callback userdata. The default callback writes to stderr at `warn`. The journal remains the readable audit trail of applied ops until compaction, and `storage.audit_log: true` additionally mirrors every op — including rejected ones — to `audit.xcdn`, which is never compacted.

`asper_get_stats` exposes counters: records per section, deprecated count, journal ops, cycles run, ops applied and rejected, recall requests served, timestamps of the last cycle and the last compaction.

---

## 16. Testing strategy

- **Unit tests** (CTest + an in-house ~100-LOC assert harness; no framework dependency): decay and scoring math, budget trimming, journal replay including the torn-tail rule, dedup conversion, every guardrail in §7.5, the JSON codec, xCDN round-trips against the official grammar/examples.
- **Injected clock**: all time-dependent logic (decay, windows, intervals) reads an internal clock abstraction, so tests time-travel deterministically.
- **Deterministic integration**: the curator sits behind an internal vtable (`asper_curator_iface`); tests inject a scripted fake curator and drive full cycles — queue, ops, journal, index — without any model weights. (Side benefit: a future external-endpoint curator backend can be added behind the same interface without touching the core.)
- **Golden files**: fixed store + fixed inputs ⇒ byte-identical assembled prompts, committed and diffed.
- **Recall and logging**: golden recall tests through the scripted curator (ANSWER/CITE/NOMEM and the timeout path); log-rotation unit tests with injected clock and size caps.
- **Fuzzing** (optional CI stage, clang libFuzzer): journal/xCDN stream reader, JSON codec.
- **Quality harness** (not CI-gating, `tests/quality/`): scenario transcripts with expected memory diffs, run against real candidate curator models to confirm the default model choice (D4).
- **CI**: GitHub Actions matrix — linux-gcc, linux-clang, macos-clang, windows-msvc; sanitizer job on Linux.

---

## 17. Milestones

| Milestone | Scope | Acceptance |
|---|---|---|
| **M1 Store** | xCDN-C coverage verification (D3); record model, section files, journal, manifest, load/replay, compaction, torn-tail, file logging. | Unit tests green on all platforms. |
| **M2 Retrieval** | Embedding engine, cache, flat index, scoring. | Golden retrieval tests. |
| **M3 Injection** | Budgets, template, `asper_build_prompt`. | Byte-identical golden prompts. |
| **M4 Curator** | Protocol, per-cycle GBNF, guardrails, recall channel, fake-curator integration. | Scripted cycles fully deterministic. |
| **M5 Lifecycle** | Decay, access batching, maintenance review, purge. | Injected-clock time-travel tests. |
| **M6 MCP** | `asper-mcp`, all tools, in-house JSON. | Smoke conformance against a reference MCP client. |
| **M7 Hardening** | Fuzzing, sanitizers, docs, quality-harness baseline for model selection. | Full CI matrix green. |

---

## 18. Design decisions

| # | Decision |
|---|---|
| **D1** | `asper_project_select` on an unknown slug creates the project, gated by `projects.autocreate` (default true); with autocreation disabled it fails with `ASPER_ERR_NOT_FOUND`. |
| **D2** | License: MIT, matching xCDN. |
| **D3** | xCDN-C is the parser and serializer for all persistent text. Coverage verified at M1 open: complete (streaming, typed literals, tags, triple-quoted strings, pretty/compact serialization). |
| **D4** | Reference default models — curator: Qwen2.5-1.5B-Instruct Q4_K_M (lighter alternative Llama-3.2-1B-Instruct); embeddings: multilingual-e5-small q8_0, dim 384. The M7 quality harness confirms or amends the defaults before the v1 release. |
| **D5** | Identity mutation control as specified in §7.5: re-proposal quorum within `curation.identity_confirm_window` (default P7D) for destructive ops on unlocked identity records, plus `locked: true` for anything critical. No interactive user confirmation in v1. |
| **D6** | Token estimation uses the curator tokenizer as a proxy for the host tokenizer; a host-tokenizer callback is reserved for v2. |
| **D7** | Deferred to v2 behind unchanged interfaces: retrieval-query enrichment with a rolling conversation summary, int8-quantized embeddings, ANN index beyond the 100 k-record ceiling, and the host-tokenizer callback (D6). |
| **D8** | Retrieval stays on the in-RAM flat index over the xCDN store. Within the ≤100 k envelope the scan is single-digit milliseconds and the hot path is dominated by query embedding; revisit only beyond ~100 k records, behind the unchanged retrieval interface. |
| **D9** | The curator is hot-swappable by configuration alone: prompts go through the GGUF-embedded chat template (`llama_chat_apply_template`), the GBNF grammar is model-independent, and no persistent artifact depends on the curator — any instruct GGUF, no code change, no store migration. |
| **D10** | Built-in rotating file logging (§15), in-house, off by default (`logging.path: null`); the host log callback remains available and independent. |
| **D11** | Recall channel (§7.7): on-demand, grammar-constrained curator answers over retrieved candidates, exposed as `asper_recall` and the `memory_recall` MCP tool; cited memories receive the access boost. |

---

## Appendix A — Curator output grammar (illustrative)

The implementation emits real llama.cpp GBNF, regenerated each cycle so that the handle alternatives are exactly the handles listed in that cycle's input; maintenance-review cycles emit a reduced grammar admitting only `DEPRECATE`, `KEEP`, `NOOP`.

```
root      ::= noop | opline+
noop      ::= "NOOP" "\n"
opline    ::= (insert | update | deprecate | keep) "\n"
insert    ::= "INSERT " ("identity" | "context" | "project") " | " text
update    ::= "UPDATE " handle " | " text
deprecate ::= "DEPRECATE " handle " | " text
keep      ::= "KEEP " handle
handle    ::= "M1" | "M2" | ...          # concrete per-cycle alternatives
text      ::= one line, no "|" or newline, 1-500 chars
```

## Appendix B — Default injection template

See §6.2. Shipped as an embedded string (`src/inject.c`); a human-readable copy lives in `docs/injection_template.txt`; overridable via `injection.template_path`.

## Appendix C — Default curator instruction

Shipped as an embedded string (`src/grammar.c`); a human-readable copy lives in `docs/curator_instruction.txt`; overridable via `curation.instruction_path`.

```
You maintain the long-term memory of an AI assistant. You will see
the recent conversation and a list of existing memories labeled [M1]...[Mn].

Decide what deserves to be remembered.
Answer ONLY with operations, one per line:

INSERT identity | <fact>    the assistant's persona, values, tone, style
INSERT context | <fact>     the user, their preferences, their environment
INSERT project | <fact>     the currently active project
UPDATE M<n> | <fact>        replace an existing memory with a better version
DEPRECATE M<n> | <reason>   the memory is wrong or obsolete
NOOP                        nothing worth remembering

Rules:
- A fact is one self-contained sentence (max 500 characters), understandable
  without the conversation.
- Prefer UPDATE over INSERT when an existing memory covers the same topic.
- Store durable facts, not chit-chat. Never store secrets or one-off trivia.
- When in doubt: NOOP.
```
