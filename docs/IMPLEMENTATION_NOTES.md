# Asper — implementation notes (binding for all modules)

These decisions complement docs/SPEC.txt and src/asper_internal.h. When this
file, the spec and the header disagree, the header wins, then this file, then
the spec.

## General rules

- Strict C99, `-Wall -Wextra -Wpedantic -Werror`. No C++ anywhere in Asper
  sources. No `//`-style problems: `//` comments are fine in C99.
- libasper never writes to stdout/stderr on its own initiative; the only
  exception is the *default* log callback (log.c) which writes WARN+ to
  stderr until the host replaces it.
- Allocation failures return `ASPER_ERR_NOMEM`; never abort. Check every
  malloc.
- All timestamps are `asper_time` (unix seconds UTC) obtained from
  `asper_clock_now(&c->clock)` — never call `time(NULL)`/`os_now_unix`
  directly outside time.c/os code (tests inject the clock).
- Only config.c, manifest.c, store.c and journal.c may include `xcdn.h`.
- `asper_seterr(c, e, fmt, ...)` sets the per-context message and returns
  `e`; use it for every failure path that has a ctx.
- File writes that replace existing files go through: write `<file>.tmp`,
  `os_fsync`, `os_file_replace(tmp, target)`.

## Journal format (journal.c)

- The journal is written as ONE COMPACT xCDN `#op` VALUE PER LINE
  (`xcdn_to_string_compact` emits no raw newlines; strings are escaped —
  verified in deps/xcdn-c/src/ser.c). Each append writes `<compact-op>\n`.
- Torn-tail rule implementation: read the whole file; parse it. On parse
  failure, retry parsing the buffer truncated at the last `\n` that is
  followed by a non-empty remainder (i.e. drop the final line); if the
  remainder now parses, truncate the file to that offset (os_truncate) and
  log WARN. If dropping ONE final line is not enough, the error is mid-file:
  return `ASPER_ERR_PARSE` and leave the file untouched.
- `asper_journal_append` serializes, appends to `store.journal_fp`, flushes
  (`fflush`), and fsyncs per policy: `always` => every op; `batch` => when
  `force_sync` (callers pass force_sync=true after each curation batch and
  after every manual op); `never` => nothing. Mirrors the same line to
  audit.xcdn when `cfg.audit_log` (audit failures only WARN).
- Section files (compaction) are written PRETTY, one `#memory` value after
  another separated by a blank line, via `xcdn_to_string_pretty` per value
  (serialize a single-value document per record and concatenate).

## Op application semantics (store.c `asper_store_apply`)

- INSERT: record added as-is (journal carries the full record; `emb_row`
  initialized to -1). Duplicate id => ASPER_ERR_INVALID.
- UPDATE: replace `content`, set `updated_at = op->at`. Target missing =>
  ASPER_ERR_NOT_FOUND. Target deprecated => ASPER_ERR_INVALID.
- DEPRECATE: `deprecated = true`, `deprecated_at = op->at`. Missing =>
  NOT_FOUND; already deprecated => ASPER_OK (idempotent).
- KEEP: if deprecated, un-deprecate (restore). Always: `last_access =
  op->at`, `relevance = min(1.0, relevance + 0.05)`.
- SET_LOCKED: set flag. Missing => NOT_FOUND.
- ACCESS: for each id present: `access_count++`, `last_access = op->at`,
  `relevance = min(1.0, relevance + 0.05)`. Unknown ids are skipped
  silently (records may have been purged).
- PROJECT_CREATE: register slug (idempotent).
- During journal REPLAY, application errors are logged WARN and skipped
  (the journal may reference records purged by later compactions only in
  pathological hand-edit cases); replay never fails on apply errors, only
  on parse errors.

## Guardrail split

`asper_apply_op` (api.c) enforces UNIVERSAL invariants for every source
(API, MCP, curator), rejecting with the listed error:

- UPDATE/DEPRECATE on a locked record => ASPER_ERR_LOCKED. (SET_LOCKED
  itself is always allowed; KEEP on locked is allowed only as rescue of a
  candidate — curator KEEP on locked is rejected upstream in curator.c.)
- INSERT/UPDATE with empty or whitespace-only content => ASPER_ERR_INVALID.
- INSERT/UPDATE content longer than `cfg.content_max_chars` (bytes) =>
  ASPER_ERR_INVALID.
- INSERT identity when active identity count >= `cfg.identity_max_records`
  => ASPER_ERR_INVALID.
- INSERT project: `record->project` must be non-NULL, slug-valid and a
  known project => ASPER_ERR_INVALID / ASPER_ERR_NOT_FOUND.
