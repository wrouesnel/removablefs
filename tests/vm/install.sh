#!/usr/bin/env bash
# Install removablefs into a test VM: the module built from source plus the udev
# rule and SELinux policy (MODE=source, the default), or the deb/rpm
# packages built in the VM (MODE=package).
#
# Usage: DISTRO=<distro> [MODE=source|package] install.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOP="$(cd "${HERE}/../.." && pwd)"
V="${HERE}/vm.sh"
DISTRO="${DISTRO:-noble}"
export DISTRO

log() { printf '>>> [%s] %s\n' "$DISTRO" "$*"; }

if [[ "${MODE:-source}" == source ]]; then
    log "building module from source"
    "${HERE}/sync-build.sh"
    # Packages from an earlier MODE=package run would shadow this build:
    # Ubuntu's DKMS installs into updates/, which modprobe prefers.
    "$V" ssh 'set -e
        sudo modprobe -r removablefs 2>/dev/null || true
        if dpkg -s removablefs-dkms >/dev/null 2>&1; then sudo DEBIAN_FRONTEND=noninteractive apt-get purge -y -q removablefs removablefs-dkms >/dev/null; fi
        if rpm -q removablefs-dkms >/dev/null 2>&1; then sudo dnf -y -q remove removablefs removablefs-dkms >/dev/null; fi
        sudo rm -rf /lib/modules/$(uname -r)/updates/dkms/removablefs.ko*
        d=/lib/modules/$(uname -r)/extra
        sudo mkdir -p $d && sudo cp removablefs/removablefs.ko $d/ && sudo depmod -a'
    "$V" scp "${TOP}/data/udev/85-removablefs.rules" /tmp/85-removablefs.rules >/dev/null
    "$V" ssh 'sudo install -m 0644 /tmp/85-removablefs.rules /etc/udev/rules.d/85-removablefs.rules && sudo udevadm control --reload'
    if "$V" ssh 'command -v semodule >/dev/null && selinuxenabled'; then
        log "installing SELinux policy"
        "$V" scp "${TOP}/data/selinux/removablefs.cil" /tmp/removablefs.cil >/dev/null
        "$V" ssh 'sudo semodule -i /tmp/removablefs.cil'
    fi
fi

if [[ "${MODE:-source}" == package ]]; then
    log "building and installing packages"
    make -s -C "$TOP" tarball DIST="${TOP}/dist" >/dev/null
    ver="$(sed -n 's/^Version: *//p' "${TOP}/rpm/removablefs.spec")"
    "$V" scp "${TOP}/dist/removablefs-${ver}.tar.gz" /tmp/removablefs.tar.gz >/dev/null
    "$V" ssh "set -e
        sudo modprobe -r removablefs 2>/dev/null || true
        sudo rm -f /etc/udev/rules.d/85-removablefs.rules /lib/modules/\$(uname -r)/extra/removablefs.ko
        sudo depmod -a
        rm -rf pkg && mkdir pkg && tar -C pkg -xzf /tmp/removablefs.tar.gz && cd pkg/removablefs-${ver}
        if command -v dpkg-buildpackage >/dev/null; then
            make deb >/tmp/pkgbuild.log 2>&1 || { tail -30 /tmp/pkgbuild.log; exit 1; }
            sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q ./dist/*.deb >/tmp/pkginst.log 2>&1 || { tail -30 /tmp/pkginst.log; exit 1; }
        else
            make rpm >/tmp/pkgbuild.log 2>&1 || { tail -30 /tmp/pkgbuild.log; exit 1; }
            sudo dnf -y -q install ./dist/noarch/*.rpm >/tmp/pkginst.log 2>&1 || { tail -30 /tmp/pkginst.log; exit 1; }
        fi
        dkms status removablefs
        modinfo -F filename removablefs
        ls /usr/lib/udev/rules.d/85-removablefs.rules /lib/udev/rules.d/85-removablefs.rules 2>/dev/null
        if command -v semodule >/dev/null; then sudo semodule -l | grep -w removablefs; fi"
fi

