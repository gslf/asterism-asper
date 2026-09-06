# Bounded source admission

Source events are persisted before semantic work is admitted. The in-memory
curation FIFO obeys `curation.event_queue_max` (default 256, allowed 1–4,096)
and a separate 32 MiB text limit, including terminating bytes. Both limits
include inputs currently owned by a curation cycle. Queue array capacity also
reserves their slots, so a failed generation can restore the original prefix
ahead of concurrent arrivals without allocating another array.

Excess events remain in their scoped logs. Admission retains a per-scope event
cursor and checks metadata before loading an uncurated user/assistant payload.
Completed events are not rescanned from the beginning on every refill. Scope
selection rotates between refills; order within each scope is preserved. This
is not a global timestamp ordering or a calibrated fairness scheduler.

Restart reconstructs these volatile cursors from the bounded scope inventory
and curated-ID set. No cursor itself acknowledges a source: only a completed
curation receipt, or explicit reconciliation of an interrupted receipt, does.
The existing receipt guard suspends new admission during uncertain mutations.
Source corruption encountered during admission returns an error without advancing
that event's cursor or acknowledging it.

User/assistant events without a caller-supplied object receive a content-addressed
`#turn` origin containing the active project at append time. Admission never
guesses the project from the host's later active selection. Explicit source
objects are preserved; an opaque object without turn metadata leaves project
identity unknown/unscoped. Missing/hash-invalid objects and recognized turn
metadata with invalid field types or lengths fail admission. The origin snapshot
is not an authorization grant.

The worker and `asper_tick` refill from disk as space becomes available. A full
`asper_flush` captures current per-scope endpoints while holding the cycle slot.
Appends during that drain stay outside the captured endpoints, including events
in new scopes; another flush or background cycle can admit them afterward. This
prevents concurrent producers from extending a requested drain indefinitely.
An event that cannot fit the [transcript envelope](curation-recovery.md) still
stops with `ASPER_ERR_LIMIT`; it is neither truncated nor acknowledged. Segmentation
and operator-directed deferral remain future work.

The payload cap is not a whole-process RSS limit. Temporary frame decoding,
source-object metadata parsing, rendered prompts, source indices, model state and
other store objects have their own contracts. The admission table has a hard
16,384-scope limit; the scope inventory and initial curated-ID text retain their
1 MiB and 8 MiB limits. Many unrelated diagnostic events can still require a long
initial metadata scan. These bounds do not replace retention or an inverted index.

## Observability and validation

ABI 7 adds queue observations to `asper_get_stats` and MCP `memory_stats`:

| Field | Meaning |
|---|---|
| `curation_queued` | Admitted events waiting in the FIFO |
| `curation_inflight` | Events owned by the current cycle |
| `curation_bytes` | Combined retained text bytes of those two groups |
| `curation_queue_limit` | Effective event-count limit |
| `curation_backlog` | Durable data may still need admission; not an exact pending count |
| `curation_suspended` | A receipt is currently pending, including transient completion work |

Stats are observations under short locks, not an atomic snapshot of model and
store state. A suspended receipt after restart requires the existing recovery
or operator-review workflow; its presence during an active cycle is transient.

Eleven deterministic cases cover bounded startup, multiple scopes, exact eventual
coverage, restart after acknowledgement, finite full flush with reentrant appends,
retry ownership, byte pressure, reserved array capacity, corrupt deferred payloads,
configuration bounds, project changes across restart, missing/malformed origins, four concurrent producers
and actual worker draining. The shared-library ABI and real
MCP process also exercise the status fields. No model weights are needed for
these contract tests; they do not measure semantic quality or real-model latency.
