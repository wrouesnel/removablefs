# ownfs

Plug in a USB stick or SD card formatted with a Linux filesystem (ext4,
XFS, btrfs, f2fs) and it just works for the logged-in desktop user: every
file is theirs, whatever user and group IDs are stored on the drive. That's
the experience FAT, exFAT and NTFS drives already give.

Without ownfs, a drive formatted on another machine shows its files as
belonging to whatever UID that machine used. Often that's nobody on this
machine, so you can't write to it without `sudo chown -R`.

## How it works

ownfs has two parts:

- **The `ownfs` kernel module**, a small stacking filesystem. It mounts the
  device's real filesystem on a private, invisible mount and stacks on top.
  - Every file is presented as owned by the `uid=`/`gid=` the mount was made
    with, like `vfat` and `ntfs3`.
  - Permission is checked once, at the ownfs layer, against that presented
    ownership.
  - Changes are made on the real filesystem with the credentials of the
    *drive's owner*: the user and group that own the drive's top-level
    directory. So files you create are stored on disk as that owner, and the
    drive stays consistent if you take it back to the machine it came from.
- **A udev rule for udisks2.** udisks2 takes a device's filesystem type
  from udev and accepts mount options from udev properties. For removable
  ext2/3/4, XFS, btrfs and f2fs devices, the rule loads the module and then
  reports the device's type as `ownfs`, with options
  `uid=$UID,gid=$GID,lower=<real type>`. udisks2 fills in the desktop user's
  IDs, exactly as it does for FAT and NTFS. If the module can't be loaded,
  the device keeps its real type and is mounted natively. Internal disks are
  left alone.

The mount reports the drive's own block device, so udisks2 and the file
manager treat it like any other mounted drive: it appears under
`/media/<user>/<label>` (`/run/media/<user>/<label>` on RHEL), and *Eject*
or *Safely Remove* work normally.

## Behaviour

| What | On an ownfs mount |
|---|---|
| Owner shown by `ls -l`/`stat` | Always the mounting user and their group |
| Permission bits | The real ones from disk; `chmod` changes them on disk |
| New files on disk | Owned by the owner of the drive's root directory (see `diskuid=`) |
| `chown`/`chgrp` | Succeeds when the owner doesn't change (so `cp -a` and `rsync -a` work); anything else fails with `EPERM`, or is ignored with `quiet` |
| setuid / setgid | Stripped from files (setgid stays on directories) unless the caller has `CAP_FSETID`. udisks2 also mounts `nosuid,nodev` |
| Device nodes | Creating them needs `CAP_MKNOD`, as usual; fifos and sockets are fine |
| Extended attributes | `user.*` is passed through; `trusted.*`, `security.*` and POSIX ACLs aren't, because they encode the other machine's identities and policy |
| SELinux (RHEL) | Labelled `dosfs_t`, like other removable media |
| Filesystem type in GNOME Disks, `lsblk -f` | `ownfs`; the real type is in the udev property `OWNFS_LOWER_TYPE`. Disks can't run its *Repair* action on these drives; run `fsck` by hand if needed |
| Other local users | See the files as owned by the mounting user, so they get the "other" permission bits |

To set which owner the drive's files are stored as, change the owner of its
top-level directory, or format it with that owner
(`mkfs.ext4 -E root_owner=1000:1000 …`).

### Mount options

You can also mount by hand (as root):
`mount -t ownfs -o uid=1000,gid=1000 /dev/sdb1 /mnt/stick`

| Option | Meaning |
|---|---|
| `uid=`, `gid=` | Owner to present (default: the mounting process) |
| `lower=` | The device's real filesystem type (default: try ext4, xfs, btrfs, f2fs) |
| `diskuid=`, `diskgid=` | Owner to store new files as (default: owner of the root directory) |
| `quiet` | `chown`/`chgrp` to another owner succeeds without doing anything |
| anything else | Passed to the real filesystem (e.g. `errors=remount-ro`, `discard`) |

### Leaving a device alone

To keep normal POSIX ownership on a particular removable device, give it
`OWNFS_IGNORE=1` in a udev rule that sorts before `85-ownfs.rules`, for
example `/etc/udev/rules.d/80-ownfs-ignore.rules`:

```
ENV{ID_FS_UUID}=="0b6a2c66-…", ENV{OWNFS_IGNORE}="1"
```

## Supported systems

Tested in VMs on every release below, both from source and from the
packages:

| Distribution | Kernel tested | udisks2 |
|---|---|---|
| Ubuntu 22.04 LTS | 5.15 | 2.9.4 |
| Ubuntu 24.04 LTS | 6.8 | 2.10.1 |
| Ubuntu 26.04 LTS | 7.0 | 2.10.91 |
| RHEL / AlmaLinux / Rocky 8 | 4.18 (8.10) | 2.9.0 |
| RHEL / AlmaLinux / Rocky 9 | 5.14 (9.8) | 2.9.4 |
| RHEL / AlmaLinux / Rocky 10 | 6.12 (10.2) | 2.11.0 |

RHEL doesn't ship btrfs or f2fs, so on RHEL those filesystems aren't
available to ownfs either.

The module's build probes the target kernel's headers for each VFS API
difference instead of trusting version numbers, because RHEL kernels
backport VFS changes freely. RHEL 9's 5.14 kernel, for instance, has the 6.3
idmapped-mount API.

## Installing

The release workflow publishes packages to a Launchpad PPA (Ubuntu) and a
COPR project (RHEL family). Neither repository exists yet; until they do,
build the packages yourself (see [Building from source](#building-from-source)).

### Ubuntu (PPA)

```sh
sudo add-apt-repository ppa:w-rouesnel/ownfs
sudo apt install ownfs
```

### RHEL, AlmaLinux, Rocky Linux 8–10 (COPR)

DKMS comes from EPEL.

```sh
sudo dnf install epel-release
sudo dnf copr enable wrouesnel/ownfs
sudo dnf install ownfs
```

### Secure Boot

DKMS builds the module on your machine, so with Secure Boot enabled the
module must be signed by a key your firmware trusts.

- **Ubuntu:** DKMS signs modules with the machine's MOK key automatically.
  The first time, the installer asks for a password and you confirm the key
  in the blue *MOK management* screen on the next boot.
- **RHEL family:** DKMS signs with `/var/lib/dkms/mok.pub`. Enrol that key
  once with `sudo mokutil --import /var/lib/dkms/mok.pub`, reboot, and
  confirm it in *MOK management*.

## Building from source

```sh
make -C module              # builds module/ownfs.ko for the running kernel
make -C module KVER=6.8.0-55-generic
make deb                    # .debs in dist/
make rpm                    # .rpms in dist/
```

## Testing

Tests run in throwaway VMs under libvirt's `qemu:///session` (see
`tests/vm/`). Each VM is a cloud image of one supported distribution, with
USB disks hot-plugged into it.

```sh
DISTRO=noble tests/vm/test.sh          # build, install, run the suite
DISTRO=alma9 tests/vm/test.sh ext4 xfs
tests/vm/vm.sh list
```

The suite (`tests/functional/run.sh`) formats the disk as if it came from
another machine, then mounts it through udisks2 as an unprivileged desktop
user. It exercises reads, writes, mmap, renames, links, xattrs and the
permission policy, checks what landed on disk, runs fsck, and fails on any
kernel warning. A final test pulls the disk out while it's mounted and being
written to.

## Licence

GPL-2.0-only. See [LICENSE](LICENSE).
