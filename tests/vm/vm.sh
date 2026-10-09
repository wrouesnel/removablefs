#!/usr/bin/env bash
# Test VM harness. Runs distribution cloud images under libvirt
# qemu:///session and hot-plugs raw disk images into them as USB
# mass-storage devices.
#
# Usage: DISTRO=<distro> vm.sh <command> [args]
#   distros: jammy noble resolute alma8 alma9 alma10   (default: noble)
#            noble-desktop: Ubuntu 24.04 with GNOME, alice logged in on a
#            VNC display (127.0.0.1), for tests/vm/desktop-test.sh
#
#   up                   create (if needed) and start the VM, wait for SSH,
#                        provision it on first boot (build tools, headers)
#   ssh [cmd...]         run a command in the VM (VM_USER=, default 'tester')
#   scp <src> <dst>      copy files into the VM (dst is a path in the VM)
#   mkdisk <name> [size] create a blank raw image in the shared disk dir
#   plug <name>          hot-plug disks/<name>.img as a USB disk
#   unplug <name>        hot-unplug it
#   reseed               rerun cloud-init on the next boot (fixes networking)
#   stop                 shut the VM down (keeps its disk)
#   destroy              remove the VM and its state (disk images are kept)
#   list                 show known distros and VM state
set -euo pipefail

export LIBVIRT_DEFAULT_URI=qemu:///session

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGES="${IMAGES:-${HOME}/opt/iso}"
DISTRO="${DISTRO:-noble}"

declare -A BASE=(
    [jammy]="jammy-server-cloudimg-amd64.img"
    [noble]="noble-server-cloudimg-amd64.img"
    [noble-desktop]="noble-server-cloudimg-amd64.img"
    [resolute]="resolute-server-cloudimg-amd64.img"
    [alma8]="AlmaLinux-8-GenericCloud-latest.x86_64.qcow2"
    [alma9]="AlmaLinux-9-GenericCloud-latest.x86_64.qcow2"
    [alma10]="AlmaLinux-10-GenericCloud-latest.x86_64.qcow2"
)
declare -A OSINFO=(
    [jammy]=ubuntu22.04 [noble]=ubuntu24.04 [noble-desktop]=ubuntu24.04 [resolute]=ubuntu24.04
    [alma8]=almalinux8 [alma9]=almalinux9 [alma10]=almalinux9
)

die() { echo "vm.sh: $*" >&2; exit 1; }

[[ -n "${BASE[$DISTRO]:-}" ]] || die "unknown DISTRO '$DISTRO'"
NAME="usbown-${DISTRO}"
STATE="${HERE}/state/${DISTRO}"
DISKS="${HERE}/state/disks"
ROOT="${STATE}/root.qcow2"
SEED="${STATE}/seed.iso"
KEY="${HERE}/state/id_ed25519"
BASE_IMAGE="${IMAGES}/${BASE[$DISTRO]}"

ssh_opts=(-i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o LogLevel=ERROR -o ConnectTimeout=5 -o BatchMode=yes -o ServerAliveInterval=15)

ssh_port() {
    cat "${STATE}/ssh-port" 2>/dev/null || die "VM $NAME not created"
}

free_port() {
    local p used
    used="$(cat "${HERE}"/state/*/ssh-port 2>/dev/null || true)"
    for p in $(seq 22220 22299); do
        grep -qx "$p" <<<"$used" && continue
        ss -Hltn "sport = :$p" | grep -q . || { echo "$p"; return; }
    done
    die "no free port for SSH forward"
}

vssh() {
    ssh "${ssh_opts[@]}" -p "$(ssh_port)" "${VM_USER:-tester}@127.0.0.1" "$@"
}

create() {
    mkdir -p "$STATE" "$DISKS"
    [[ -f "$BASE_IMAGE" ]] || die "base image $BASE_IMAGE not found"
    [[ -f "$KEY" ]] || ssh-keygen -q -t ed25519 -N '' -C "usbown-harness" -f "$KEY"

    qemu-img create -q -f qcow2 -F qcow2 -b "$BASE_IMAGE" "$ROOT" 20G

    local pub
    pub="$(cat "${KEY}.pub")"
    # 'tester' runs the tests (sudo); alice and bob are the desktop users
    # whose ownership the tests check.  uid 1000 is left free on purpose.
    cat > "${STATE}/user-data" <<EOF
#cloud-config
hostname: ${NAME}
users:
  - name: tester
    uid: 900
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys: ["${pub}"]
  - name: alice
    uid: 1001
    shell: /bin/bash
    ssh_authorized_keys: ["${pub}"]
  - name: bob
    uid: 1002
    shell: /bin/bash
    ssh_authorized_keys: ["${pub}"]
ssh_pwauth: false
# Console logins, for debugging a VM whose network is down.  The VMs only
# listen on 127.0.0.1 and refuse SSH passwords.
chpasswd:
  expire: false
  users:
    - {name: tester, password: tester, type: text}
runcmd:
  - [touch, /var/lib/cloud/harness-ready]
EOF
    write_seed
    # Several VMs may be created at once; claim a port under a lock.
    (
        flock 9
        free_port > "${STATE}/ssh-port"
    ) 9>"${HERE}/state/.port-lock"

    local display=(--graphics none)
    [[ "$DISTRO" == *-desktop ]] && display=(--graphics vnc,listen=127.0.0.1 --video virtio)

    # User-mode networking with an SSH forward on localhost: works without
    # a privileged bridge helper.  The NIC sits at a fixed high PCI address
    # so it can't collide with the slots libvirt assigns.
    virt-install --name "$NAME" --memory 3072 --vcpus 4 \
        --osinfo "${OSINFO[$DISTRO]}" --import \
        --cpu host-passthrough \
        --disk "path=${ROOT},format=qcow2,bus=virtio" \
        --disk "path=${SEED},device=cdrom" \
        --controller usb,model=qemu-xhci \
        --network none \
        --qemu-commandline="-netdev user,id=hn0,hostfwd=tcp:127.0.0.1:$(ssh_port)-:22" \
        --qemu-commandline="-device virtio-net-pci,netdev=hn0,bus=pcie.0,addr=0x1f.0x7" \
        --serial "pty,log.file=${STATE}/console.log" \
        "${display[@]}" --noautoconsole >/dev/null
}

