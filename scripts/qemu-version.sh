#!/bin/sh

set -eu

dir="$1"
pkgversion="$2"
version="$3"

if [ -z "$pkgversion" ]; then
    cd "$dir"
    if [ -e .git ]; then
        # Downstream forks and shallow CI checkouts do not always carry an
        # upstream v* tag. Keep the normal tagged form when available, then
        # fall back to the abbreviated commit so deployed builds remain
        # identifiable instead of reporting only the base QEMU version.
        pkgversion=$(git describe --match 'v*' --dirty 2>/dev/null ||
                     git describe --always --dirty 2>/dev/null) || :
    fi
fi

if [ -n "$pkgversion" ]; then
    fullversion="$version ($pkgversion)"
else
    fullversion="$version"
fi

cat <<EOF
#define QEMU_PKGVERSION "$pkgversion"
#define QEMU_FULL_VERSION "$fullversion"
EOF
