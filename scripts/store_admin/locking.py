"""The same flock as libasper; paths and owner checks are host policy."""
import os
import stat
import sys
from contextlib import contextmanager
from .files import LOCK, ERASE, check_owner, directory, identity


def present(root, name):
    try:
        os.stat(name, dir_fd=root, follow_symlinks=False)
        return True
    except FileNotFoundError:
        return False


@contextmanager
def store(path, *, allow_erasing=False):
    if sys.platform != "linux":
        raise ValueError("offline store maintenance currently requires Linux with procfs")
    import fcntl
    with directory(path) as (absolute, root):
        check_owner(os.fstat(root))
        # Do not turn an arbitrary directory into a store by creating a lock.
        if not present(root, "manifest.xcdn") and not present(root, ERASE):
            raise ValueError("not an initialized Asper store")
        fd = os.open(LOCK, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW |
                     os.O_NONBLOCK | os.O_CLOEXEC, 0o600, dir_fd=root)
        try:
            st = os.fstat(fd)
            check_owner(st)
            if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1:
                raise ValueError("invalid store writer lock")
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise ValueError("store is busy; close its host before maintenance") from error
            if identity(os.stat(LOCK, dir_fd=root, follow_symlinks=False)) != identity(st):
                raise ValueError("writer lock changed")
            if present(root, ERASE) and not allow_erasing:
                raise ValueError("store erasure is pending or complete; inspect before resuming")
            os.fsync(root)
            yield absolute, root
        finally:
            # The inode stays linked so a second writer cannot bypass this lock.
            os.close(fd)
