# Nothing here is compiled at package build time; DKMS builds the module.
%global debug_package %{nil}

Name:           removablefs
Version:        0.1.0
Release:        1%{?dist}
Summary:        Removable Linux filesystems owned by the desktop user
License:        GPL-2.0-only
URL:            https://github.com/wrouesnel/removablefs
Source0:        %{name}-%{version}.tar.gz
BuildArch:      noarch

BuildRequires:  systemd-rpm-macros
Requires:       %{name}-dkms = %{version}-%{release}
Requires:       udisks2
Requires(post): policycoreutils
Requires(postun): policycoreutils

%description
Makes udisks2 mount removable drives (USB, SD cards and FireWire) that carry
ext2/3/4, XFS, btrfs or f2fs through removablefs, so the desktop user can read and
write everything on them regardless of the user and group IDs stored on
disk, just as with FAT, exFAT and NTFS drives. Internal disks are not
affected.

%package        dkms
Summary:        removablefs kernel module source, built by DKMS
Requires:       dkms
Requires:       gcc
Requires:       make
Requires:       kernel-devel

%description    dkms
removablefs mounts a Linux filesystem such as ext4, XFS, btrfs or f2fs so that
every file appears to belong to the user who mounted it, the way FAT, exFAT
and NTFS drives behave. New files are stored on disk as the owner of the
drive's top-level directory.

This package provides the kernel module source, built for each installed
kernel by DKMS.

%prep
%autosetup

%build

%install
src=%{buildroot}%{_usrsrc}/%{name}-%{version}
install -d "$src"
install -m 0644 module/*.c module/*.h module/Kbuild module/Makefile module/dkms.conf "$src"/
install -m 0755 module/configure "$src"/
sed -i 's/#MODULE_VERSION#/%{version}/' "$src"/dkms.conf
install -D -m 0644 data/udev/85-removablefs.rules %{buildroot}%{_udevrulesdir}/85-removablefs.rules
install -D -m 0644 data/selinux/removablefs.cil %{buildroot}%{_datadir}/selinux/packages/removablefs.cil

%post dkms
dkms add -m %{name} -v %{version} -q --rpm_safe_upgrade || :
# Build for the running kernel now; DKMS handles future kernels itself.
dkms build -m %{name} -v %{version} -q || :
dkms install -m %{name} -v %{version} -q --force || :

%preun dkms
dkms remove -m %{name} -v %{version} -q --all --rpm_safe_upgrade || :

%post
semodule -N -X 300 -i %{_datadir}/selinux/packages/removablefs.cil >/dev/null 2>&1 || :
if selinuxenabled >/dev/null 2>&1; then load_policy || :; fi
udevadm control --reload-rules >/dev/null 2>&1 || :

%postun
if [ $1 -eq 0 ]; then
    semodule -N -X 300 -r removablefs >/dev/null 2>&1 || :
    if selinuxenabled >/dev/null 2>&1; then load_policy || :; fi
    udevadm control --reload-rules >/dev/null 2>&1 || :
fi

%files
%license LICENSE
%doc README.md
%{_udevrulesdir}/85-removablefs.rules
%{_datadir}/selinux/packages/removablefs.cil

%files dkms
%license LICENSE
%{_usrsrc}/%{name}-%{version}

%changelog
* Fri Oct 09 2026 Will Rouesnel <wrouesnel@wrouesnel.com> - 0.1.0-1
- Initial release.
