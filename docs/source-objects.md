# Exact object reads

Content-addressed objects use a lowercase `sha256:` reference and are bounded to
64 MiB each. A range read streams the complete object through an 8 KiB hash buffer
while retaining only the requested slice. Damage outside that slice still
invalidates the reference. No partial output is returned on hash mismatch, short
read, oversize input or invalid range. Deduplicated puts validate an existing
object before acknowledging it, rather than trusting its filename.

The configured store root is canonicalized once. Regular-object reads reject
symlink ancestors and special files on POSIX. The Windows implementation checks
the opened handle and resolved path; it has not been compiled or exercised in
this Linux validation. Other source/store I/O and new-object writes still use
existing path-based operations: this reader does not establish an end-to-end
containment guarantee for every store write. Keep the store operator-owned.

Every range currently pays for complete hashing. In a single Linux Release-build
component probe, reading 16 bytes of a 32 MiB object changed observed `wait4`
maximum RSS from 38,584 to 17,400 KiB, and wall time from 0.0087 to 0.1306 seconds.
The old implementation loaded the complete file without checking its hash;
the new implementation checks all bytes and allocates only the slice. The probe
used one process per variant, no models and no worker threads. These numbers
include the process/loader memory floor, have no repeated-run uncertainty estimate
and are not an end-to-end agent performance result. Hash validation caching would
need independently verified invalidation; no such cache is implied here.

Tests reproduce corruption outside a requested slice, empty objects, invalid and
oversized ranges, a size-limit check before hashing input, an aliased input/output
buffer, symlinked objects and a FIFO that must be refused without blocking.
