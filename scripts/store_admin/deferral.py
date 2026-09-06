"""Explicit reversible curation postponement, separate from acknowledgement."""
import hashlib
import json
import os
import re
import uuid
from . import frames
from .files import LOCK, check_owner, inventory, mount_id, opened, write_new
from .locking import present

POLICY = "curation.deferred"
MAX_POLICY = 1024 * 1024


def scope_valid(scope):
    return isinstance(scope, str) and re.fullmatch(r"[a-zA-Z0-9_.-]{1,64}", scope) and scope not in (".", "..")


def validate(policy):
    if (not isinstance(policy, dict) or policy.keys() != {"schema", "events"} or
            type(policy["schema"]) is not int or policy["schema"] != 1 or
            not isinstance(policy["events"], list) or len(policy["events"]) > 4096):
        raise ValueError("invalid deferral policy schema")
    seen = set()
    for row in policy["events"]:
        if not isinstance(row, dict) or row.keys() != {"scope", "id", "sequence", "sha256", "note"}:
            raise ValueError("invalid deferral entry")
        if (not scope_valid(row["scope"]) or not isinstance(row["id"], str) or
                not frames.ID.fullmatch(row["id"]) or type(row["sequence"]) is not int or
                not 1 <= row["sequence"] <= 2**63-1 or not isinstance(row["sha256"], str) or
                not re.fullmatch(r"[0-9a-f]{64}", row["sha256"]) or not isinstance(row["note"], str) or
                not row["note"].strip() or "\0" in row["note"] or len(row["note"].encode("utf-8")) > 1024):
            raise ValueError("invalid deferral identity, hash or note")
        key = (row["scope"], row["id"])
        if key in seen:
            raise ValueError("duplicate deferred source")
        seen.add(key)
    return policy


def read(root):
    if not present(root, POLICY):
        return {"schema": 1, "events": []}
    try:
        return validate(json.loads(frames.checked_read(root, POLICY, MAX_POLICY), object_pairs_hook=frames.unique))
    except (UnicodeError, RecursionError) as error:
        raise ValueError("invalid deferral text") from error


def source(root, scope, event_id):
    if not scope_valid(scope) or not isinstance(event_id, str) or not frames.ID.fullmatch(event_id):
        raise ValueError("invalid source scope or UUID")
    with opened("scopes", root, directory=True) as scopes, opened(scope, scopes, directory=True) as directory:
        for fd in (scopes, directory):
            check_owner(os.fstat(fd))
            if mount_id(fd) != mount_id(root):
                raise ValueError("mounted source directory")
        with frames.file(directory, "events.log", 512 * 1024**2) as stream:
            sequence = 0
            while row := frames.event(stream):
                sequence += 1
                if row["sequence"] != sequence:
                    raise ValueError("invalid source sequence")
                if row["id"] != event_id:
                    continue
                if row["kind"] not in (0, 1):
                    raise ValueError("only user and assistant sources enter curation")
                text = row["text"].encode("utf-8")
                digest = hashlib.sha256(bytes([row["kind"]]) + row["object_ref"].encode("ascii") + b"\0" + text).hexdigest()
                return {"scope": scope, "id": event_id, "sequence": sequence, "sha256": digest,
                        "bytes": len(text), "preview": row["text"][:1024], "preview_truncated": len(row["text"]) > 1024}
    raise ValueError("source event not found")


def inspect(root, scope=None, event_id=None):
    policy = read(root)
    selected = source(root, scope, event_id) if scope is not None else None
    plan = inventory(root, exclude=(LOCK,))
    if read(root) != policy or (selected is not None and source(root, scope, event_id) != selected):
        raise ValueError("curation sources changed during inspection")
    return {"snapshot": plan["snapshot"], "deferred": policy["events"], "source": selected}


def update(root, expected, scope, event_id, note, *, resume=False):
    if not scope_valid(scope) or not isinstance(event_id, str) or not frames.ID.fullmatch(event_id):
        raise ValueError("invalid source scope or UUID")
    # A live or interrupted batch owns its inputs; the operator must reconcile it first.
    if present(root, "curation.pending"):
        raise ValueError("reconcile the pending curation receipt first")
    # Removal also repairs a stale decision after source loss. It needs no model
    # or source read, and cannot acknowledge, restore or recreate the source.
    plan = inspect(root) if resume else inspect(root, scope, event_id)
    if plan["snapshot"] != expected:
        raise ValueError("store snapshot changed; inspect again")
    if not resume and present(root, "curated-events.log"):
        with frames.file(root, "curated-events.log", 8 * 1024**2) as stream:
            curated = stream.read(8 * 1024**2 + 1)
            if len(curated) > 8 * 1024**2 or any(not frames.ID.fullmatch(line) for line in curated.decode("ascii").splitlines()):
                raise ValueError("invalid curation acknowledgement set")
            if event_id in curated.decode("ascii").splitlines():
                raise ValueError("source is already acknowledged")
    rows = plan["deferred"]
    existing = next((row for row in rows if row["scope"] == scope and row["id"] == event_id), None)
    if resume:
        if existing is None:
            raise ValueError("source is not deferred")
        rows.remove(existing)
    else:
        if existing is not None:
            raise ValueError("source is already deferred")
        rows.append({key: plan["source"][key] for key in ("scope", "id", "sequence", "sha256")})
        rows[-1]["note"] = note
    text = json.dumps(validate({"schema": 1, "events": rows}), ensure_ascii=False, separators=(",", ":"))
    if len(text.encode("utf-8")) > MAX_POLICY:
        raise ValueError("deferral policy exceeds 1 MiB")
    temporary = ".deferral-" + str(uuid.uuid4()) + ".tmp"
    write_new(root, temporary, frames.checked_encode(text))
    os.replace(temporary, POLICY, src_dir_fd=root, dst_dir_fd=root)
    os.fsync(root)
    return {"state": "deferral_removed" if resume else "deferred", "source": event_id,
            "acknowledged": False, "deleted": False}
