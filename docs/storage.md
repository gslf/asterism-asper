# Store format 2 and recovery

The store has one OS writer lock. Records use inspectable xCDN payloads inside
AEV2 frames; the frame includes version, sequence, lengths and separate metadata
and payload SHA-256 checks. A checksum detects accidental damage, not an attacker
who can rewrite both the data and its checksums. Keep the store private to its
owner; project/scope names do not provide multi-user authorization.

`journal.xcdn` contains one checked operation per frame. Recovery validates each
complete frame, its sequence, operation shape and state transition before applying
it. A damaged complete frame is an error. Only an incomplete final frame with the
expected format prefix may be truncated, followed by file sync. Replay uses one
frame at a time and never silently skips an invalid operation.

Manual mutations sync before acknowledgement. Curator operations follow the
configured `storage.journal_sync` policy (`always`, `batch`, or `never`); batch and
never do not promise that every returned curator operation survives power loss.
A short write or failed buffered flush is rolled back only after truncation is
synced. An uncertain file sync or live projection failure poisons the store:
mutations, retrieval and compaction fail until reopen. The journal is preserved,
including operations whose caller received an error. Reconcile current records
before retrying those operations.

Identity, context and project snapshots each contain one complete checked frame.
Invalid records, duplicate IDs, mismatched sections and truncated snapshots fail
open with an error. Snapshot reads never repair or skip damaged content. Update
records through the API/MCP tools; directly editing the payload invalidates its
checksum. Store version 1 and unframed snapshots are rejected. There is no implicit
migration or compatibility mode; keep original data for a deliberate conversion.

Compaction follows these steps:

1. Write and sync all backups.
2. Persist a checked transaction marker binding relative paths to backup hashes.
3. Replace section snapshots, then reset the operation journal.
4. Delete and sync the marker before deleting backups.

Backups are streamed through a 16 KiB buffer.
Recovery validates the entire marker and every backup before restoring the first
target. It rechecks each backup when copying it, and uses copy rather than rename
so another interrupted recovery can restart. An uncertain commit blocks the live
store until reopen. On POSIX, creation/replacement/removal syncs the affected parent
directories. Windows requests write-through moves but has no equivalent directory
sync guarantee in this implementation.

Bounds are 65,536 live/deprecated records, 4,093 projects, 16 MiB per operation or
section snapshot, 512 MiB per journal/raw backup and 1 MiB per transaction marker.
Manifest reads are bounded to 64 KiB. These are per-store/per-file bounds, not a
quota on the entire source/object/archive tree. Retention and export/delete across
all derivatives remain separate work.

Tests inject short writes, flush errors and uncertain sync; corrupt complete
payloads and snapshots; reject legacy versions; and interrupt a process at five
compaction boundaries. Recovery preserves acknowledged updates without replaying
access counters twice. A damaged later backup leaves earlier targets untouched.
Linux ASan/UBSan/LeakSanitizer runs cover these paths. Process-crash tests do not
establish power-loss durability on every filesystem, controller or OS.