- Non-project INSERT with non-NULL project => ASPER_ERR_INVALID.

After store-apply succeeds, the funnel journal-appends, then maintains the
index: INSERT/UPDATE embed content (passage) when `has_embedder` and
`asper_index_put`; DEPRECATE => `asper_index_remove`; KEEP (restore) =>
re-embed and put back. Then updates stats (`ops_applied`), sets
`cache_dirty`. Journal append happens BEFORE the in-memory apply (§4.3:
"appended here first"); if the store-apply then fails, the op is
journal-present but skipped in memory — same as the replay-tolerance rule.
Order per §8 write path: take write lock; validate; append (journal_mu);
apply; index; unlock.

CURATOR-ONLY guardrails (curator.c, §7.5), applied before calling
`asper_apply_op(..., from_curator=true)`:

- `max_ops_per_cycle` cap (excess dropped).
- Handle out of range (grammar should prevent it) => drop.
- UPDATE/DEPRECATE/KEEP on locked target => drop + count.
- Identity quorum: UPDATE/DEPRECATE on unlocked identity records go to the
  pending set (`asper_pending`, matching on id+kind+content_hash of the
  proposed text); applied only when re-proposed within
  `identity_confirm_window`; expired pendings pruned lazily.
- Dedup for INSERT (any section): embed proposed text (passage); against
  ACTIVE records in the same scope (same section; for project, same
  project) with an embedding row, if max cos >= `dup_threshold`, convert to
  access boost on the winner (asper_access_note + immediate ACCESS op at
  cycle end) and do not insert.
- INSERT project while no active project => drop. The inserted record's
  `project` field is set to the ACTIVE project at op-build time.
- Manual (API/MCP) ops NEVER go through quorum or dedup: quorum is defined
  over curator cycles (§7.5/D5); "identically applied guardrails" (§10)
  refers to locked/caps/content rules, which live in the funnel.

## Curation cycle (curator.c)

1. Snapshot up to `turn_batch` oldest unprocessed turns (all when `force`)
   under ev_mu; leave the rest queued.
2. Related memories: for each turn, `asper_retrieve(c, turn->text,
   ASPER_SECTION_ANY, active_project, 3, cfg.min_similarity, ...)`;
   union by id, cap 12, order by best cos desc. Handles M1..Mn in that
   order. (ANY-scan must include ONLY: context, active-project records,
   and identity — identity is included here so the curator can UPDATE it.)
3. Prompt: instruction (built-in or file) as system; user content =
   "Existing memories:\n[M1] (section) content\n...\n\nConversation:\nUSER: ...\nASSISTANT: ..."
   Oldest turns trimmed so the transcript stays within
   `cfg.transcript_tokens` (estimate via asper_estimate_tokens).
   No memories => "Existing memories: (none)".
4. Generate with GBNF `asper_gbnf_build(ASPER_GRAMMAR_CURATION, n_handles,
   content_max_chars)`, max 1024 tokens.
5. Parse (`asper_protocol_parse`), validate per guardrails, apply.
   NOOP => nothing. KEEP is invalid in normal cycles (drop + count).
6. Update stats (cycles_run, last_cycle_at), flush access batch as one
   ACCESS op, force journal sync (batch policy), log INFO summary.
7. Turns processed are freed; on curator generate failure, turns are
   dropped (logged ERROR) to avoid poison loops.

Maintenance review (§7.6): candidates = active, unlocked, non-identity
records with `eff < prune_threshold`, sorted eff asc, cap `review_batch`.
Prompt lists them as handles with content + age; grammar REVIEW; DEPRECATE
applies with reason, KEEP applies boost/restore semantics. Gate: at most
once per `maintenance_interval` (ctx->last_maintenance), always run when
`force` (asper_flush full).

Recall (§7.7): candidates via one ANY retrieve (identity + context +
active project), k = cfg.recall_k. Grammar RECALL. Parse ANSWER/CITE/
NOMEM; NOMEM or no ANSWER line => ASPER_ERR_NOT_FOUND, *out_answer NULL.
Cited handles => clones returned + access notes queued; stats
recalls_served++ on success. When `has_curator` is false => ASPER_ERR_MODEL.

## Worker (worker.c)

- Loop: under ev_mu, compute next deadline (idle_flush since last_turn_at
  when turns pending; maintenance_interval since last_maintenance);
  timedwait on ev_cv. Wake on: observe_turn (kick when turns_n >=
  turn_batch), flush request, recall waiting, stop.
