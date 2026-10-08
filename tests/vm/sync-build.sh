#!/usr/bin/env bash
# Copy module/ into the test VM and build it there against the VM's kernel.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V="${HERE}/vm.sh"   # honours DISTRO=
"$V" ssh 'rm -rf ownfs && mkdir ownfs'
tar -C "${HERE}/../../module" --exclude='*.o' --exclude='*.ko' --exclude='.*.cmd' \
    --exclude='*.mod*' --exclude=ownfs_config.h -cf - . | "$V" ssh 'tar -C ownfs -xf -'
"$V" ssh 'cd ownfs && make 2>&1 | grep -E "warning:|error|ERROR|features" | grep -v "compiler differs"; test -f ownfs.ko'
