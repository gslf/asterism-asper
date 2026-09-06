"""Descriptor-relative traversal and bounded, checked file copies on POSIX."""
import hashlib
import json
import os
import stat
from contextlib import contextmanager

MAX_ENTRIES = 65536
MAX_BYTES = 4 * 1024**3
MAX_FILE = 512 * 1024**2
MAX_DEPTH = 64
LOCK = ".writer.lock"
ERASE = ".erase.pending"


def encoded(value):
    return (json.dumps(value, ensure_ascii=True, sort_keys=True,
                       separators=(",", ":")) + "\n").encode("ascii")


def identity(st):
    # Access time changes during a read and is deliberately excluded.
    return [st.st_dev, st.st_ino, st.st_mode, st.st_uid, st.st_gid, st.st_nlink,
            st.st_size, st.st_mtime_ns, st.st_ctime_ns]


def check_owner(st):
    if st.st_uid != os.geteuid() or (not stat.S_ISLNK(st.st_mode) and st.st_mode & 0o022):
        raise ValueError("store entries must be owned by this user and not writable by others")


def mount_id(fd):
    # st_dev alone cannot distinguish a bind mount on the same filesystem.
    with open(f"/proc/self/fdinfo/{fd}", encoding="ascii") as info:
        for line in info:
            if line.startswith("mnt_id:"):
                return int(line.split()[1])
    raise ValueError("Linux mount identity is unavailable")


@contextmanager
def opened(name, parent=None, directory=False):
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
    if directory:
        flags |= os.O_DIRECTORY
    fd = os.open(name, flags, dir_fd=parent)
    try:
        yield fd
    finally:
        os.close(fd)


@contextmanager
def directory(path):
    """The caller may choose a root alias; resolve it once before traversal."""
    absolute = os.path.realpath(os.fspath(path), strict=True)
    with opened("/", directory=True) as root:
        fd = os.dup(root)
        try:
            for name in absolute.split("/")[1:]:
                if name:
                    child = os.open(name, os.O_RDONLY | os.O_DIRECTORY |
                                    os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
                    os.close(fd)
                    fd = child
            yield absolute, fd
        finally:
            os.close(fd)


def stream_file(parent, name, expected, destination=None):
    digest, count = hashlib.sha256(), 0
    with opened(name, parent) as fd:
        before = os.fstat(fd)
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                identity(before) != expected or mount_id(fd) != mount_id(parent)):
            raise ValueError("file changed or is aliased: " + name)
        if before.st_size > MAX_FILE:
            raise ValueError("file exceeds 512 MiB: " + name)
        while block := os.read(fd, 65536):
            count += len(block)
            if count > before.st_size:
                raise ValueError("file grew while reading: " + name)
            digest.update(block)
            if destination is not None:
                view = memoryview(block)
                while view:
                    written = os.write(destination, view)
                    if written <= 0:
                        raise OSError("short file write")
                    view = view[written:]
        after = os.stat(name, dir_fd=parent, follow_symlinks=False)
        if (count != before.st_size or identity(os.fstat(fd)) != expected or
                identity(after) != expected):
            raise ValueError("file changed while reading: " + name)
    return digest.hexdigest()


def inventory(root, *, exclude=()):
    """A checked physical inventory, not semantic validation of memory claims."""
    rows, byte_count, names_bytes = [], 0, 0
    root_stat = os.fstat(root)
    root_mount = mount_id(root)

    def walk(fd, prefix, depth):
        nonlocal byte_count, names_bytes
        before = os.fstat(fd)
        check_owner(before)
        if (before.st_dev != root_stat.st_dev or mount_id(fd) != root_mount or
                depth > MAX_DEPTH):
            raise ValueError("mounted directory or depth limit in store")
        with os.scandir(fd) as listing:
            for item in listing:
                if not prefix and item.name in exclude:
                    continue
                path = prefix + item.name
                names_bytes += len(path.encode("utf-8", errors="strict"))
                if len(rows) >= MAX_ENTRIES or names_bytes > 16 * 1024**2:
                    raise ValueError("store inventory limit exceeded")
                st = os.stat(item.name, dir_fd=fd, follow_symlinks=False)
                check_owner(st)
                row = {"path": path, "identity": identity(st), "bytes": 0}
                rows.append(row)
                if stat.S_ISDIR(st.st_mode):
                    row["kind"] = "directory"
                    with opened(item.name, fd, directory=True) as child:
                        if identity(os.fstat(child)) != identity(st):
                            raise ValueError("directory changed: " + path)
                        walk(child, path + "/", depth + 1)
                elif stat.S_ISREG(st.st_mode) and st.st_nlink == 1:
                    row.update(kind="file", bytes=st.st_size)
                    byte_count += st.st_size
                    if byte_count > MAX_BYTES:
                        raise ValueError("store exceeds 4 GiB maintenance limit")
                    row["sha256"] = stream_file(fd, item.name, row["identity"])
                elif stat.S_ISLNK(st.st_mode):
                    row["kind"] = "symlink"
                    row["sha256"] = hashlib.sha256(os.fsencode(
                        os.readlink(item.name, dir_fd=fd))).hexdigest()
                else:
                    # Erasure unlinks these entries; it never opens their data.
                    row["kind"] = "hardlink" if stat.S_ISREG(st.st_mode) else "special"
                if identity(os.stat(item.name, dir_fd=fd, follow_symlinks=False)) != row["identity"]:
                    raise ValueError("entry changed: " + path)
        if identity(os.fstat(fd)) != identity(before):
            raise ValueError("directory changed during inventory")

    walk(root, "", 0)
    rows.sort(key=lambda row: row["path"])
    result = {"schema": 1, "root": identity(root_stat), "entries": rows,
              "bytes": byte_count}
    result["snapshot"] = hashlib.sha256(encoded(result)).hexdigest()
    return result


def write_new(parent, name, data):
    fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC |
                 os.O_NOFOLLOW, 0o600, dir_fd=parent)
    try:
        with os.fdopen(fd, "wb", closefd=False) as f:
            for block in (data,) if isinstance(data, bytes) else data:
                if f.write(block) != len(block):
                    raise OSError("short metadata write")
            f.flush()
            os.fsync(fd)
    finally:
        os.close(fd)
    os.fsync(parent)


def names_at(fd, limit=MAX_ENTRIES):
    names = set()
    with os.scandir(fd) as entries:
        for entry in entries:
            if len(names) >= limit:
                raise ValueError("directory entry limit exceeded")
            names.add(entry.name)
    return names
