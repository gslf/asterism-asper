# Offline store maintenance

`scripts/store.py` inspects, exports, verifies and erases a **whole Asper store**.
It uses Python 3.11+ and the standard library, with no model, server or network
dependency. This implementation requires Linux with `/proc/self/fdinfo`; other
platforms fail explicitly. The core library does not require Python.

It also provides [source-curation inspection and acknowledgement](curation-recovery.md).
That operation keeps reviewed partial effects and never replays model proposals.

Stop every host using the store first. Maintenance acquires the same nonblocking
writer lock as libasper and refuses a live store. Run it as the store's OS owner:
the root and entries must belong to the effective UID and must not be writable
by group or other users. Scope/project names are not authorization credentials.
This is local owner administration, not an authenticated multi-user service.

## Inspect and export

From the Asper checkout, using absolute paths for the actual store and a private
parent directory outside it:

```sh
python3 scripts/store.py inspect --root /path/to/memory
python3 scripts/store.py export --root /path/to/memory --output /path/to/private/export-001
python3 scripts/store.py verify /path/to/private/export-001
```

Inspection returns a snapshot digest binding the physical root identity, file
identities and content hashes. It includes source events, objects, all record
versions still on disk, knowledge history, caches, journals and backups under
the root. It excludes only the root writer lock. The reported byte total counts
regular files with one link; aliases and special entries are listed without
opening their targets. Inspection validates file stability, not xCDN semantics
or the truth of a memory claim.

Export requires a new destination; it never overwrites an existing directory.
It copies every inventoried regular file into `files/`, using mode 0600 for files
and 0700 for directories. It rejects symbolic links, hardlinks, special files and
nested mounts. Descriptor-relative traversal rejects symlink substitution;
Linux mount IDs also distinguish bind mounts with the same device ID. An
operator-selected root alias is resolved once before admission.

Each copy is hashed, synced and checked against its source. The entire copy is
then verified, and the source inventory is checked again before publication.
`export.jsonl` is published last, after syncing its temporary file. Its first
line identifies format/version, source snapshot and entry count; subsequent
lines contain sorted relative paths, entry kinds, lengths and SHA-256 hashes.
Verification parses one bounded line at a time and checks every copied file.
These checks detect accidental corruption; they do not authenticate an archive
against someone who can rewrite both its contents and its manifest.

A failed/interrupted export remains at its destination for inspection. A missing,
partial or inconsistent manifest cannot verify. Nothing is automatically restored
or deleted. After a successful verification, `files/` can be copied into a **new**
store root for a deliberate restore. Opening that copy runs normal checked store
recovery; the export itself does not repair a corrupt source or prove that every
stored record is semantically valid. Keep the verified export unchanged.

## Erase and resume

Inspect first and pass the returned snapshot digest to the destructive command:

```sh
python3 scripts/store.py erase --root /path/to/memory --expect-snapshot SNAPSHOT_FROM_INSPECT
```

A changed snapshot is rejected before erasure starts. The tool durably publishes
`.erase.pending` before the first unlink. Libasper checks this guard while holding
the writer lock, before file logging, store recovery, model initialization or
curation. Even an empty or damaged guard blocks open. Thus an old compaction
backup or pending curation cannot silently repopulate an interrupted erasure.

Erasure unlinks all inventoried entries under the root, including internal
backups and derived data. It unlinks aliases themselves; external targets and
other hardlinks are untouched. Changed entries or unexpected new names stop the
operation. Parent directories are synced after removals. On interruption, run
`inspect` again, review the remaining inventory and run `erase` with that new
snapshot. It resumes from the observed state and does not replay a prior plan.

The empty root retains its linked writer-lock inode and erasure guard. Inspection
reports `empty_blocked`. The guard contains an operation ID and snapshot digest,
not copied memory text. Keep it in place and use a new root for future memory.
The tool never deletes the store directory or automatically re-enables it.

## Boundaries and validation

Operations are bounded to 65,536 inventoried entries, depth 64, 16 MiB of UTF-8
path names, 512 MiB per regular file and 4 GiB total regular-file content. Export
metadata is bounded to 128 MiB and 128 KiB per line. These are maintenance bounds,
not an enforced lifetime quota on an active store. Files stream through 64 KiB
buffers. A snapshot still retains bounded path/identity metadata in memory.

The writer lock coordinates cooperating hosts. File/descriptor checks detect
observed external changes; they do not make a whole filesystem tree an atomic
snapshot or an atomic compare-and-unlink operation against a hostile process
with the same OS credentials. Parent directories and the store must remain
under the operator's control. A failed directory sync is an error, not a receipt
of durable success. Process-crash tests are not power-loss certification.

The scope is exactly this store root. Asngn journals, tool logs, configured log
files outside it, prior exports, filesystem snapshots, provider logs and backups
elsewhere need their own retention/erasure policy. Unlinking is not a forensic
wipe of storage media. Whole-store erasure avoids leaving tracked derivatives
inside this root; selective scope/record erasure, dependency-aware retention,
object garbage collection and authenticated owner APIs remain separate work.

Contract tests exercise both directions of the real MCP host lock, export and
reopen of records/source events, stale snapshots, copied-data corruption,
post-copy source changes, malformed/oversized metadata, quotas, aliases/FIFOs,
injected mount-identity changes, a process crash during erasure and uncertain
guard sync. Tests confirm that the external alias target survives and that the
host cannot reopen a partially or fully erased store. Actual privileged mount
creation and Windows/macOS maintenance are not tested or claimed.
