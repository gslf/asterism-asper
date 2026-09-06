# Grounded memory and correction history

Asper ABI 5 separates a recorded claim from the validity of its support. The
host attaches evidence; the curator's output cannot certify itself. This is a
provenance and dependency contract, not a truth estimator or formal verifier.

`asper_memory_ground` binds a definition to the SHA-256 of the current claim and
an expected grounding revision. The input is copied. Each of these collections
has a limit of 16 entries:

- Source spans identify a scope, exact event UUID and sequence, source byte range
  and claim byte range. The runtime checks existence and UTF-8 boundaries.
- Dependencies identify a resource and its required version. These can describe
  files, a workspace snapshot, toolchains, environment settings or backend policy.
- Links bind another record's UUID and content SHA-256. `supports` makes that
  record a prerequisite; `contradicts` contests both claims; `supersedes` retires
  the referenced version. Support cycles and paths beyond 32 links are rejected.

The host's assertion that a passage supports a claim can itself be wrong. Full
byte coverage establishes traceability, not semantic entailment. A reported
confidence remains explicitly unknown, heuristic or measured; reading or linking
a claim does not increase it.

| Status | Meaning |
|---|---|
| `unverified` | No grounding, partial coverage or unverified prerequisites |
| `current` | Source coverage/support and all declared preconditions are current |
| `stale` | Content/version changed, a prerequisite failed, or a correction superseded it |
| `contested` | An explicit contradiction is unresolved |
| `revoked` | The host explicitly revoked this grounding |
| `unavailable` | Required observations or source events are unavailable |

Search and identity injection exclude stale, contested, revoked and unavailable
records. Unverified claims retain their provenance label. List/read APIs keep
invalid records inspectable and expose their status and grounding revision.
Grounded prompt entries contain a record/revision handle. An ordinary content
edit cannot transfer old evidence to the changed claim. Re-establish support
with a new expected revision, or explicitly revoke it with a reason.

`asper_memory_observe_dependency` records the host's latest observation. These
observations are intentionally volatile: reopening a store requires observing
the environment again. Restoring exactly the required version may make a claim
usable again; an explicit revocation or superseding correction is separate.
Asterism's engine observes `workspace:<SHA-256 of canonical root>` before memory
materialization, using the bounded workspace fingerprint as its version. Separate
worktrees have separate resource keys. File/toolchain/environment observations
remain the integrating host's responsibility. Changes after an observation must
be detected by the next snapshot or explicitly reported by the host.

Grounding definitions, exact claim text, hashes, source intervals, relations and
revocation reasons are retained in `knowledge.events`, using checked AEV2 frames.
`asper_memory_grounding_history` pages this history by global sequence, even after
record updates and compaction. A page contains at most 100 revisions. Complete
frame corruption fails closed; an uncertain write disables knowledge retrieval
and further record mutations until reopen. Sources are checked at attachment and
reopen; later reopening an exact event verifies its frame again.

The implementation bounds live definitions and observed resource keys to 16,384,
claim text to 64 KiB and the journal to the shared 512 MiB event-log quota. It uses
a sorted UUID table and bounded support propagation, not a universal knowledge
graph. The history is append-only and is not automatically pruned. Whole-store
[offline maintenance](data-governance.md) includes this history and its internal
derivatives in export and erasure. Selective retention/deletion, coordination with
stores outside this root, owner authorization, automatic semantic
contradiction discovery and curator-proposed granular support remain separate
milestones. Candidate source UUIDs from a curation batch are labeled as candidates
and must not be interpreted as precise supporting spans.