- Cycle slot protocol is specified in src/asper_internal.h (cycle_busy /
  recall_waiting / done_cv). The worker defers new cycles while
  recall_waiting > 0.
- Worker also: flushes access batch when it runs a cycle; runs maintenance
  when due; triggers compaction when `store.journal_ops >
  cfg.journal_max_ops`.
- `asper_flush(c, 0)`: access flush + journal fsync, synchronous on caller.
- `asper_flush(c, 1)`: synchronously (on the caller, taking the cycle slot):
  run curation on ALL queued turns (force), maintenance review (force),
  access flush, compaction.
- ASPER_NO_THREADS: worker_start is a no-op (`no_threads = true`);
  asper_tick runs asper_run_due_work(false, false); observe_turn never
  blocks.
- asper_close: stop worker, then final asper_run_due_work(force, full=true)
  semantics: flush access, journal sync, cache save (when dirty), compaction.
  Close does NOT run curation cycles (turns left unprocessed are lost — the
  journal only holds applied ops); this keeps close fast. Document it.

## asper_open sequence (api.c `asper_open_with`)

1. Allocate ctx; init locks/conds; cfg defaults; `asper_config_load`.
2. `asper_log_open`; set default log callback (stderr WARN+).
3. clock = clk ? *clk : asper_clock_system().
4. store.root = strdup(memory_root); `asper_store_open` (mkdir -p root,
   projects/, cache/; manifest; sections; journal replay).
5. Backends: use provided vtables when non-NULL; else llama backends when
   ASPER_WITH_LLAMA and the model file exists — on missing file or load
   failure log WARN and continue DEGRADED (has_embedder/has_curator false);
   asper_open never fails for missing models.
6. If has_embedder: reconcile manifest embedding block (model_id, dim,
   hash); mismatch => cache invalid. `asper_index_init(dim)` (dim =
   embedder dim, else cfg.embed_dim). `asper_cache_load` => stale list =>
   embed stale rows synchronously (passage), `asper_index_put` each;
   `asper_cache_save` when anything was re-embedded; log INFO progress
   every 256 records.
7. Start worker (threaded builds).
8. Any failure: full cleanup, return error (message via a thread-local? no —
   ctx exists until failure return; asper_open frees ctx and returns code
   only).

## build_prompt (api.c)

1. `asper_collect_identity` (clones, §6.1 order).
2. `asper_retrieve` context (k_context) and, when active_project, project
   (k_project) with min_similarity floor.
3. `asper_inject_render`.
4. Queue access notes for every record that SURVIVED trimming —
   inject_render reports survivors: it takes an out bitmask/array param?
   NO — contract fix: api.c re-derives survivors deterministically by
   calling `asper_estimate_tokens` the same way? Fragile. Resolution:
   inject.c exposes the extra out-params at the END of
   asper_inject_render's signature as already declared in
   asper_internal.h? It does not. DECISION: inject.c internally marks
   survivors by setting `rec->score = -1` on DROPPED clones (clones are
   caller-owned scratch); api.c then notes access for every clone with
   score != -1. Document this in both files. (Clones only; never table
   records.)
5. Kick worker if turn_batch reached (observe_turn does that itself).

## Injection rendering (inject.c)

- Templates: `{base_system_prompt}`, `{identity_block}`, `{context_block}`,
  `{project_block}`, `{project_name}` placeholders, replaced textually.
