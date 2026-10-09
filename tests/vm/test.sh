#!/usr/bin/env bash
# Build removablefs in a test VM, install it with the udev rule, plug in a USB
# disk and run the functional suite, then a surprise-removal test.
#
# Usage: DISTRO=<distro> test.sh [fstype...]    (default: all supported)
#        STRESS=<seconds> ...  also run tests/functional/stress.py for that
#                         long on ext4 and xfs (default 60; 0 skips it)
#        MODE=package ... build the distribution's packages (deb or rpm) in
#                         the VM and install those, DKMS and all, instead
#                         of the module from the source tree
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOP="$(cd "${HERE}/../.." && pwd)"
V="${HERE}/vm.sh"
DISTRO="${DISTRO:-noble}"
export DISTRO
FSTYPES=("$@")
[[ ${#FSTYPES[@]} -gt 0 ]] || FSTYPES=(ext2 ext3 ext4 xfs btrfs f2fs)
DISK="ftest-${DISTRO}"

log() { printf '>>> [%s] %s\n' "$DISTRO" "$*"; }

"$V" up >/dev/null

"${HERE}/install.sh"

"$V" ssh 'rm -rf removablefs-tests && mkdir removablefs-tests'
"$V" scp "${TOP}/tests/functional/run.sh" removablefs-tests/run.sh >/dev/null
"$V" scp "${TOP}/tests/functional/stress.py" removablefs-tests/stress.py >/dev/null

"$V" unplug "$DISK" 2>/dev/null || true
"$V" mkdisk "$DISK" 512M >/dev/null
"$V" plug "$DISK"
dev=""
for _ in $(seq 1 30); do
    dev="$("$V" ssh "readlink -f /dev/disk/by-id/usb-*USB-${DISK}-0:0 2>/dev/null" || true)"
    [[ "$dev" == /dev/sd* ]] && break
    sleep 1
done
[[ "$dev" == /dev/sd* ]] || { log "USB disk did not appear"; exit 1; }
log "USB disk is $dev; running functional tests (${FSTYPES[*]})"

rc=0
"$V" ssh "sudo bash removablefs-tests/run.sh $dev ${FSTYPES[*]}" || rc=$?

# Concurrency: racing workers on a udisks2 mount, then fsck and the log.
STRESS="${STRESS:-60}"
if [[ "$STRESS" -gt 0 ]]; then
    for fs in ext4 xfs; do
        log "stress: $fs for ${STRESS}s"
        if "$V" ssh "set -e
            sudo dmesg -C
            sudo wipefs -aq $dev; sudo mkfs.$fs -q -f $dev 2>/dev/null || sudo mkfs.$fs -q -F $dev
            sudo udevadm settle; sudo udevadm trigger -w --action=change $dev 2>/dev/null || sudo udevadm trigger --action=change $dev; sudo udevadm settle
            out=\$(sudo -u alice udisksctl mount -b $dev); mp=\${out##* at }; mp=\${mp%.}
            sudo install -m 0644 removablefs-tests/stress.py /tmp/removablefs-stress.py
            st=0; cd /tmp; sudo -u alice python3 /tmp/removablefs-stress.py \$mp $STRESS 8 || st=\$?
            sudo -u alice udisksctl unmount -b $dev
            test \$st -eq 0
            case $fs in ext4) sudo e2fsck -fn $dev >/dev/null ;; xfs) sudo xfs_repair -n $dev >/dev/null 2>&1 ;; esac
            ! sudo dmesg | grep -E -q 'WARNING:|BUG:|Oops|general protection|KASAN|circular locking|possible recursive|inconsistent lock'
            "; then
            echo "  ok    stress: $fs"
        else
            echo "  FAIL  stress: $fs"
            "$V" ssh "for m in \$(findmnt -rn -S $dev -o TARGET); do sudo umount -l \$m; done" || true
            "$V" ssh 'sudo dmesg | grep -E -A40 "WARNING:|BUG:|Oops|KASAN|circular locking|possible recursive|inconsistent lock" | head -80'
            rc=1
        fi
    done
fi

# Surprise removal: unplug while mounted with an open file and a writer.
log "surprise removal"
"$V" ssh "sudo mkfs.ext4 -q -F -L SURPRISE $dev && sudo udevadm settle && sudo -u alice udisksctl mount -b $dev >/dev/null &&
    sudo dmesg -C &&
    (sudo -u alice bash -c 'exec 3>/media/alice/SURPRISE/held; while echo x >&3; do sleep 0.05; done' >/dev/null 2>&1 &) &&
    sleep 1"
"$V" unplug "$DISK"
sleep 5
if "$V" ssh 'sudo pkill -u alice -f "while echo x" ; sleep 1; sudo umount -l /media/alice/SURPRISE 2>/dev/null; sleep 2;
        ! sudo dmesg | grep -E -q "BUG:|Oops|general protection|KASAN|WARNING:.*removablefs"'; then
    echo "  ok    surprise removal: no kernel errors"
else
    echo "  FAIL  surprise removal left kernel errors:"
    "$V" ssh 'sudo dmesg | grep -E -A20 "BUG:|Oops|general protection|KASAN|WARNING:" | head -60'
    rc=1
fi
if "$V" ssh 'sudo modprobe -r removablefs'; then
    echo "  ok    module unloads afterwards"
else
    echo "  FAIL  module still in use after surprise removal"
    rc=1
fi

log "done: $([[ $rc -eq 0 ]] && echo PASS || echo FAIL)"
exit $rc
