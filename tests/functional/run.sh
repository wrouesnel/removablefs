#!/usr/bin/env bash
# Functional tests for ownfs + udisks2, run as root inside a test VM.
#
# Usage: run.sh <block device> <fstype>...
#
# For each filesystem type: format the device as a drive "from another
# machine" (root directory and files owned by uid 1000, which doesn't exist
# here, plus files owned by root and by uid 1234), mount it through udisks2
# as the desktop user alice, exercise it, then check what landed on disk.
#
# Needs users alice (1001) and bob (1002), and polkit letting them use
# udisks2 from an SSH session (tests/vm/provision.sh sets that up).
set -uo pipefail

DEV="${1:?usage: run.sh <device> <fstype>...}"
shift
FSTYPES=("$@")
[[ ${#FSTYPES[@]} -gt 0 ]] || FSTYPES=(ext4)

RAW=/mnt/ownfs-raw
FAILS=0
PASSES=0
SKIPS=0
CUR=""

pass() { PASSES=$((PASSES + 1)); printf '  ok    %s\n' "$*"; }
fail() { FAILS=$((FAILS + 1)); printf '  FAIL  %s\n' "$*"; }
skip() { SKIPS=$((SKIPS + 1)); printf '  skip  %s\n' "$*"; }
check() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then pass "$d"; else fail "$d"; fi; }
check_not() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then fail "$d"; else pass "$d"; fi; }
as() { local u="$1"; shift; runuser -u "$u" -- "$@"; }
check_as() { local d="$1" u="$2"; shift 2; check "$d" runuser -u "$u" -- "$@"; }
check_not_as() { local d="$1" u="$2"; shift 2; check_not "$d" runuser -u "$u" -- "$@"; }

have() { command -v "$1" >/dev/null 2>&1; }

mkfs_dev() {
    local t="$1"
    wipefs -aq "$DEV"
    case "$t" in
        ext2|ext3|ext4) "mkfs.$t" -q -F -L "OWN_$t" "$DEV" ;;
        xfs) mkfs.xfs -q -f -L "OWN_$t" "$DEV" ;;
        btrfs) mkfs.btrfs -q -f -L "OWN_$t" "$DEV" ;;
        f2fs) mkfs.f2fs -q -f -l "OWN_$t" "$DEV" ;;
        *) return 1 ;;
    esac
}

fsck_dev() {
    case "$1" in
        ext2|ext3|ext4) e2fsck -fn "$DEV" ;;
        xfs) xfs_repair -n "$DEV" ;;
        btrfs) btrfs check --readonly "$DEV" ;;
        f2fs) fsck.f2fs "$DEV" ;;
    esac
}

# Lay out a drive as another machine's user (uid 1000) would leave it.
populate() {
    mkdir -p "$RAW"
    mount "$DEV" "$RAW" || return 1
    chown 1000:1000 "$RAW"
    chmod 0755 "$RAW"
    install -d -o 1000 -g 1000 -m 0700 "$RAW/private"
    printf 'secret\n' > "$RAW/private/notes.txt"
    chown 1000:1000 "$RAW/private/notes.txt"; chmod 0600 "$RAW/private/notes.txt"
    install -d -o 1234 -g 1234 -m 0700 "$RAW/stranger"
    printf 'theirs\n' > "$RAW/stranger/file"; chown 1234:1234 "$RAW/stranger/file"
    printf 'root\n' > "$RAW/rootfile"; chmod 0644 "$RAW/rootfile"
    printf '#!/bin/sh\necho ran\n' > "$RAW/script.sh"
    chown 1000:1000 "$RAW/script.sh"; chmod 0755 "$RAW/script.sh"
    dd if=/dev/urandom of="$RAW/blob" bs=1M count=8 status=none
    chown 1000:1000 "$RAW/blob"
    sha256sum "$RAW/blob" | cut -d' ' -f1 > /tmp/ownfs-blob.sha
    umount "$RAW"
}

udisks_mount() {
    local out i
    # udisks2 re-probes a freshly formatted device asynchronously.
    for i in $(seq 1 20); do
        out="$(as alice udisksctl mount -b "$DEV" 2>&1)" && break
        grep -q 'not a mountable filesystem' <<<"$out" || break
        sleep 0.5
    done
    if [[ "$out" != Mounted* ]]; then
        echo "$out" >&2
        udisksctl info -b "$DEV" | grep -E 'IdType|IdUsage|UDisks2.Filesystem' >&2
        udevadm info "$DEV" | grep -E 'ID_FS_TYPE|ID_FS_USAGE|OWNFS' >&2
        return 1
    fi
    # "Mounted /dev/sdb at /media/alice/LABEL"
    MP="${out##* at }"; MP="${MP%.}"
    [[ -d "$MP" ]]
}