# A new instance-id makes cloud-init configure the guest again, network
# included.  Matching the NIC by driver keeps the network up whatever the
# interface ends up being called (installing a desktop renamed it once).
write_seed() {
    printf 'instance-id: %s-%s\nlocal-hostname: %s\n' "$NAME" "$(date +%s)" "$NAME" > "${STATE}/meta-data"
    cat > "${STATE}/network-config" <<EOF
version: 2
ethernets:
  nic:
    match: {driver: virtio_net}
    dhcp4: true
EOF
    cloud-localds -N "${STATE}/network-config" "$SEED" "${STATE}/user-data" "${STATE}/meta-data"
}

wait_ssh() {
    local i
    for i in $(seq 1 120); do
        vssh test -f /var/lib/cloud/harness-ready 2>/dev/null && return 0
        sleep 5
    done
    die "VM $NAME did not become ready"
}

provision() {
    [[ -f "${STATE}/provisioned" ]] && return 0
    echo "provisioning $NAME..." >&2
    vssh "sudo DESKTOP=$([[ "$DISTRO" == *-desktop ]] && echo 1 || echo 0) bash -s" < "${HERE}/provision.sh" > "${STATE}/provision.log" 2>&1 \
        || { tail -n 30 "${STATE}/provision.log" >&2; die "provisioning failed"; }
    # Provisioning may have installed a newer kernel; boot into it.
    if vssh 'test -f /run/harness-reboot-needed'; then
        vssh 'sudo systemctl reboot' || true
        sleep 15
        wait_ssh
    fi
    touch "${STATE}/provisioned"
}

usb_xml() {
    local name="$1" img="${DISKS}/${1}.img"
    [[ -f "$img" ]] || die "no such disk image: $img"
    # The serial identifies the disk again for unplug.
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
    for l in b c d e f g h i j k l m n o p; do
        grep -qx "sd${l}" <<<"$used" || { echo "$l"; return; }
    done
    die "no free USB disk slots"
}

cmd="${1:-}"; shift || true
case "$cmd" in
    up)
        virsh dominfo "$NAME" >/dev/null 2>&1 || create
        [[ "$(virsh domstate "$NAME")" == running ]] || virsh start "$NAME" >/dev/null
        wait_ssh
        provision
        echo "$NAME ready: ssh port $(ssh_port), kernel $(vssh uname -r)"
        ;;
    ssh)
        vssh "$@"
        ;;
    scp)
        scp -r "${ssh_opts[@]}" -P "$(ssh_port)" "$1" "${VM_USER:-tester}@127.0.0.1:$2"
        ;;
    mkdisk)
        mkdir -p "$DISKS"
        rm -f "${DISKS}/${1}.img"
        truncate -s "${2:-256M}" "${DISKS}/${1}.img"
        echo "${DISKS}/${1}.img"
        ;;
    plug)
        virsh attach-device "$NAME" /dev/stdin --live <<<"$(usb_xml "$1" "$(next_letter)")" >/dev/null
        ;;
    unplug)
        dev="$(virsh dumpxml "$NAME" | awk -v s="USB-$1" '
            /<target dev=/ {match($0, /dev=.([a-z]+)/, a); t=a[1]}
            index($0, "<serial>" s "</serial>") {print t; exit}')"
        [[ -n "$dev" ]] || die "$1 is not plugged in"
        virsh detach-disk "$NAME" "$dev" --live >/dev/null
        ;;
    reseed)
        # Recover a VM whose network or users got broken: cloud-init runs
        # again on the next boot.
        virsh destroy "$NAME" >/dev/null 2>&1 || true
        write_seed
        virsh start "$NAME" >/dev/null
        wait_ssh
        echo "$NAME reseeded"
        ;;
    stop)
        virsh shutdown "$NAME" >/dev/null 2>&1 || true
        ;;
    destroy)
        if virsh dominfo "$NAME" >/dev/null 2>&1; then
            virsh destroy "$NAME" >/dev/null 2>&1 || true
            virsh undefine "$NAME" --nvram >/dev/null 2>&1 || virsh undefine "$NAME" >/dev/null
        fi
        rm -rf "$STATE"
        ;;
    list)
        for d in "${!BASE[@]}"; do
            printf '%-9s %-16s %s\n' "$d" "usbown-$d" \
                "$(virsh domstate "usbown-$d" 2>/dev/null || echo absent)"
        done | sort
        ;;
    *)
        sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'
        exit 2
        ;;
esac
