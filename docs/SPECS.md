# Asper — Architecture and Design

## 1. Why this project exists

A model context is temporary. Identity, user knowledge, project facts, tool
evidence and unfinished work must survive after that context is full or the
process restarts. Replaying the complete history is lossless but becomes slower
and more expensive at every turn; keeping only a summary is cheap but eventually
loses evidence and accumulates errors.

Asper resolves this tension by separating permanent source memory from bounded
model context:

> Exact events are the source of truth. Semantic memory, checkpoints and prompt
> context are compact views that can be rebuilt without deleting the source.

The result is durable memory designed for small local models: limited context is
used for the information most likely to matter, while every omitted detail
remains recoverable.

## 2. Place in Asterism

The four projects divide responsibilities as follows:

- **asmodel** runs and shares model providers.
- **Asper** owns all durable memory and context compaction.
- **astools** describes and executes tools safely.
- **asngn** orchestrates turns using the other three components.

This ownership boundary is deliberate. asngn may request a context or commit a
checkpoint, but it does not maintain a second transcript summary or a competing
memory store. Tool payloads that are too large for a prompt also become Asper
objects rather than private engine blobs.

## 3. The memory model

Asper maintains three complementary forms of information.

### 3.1 Exact scoped events

Every relevant occurrence is appended to a scope-specific event log. Event
kinds cover user and assistant messages, decisions, tool calls and results,
diagnostics, checkpoints and artifacts.

Events are immutable and ordered. Appending is synchronous and durable: success
means the exact UTF-8 payload is stored before the call returns. A curator model
is never on this critical path.

A scope is a host-defined stable name, usually an asngn session. Events can be
pinned so context selection retains particularly important source material.

### 3.2 Semantic records

The curator converts batches of exact events into compact records organized in
three sections:

- **Identity:** stable facts about the assistant and its operating principles.
- **Context:** facts about the user, environment and continuing relationship.
- **Project:** facts local to a named body of work.

Records support relevance, tags, locking, deprecation and supersession. Every
curator-created record carries the UUIDs of the exact events that substantiate
it. Semantic memory is useful because it is compact and searchable; it is not
authoritative.

### 3.3 Working checkpoints

A checkpoint is the current structured state of unfinished work in one scope.
Replacing it is atomic, and every committed checkpoint is also appended as an
immutable event. A long generation can therefore resume from a concise working
state without pretending that state is the whole history.

### 3.4 Content-addressed objects

Large exact payloads are stored by SHA-256 reference. Writes are atomic and
deduplicated. Range reads allow a caller to reopen only the needed slice of a
large tool result or draft while retaining the complete object.

## 4. Data flow

The normal flow is linear:

1. The host appends exact events to a scope.
2. Appending queues those event UUIDs for background curation.
3. The curator derives or updates semantic records with source provenance.
4. The successful curation cycle acknowledges the processed UUIDs.
5. Before a model call, the host asks Asper to materialize a bounded context.
6. Asper combines semantic memory, the current checkpoint and selected exact
   events within the supplied token budgets.
7. The model receives that temporary view; the source store remains unchanged.

If curation fails or the process stops, unacknowledged source events are replayed
after restart. No inference failure can erase the original information.

## 5. Storage architecture

The store is local, inspectable xCDN plus exact binary objects. Each scope owns a
length-framed append-only event log and its pin state. Length framing makes torn
tail writes detectable and repairable without discarding earlier events.

Semantic section files, indexes, embedding data and curator acknowledgements are
derived operational state. They may be compacted or rebuilt from durable source
events. Checkpoints are replaced atomically. Content-addressed objects avoid
writing duplicate large payloads.

This layout favors debuggability over an opaque database: operators can inspect
the durable representation, while atomic replacement and append-only logs retain
clear crash semantics.

## 6. Context materialization

`asper_context_materialize` builds one bounded view for one consuming model. The
request provides:

- the scope and current query;
- the base system prompt;
- a budget for exact history;
- a separate checkpoint budget;
- preferably, the consuming model's tokenizer callback.

The output has two zones:

- a system prompt enriched with relevant identity, context and project records;
- context text containing the checkpoint and selected pinned or recent events.

Selection is deterministic. Whole records and whole events are admitted when
they fit; arbitrary text truncation is avoided because a broken half-fact is
often worse than an omitted fact. When no tokenizer is supplied, Asper uses a
deterministic conservative heuristic.

Materialization never mutates or deletes source data. A smaller context is only
a smaller view.

## 7. Curation and retrieval

