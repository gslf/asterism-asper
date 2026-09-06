# Progressive source history

Context selection captures an indexed event prefix and its pin overlay. It
checks event metadata to find old pins, then fills the remaining history budget
from the newest fitting events. Presentation restores chronological order.
Every selected text receives complete frame/payload verification. Metadata-only
inspection is a shortlist operation and cannot certify an unread payload.

The source reader retains one decoded event and the selected output lines,
rather than a second copy of the complete scope. Context payloads are bounded
to 4 MiB and 4,096 selected events independently of the counting callback.
The system prompt still uses the separate semantic-memory rendering contract.
An event is included whole or omitted; the durable source remains reopenable.
`events_available` describes the captured prefix, while `events_included` counts
the returned events. These counts are not a measurement of semantic recall.

Headings consume the appropriate history/checkpoint budget. The renderer checks
the joined history and final context again because token counts need not be
additive across concatenated strings. An exceeded envelope returns `LIMIT` and
no partial pack. Missing/failed token callbacks use the documented heuristic;
this does not establish exact tokenization or a calibrated remote margin.

Callbacks execute outside the source mutex. A callback may append an event
without deadlocking; that event belongs to a later prefix. Pin updates likewise
do not change an already captured overlay. Reads check the open file before and
after access, detecting shrinkage, unlinking and observed equal-size rewrites
even when stdio retains old bytes. Append growth is allowed under the cooperating
writer contract. File timestamps are not a cryptographic snapshot: a hostile
external writer could change a prefix while growing the file. Keep the store
under operator control; these observations do not implement atomic filesystem
snapshots against another process with the same privileges.

The Windows implementation uses handle metadata including `LastWriteTime` and
`ChangeTime`; only the Linux implementation has been compiled and exercised here.
The field contract is documented by Microsoft in
[FILE_BASIC_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_basic_info).

## Checkpoints, replay and bounds

A missing checkpoint projection is recovered by scanning metadata backwards to
the latest checkpoint and validating that payload. A requested checkpoint that
is corrupt, oversized or unreadable is an error, rather than a silent omission.
A checkpoint budget of zero excludes the projection explicitly.

Restart discovery reads metadata before loading uncurated user/assistant events;
already curated events and diagnostic payloads need not be materialized. Source
metadata checks are not a full audit of every skipped payload. Grounding reopens
and verifies its specific source frames. The pending curation queue still owns
its accepted turns; bounding that queue and adding incremental replay admission
remain separate work. The explicit bulk `asper_event_list` API and Asngn's bulk
transcript restoration also remain available and can retain complete histories.

Metadata reads reject aliases and special files and check sizes before allocation:
scope inventory is bounded to 1 MiB, the pin and curated-ID logs to 8 MiB each,
and a checkpoint projection to the 16 MiB event limit. Invalid UTF-8, embedded NULs
and incomplete curated IDs are rejected. Pin overlays retain their last update;
duplicate curated IDs are coalesced in the in-memory lookup. These logs remain
separate from checked record transactions and are not a global lifetime quota.

Curation checks acknowledgement and receipt-history capacity before retrieval,
embedding or generation. A full log preserves the pending turn and returns
`LIMIT` without inference. [Curation receipts](curation-recovery.md) now distinguish
completed bookkeeping from interrupted mutations: a completed receipt reconciles
without a model, while a prepared receipt suspends re-proposal for operator review.
Individual record updates remain separate transactions. Selective retention and
cross-store erasure remain separate work.

## Reproducible component probe

The opt-in `bench_source_context` target creates one old pin, large diagnostic
events, and eight short recent diagnostics. Both variants must return exactly
the same nine events and identical rendered text. It measures store open,
context materialization and close, with no model generation.

Build the target in a Release, no-llama configuration, then compare two versions
of the same probe linked against the corresponding library revisions:

```sh
cmake --build /path/to/build --target bench_source_context
python3 scripts/bench_source_context.py --before /path/to/old/probe \
  --after /path/to/new/probe --output /path/to/new-results.json
```

For a baseline revision predating the target, compile the same
`tests/bench_source_context.c` against that revision's `libasper.a`, `libxcdn.a`
and `libasmodel.a`, with the same compiler/options and curl/math dependencies.
The recorded comparison uses `-O3 -DNDEBUG -std=c99 -fPIE` for both probe builds.
The baseline Asper revision is `68281adb0788cd9a72fabdc52c98fae7d47ca379`;
the comparison implementation is the progressive reader in this commit.

The driver uses a freshly copied, warm fixture for each run and alternates the
variant order. It records executable hashes, each measurement, observed ranges
and medians. Each measurement has a 60-second deadline. Runtime-generated UUIDs
are shared across both variants within an experiment; regenerating the fixture
changes those IDs, not the required selection. The corpus is limited to 256 MiB.

This is a component measurement, not a real-model task-success evaluation.
`wait4` peak RSS includes the process launch/loader floor and excludes filesystem
cache and the controller's memory. The local run uses warm `/tmp` tmpfs storage;
it does not predict cold-disk, GPU, multi-session or end-to-end agent behavior.
Five repeats provide descriptive ranges, not a calibrated confidence interval.

The [2026-09-06 Linux measurements](benchmarks/source-context-linux-2026-09-06.json)
use 2,048 diagnostics of 64 KiB, plus the nine retained events. No other build or
test started by this task overlapped this run; desktop activity was not controlled.

| Measurement | Baseline median (range) | Progressive reader median (range) |
|---|---|---|
| Wall time, seconds | 1.452 (1.349–1.489) | 0.712 (0.633–0.740) |
| Peak process RSS, KiB | 137,836 (137,748–138,036) | 24,784 (24,784–24,784) |

The complete output is byte-identical across all ten runs. These figures apply
to this large-history fixture and do not justify a general latency or task-success
claim. The raw report records each observation and executable digest.
