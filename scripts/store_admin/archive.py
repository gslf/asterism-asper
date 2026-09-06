"""Portable file inventory with a completion manifest, never an executable archive."""
import json
import os
import stat
from contextlib import contextmanager
from itertools import chain
from .files import (LOCK, MAX_ENTRIES, check_owner, directory, encoded, identity,
                    inventory, mount_id, names_at, opened, stream_file, write_new)

MANIFEST = "export.jsonl"
MAX_LINE = 128 * 1024
MAX_MANIFEST = 128 * 1024**2


@contextmanager
def parent_at(root, relative):
    parts = relative.split("/")
    if any(p in ("", ".", "..") for p in parts) or "\0" in relative:
        raise ValueError("invalid relative archive path")
    fd = os.dup(root)
    try:
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                            os.O_CLOEXEC, dir_fd=fd)
            os.close(fd)
            fd = child
        yield fd, parts[-1]
    finally:
        os.close(fd)


def content_rows(rows):
    return [{k: v for k, v in row.items() if k != "identity"} for row in rows]


def export_store(root, absolute, destination):
    before = inventory(root, exclude=(LOCK,))
    if any(row["kind"] not in ("file", "directory") for row in before["entries"]):
        raise ValueError("export refuses aliases and special files")
    target = os.path.abspath(os.fspath(destination))
    with directory(os.path.dirname(target)) as (parent_path, parent):
        check_owner(os.fstat(parent))
        resolved = os.path.join(parent_path, os.path.basename(target))
        source_dirs = {tuple(before["root"][:2])} | {
            tuple(row["identity"][:2]) for row in before["entries"] if row["kind"] == "directory"}
        if (os.path.commonpath((absolute, resolved)) == absolute or
                tuple(identity(os.fstat(parent))[:2]) in source_dirs):
            raise ValueError("export destination must be outside the store")
        # Refuse any existing destination, including empty directories or links.
        os.mkdir(os.path.basename(target), mode=0o700, dir_fd=parent)
        os.fsync(parent)
        with opened(os.path.basename(target), parent, directory=True) as output:
            os.mkdir("files", mode=0o700, dir_fd=output)
            with opened("files", output, directory=True) as files:
                for row in before["entries"]:
                    with parent_at(files, row["path"]) as (dst, name):
                        if row["kind"] == "directory":
                            os.mkdir(name, mode=0o700, dir_fd=dst)
                        else:
                            fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                                         os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=dst)
                            try:
                                with parent_at(root, row["path"]) as (src, leaf):
                                    digest = stream_file(src, leaf, row["identity"], fd)
                                if digest != row["sha256"]:
                                    raise ValueError("source changed during export")
                                os.fsync(fd)
                            finally:
                                os.close(fd)
                        os.fsync(dst)
                if content_rows(inventory(files)["entries"]) != content_rows(before["entries"]):
                    raise ValueError("export copy did not verify")
                os.fsync(files)
            if inventory(root, exclude=(LOCK,))["snapshot"] != before["snapshot"]:
                raise ValueError("store changed during export")
            manifest = {"format": "asper-store-export", "version": 1,
                        "source_snapshot": before["snapshot"],
                        "entries": len(before["entries"])}
            # Incomplete exports remain inspectable and are never auto-restored.
            def lines():
                total = 0
                for row in chain((manifest,), content_rows(before["entries"])):
                    line = encoded(row)
                    total += len(line)
                    if len(line) > MAX_LINE or total > MAX_MANIFEST:
                        raise ValueError("export manifest limit exceeded")
                    yield line
            write_new(output, ".export.tmp", lines())
            os.replace(".export.tmp", MANIFEST, src_dir_fd=output, dst_dir_fd=output)
            os.fsync(output)
    return {"destination": resolved, "snapshot": before["snapshot"],
            "entries": len(before["entries"]), "bytes": before["bytes"]}


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate archive metadata key")
        result[key] = value
    return result


def verify_export(path):
    with directory(path) as (_, root):
        check_owner(os.fstat(root))
        if names_at(root, 2) != {MANIFEST, "files"}:
            raise ValueError("incomplete export or unexpected archive entries")
        st = os.stat(MANIFEST, dir_fd=root, follow_symlinks=False)
        check_owner(st)
        if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1 or st.st_size > MAX_MANIFEST:
            raise ValueError("invalid export manifest file or size")
        with opened(MANIFEST, root) as fd:
            if identity(os.fstat(fd)) != identity(st) or mount_id(fd) != mount_id(root):
                raise ValueError("export manifest changed or is mounted")
            with os.fdopen(os.dup(fd), "rb") as f:
                manifest = read_line(f)
                if (not isinstance(manifest, dict) or
                        set(manifest) != {"format", "version", "source_snapshot", "entries"} or
                        manifest["format"] != "asper-store-export" or type(manifest["version"]) is not int or
                        manifest["version"] != 1 or type(manifest["entries"]) is not int or
                        not 0 <= manifest["entries"] <= MAX_ENTRIES):
                    raise ValueError("unsupported export manifest")
                snapshot = manifest["source_snapshot"]
                if not isinstance(snapshot, str) or len(snapshot) != 64 or any(c not in "0123456789abcdef" for c in snapshot):
                    raise ValueError("invalid source snapshot")
                with opened("files", root, directory=True) as files:
                    actual = inventory(files)
                if len(actual["entries"]) != manifest["entries"]:
                    raise ValueError("archive entry count does not match")
                for row in content_rows(actual["entries"]):
                    if row["kind"] not in ("file", "directory") or read_line(f) != row:
                        raise ValueError("archive content does not match its manifest")
                if f.read(1):
                    raise ValueError("unexpected trailing export metadata")
            if (identity(os.fstat(fd)) != identity(st) or
                    identity(os.stat(MANIFEST, dir_fd=root, follow_symlinks=False)) != identity(st)):
                raise ValueError("export manifest changed during verification")
    return {"verified": True, "snapshot": snapshot, "entries": len(actual["entries"]),
            "bytes": actual["bytes"]}


def read_line(f):
    line = f.readline(MAX_LINE + 1)
    if len(line) > MAX_LINE or not line.endswith(b"\n"):
        raise ValueError("missing or oversized export metadata line")
    try:
        return json.loads(line, object_pairs_hook=unique)
    except RecursionError as error:
        raise ValueError("export metadata nesting limit exceeded") from error
