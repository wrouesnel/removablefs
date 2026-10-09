# removablefs — project notes

Kernel module + udev rule that make udisks2 mount removable ext2/3/4, XFS,
btrfs and f2fs drives so the desktop user owns every file (NTFS/FAT-style).
See README.md for user-facing behaviour.

## Layout

- `module/` — the kernel module (also the DKMS source tree). `configure`
  probes the target kernel's headers and writes `removablefs_config.h`; all API
  differences live in `compat.h` behind `HAVE_*` macros.
- `data/udev/85-removablefs.rules` — the udisks2 integration. `data/selinux/removablefs.cil`
  — RHEL labelling (`dosfs_t`).
- `debian/` (3.0 native, for the PPA), `rpm/removablefs.spec` (EL8–10, COPR via
  `.copr/Makefile`). Root `Makefile`: `tarball`, `srpm`, `rpm`, `deb`.
- `tests/vm/` — libvirt VM harness; `tests/functional/` — the suite and the
  stress tool that run inside the VMs.

## Testing

All testing runs in VMs under `qemu:///session` (never load the module on the
host). `DISTRO=` is one of jammy, noble, resolute, alma8, alma9, alma10.

```sh
DISTRO=noble tests/vm/test.sh                  # module from source, all fs types
DISTRO=alma9 MODE=package tests/vm/test.sh     # build + install deb/rpm first
DISTRO=alma9 STRESS=120 tests/vm/test.sh ext4  # longer concurrency stress
tests/vm/vm.sh list
```

- VMs use user-mode networking with an SSH forward (`--qemu-commandline`):
  the host's qemu-bridge-helper can't create taps, so `virbr0` doesn't work.
  It's still libvirt session; no other VM runner is involved.
- The harness user is `tester` (uid 900); `alice` (1001) and `bob` (1002) are
  the desktop users. Ubuntu images already have an `admin` group, so don't
  name the harness user `admin`.
- uid 1000 deliberately doesn't exist in the VMs: test drives are "from
  another machine" owned by 1000.
- The surprise-removal step leaves the USB disk unplugged; `vm.sh plug
  ftest-<distro>` before poking at a VM by hand.
- Back-to-back mkfs/mount/umount can leave udev's probe stale (seen on
  RHEL 8 and Ubuntu 26.04); the suite triggers a change event after
  preparing a disk, which stands in for plugging it in.
- `noble-desktop` is the GNOME VM for `tests/vm/desktop-test.sh` (VNC on
  127.0.0.1, `virsh screenshot` for pictures). NetworkManager manages its NIC;
  installing the desktop over cloud-init's networkd config once left the NIC
  down. `vm.sh reseed` reruns cloud-init. Console login is tester/tester.
- The alma9 VM can boot the RHEL debug kernel (KASAN + lockdep): `grubby
  --set-default /boot/vmlinuz-<ver>+debug`.

## Decisions worth knowing

- **udev mechanism:** the rule sets `ID_FS_TYPE=removablefs` and
  `UDISKS_MOUNT_OPTIONS_REMOVABLEFS_{DEFAULTS,ALLOW}`. udisks2 2.10's per-driver
  options (`<sig>:<driver>_defaults`) would keep the real IdType, but from
  udev they need property names with `:`, which systemd 259 rejects (and the
  failed assignment leaves the device's udev record stale). udisks2 has no
  mount_options.conf drop-in directory, so we don't use the config file
  either. The rule `modprobe`s first so a missing module means a native mount.
- **Superblock dev_t:** removablefs reports the lower device's dev_t (via
  `lookup_bdev()` when the lower fs has an anonymous one, e.g. btrfs) so
  udisks2 ties the mount to the drive; udisks only resolves anonymous devs
  for fstype `btrfs`.
- **Credentials:** lower operations run with the mounter's creds, fsuid/fsgid
  set to the drive owner (root dir owner, or `diskuid=`/`diskgid=`), no
  groups, and only DAC_OVERRIDE/DAC_READ_SEARCH/FOWNER.
- **Policy:** setuid/setgid stripped without CAP_FSETID; only `user.*`
  xattrs pass through; chown to anyone but the presented owner is EPERM.
- **Kernel calls that write to or consume what you pass them** (both found
  only by stress on 7.0):
  - `lookup_noperm*()` (6.15+) store a salted hash into the `qstr` you pass.
    Never pass a removablefs dentry's `d_name`; rewriting a hashed dentry's hash
    corrupts the dcache (seen as RCU-callback oopses in `in_lookup_hashtable`).
    `compat.h` copies it.
  - `vfs_mkdir()` (6.19+, together with the `delegated_inode` argument)
    unlocks the parent on error; `removablefs_vfs_mkdir()` re-locks it.
  When adopting a new kernel, read `Documentation/filesystems/porting.rst`
  for that release and run `STRESS=` on it, not just the functional suite.
- **Never trust LINUX_VERSION_CODE:** RHEL 9's 5.14 has the 6.3 idmap API,
  RHEL 8's 4.18 has fs_context. Add a probe to `configure` for any new API
  difference.

## Releases

Tag `vX.Y.Z` (after `scripts/check-version.sh` agrees: spec, changelog,
`REMOVABLEFS_VERSION` in super.c). `.github/workflows/release.yml` makes a
source-only GitHub release, uploads to `ppa:w-rouesnel/removablefs` with the
shared Launchpad key, and has COPR `wrouesnel/removablefs` (EPEL 8/9/10,
x86_64 and aarch64) build from the tag.

- Remotes: `origin` is the local backup
  `~/git/will/pluggable-usbs-with-linux-filesystems.git` (named before the
  project was renamed from ownfs); `github` is `wrouesnel/removablefs`.
- GitHub secrets on `wrouesnel/removablefs`, set 2026-10-09 when the user
  asked for the GitHub repo, PPA and COPR to be set up: `PACKAGE_SIGNING_KEY`
  and `PACKAGE_SIGNING_KEY_PASSPHRASE` (the shared Launchpad key
  `2A128435A6FE8BD751AA578720959AB807096ADB`, piped from gpg and
  `secret-tool lookup service gpg-passphrase fingerprint <fpr>`), the
  `PACKAGE_SIGNING_KEY_FINGERPRINT` variable, and `COPR_CONFIG` (a copy of
  `~/.config/copr`; its API token expires 2027-03-31 and must be refreshed
  then).