test_fstype() {
    local t="$1" mp
    CUR="$t"
    echo "== $t"

    if ! mkfs_dev "$t" >/dev/null 2>&1; then
        skip "$t: cannot create filesystem (tools or kernel support missing)"
        return
    fi
    if ! populate; then
        skip "$t: kernel cannot mount it natively"
        return
    fi
    # Back-to-back mkfs/mount/umount can leave udev's last probe stale; a
    # fresh change event stands in for plugging the prepared drive in.
    udevadm settle
    udevadm trigger -w --action=change "$DEV" 2>/dev/null || udevadm trigger --action=change "$DEV"
    udevadm settle

    # --- mount through udisks2 -----------------------------------------
    if ! udisks_mount; then
        fail "$t: udisksctl mount as alice"
        return
    fi
    pass "$t: udisksctl mount as alice at $MP"
    mp="$MP"

    check "$t: mounted with ownfs" test "$(findmnt -no FSTYPE "$mp")" = ownfs
    check "$t: nosuid,nodev from udisks" bash -c "findmnt -no OPTIONS '$mp' | grep -q nosuid && findmnt -no OPTIONS '$mp' | grep -q nodev"
    check "$t: udisks reports the mount point" bash -c "udisksctl info -b '$DEV' | grep -q 'MountPoints: *$mp'"
    check "$t: mountinfo device matches the drive" test \
        "$(findmnt -no MAJ:MIN "$mp" | tr -d ' ')" = "$(lsblk -dno MAJ:MIN "$DEV" | tr -d ' ')"
    if [[ "$t" == btrfs ]]; then
        skip "$t: st_dev is btrfs's per-subvolume device"
    else
        check "$t: stat st_dev matches the drive" test \
            "$(python3 -c 'import os, sys; d = os.stat(sys.argv[1]).st_dev; print("%d:%d" % (os.major(d), os.minor(d)))' "$mp")" = \
            "$(lsblk -dno MAJ:MIN "$DEV" | tr -d ' ')"
    fi

    # --- ownership as presented -----------------------------------------
    check "$t: root dir owned by alice" test "$(stat -c %U:%G "$mp")" = alice:alice
    check "$t: every file owned by alice" bash -c \
        "test -z \"\$(find '$mp' ! -user alice -o ! -group alice | grep -v lost+found)\""
    check_as "$t: alice reads uid-1000 0600 file" alice cat "$mp/private/notes.txt"
    check_as "$t: alice reads uid-1234 file in 0700 dir" alice cat "$mp/stranger/file"
    check_as "$t: alice writes into uid-1234 0700 dir" alice touch "$mp/stranger/mine"
    check_as "$t: alice appends to root-owned file" alice bash -c "echo more >> '$mp/rootfile'"
    check "$t: blob intact" test "$(as alice sha256sum "$mp/blob" | cut -d' ' -f1)" = "$(cat /tmp/ownfs-blob.sha)"
    if findmnt -no OPTIONS "$mp" | grep -q noexec; then
        skip "$t: exec (mounted noexec)"
    else
        check "$t: alice runs a script from the drive" test "$(as alice "$mp/script.sh")" = ran
    fi

    # --- namespace operations ---------------------------------------------
    check_as "$t: create file" alice bash -c "echo hello > '$mp/new.txt'"
    check_as "$t: mkdir -p" alice mkdir -p "$mp/a/b/c"
    check_as "$t: rename file across dirs" alice mv "$mp/new.txt" "$mp/a/b/moved.txt"
    check_as "$t: rename dir" alice mv "$mp/a/b" "$mp/a/bb"
    check_as "$t: rename over existing file" alice bash -c "echo x > '$mp/t1' && echo y > '$mp/t2' && mv '$mp/t1' '$mp/t2' && test \"\$(cat '$mp/t2')\" = x && test ! -e '$mp/t1'"
    check_as "$t: RENAME_EXCHANGE" alice bash -c "echo 1 > '$mp/x1' && echo 2 > '$mp/x2' && python3 -c '
import ctypes, os, sys
libc = ctypes.CDLL(None, use_errno=True)
AT_FDCWD = -100
if libc.renameat2(AT_FDCWD, sys.argv[1].encode(), AT_FDCWD, sys.argv[2].encode(), 2) != 0:
    sys.exit(os.strerror(ctypes.get_errno()))
' '$mp/x1' '$mp/x2' && test \"\$(cat '$mp/x1')\" = 2 && test \"\$(cat '$mp/x2')\" = 1"
    check_as "$t: hard link shares inode" alice bash -c "ln '$mp/a/bb/moved.txt' '$mp/hl' && test \"\$(stat -c %i '$mp/hl')\" = \"\$(stat -c %i '$mp/a/bb/moved.txt')\" && test \"\$(stat -c %h '$mp/hl')\" = 2"
    check_as "$t: symlink" alice bash -c "ln -s a/bb/moved.txt '$mp/sl' && test \"\$(cat '$mp/sl')\" = hello && test \"\$(readlink '$mp/sl')\" = a/bb/moved.txt"
    check_as "$t: unlink one hard link" alice bash -c "rm '$mp/hl' && test \"\$(stat -c %h '$mp/a/bb/moved.txt')\" = 1"
    check_as "$t: mkfifo" alice mkfifo "$mp/fifo"
    check_not_as "$t: mknod device refused" alice mknod "$mp/dev" c 1 3
    check_as "$t: rmdir non-empty fails, empty succeeds" alice bash -c "! rmdir '$mp/a' 2>/dev/null && rmdir '$mp/a/bb/c' "
    check_as "$t: rm -r" alice rm -r "$mp/a"
    check_as "$t: deep tree create and remove" alice bash -c "cd '$mp' && for i in \$(seq 1 200); do mkdir -p tree/d\$((i % 10))/e\$i && echo \$i > tree/d\$((i % 10))/e\$i/f; done && test \"\$(find tree -type f | wc -l)\" = 200 && rm -rf tree"
    check_as "$t: readdir of 1000 entries" alice bash -c "cd '$mp' && mkdir many && for i in \$(seq 1 1000); do : > many/f\$i; done && test \"\$(ls many | wc -l)\" = 1000 && rm -rf many"

    # --- data paths --------------------------------------------------------
    check_as "$t: write/read 32MiB" alice bash -c "dd if=/dev/urandom of='$mp/big' bs=1M count=32 status=none && cmp <(cat '$mp/big') '$mp/big' && test \"\$(stat -c %s '$mp/big')\" = 33554432"
    check_as "$t: truncate shrink/grow" alice bash -c "truncate -s 1000 '$mp/big' && test \"\$(stat -c %s '$mp/big')\" = 1000 && truncate -s 5M '$mp/big' && test \"\$(stat -c %s '$mp/big')\" = 5242880"
    check_as "$t: O_TRUNC on open" alice bash -c "echo short > '$mp/big' && test \"\$(cat '$mp/big')\" = short"
    check_as "$t: append" alice bash -c "echo a > '$mp/ap' && echo b >> '$mp/ap' && test \"\$(cat '$mp/ap')\" = \$'a\nb'"
    if [[ "$t" == ext2 || "$t" == ext3 ]]; then
        check_as "$t: fallocate unsupported, like the native fs" alice bash -c "! fallocate -l 4M '$mp/fa' 2>/dev/null"
    else
        check_as "$t: fallocate" alice bash -c "fallocate -l 4M '$mp/fa' && test \"\$(stat -c %s '$mp/fa')\" = 4194304"
    fi
    check_as "$t: mmap shared write is persisted" alice python3 -c "
import mmap, os
p = '$mp/mm'
with open(p, 'wb') as f: f.write(b'\0' * 8192)
with open(p, 'r+b') as f:
    m = mmap.mmap(f.fileno(), 8192)
    m[100:105] = b'hello'
    m.flush(); m.close()
with open(p, 'rb') as f:
    d = f.read()
assert d[100:105] == b'hello', d[95:110]
"
    check_as "$t: O_DIRECT read" alice dd if="$mp/blob" of=/dev/null bs=4096 count=16 iflag=direct status=none
    check_as "$t: sendfile/copy_file_range (cp)" alice bash -c "cp '$mp/blob' '$mp/blob2' && cmp '$mp/blob' '$mp/blob2'"
    check_as "$t: fsync" alice python3 -c "import os; fd = os.open('$mp/blob2', os.O_RDWR); os.fsync(fd); os.fdatasync(fd); os.close(fd)"
    check_as "$t: seek SEEK_END" alice python3 -c "
import os; fd = os.open('$mp/blob2', os.O_RDONLY); assert os.lseek(fd, 0, os.SEEK_END) == 8 << 20; os.close(fd)"
    check_as "$t: utimes" alice bash -c "touch -d '2001-02-03 04:05:06' '$mp/ap' && test \"\$(stat -c %Y '$mp/ap')\" = \"\$(date -d '2001-02-03 04:05:06' +%s)\""
    check_as "$t: statfs" alice df "$mp"

    # --- metadata policy -----------------------------------------------------
    check_as "$t: chmod" alice bash -c "chmod 0640 '$mp/ap' && test \"\$(stat -c %a '$mp/ap')\" = 640"
    check_as "$t: setuid bit stripped" alice bash -c "chmod 4755 '$mp/ap' && test \"\$(stat -c %a '$mp/ap')\" = 755"
    check_as "$t: setgid on file stripped" alice bash -c "chmod 2755 '$mp/ap' && test \"\$(stat -c %a '$mp/ap')\" = 755"
    check_as "$t: setgid on dir kept" alice bash -c "mkdir '$mp/sg' && chmod 2775 '$mp/sg' && test \"\$(stat -c %a '$mp/sg')\" = 2775"
    check_as "$t: chown to self succeeds" alice chown alice:alice "$mp/ap"
    check_not_as "$t: chown to bob refused" alice chown bob "$mp/ap"
    check_not "$t: root chown to bob refused" chown bob "$mp/ap"
    if as alice setfattr -n user.k -v v "$mp/ap" 2>/dev/null; then
        check_as "$t: user xattr round trip" alice bash -c "test \"\$(getfattr --only-values -n user.k '$mp/ap')\" = v"
        check_as "$t: listxattr shows user xattr" alice bash -c "getfattr -d '$mp/ap' | grep -q user.k"
        check_as "$t: remove user xattr" alice setfattr -x user.k "$mp/ap"
    else
        skip "$t: user xattrs not supported by lower fs"
    fi
    check_not "$t: trusted xattr refused even for root" setfattr -n trusted.k -v v "$mp/ap"
    check_not_as "$t: POSIX ACL refused" alice setfacl -m u:bob:rw "$mp/ap"

    # --- unmount and inspect the disk ------------------------------------------
    sync
    check "$t: udisksctl unmount as alice" as alice udisksctl unmount -b "$DEV"
    check "$t: device no longer mounted anywhere" bash -c "! grep -q '^$DEV ' /proc/mounts"
    check "$t: fsck clean" fsck_dev "$t"

    mount "$DEV" "$RAW"
    check "$t: new files owned by disk owner 1000 on disk" bash -c \
        "test \"\$(stat -c %u:%g '$RAW/ap' '$RAW/stranger/mine' '$RAW/blob2' '$RAW/sg' | sort -u)\" = 1000:1000"
    check "$t: existing owners untouched" test "$(stat -c %u "$RAW/stranger/file") $(stat -c %u "$RAW/rootfile")" = "1234 0"
    check "$t: no setuid/setgid files on disk" bash -c "test -z \"\$(find '$RAW' -type f -perm /6000)\""
    check "$t: data written through ownfs readable natively" bash -c "cmp '$RAW/blob' '$RAW/blob2' && test \"\$(cat '$RAW/rootfile')\" = \$'root\nmore'"
    umount "$RAW"

    # --- other users, on a mount bob can reach (/media/alice is private) -------
    mkdir -p /mnt/ownfs-shared
    if mount -t ownfs -o "uid=1001,gid=1001,lower=$t" "$DEV" /mnt/ownfs-shared; then
        mp=/mnt/ownfs-shared
        check_as "$t: bob reads a world-readable file" bob cat "$mp/rootfile"
        check_not_as "$t: bob cannot create in alice's 0755 root" bob touch "$mp/bob"
        check_not_as "$t: bob cannot read a 0600 file" bob cat "$mp/private/notes.txt"
        check_not_as "$t: bob cannot chmod" bob chmod 777 "$mp/rootfile"
        check_not_as "$t: bob cannot delete" bob rm -f "$mp/rootfile"
        check "$t: root can still do everything" bash -c "touch '$mp/rootmade' && rm '$mp/rootmade'"
        umount /mnt/ownfs-shared
    else
        fail "$t: manual mount -t ownfs"
    fi
}

# Surprise removal is tested by the host harness (it has to unplug).

dmesg -C
for t in "${FSTYPES[@]}"; do
    test_fstype "$t"
done

echo "== kernel log"
if dmesg | grep -E -q 'WARNING:|BUG:|Oops|general protection|KASAN|lockdep|possible recursive locking|circular locking'; then
    fail "kernel log has warnings"
    dmesg | grep -E -B5 -A30 'WARNING:|BUG:|Oops|general protection|KASAN|circular locking|possible recursive' | head -100
else
    pass "kernel log clean"
fi

echo "== $PASSES passed, $FAILS failed, $SKIPS skipped"
[[ $FAILS -eq 0 ]]
