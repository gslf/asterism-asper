"""Explicit whole-store erasure. A durable guard prevents later resurrection."""
import os
import uuid
from .files import (ERASE, LOCK, encoded, identity, inventory, mount_id, names_at,
                    opened, write_new)


def inspect_store(root):
    result = inventory(root, exclude=(LOCK,))
    paths = {row["path"] for row in result["entries"]}
    result["state"] = ("empty_blocked" if paths == {ERASE} else
                       "erasure_pending" if ERASE in paths else "offline")
    return result


def erase_store(root, expected):
    plan = inventory(root, exclude=(LOCK,))
    if plan["snapshot"] != expected:
        raise ValueError("store snapshot changed; inspect it again before erasing")
    operation = str(uuid.uuid4())
    # Write, sync, replace, sync. A crash before publication has erased nothing;
    # after publication libasper refuses to open, even without a manifest.
    temporary = ".erase-" + operation + ".tmp"
    write_new(root, temporary, encoded({"format": "asper-erasure", "version": 1,
              "operation": operation, "source_snapshot": expected}))
    os.replace(temporary, ERASE, src_dir_fd=root, dst_dir_fd=root)
    os.fsync(root)
    entries = {row["path"]: row for row in plan["entries"] if row["path"] != ERASE}
    children = {}
    for path in entries:
        parent, _, name = path.rpartition("/")
        children.setdefault(parent + "/" if parent else "", set()).add(name)
    removed = 0

    def prune(fd, prefix):
        nonlocal removed
        names = names_at(fd, files_limit)
        wanted = children.get(prefix, set())
        if not prefix:
            names -= {LOCK, ERASE}
        if names != wanted:
            raise ValueError("store directory changed during erasure; inspect before resuming")
        for name in sorted(names):
            path = prefix + name
            row = entries[path]
            current = identity(os.stat(name, dir_fd=fd, follow_symlinks=False))
            if row["kind"] == "directory":
                with opened(name, fd, directory=True) as child:
                    if (identity(os.fstat(child)) != current or current != row["identity"] or
                            mount_id(child) != mount_id(fd)):
                        raise ValueError("directory changed during erasure: " + path)
                    prune(child, path + "/")
                    if identity(os.stat(name, dir_fd=fd, follow_symlinks=False))[:2] != current[:2]:
                        raise ValueError("directory replaced during erasure: " + path)
                os.rmdir(name, dir_fd=fd)
            else:
                # Unlinking another hardlink changes nlink/ctime on this inode.
                same = (current[:5] + current[6:8] == row["identity"][:5] + row["identity"][6:8]
                        if row["kind"] == "hardlink" else current == row["identity"])
                if not same:
                    raise ValueError("entry changed during erasure: " + path)
                os.unlink(name, dir_fd=fd)
            removed += 1
            os.fsync(fd)

    files_limit = len(entries) + 2
    prune(root, "")
    if names_at(root, 2) != {LOCK, ERASE}:
        raise ValueError("unexpected entries remain after erasure")
    return {"erased": True, "operation": operation, "removed_entries": removed,
            "state": "empty_blocked"}
