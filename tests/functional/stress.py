#!/usr/bin/env python3
"""Concurrent filesystem stress for ownfs.

Usage: stress.py <directory> [seconds] [workers]

Workers run random namespace and data operations over a small shared set of
names, so operations race on the same files and directories: create, write,
append, truncate, mmap, rename (plain, RENAME_NOREPLACE and RENAME_EXCHANGE),
hard link, symlink, unlink, mkdir, rmdir, readdir, stat, chmod, utime,
fsync and xattrs.  Errors that racing operations legitimately produce
(ENOENT, EEXIST, ENOTEMPTY, ...) are expected; anything else is reported and
fails the run.  Afterwards the tree is checked to be walkable and is removed.
"""
import ctypes
import errno
import mmap
import os
import random
import sys
import time
import traceback
import multiprocessing

EXPECTED = {
    errno.ENOENT, errno.EEXIST, errno.ENOTEMPTY, errno.ENOTDIR, errno.EISDIR,
    errno.EINVAL,   # rename of a dir into itself, mmap of an empty file
    errno.EBUSY, errno.EMLINK, errno.ENODATA, errno.ELOOP,
    errno.EACCES,   # random chmods take the search bit off directories
    errno.ENOSPC,   # a fast VM fills a small test disk
}

libc = ctypes.CDLL(None, use_errno=True)
AT_FDCWD = -100
RENAME_NOREPLACE, RENAME_EXCHANGE = 1, 2


def renameat2(a, b, flags):
    if libc.renameat2(AT_FDCWD, a.encode(), AT_FDCWD, b.encode(), flags) != 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e))


def worker(root, seconds, seed, out):
    rnd = random.Random(seed)
    dirs = [root] + [os.path.join(root, f"d{i}") for i in range(4)] + \
           [os.path.join(root, f"d{i}", f"e{j}") for i in range(2) for j in range(2)]
    names = [f"f{i}" for i in range(12)]

    def path():
        return os.path.join(rnd.choice(dirs), rnd.choice(names))

    def op_create():
        with open(path(), "wb") as f:
            f.write(os.urandom(rnd.randint(0, 65536)))

    def op_append():
        with open(path(), "ab") as f:
            f.write(b"x" * rnd.randint(1, 4096))

    def op_read():
        with open(path(), "rb") as f:
            f.read()

    def op_truncate():
        os.truncate(path(), rnd.randint(0, 131072))

    # mmap uses a file of this worker's own: touching a mapping that
    # another worker has truncated raises SIGBUS, correctly, but fatally.
    mmfile = os.path.join(root, f"mm{seed}")

    def op_mmap():
        size = rnd.randint(1, 16) * 4096
        fd = os.open(mmfile, os.O_RDWR | os.O_CREAT, 0o644)
        try:
            os.ftruncate(fd, size)
            m = mmap.mmap(fd, size)
            m[size - 1:size] = b"m"
            m.flush()
            m.close()
        finally:
            os.close(fd)

    def op_rename():
        renameat2(path(), path(), rnd.choice([0, 0, RENAME_NOREPLACE, RENAME_EXCHANGE]))

    def op_rename_dir():
        renameat2(rnd.choice(dirs[1:]), os.path.join(rnd.choice(dirs), rnd.choice(names)),
                  rnd.choice([0, RENAME_NOREPLACE]))

    def op_link():
        os.link(path(), path())

    def op_symlink():
        os.symlink(rnd.choice(names), path())

    def op_unlink():
        os.unlink(path())

    def op_mkdir():
        os.mkdir(rnd.choice(dirs[1:]))

    def op_rmdir():
        os.rmdir(rnd.choice(dirs[1:]))

    def op_readdir():
        d = rnd.choice(dirs)
        for e in os.scandir(d):
            e.stat(follow_symlinks=False)

    def op_stat():
        os.lstat(path())

    def op_chmod():
        os.chmod(path(), rnd.choice([0o600, 0o644, 0o755, 0o4755]))

    def op_utime():
        os.utime(path(), (rnd.randint(0, 2**31), rnd.randint(0, 2**31)))

    def op_fsync():
        fd = os.open(path(), os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)

    def op_xattr():
        p = path()
        if rnd.random() < 0.5:
            os.setxattr(p, "user.stress", os.urandom(rnd.randint(1, 64)))
        else:
            os.listxattr(p)
            try:
                os.removexattr(p, "user.stress")
            except OSError as e:
                if e.errno != errno.ENODATA:
                    raise

    ops = [op_create, op_append, op_read, op_truncate, op_mmap, op_rename,
           op_rename_dir, op_link, op_symlink, op_unlink, op_mkdir, op_rmdir,
           op_readdir, op_stat, op_chmod, op_utime, op_fsync, op_xattr]
    counts = 0
    unexpected = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        op = rnd.choice(ops)
        try:
            op()
        except OSError as e:
            # Names get reused for directories, which can't be hard linked.
            if e.errno not in EXPECTED and not (op is op_link and e.errno == errno.EPERM):
                unexpected.append(f"{op.__name__}: {e}")
        except Exception:
            unexpected.append(f"{op.__name__}: {traceback.format_exc()}")
        counts += 1
    out.put((counts, unexpected[:20]))


def main():
    root = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60
    nworkers = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    root = os.path.join(root, "stress")
    os.makedirs(root, exist_ok=True)
    for i in range(4):
        os.makedirs(os.path.join(root, f"d{i}"), exist_ok=True)

    # Plain fork: Python 3.14's default forkserver changes into the
    # parent's working directory, which the test user may not be able to.
    mp = multiprocessing.get_context("fork")
    q = mp.Queue()
    procs = [mp.Process(target=worker, args=(root, seconds, i, q)) for i in range(nworkers)]
    for p in procs:
        p.start()
    results = []
    for _ in procs:
        try:
            results.append(q.get(timeout=seconds + 300))
        except Exception:
            break
    for p in procs:
        p.join(timeout=60)
    dead = [p.exitcode for p in procs if p.exitcode != 0]
    if dead or len(results) != len(procs):
        print(f"stress: workers died or hung: exit codes {[p.exitcode for p in procs]}")
        sys.exit(1)

    total = sum(c for c, _ in results)
    bad = [u for _, us in results for u in us]
    print(f"stress: {total} operations by {nworkers} workers in {seconds:.0f}s")

    if os.environ.get("STRESS_KEEP"):
        print(f"stress: leaving {root} in place")
        sys.exit(1 if bad else 0)

    # The tree must still be walkable and removable.  Random chmods may
    # have left directories unsearchable, as they would anywhere.
    for dirpath, dirnames, filenames in os.walk(root):
        for d in dirnames:
            p = os.path.join(dirpath, d)
            if not os.path.islink(p):
                os.chmod(p, 0o755)
    n = 0
    for dirpath, dirnames, filenames in os.walk(root):
        for f in filenames:
            os.lstat(os.path.join(dirpath, f))
            n += 1
    for dirpath, dirnames, filenames in os.walk(root, topdown=False):
        for f in filenames:
            os.unlink(os.path.join(dirpath, f))
        for d in dirnames:
            p = os.path.join(dirpath, d)
            if os.path.islink(p):
                os.unlink(p)
            else:
                os.rmdir(p)
    os.rmdir(root)
    print(f"stress: walked and removed {n} files")

    if bad:
        print("stress: unexpected errors:")
        for u in bad:
            print("  " + u)
        sys.exit(1)


if __name__ == "__main__":
    main()
