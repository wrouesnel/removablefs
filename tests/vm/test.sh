#!/usr/bin/env bash
# Build ownfs in a test VM, install it with the udev rule, plug in a USB
# disk and run the functional suite, then a surprise-removal test.
#
# Usage: DISTRO=<distro> test.sh [fstype...]    (default: all supported)
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

if [[ "${MODE:-source}" == source ]]; then
    log "building module from source"
    "${HERE}/sync-build.sh"
    "$V" ssh 'set -e
        sudo modprobe -r ownfs 2>/dev/null || true
        d=/lib/modules/$(uname -r)/extra
        sudo mkdir -p $d && sudo cp ownfs/ownfs.ko $d/ && sudo depmod -a'
    "$V" scp "${TOP}/data/udev/85-ownfs.rules" /tmp/85-ownfs.rules >/dev/null
    "$V" ssh 'sudo install -m 0644 /tmp/85-ownfs.rules /etc/udev/rules.d/85-ownfs.rules && sudo udevadm control --reload'
    if "$V" ssh 'command -v semodule >/dev/null && selinuxenabled'; then
        log "installing SELinux policy"
        "$V" scp "${TOP}/data/selinux/ownfs.cil" /tmp/ownfs.cil >/dev/null
        "$V" ssh 'sudo semodule -i /tmp/ownfs.cil'
    fi
fi

if [[ "${MODE:-source}" == package ]]; then
    log "building and installing packages"
    make -s -C "$TOP" tarball DIST="${TOP}/dist" >/dev/null
    ver="$(sed -n 's/^Version: *//p' "${TOP}/rpm/ownfs.spec")"
    "$V" scp "${TOP}/dist/ownfs-${ver}.tar.gz" /tmp/ownfs.tar.gz >/dev/null
    "$V" ssh "set -e
        sudo modprobe -r ownfs 2>/dev/null || true
        sudo rm -f /etc/udev/rules.d/85-ownfs.rules /lib/modules/\$(uname -r)/extra/ownfs.ko
        sudo depmod -a
        rm -rf pkg && mkdir pkg && tar -C pkg -xzf /tmp/ownfs.tar.gz && cd pkg/ownfs-${ver}
        if command -v dpkg-buildpackage >/dev/null; then
            make deb >/tmp/pkgbuild.log 2>&1 || { tail -30 /tmp/pkgbuild.log; exit 1; }
            sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q ./dist/*.deb >/tmp/pkginst.log 2>&1 || { tail -30 /tmp/pkginst.log; exit 1; }
        else
            make rpm >/tmp/pkgbuild.log 2>&1 || { tail -30 /tmp/pkgbuild.log; exit 1; }
            sudo dnf -y -q install ./dist/noarch/*.rpm >/tmp/pkginst.log 2>&1 || { tail -30 /tmp/pkginst.log; exit 1; }
        fi
        dkms status ownfs
        modinfo -F filename ownfs
        ls /usr/lib/udev/rules.d/85-ownfs.rules /lib/udev/rules.d/85-ownfs.rules 2>/dev/null
        if command -v semodule >/dev/null; then sudo semodule -l | grep -w ownfs; fi"
fi

"$V" ssh 'rm -rf ownfs-tests && mkdir ownfs-tests'
"$V" scp "${TOP}/tests/functional/run.sh" ownfs-tests/run.sh >/dev/null

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
"$V" ssh "sudo bash ownfs-tests/run.sh $dev ${FSTYPES[*]}" || rc=$?

# Surprise removal: unplug while mounted with an open file and a writer.
log "surprise removal"
"$V" ssh "sudo mkfs.ext4 -q -F -L SURPRISE $dev && sudo udevadm settle && sudo -u alice udisksctl mount -b $dev >/dev/null &&
    sudo dmesg -C &&
    (sudo -u alice bash -c 'exec 3>/media/alice/SURPRISE/held; while echo x >&3; do sleep 0.05; done' >/dev/null 2>&1 &) &&
    sleep 1"
"$V" unplug "$DISK"
sleep 5
if "$V" ssh 'sudo pkill -u alice -f "while echo x" ; sleep 1; sudo umount -l /media/alice/SURPRISE 2>/dev/null; sleep 2;
        ! sudo dmesg | grep -E -q "BUG:|Oops|general protection|KASAN|WARNING:.*ownfs"'; then
    echo "  ok    surprise removal: no kernel errors"
else
    echo "  FAIL  surprise removal left kernel errors:"
    "$V" ssh 'sudo dmesg | grep -E -A20 "BUG:|Oops|general protection|KASAN|WARNING:" | head -60'
    rc=1
fi
if "$V" ssh 'sudo modprobe -r ownfs'; then
    echo "  ok    module unloads afterwards"
else
    echo "  FAIL  module still in use after surprise removal"
    rc=1
fi

log "done: $([[ $rc -eq 0 ]] && echo PASS || echo FAIL)"
exit $rc