The curator is a small, separate model whose narrow job is to reorganize memory.
It does not answer the user's main request. Working in batches keeps curation off
the interactive path and amortizes prompt overhead across multiple events.

The curator may insert, update, supersede or deprecate semantic records, subject
to validation. Locked records remain under operator control. Failed or malformed
operations are rejected without acknowledging their source events.

Retrieval combines semantic similarity with section, project, recency, stored
relevance and access signals. Identity is always eligible; project knowledge is
selected only for the relevant active project. Direct recall can synthesize a
focused answer with citations to retrieved records, while ordinary context
materialization injects the records themselves.

## 8. Performance strategy

Asper keeps the synchronous path intentionally small:

- event appends are sequential framed writes;
- curation and maintenance run on a worker thread;
- events are curated in batches rather than one model call per message;
- embeddings and indexes accelerate semantic retrieval;
- content-addressed objects deduplicate large payloads;
- range reads avoid loading an entire object for a small excerpt;
- the scope index and sorted curator acknowledgements make replay lookup cheap;
- access updates are batched before persistence;
- compaction rewrites derived state atomically instead of blocking every read.

When embedded in asngn, curator and embedding models borrow its process-wide
asmodel manager. Weights, contexts and residency budgets are not duplicated.
Standalone Asper can own a manager and retain the same behavior.

## 9. Token economy without information loss

Asper reduces model input, not stored knowledge.

- Semantic records turn repeated conversations into concise reusable facts.
- Query-directed retrieval injects relevant memory instead of the entire store.
- Checkpoints capture current working state without replaying every step that
  produced it.
- Pinned and recent exact events preserve high-value verbatim evidence.
- Separate zone budgets stop one kind of memory from consuming all context.
- Exact token counting budgets against the model that will actually consume the
  prompt.
- Large payloads remain as objects and only useful ranges enter the prompt.
- Source provenance permits compact records to be verified or rebuilt.

This is not lossy deletion disguised as compression. The compact view may omit
information for a particular call, but exact events and objects remain available
for recall, reopening or future rematerialization.

## 10. How this helps small language models

Small models have less contextual capacity and are more easily distracted by
irrelevant history. Asper increases their effective capability in four ways:

1. **Continuity:** identity and project facts survive beyond a context window.
2. **Focus:** retrieval places a small set of relevant facts near the task.
3. **Working memory:** checkpoints let long tasks continue after compaction or a
   process restart.
4. **Grounding:** exact events, object references and source UUIDs preserve the
   evidence behind compact memory.

The model is not asked to remember everything, search everything and solve the
task simultaneously. Those responsibilities are externalized into a durable,
deterministic subsystem.

## 11. Reliability and concurrency

Public operations on an open context are thread-safe; opening and closing the
same context remain lifecycle operations that the host must serialize. The
source store is protected so append, checkpoint and object operations cannot
observe partial shared state.

Crash recovery follows the authority hierarchy:

1. repair only a torn event-log tail;
2. retain all complete exact events and objects;
3. replay events not acknowledged by a successful curator cycle;
4. rebuild semantic derivatives when necessary.

If curator or embedding models are unavailable, exact storage continues to
work. Retrieval and curation may degrade, but durable capture does not depend on
inference availability.

## 12. Model execution

Curator and embedding roles use asmodel. They can run through embedded
llama.cpp or explicit remote profiles for llama.cpp server, LM Studio, vLLM and
generic endpoints. Provider details stay below the Asper memory API.

Model-controlled calls in the embedded llama.cpp adapter are contained at its
C++ boundary so exceptions become normal Asper errors rather than terminating
the host process.

## 13. Fundamental invariants

The implementation must preserve these rules:

1. Exact scoped events are the authoritative history.
2. Durable append success never depends on curator success.
3. Semantic records are bounded derivatives with source provenance.
4. All unfinished curation is replayable after restart.
5. Context materialization never deletes or rewrites source information.
6. Checkpoint replacement is atomic and also recorded as an event.
7. Objects are addressed by content and support lossless range reads.
8. Durable memory and compaction belong to Asper, not to its hosts.
9. Token budgets select information; they do not redefine truth.

## 14. Public surfaces

`include/asper.h` is the authoritative C99 API. Its main groups are exact event
and object storage, checkpoint lifecycle, context materialization, semantic
record access, search and recall, project selection, maintenance and statistics.

`asper-mcp` exposes the same concepts to external clients: direct memory access,
source append, context materialization, recall, project selection and stats.

Configuration examples, build steps and the complete MCP tool list belong in
the README and `examples/config.xcdn`. This document defines the architecture
and its invariants, not a second configuration reference.
