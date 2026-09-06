# Source curation receipts and reconciliation

A source-driven curation cycle writes a checked `curation.pending` receipt after
generation and parsing, before applying proposed memory mutations. It contains
schema version 1, a batch UUID, scope/project, exact source UUIDs, the ordered
handle-to-record UUID map, proposal text, creation time and starting journal
position. The proposal is untrusted model output, not a runtime instruction.

Each operation uses the existing validation and WAL path. The runtime flushes
access updates and syncs the journal, even under the general `never` sync policy,
before replacing the receipt with outcome `processed`. That outcome includes
applied/rejected/pending counts and the ending journal operation count. It
describes processing the proposal; it does not certify that claims are true or
that a user task succeeded.

Completion proceeds in this order:

1. Persist the terminal checked receipt.
2. Append it to checked `curation.events` history, deduplicating the last batch UUID.
3. Atomically replace the bounded, deduplicated `curated-events.log` set.
4. Remove and sync the pending guard.

A restart with a terminal receipt completes only these bookkeeping steps. It
does not call a model or repeat a memory mutation. A `prepared` receipt instead
suspends source replay, new curation, maintenance review and compaction for this
store. Full flush returns `ASPER_ERR_BUSY`; startup logs the batch UUID. Source
events remain reopenable, and the record WAL is preserved for inspection.
Ordinary host reads/writes still use their existing contracts. The journal range
may include host operations; it is not an exclusive per-batch effect list.
The public stats structure does not expose the suspended state; use the error/log
and offline inspection below.

This preserves partial effects. It is not an atomic batch transaction, rollback
or exactly-once execution guarantee. Maintenance reviews and ordinary access
accounting have their own WAL behavior; this protocol covers source-driven
curation. Granular curator-proposed grounding spans and automatic reconciliation
of effects remain separate work.

## Offline operator review

Close the host and inspect the receipt with the Linux/Python 3.11+ maintenance
tool. It takes the runtime's single-writer lock without loading models:

```sh
python3 scripts/store.py curation-inspect --root /path/to/memory
```

Review the proposal, source IDs, record handles, persisted records and journal.
The snapshot covers the physical store under the limits of
[offline maintenance](data-governance.md). To keep the reviewed partial effects:

```sh
python3 scripts/store.py curation-acknowledge --root /path/to/memory \
  --expect-snapshot SNAPSHOT_FROM_INSPECTION \
  --note 'Reviewed the persisted partial effects and kept them.'
```

This writes `interrupted_acknowledged`, with unknown operation counts and the
operator's note. Next open archives that outcome and acknowledges the source IDs
without generating or applying more operations. It does not undo partial changes
or claim successful curation. The host can submit new source events after review
if more work is needed. There is no automatic requeue or model-facing reconciliation
tool.

A stale snapshot, empty note, malformed receipt, alias, active writer or erasure
guard prevents acknowledgement. Inspect uncertain writes again. This is an
operator-owned single-user facility with the external-writer limitations of the
offline maintenance contract, not an authenticated multi-tenant API.

## Bounds and validation

Each batch considers at most 256 queued events, retaining scope/revision boundaries.
Before retrieval or inference, the runtime selects the oldest complete prefix
that fits `transcript_tokens` and a 64 KiB rendered transcript envelope. It counts
the joined text, including roles and the heading; a missing or failed counter
uses the existing heuristic. This is not an exact remote token guarantee.
Only those inputs reach retrieval, generation, candidate provenance and the
receipt. The omitted tail remains queued. A failed generation restores the selected
prefix before the tail and any newly appended events.

An oversized first event returns `ASPER_ERR_LIMIT` without inference or an
acknowledgement. It remains at the head of the queue. Raising the token budget
can admit it within the byte envelope; larger events require an explicit future
segmentation or defer contract. The runtime never truncates and acknowledges
such an event silently. Full flush drains fitting inputs through multiple bounded
batches in both threaded and non-threaded builds, stopping on an error.
Receipts are at most 1 MiB, with at most 64 KiB
of proposal text and twelve handles. History uses the 512 MiB event-log limit;
history and acknowledgement capacity are checked before inference. The 8 MiB
acknowledgement set is rewritten atomically, adding work proportional to that set
on completion. The pending queue itself is still unbounded. Selective retention
of receipts, sources and outside-store derivatives remains open.

Checked snapshot reads and backup copies reject aliases. Temporary writes use
unique, exclusively created files; cleanup only removes a temporary created by
that call. POSIX creation walks parent descriptors without following aliases;
replacement still uses the existing path-based rename contract. Windows uses
`CREATE_NEW` for the leaf; its ancestor/replacement guarantees remain unvalidated
here. This does not establish containment of every store write against a process
with the operator's privileges.

Tests cover seven actual process-exit boundaries and seven live I/O boundaries,
uncertain journal sync under all three policies, corrupt receipts, duplicate
acknowledgements, Unicode/NUL/schema rejection, quota admission before model calls,
a 257-event full flush, temporary aliases and six offline integration cases using
a real interrupted C batch and MCP restart. Linux threaded sanitizer and
non-threaded suites pass; these are not power-loss durability tests.

Four input-coverage cases additionally check exact receipt-to-prompt membership,
ordered retry with a reentrant append, oversized inputs, joined token counts and
the byte cap with a zero-returning counter. The same first regression case,
compiled against `cb1ed0b` and the revised library, reproduced silent omission:
with twenty inputs and a forty-token transcript budget, the prior runtime sent
thirteen inputs in one call while acknowledging all twenty. The revised runtime
sent all twenty once across three calls, with matching receipts. These are
scripted contract checks, not model-quality measurements.

## Reproduced baseline failure

Compile the same optional `curation_replay_probe` source against each revision's
headers, fake interfaces and static libraries. It exits during the third embedding
after generation, when the first insertion is already journaled. A fresh process
then opens the store and requests a full flush:

```sh
cmake --build /path/to/build --target curation_replay_probe
/path/to/build/tests/curation_replay_probe interrupt /tmp/new-probe-store
# Expected process exit: 82.
/path/to/build/tests/curation_replay_probe resume /tmp/new-probe-store
```

The [recorded comparison](benchmarks/curation-replay-2026-09-06.json) uses the
previous `5018d7d` library and this implementation, with the same Release flags.

| Observation after restart | Previous runtime | Receipt protocol |
|---|---|---|
| Preserved records | 1 | 1 |
| Source events automatically requeued | 2 | 0 |
| Further curator calls | 1 | 0 |
| Full-flush outcome | `ASPER_OK` | `ASPER_ERR_BUSY` |

The fake reply after restart is `NOOP`. This reproduces re-proposal after a partial
mutation, not an actual duplicated insertion or a behavioral model benchmark.