- Section blocks: each record renders as `- <content>\n` (last line keeps
  its newline; block has no trailing blank line beyond the template's own).
- A section with zero surviving records: remove the ENTIRE template
  segment for that section: the heading line(s) immediately preceding the
  placeholder up to the previous blank line, the placeholder line itself.
  Implementation: the default template is processed as five segments;
  custom templates are handled with the same rule: drop the contiguous
  non-empty line-block containing the placeholder (and its preceding
  heading block separated by one blank line). Keep it simple and
  deterministic; golden tests pin the exact output.
- Nothing injected at all => return strdup(base_system_prompt).
- Budgets: identity budget over the whole identity block text; context and
  project budgets over their block text; count tokens per record line
  (`- ` + content + `\n`) and drop whole records from the tail (already
  ordered). Headings don't count toward budgets (documented).
- Token estimator: `asper_estimate_tokens` = curator->count_tokens when
  cfg says curator AND has_curator, else heuristic (bytes/4, min 1).

## GBNF (grammar.c)

Follow Appendix A shape; handles = "M1".."M<n>" as literal alternatives;
text ::= `[^|\x0A\x0D]+` (length enforced engine-side); root for CURATION:
`noop | opline+` where opline excludes KEEP; REVIEW: only DEPRECATE/KEEP
lines or NOOP; RECALL: `nomem | (answer cite*)`. Every line ends "\n".
llama.cpp GBNF syntax: rules `name ::= ...` with `root` required.

## MCP (mcp/)

- Pin protocol version "2025-06-18"; respond to initialize with
  serverInfo {name:"asper-mcp", version}, capabilities {tools:{}}.
- Handle: initialize, notifications/initialized (ignore), ping,
  tools/list, tools/call. Unknown method => -32601.
- tools/call result: {content:[{type:"text", text:"<compact JSON payload>"}],
  isError:false} — compact so every response stays one wire line; asper
  errors => isError:true with the asper_err name +
  asper_last_error message in the text; invalid params => JSON-RPC -32602.
- Requests are processed sequentially from stdin: one JSON value per line
  (LSP-style Content-Length framing is NOT used by MCP stdio; newline-
  delimited JSON per the MCP stdio transport).
- json.c: strict RFC 8259, UTF-8 only, depth cap 64, no NaN/Inf; DOM-style
  value tree + writer. Numbers: double + int64 discrimination.
- `--root <dir>` required; `--config <file>` optional; `--help`, `--version`.

## Tests (tests/)

- tests/CMakeLists.txt: one executable per test file `test_*.c`, linking
  asper_static, includes src/ (internal headers) — add_test per exe.
- Harness: tests/asper_test.h (~100 LOC): ASSERT_* macros, test registry,
  main runner, temp-dir helper (mkdtemp / _mktemp fallback), fail counts.
- Fake embedder for tests: deterministic bag-of-words: dim 16; for each
  whitespace-separated lowercase word, vec[fnv1a64(word) % 16] += 1;
  L2-normalize; model_id "fake-embedder", hash = all 0x11. Shared texts
  overlap => high cosine; disjoint texts => 0.
- Fake curator: user data holds a queue of canned reply strings;
  generate() pops the next one (ignores prompt/grammar); count_tokens =
  heuristic. Empty queue => returns "NOOP\n".
- Tests must pass with ASPER_WITH_LLAMA=ON without any model file present
  (degraded open) — they wire fakes via `asper_open_with`.

## Post-review amendments (binding, applied after the adversarial review)

- `asper_cache_save` snapshots entries into memory under the WRITE lock
  (clearing `cache_dirty` inside that critical section; restoring it to
  true on a later save failure), then writes tmp + atomic replace outside
  the lock. Callers' unlocked `if (cache_dirty)` pre-checks are advisory
  only.
- `asper_observe_turn` wakes the worker on EVERY enqueued turn so the
  §7.2 idle-flush deadline arms; the turn_batch threshold only decides
  when a cycle is due, not when the worker wakes.
- Journal append failure (short write / failed flush) truncates the file
  back to the last fully-written line offset and reopens; if recovery
  fails the journal stream is closed and later mutations fail with
  ASPER_ERR_IO rather than corrupting the mid-file region.
- xCDN-C leaves `\n`-style escapes undecoded in regular strings and treats
  triple-quoted strings as raw. journal.c's esc_decode is therefore
  applied only to strings that contain NO raw control characters (our own
  serializer always escapes them); hand-written triple-quoted multi-line
  content is preserved verbatim. Known limitation: single-line
  triple-quoted hand-edited content containing literal `\x` sequences is
  still interpreted as escapes — the proper fix is an is_raw flag in
  xCDN-C's AST (upstream).
- Section-file records that parse as xCDN but fail semantic validation
  are not silently dropped: they are appended verbatim to
  `<root>/quarantine.xcdn` (WARN) before being skipped, so compaction can
  never destroy hand-edited data.
- `asper_memory_search` with project == NULL uses the ACTIVE project for
  PROJECT/ANY scans (mirrors §7.7); with no active project, project
  records are simply absent from results.
- KEEP from the curator (from_curator=true) is rejected on locked records
  in the apply funnel (§7.5 "applies to UPDATE, DEPRECATE, and KEEP");
  replay and rescue via journal are unaffected.
- stats.ops_applied/ops_rejected are incremented ONLY inside
  asper_apply_op; curator.c routes pre-funnel drops (bad lines, caps,
  guardrails, quorum pendings) through asper_count_reject and never
  bulk-adds its per-cycle tallies to stats.
- Known limitation (documented in store.c): a crash between compaction's
  section-file replace and its journal reset can replay ACCESS/KEEP
  boosts twice (bounded drift: +0.05 relevance / +1 access_count per op);
  no structural corruption is possible.
