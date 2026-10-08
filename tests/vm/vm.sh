#!/usr/bin/env bash
# Test VM harness. Runs an Ubuntu cloud image under libvirt qemu:///session and
# hot-plugs raw disk images into it as USB mass-storage devices.
#
# Usage:
#   vm.sh up                 create (if needed) and start the VM, wait for SSH
#   vm.sh ssh [cmd...]       run a command in the VM (as the 'ubuntu' user)
#   vm.sh scp <src> <dst>    copy files to the VM (dst is a path inside the VM)
#   vm.sh mkdisk <name> <size>   create a blank raw image in the state dir
#   vm.sh plug <name>        hot-plug state/<name>.img as a USB disk
#   vm.sh unplug <name>      hot-unplug it
#   vm.sh reset              revert the root disk to a fresh overlay (VM is recreated)
#   vm.sh destroy            remove the VM and all state
set -euo pipefail

export LIBVIRT_DEFAULT_URI=qemu:///session

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE="${HERE}/state"
NAME="${VM_NAME:-usbown-test}"
BASE_IMAGE="${BASE_IMAGE:-${HOME}/opt/iso/noble-server-cloudimg-amd64.img}"
ROOT="${STATE}/root.qcow2"
SEED="${STATE}/seed.iso"
KEY="${STATE}/id_ed25519"

die() { echo "vm.sh: $*" >&2; exit 1; }

ssh_opts=(-i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o LogLevel=ERROR -o ConnectTimeout=5 -o BatchMode=yes)

# User-mode networking with an SSH forward on localhost: works without a
# privileged bridge helper.
ssh_port() {
    cat "${STATE}/ssh-port" 2>/dev/null || die "VM not created"
}

free_port() {
    local p
    for p in $(seq 22220 22299); do
        ss -Hltn "sport = :$p" | grep -q . || { echo "$p"; return; }
    done
    die "no free port for SSH forward"
}

create() {
    mkdir -p "$STATE"
    [[ -f "$BASE_IMAGE" ]] || die "base image $BASE_IMAGE not found"
    [[ -f "$KEY" ]] || ssh-keygen -q -t ed25519 -N '' -C "${NAME}-harness" -f "$KEY"

    qemu-img create -q -f qcow2 -F qcow2 -b "$BASE_IMAGE" "$ROOT" 20G

    cat > "${STATE}/user-data" <<EOF
#cloud-config
hostname: ${NAME}
users:
  - name: ubuntu
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys: ["$(cat "${KEY}.pub")"]
  # Second and third local users for checking per-user ownership mapping.
  - name: alice
    uid: 1001
    shell: /bin/bash
    ssh_authorized_keys: ["$(cat "${KEY}.pub")"]
  - name: bob
    uid: 1002
    shell: /bin/bash
    ssh_authorized_keys: ["$(cat "${KEY}.pub")"]
package_update: true
packages: [qemu-guest-agent, bindfs, xfsprogs, btrfs-progs, f2fs-tools, exfatprogs, acl]
runcmd:
  - [systemctl, enable, --now, qemu-guest-agent]
  - [touch, /var/lib/cloud/harness-ready]
EOF
    printf 'instance-id: %s-%s\nlocal-hostname: %s\n' "$NAME" "$(date +%s)" "$NAME" > "${STATE}/meta-data"
    cloud-localds "$SEED" "${STATE}/user-data" "${STATE}/meta-data"
    free_port > "${STATE}/ssh-port"

    virt-install --name "$NAME" --memory 2048 --vcpus 2 \
        --osinfo ubuntu24.04 --import \
        --disk "path=${ROOT},format=qcow2,bus=virtio" \
        --disk "path=${SEED},device=cdrom" \
        --controller usb,model=qemu-xhci \
        --network none \
        --qemu-commandline="-netdev user,id=hn0,hostfwd=tcp:127.0.0.1:$(ssh_port)-:22" \
        --qemu-commandline="-device virtio-net-pci,netdev=hn0,bus=pcie.0,addr=0x1f.0x7" \
        --channel unix,target.type=virtio,target.name=org.qemu.guest_agent.0 \
        --graphics none --noautoconsole >/dev/null
}

wait_ready() {
    local i
    for i in $(seq 1 120); do
        ssh "${ssh_opts[@]}" -p "$(ssh_port)" ubuntu@127.0.0.1 \
            test -f /var/lib/cloud/harness-ready 2>/dev/null && return 0
        sleep 5
    done
    die "VM did not become ready"
}

usb_xml() {
    local name="$1" img="${STATE}/${1}.img"
    [[ -f "$img" ]] || die "no such disk image: $img"
    # Serial doubles as a stable identifier so unplug can find it again.
    cat <<EOF
<disk type='file' device='disk'>
  <driver name='qemu' type='raw' cache='none'/>
  <source file='${img}'/>
  <target dev='sd${2}' bus='usb' removable='on'/>
  <serial>USB-${name}</serial>
</disk>
EOF
}

next_letter() {
    local used l
    used="$(virsh domblklist "$NAME" | awk '{print $1}')"
    for l in b c d e f g h i j; do
        grep -qx "sd${l}" <<<"$used" || { echo "$l"; return; }
    done
    die "no free USB disk slots"
}

cmd="${1:-}"; shift || true
case "$cmd" in
    up)
        virsh dominfo "$NAME" >/dev/null 2>&1 || create
        [[ "$(virsh domstate "$NAME")" == running ]] || virsh start "$NAME" >/dev/null
        wait_ready
        echo "ready: ssh port $(ssh_port)"
        ;;
    ssh)
        user="${VM_USER:-ubuntu}"
        exec ssh "${ssh_opts[@]}" -p "$(ssh_port)" "${user}@127.0.0.1" "$@"
        ;;
    scp)
        exec scp -r "${ssh_opts[@]}" -P "$(ssh_port)" "$1" "ubuntu@127.0.0.1:$2"
        ;;
    mkdisk)
        mkdir -p "$STATE"
        rm -f "${STATE}/${1}.img"
        truncate -s "${2:-256M}" "${STATE}/${1}.img"
        ;;
    plug)
        xml="$(usb_xml "$1" "$(next_letter)")"
        virsh attach-device "$NAME" /dev/stdin --live <<<"$xml" >/dev/null
        ;;
    unplug)
        dev="$(virsh dumpxml "$NAME" | awk -v s="USB-$1" '
            /<target dev=/ {match($0, /dev=.([a-z]+)/, a); t=a[1]}
            index($0, "<serial>" s "</serial>") {print t; exit}')"
        [[ -n "$dev" ]] || die "$1 is not plugged in"
        virsh detach-disk "$NAME" "$dev" --live >/dev/null
        ;;
    reset|destroy)
        if virsh dominfo "$NAME" >/dev/null 2>&1; then
            virsh destroy "$NAME" >/dev/null 2>&1 || true
            virsh undefine "$NAME" --nvram >/dev/null 2>&1 || virsh undefine "$NAME" >/dev/null
        fi
        if [[ "$cmd" == destroy ]]; then rm -rf "$STATE"; else rm -f "$ROOT" "$SEED" "${STATE}/ssh-port"; fi
        ;;
    *)
        sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'
        exit 2
        ;;
esac
