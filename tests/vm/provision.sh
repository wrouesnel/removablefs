#!/usr/bin/env bash
# First-boot provisioning for test VMs, run as root by vm.sh.  Installs the
# toolchain and headers for the newest installed kernel, udisks2 and the
# filesystem tools the tests need.  Leaves /run/harness-reboot-needed when
# the running kernel isn't the newest one installed.
set -euxo pipefail

. /etc/os-release

case "$ID" in
ubuntu|debian)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get -y -q dist-upgrade
    apt-get install -y -q build-essential dkms udisks2 attr acl \
        xfsprogs btrfs-progs f2fs-tools e2fsprogs python3 \
        devscripts debhelper fakeroot
    # dh_dkms ships in dkms itself before Ubuntu 24.04.
    apt-get install -y -q dh-dkms || true
    kver="$(ls -1 /boot/vmlinuz-* | sed 's|.*/vmlinuz-||' | sort -V | tail -1)"
    apt-get install -y -q "linux-headers-${kver}"
    # f2fs and friends live in modules-extra on Ubuntu cloud kernels.
    apt-get install -y -q "linux-modules-extra-${kver}" || true
    ;;
almalinux|rhel|rocky|centos)
    dnf -y -q install epel-release
    if [[ "${VERSION_ID%%.*}" == 8 ]]; then
        dnf config-manager --set-enabled powertools
    else
        dnf config-manager --set-enabled crb
    fi
    dnf -y -q upgrade
    dnf -y -q install gcc make elfutils-libelf-devel dkms udisks2 attr acl \
        xfsprogs e2fsprogs python3 rpm-build rpmdevtools \
        selinux-policy-devel policycoreutils-python-utils
    # Not in every release: btrfs is absent from RHEL, f2fs is EPEL-only.
    dnf -y -q install btrfs-progs f2fs-tools || true
    kver="$(rpm -q kernel --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | sort -V | tail -1)"
    dnf -y -q install "kernel-devel-${kver}"
    systemctl enable --now udisks2 || true
    ;;
*)
    echo "provision.sh: unsupported distribution $ID" >&2
    exit 1
    ;;
esac

# Let the SSH-logged-in test users act like an active desktop session.
# TEST VMS ONLY.
mkdir -p /etc/polkit-1/rules.d
cat > /etc/polkit-1/rules.d/10-ownfs-test.rules <<'EOF'
// TEST VM ONLY: treat the SSH users alice and bob like an active desktop
// session for udisks2 mount, unmount, eject and power-off.
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("org.freedesktop.udisks2.") == 0 &&
        (subject.user == "alice" || subject.user == "bob"))
        return polkit.Result.YES;
});
EOF

# polkit before 0.106 (Ubuntu 22.04) ignores JavaScript rules.
if [[ -d /etc/polkit-1/localauthority ]]; then
    mkdir -p /etc/polkit-1/localauthority/50-local.d
    cat > /etc/polkit-1/localauthority/50-local.d/10-ownfs-test.pkla <<'EOF'
# TEST VM ONLY: treat the SSH users alice and bob like an active desktop
# session for udisks2.
[ownfs test users may use udisks2]
Identity=unix-user:alice;unix-user:bob
Action=org.freedesktop.udisks2.*
ResultAny=yes
ResultInactive=yes
ResultActive=yes
EOF
fi

if [[ "$(uname -r)" != "$kver" ]]; then
    touch /run/harness-reboot-needed
fi
