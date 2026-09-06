"""Inspect a suspended source batch; acknowledge partial effects without replay."""
import json
import os
import re
import uuid
from .files import LOCK, inventory, write_new
from .frames import ID, unique, checked_decode, checked_encode, checked_read

PENDING = "curation.pending"
MAX_RECEIPT = 1024 * 1024


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
    try:
        return validate(json.loads(checked_decode(data, MAX_RECEIPT), object_pairs_hook=unique))
    except (UnicodeError, RecursionError) as error:
        raise ValueError("invalid receipt text") from error


def encode(receipt):
    text = json.dumps(validate(receipt), ensure_ascii=False, separators=(",", ":"))
    if len(text.encode("utf-8")) > MAX_RECEIPT:
        raise ValueError("curation receipt exceeds its limit")
    return checked_encode(text)


def read_pending(root):
    try:
        return validate(json.loads(checked_read(root, PENDING, MAX_RECEIPT), object_pairs_hook=unique))
    except (UnicodeError, RecursionError) as error:
        raise ValueError("invalid receipt text") from error


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
