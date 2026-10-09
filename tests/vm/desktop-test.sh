#!/usr/bin/env bash
# The desktop experience, end to end: GNOME on Ubuntu 24.04 with alice
# logged in, no test polkit rules.  Plug in an ext4 drive that came from
# another machine (everything owned by uid 1000), let GNOME automount it,
# check alice owns and can change everything, then eject it through gvfs
# from alice's session, as the Files sidebar's eject button does.
#
# Usage: [MODE=source|package] desktop-test.sh
# Screenshots land in tests/vm/state/noble-desktop/.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export DISTRO=noble-desktop
export LIBVIRT_DEFAULT_URI=qemu:///session
V="${HERE}/vm.sh"
STATE="${HERE}/state/${DISTRO}"
DISK=desktop-drive
LABEL=HOLIDAYS
rc=0

log() { printf '>>> [desktop] %s\n' "$*"; }
ok() { printf '  ok    %s\n' "$*"; }
bad() { printf '  FAIL  %s\n' "$*"; rc=1; }
shot() { virsh screenshot "usbown-${DISTRO}" "${STATE}/$1.png" >/dev/null && log "screenshot: ${STATE}/$1.png"; }
as_alice() { "$V" ssh "sudo -u alice env XDG_RUNTIME_DIR=/run/user/1001 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1001/bus $*"; }

"$V" up >/dev/null
MODE="${MODE:-package}" "${HERE}/install.sh"

log "waiting for alice's graphical session"
for _ in $(seq 1 60); do
    "$V" ssh 'for s in $(loginctl list-sessions --no-legend | awk "\$3 == \"alice\" {print \$1}"); do
                  loginctl show-session "$s" -p Active -p Type | tr "\n" " "; echo; done' 2>/dev/null \
        | grep -q 'Active=yes Type=\(wayland\|x11\)' && break
    sleep 5
done || true
if "$V" ssh 'loginctl list-sessions --no-legend | grep -q alice'; then
    ok "alice has an active graphical session"
else
    bad "alice never logged in"; exit 1
fi
if "$V" ssh 'ls /etc/polkit-1/rules.d/10-removablefs-test.rules /etc/polkit-1/localauthority/50-local.d/10-removablefs-test.pkla 2>/dev/null | grep -q .'; then
    bad "test polkit rules present; this would not test real authorisation"
fi

# A drive from another machine: ext4, root directory and files owned by
# uid 1000, a private directory, and a file only its owner may write.
img="$("$V" mkdisk "$DISK" 256M)"
tree="$(mktemp -d)"
mkdir -p "$tree/Photos" "$tree/private"
echo "beach" > "$tree/Photos/day1.txt"
echo "secret" > "$tree/private/diary.txt"
chmod 0700 "$tree/private"; chmod 0600 "$tree/private/diary.txt"
mkfs.ext4 -q -F -L "$LABEL" -E root_owner=1000:1000 -d "$tree" "$img"
rm -rf "$tree"

"$V" unplug "$DISK" 2>/dev/null || true
log "plugging in the drive"
"$V" plug "$DISK"

mp=""
for _ in $(seq 1 30); do
    mp="$("$V" ssh "findmnt -rno TARGET,FSTYPE | awk '\$2 == \"removablefs\" && \$1 ~ /${LABEL}\$/ {print \$1}'" || true)"
    [[ -n "$mp" ]] && break
    sleep 1
done
sleep 3
shot plugged-in
if [[ -n "$mp" ]]; then
    ok "GNOME automounted the drive through removablefs at $mp"
else
    bad "drive was not automounted with removablefs"
    "$V" ssh 'findmnt -rno TARGET,SOURCE,FSTYPE | grep -E "media|sd"; sudo journalctl -b --no-pager -u udisks2 | tail -5'
    exit 1
fi

"$V" ssh "findmnt -no OPTIONS '$mp'" | grep -q "uid=1001,gid=1001" \
    && ok "mounted for alice (uid=1001,gid=1001)" || bad "wrong owner in mount options"
[[ "$("$V" ssh "sudo stat -c %U '$mp/Photos/day1.txt' '$mp/private' '$mp/private/diary.txt' | sort -u")" == alice ]] \
    && ok "every file shows as alice's" || bad "files not shown as alice's"
as_alice "cat '$mp/private/diary.txt'" >/dev/null \
    && ok "alice reads the other machine's private 0600 file" || bad "alice cannot read the private file"
as_alice "sh -c 'echo more >> \"$mp/private/diary.txt\" && mkdir \"$mp/New\" && cp /etc/hostname \"$mp/New/\"'" \
    && ok "alice writes, creates directories and copies files in" || bad "alice cannot write"
as_alice "gio info '$mp/Photos/day1.txt'" | grep -q 'access::can-write: TRUE' \
    && ok "gvfs/Files sees the files as writable" || bad "gvfs says not writable"
as_alice "gio list '$mp'" >/dev/null && ok "Files can list the drive"

# Show the drive in a Files window for the screenshot.
as_alice "sh -c 'WAYLAND_DISPLAY=wayland-0 DISPLAY=:0 setsid nautilus \"$mp\" >/dev/null 2>&1 &'" || true
sleep 6
shot files-window

# Files' own eject button leaves the drive before unmounting it; a CLI
# eject with a Files window still showing the drive gets "busy", exactly
# as it does for a FAT drive.  So close Files first.
as_alice "nautilus -q" 2>/dev/null || true
sleep 2

log "ejecting through gvfs from alice's session"
if as_alice "gio mount -e '$mp'"; then
    ok "gio mount -e (the eject button) succeeded"
else
    bad "eject failed"
fi
sleep 3
"$V" ssh "! findmnt -rno FSTYPE | grep -qx removablefs" && ok "drive unmounted" || bad "still mounted"
shot ejected

# Plug it in again and check what landed on disk, natively.
"$V" unplug "$DISK" 2>/dev/null || true
on_disk="$(debugfs -R 'stat /New/hostname' "$img" 2>/dev/null | sed -n 's/.*User: *\([0-9]*\).*Group: *\([0-9]*\).*/\1:\2/p')"
[[ "$on_disk" == "1000:1000" ]] \
    && ok "files alice created are stored as the drive owner (1000:1000)" \
    || bad "new file stored as '$on_disk', not 1000:1000"
e2fsck -fn "$img" >/dev/null 2>&1 && ok "filesystem clean" || bad "fsck reports errors"

log "done: $([[ $rc -eq 0 ]] && echo PASS || echo FAIL)"
exit $rc
