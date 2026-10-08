#!/bin/sh
# Fail unless every place that states the version agrees.
set -eu
cd "$(dirname "$0")/.."
spec="$(sed -n 's/^Version: *//p' rpm/ownfs.spec)"
deb="$(sed -n '1s/^ownfs (\([^)~+-]*\).*/\1/p' debian/changelog)"
mod="$(sed -n 's/^#define OWNFS_VERSION "\(.*\)"/\1/p' module/super.c)"
echo "rpm spec: $spec  debian/changelog: $deb  module: $mod"
[ "$spec" = "$deb" ] && [ "$spec" = "$mod" ] || { echo "version mismatch" >&2; exit 1; }
if [ -n "${GITHUB_REF_NAME:-}" ] && [ "${GITHUB_REF_TYPE:-}" = tag ]; then
	[ "$GITHUB_REF_NAME" = "v$spec" ] || { echo "tag $GITHUB_REF_NAME != v$spec" >&2; exit 1; }
fi
