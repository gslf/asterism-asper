"""Inspect a suspended source batch; acknowledge partial effects without replay."""
import hashlib
import json
import os
import re
import stat
import uuid
from .files import LOCK, check_owner, identity, inventory, mount_id, opened, write_new

PENDING = "curation.pending"
MAX_RECEIPT = 1024 * 1024
ID = re.compile(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\Z")


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate receipt field")
        result[key] = value
    return result


def validate(receipt):
    keys = {"schema", "id", "scope", "project", "created_at", "journal_ops_before",
            "journal_bytes_before", "sources", "handles", "proposal", "outcome",
            "resolution_note", "counts", "journal_ops_after"}
    if not isinstance(receipt, dict) or receipt.keys() != keys or receipt["schema"] != 1:
        raise ValueError("invalid curation receipt schema")
    for key, limit in (("id", 36), ("scope", 64), ("project", 128), ("proposal", 65536),
                       ("outcome", 32), ("resolution_note", 1024)):
        value = receipt[key]
        if not isinstance(value, str) or "\0" in value or len(value.encode("utf-8")) > limit:
            raise ValueError("invalid curation receipt string: " + key)
    if (not ID.fullmatch(receipt["id"]) or
            not re.fullmatch(r"[a-zA-Z0-9_.-]{1,64}", receipt["scope"]) or
            receipt["scope"] in (".", "..")):
        raise ValueError("invalid curation identity or scope")
    for key in ("schema", "created_at", "journal_ops_before", "journal_bytes_before"):
        if type(receipt[key]) is not int or not 0 <= receipt[key] <= 2**63 - 1:
            raise ValueError("invalid curation receipt integer: " + key)
    for key, limit, minimum in (("sources", 256, 1), ("handles", 12, 0)):
        ids = receipt[key]
        if (not isinstance(ids, list) or not minimum <= len(ids) <= limit or
                any(not isinstance(item, str) or not ID.fullmatch(item) for item in ids) or
                len(set(ids)) != len(ids)):
            raise ValueError("invalid curation receipt IDs")
    counts, end, outcome = receipt["counts"], receipt["journal_ops_after"], receipt["outcome"]
    if outcome == "processed":
        if (not isinstance(counts, dict) or counts.keys() != {"applied", "rejected", "pending"} or
                any(type(n) is not int or not 0 <= n <= 2**63 - 1 for n in counts.values()) or
                type(end) is not int or not receipt["journal_ops_before"] <= end <= 2**63 - 1 or
                receipt["resolution_note"]):
            raise ValueError("invalid processed receipt")
    elif outcome in ("prepared", "interrupted_acknowledged"):
        if counts is not None or end is not None or bool(receipt["resolution_note"]) != (outcome != "prepared"):
            raise ValueError("invalid interrupted receipt")
    else:
        raise ValueError("unknown curation outcome")
    return receipt


def decode(data):
    header, separator, payload = data.partition(b"\n")
    fields = header.split(b" ")
    if (not separator or len(fields) != 10 or fields[:5] != [b"AEV2", b"1", b"0", b"5", b"0"] or
            not ID.fullmatch(fields[5].decode("ascii")) or fields[6] != b"0" or
            not payload.endswith(b"\n")):
        raise ValueError("invalid checked receipt frame")
    text = payload[:-1]
    prefix = b" ".join(fields[:8])
    if (len(text) > MAX_RECEIPT or fields[7] != str(len(text)).encode("ascii") or
            fields[8] != hashlib.sha256(prefix).hexdigest().encode("ascii") or
            fields[9] != hashlib.sha256(prefix + text).hexdigest().encode("ascii")):
        raise ValueError("receipt checksum or length mismatch")
    try:
        return validate(json.loads(text.decode("utf-8"), object_pairs_hook=unique))
    except (UnicodeError, RecursionError) as error:
        raise ValueError("invalid receipt text") from error


def encode(receipt):
    validate(receipt)
    text = json.dumps(receipt, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    prefix = f"AEV2 1 0 5 0 {uuid.uuid4()} 0 {len(text)}".encode("ascii")
    return (prefix + b" " + hashlib.sha256(prefix).hexdigest().encode("ascii") + b" " +
            hashlib.sha256(prefix + text).hexdigest().encode("ascii") + b"\n" + text + b"\n")


def read_pending(root):
    with opened(PENDING, root) as fd:
        before = os.fstat(fd)
        check_owner(before)
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                before.st_size > MAX_RECEIPT + 512 or mount_id(fd) != mount_id(root)):
            raise ValueError("invalid or oversized curation receipt file")
        data = bytearray()
        while block := os.read(fd, min(65536, MAX_RECEIPT + 513 - len(data))):
            data.extend(block)
            if len(data) > MAX_RECEIPT + 512:
                raise ValueError("curation receipt grew past its limit")
        if (identity(os.fstat(fd)) != identity(before) or len(data) != before.st_size or
                identity(os.stat(PENDING, dir_fd=root, follow_symlinks=False)) != identity(before)):
            raise ValueError("curation receipt changed")
        return decode(bytes(data))


def inspect_curation(root):
    receipt = read_pending(root)
    plan = inventory(root, exclude=(LOCK,))
    if read_pending(root) != receipt:
        raise ValueError("curation receipt changed during inspection")
    return {"snapshot": plan["snapshot"], "receipt": receipt,
            "state": "requires_reconciliation" if receipt["outcome"] == "prepared" else "completion_pending"}


def acknowledge(root, expected, note):
    plan = inspect_curation(root)
    if plan["snapshot"] != expected:
        raise ValueError("store snapshot changed; inspect curation again")
    receipt = plan["receipt"]
    if receipt["outcome"] != "prepared":
        raise ValueError("only an interrupted prepared batch requires acknowledgement")
    if not note.strip():
        raise ValueError("a reconciliation note is required")
    receipt.update(outcome="interrupted_acknowledged", resolution_note=note)
    data = encode(receipt)
    temporary = ".curation-" + str(uuid.uuid4()) + ".tmp"
    write_new(root, temporary, data)
    os.replace(temporary, PENDING, src_dir_fd=root, dst_dir_fd=root)
    os.fsync(root)
    return {"state": "completion_pending", "batch": receipt["id"],
            "effects_replayed": False, "source_requeued": False}
