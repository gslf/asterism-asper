# Postpone an exact source without acknowledging it

An event larger than the curator's transcript budget stops a cycle with `LIMIT`.
Offline source deferral lets the operator keep that input pending while later
events proceed. It is explicit and reversible. It never truncates the source,
marks it curated, deletes it or turns it into a verified memory. Exact history
search and context materialization still see the original event.

Close the store's host. On Linux with Python 3.11+, inspect the event named in
the curation error, using its scope and UUID:

```sh
python3 scripts/store.py curation-sources --root /path/to/memory \
  --scope SESSION_SCOPE --event EVENT_UUID
```

The result includes a physical store snapshot, source byte count, bounded text
preview, source identity and installed deferrals. The preview is limited to 1,024
Unicode characters and reports truncation. The content hash covers the source's
role, origin reference and full text; the decision also binds its scope, UUID
and sequence. It is not a hash of the preview or a judgment about the content.

After reviewing the source, postpone it:

```sh
python3 scripts/store.py curation-defer --root /path/to/memory \
  --scope SESSION_SCOPE --event EVENT_UUID --expect-snapshot INSPECTED_SNAPSHOT \
  --note 'Revisit with a larger transcript budget.'
```

Reopen the host to apply this policy. ABI 8 and MCP `memory_stats` expose
`curation_deferred`, the number of installed decisions. This is separate from
queued, in-flight and possible durable backlog. A successful full flush can
leave explicitly deferred sources unprocessed; it is not evidence of their
curation. The decisions remain effective across restarts.

To remove one decision, inspect the current store again and use that snapshot:

```sh
python3 scripts/store.py curation-sources --root /path/to/memory
python3 scripts/store.py curation-resume-source --root /path/to/memory \
  --scope SESSION_SCOPE --event EVENT_UUID --expect-snapshot CURRENT_SNAPSHOT
```

On reopen, an existing, unacknowledged source becomes eligible again. It must
still fit the transcript envelope; removal does not enlarge budgets, restore a
lost source or bypass an interrupted receipt. Decision removal also works when
its source was externally lost or changed, allowing the operator to repair that
stale policy. Curation segmentation remains unimplemented.

## Ownership and durability

The tool takes the same single-writer lock as the runtime and requires an exact
store snapshot. A pending curation receipt must be reconciled first. Only stored
user/assistant events can be newly deferred; already acknowledged sources and
duplicate decisions are rejected. No model-facing tool grants these operations.

`curation.deferred` is a checked AEV2 snapshot containing strict JSON schema 1,
at most 4,096 decisions, 1,024 UTF-8 bytes per review note and 1 MiB total text.
Replacement uses an exclusively created temporary, file fsync, rename and parent
fsync. A failed or uncertain write is reported; inspect again before retrying.
The current policy retains active notes. Removing a decision removes its note
from this policy; there is no separate immutable administrative audit log.

Before loading models or starting the worker, the runtime validates the policy
and every referenced event, including the full payload checksum. Corrupt frames,
changed roles/origins/text, wrong IDs/sequences, missing sources and invalid
schemas cause store opening to fail. Admission checks the binding again before skipping
an event. The sorted decision array keeps lookup logarithmic; payload validation
still reads the complete deferred events on each store open.

The operator tool uses descriptor-relative no-follow reads, rejects alias files
and mounted source directories, and checks observed file identities. Runtime
reads retain the existing store/platform contracts. This is a single-owner
facility, not authenticated multi-user access control or protection against an
external writer with the owner's privileges. Windows/macOS administration and
power-loss behavior have not been validated. Physical export and whole-store
erasure include the policy through the existing inventory contract.

Eight integration cases cross the actual C runtime and Python administration:
oversized-head blocking, later-source progress, exact acknowledgement membership,
restart/removal, independent decisions, checked export, stale snapshots, source
loss, changed-but-valid checksums, corruption, aliases, quotas, duplicate fields,
pending receipts and uncertain replacement/sync. The MCP and shared-library
probes also check the new observation. No model-quality improvement is measured.
