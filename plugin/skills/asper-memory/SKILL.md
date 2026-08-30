---
name: asper-memory
description: Work with the Asper persistent memory (asper MCP server) — recall what is known about the user, the environment, and past projects; keep the memory current as the conversation unfolds. Use at the start of a conversation, whenever the user references past interactions or preferences, when switching projects, and when the user asks you to remember or forget something.
---

# Asper memory

Asper is a local, offline, long-term memory attached to this agent through the
`asper` MCP server. Records live in three sections: **identity** (who the agent
is: persona, values, tone — always injected, never decays), **context** (facts
about the user and the environment — decays with a 30-day half-life), and
**project** (knowledge scoped to one named project — decays with a 90-day
half-life). A small background *curator* model watches observed conversation
turns and autonomously writes, merges, and prunes records; the store is plain
text the user can inspect and hand-edit.

## The memory loop

Follow this discipline in every conversation:

1. **Ground first.** Early in a conversation — and again on a clear topic
   shift — call `context_materialize` with a stable conversation `scope`, the
   current message as `query`, and explicit `history_tokens` and
   `checkpoint_tokens` budgets sized to the consuming model. Read both the
   returned semantic system prompt and exact source context.
2. **Append source continuously.** After each meaningful exchange, append both
   sides with `source_append`, using the same stable `scope`, `kind: "user"`
   for the user's message and `kind: "assistant"` for the reply. Source events
   are immutable and curation is asynchronous. Prefer this over manual record
   insertion; let the curator decide what becomes semantic memory.
3. **Recall on demand.** When the user references something from the past
   ("as we discussed", "my usual setup", "where were we on X"):
   - `memory_recall` (`question`) asks the curator and returns an answer with
     cited records — best for natural-language questions.
   - `memory_search` (`query`, optional `section`, `project`, `k`) returns
     ranked matches — best for targeted lookups.
   - `memory_list` (optional `section`, `project`, `include_deprecated`)
     browses everything — best for audits and "what do you know about me?".

## Projects

Project memory only works while a project is active. `project_list` shows
known slugs; `project_select` with a `slug` (lowercase `[a-z0-9-]`, e.g.
`asterism-asper`) activates one, creating it if new; `slug: null` deselects.
Select the matching project when the user starts working on something named,
and keep durable, project-scoped knowledge there rather than in context.

## Writing and editing memory directly

Use direct writes when the user explicitly asks to remember, correct, or
forget something — otherwise trust the curator.

- `memory_insert` (`section`, `content`, `project` when section is
  `"project"`, optional `locked`): content must be a **self-contained
  statement** understandable without this conversation, max ~500 characters.
  Set `locked: true` only for facts the curator must never alter (core
  persona, hard user requirements). Identity is capped (64 records) — keep it
  for durable persona traits, not trivia.
- `memory_update` (`id`, `content`): rewrite one record in place.
- `memory_deprecate` (`id`, optional `reason`): soft-delete — the record
  leaves retrieval but stays on disk for a while and is recoverable. There is
  no hard delete; tell the user deletion is soft until compaction purges it.
- `memory_stats`: store and curation counters — use it to answer "how big is
  your memory?" or to sanity-check that curation is running.

## Degraded mode

If the local GGUF models (curator + embeddings) are not installed, Asper runs
degraded: identity injection, insert, list, update, and deprecate still work,
but semantic search, recall, retrieval-based injection, and curation are
disabled. If `memory_search`/`memory_recall` error or context materialization
returns only identity, say so plainly and tell the user to install the two model files
into the `models/` directory of Asper's data directory (see the plugin README)
— do not silently pretend the memory is empty.
