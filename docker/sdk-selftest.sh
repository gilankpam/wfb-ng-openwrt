#!/bin/sh
# Cross-build the relay's pure-unit tests for the target and run them under
# qemu (big-endian MIPS): the golden LE byte vectors fail here if any code
# writes host order. Built out of tree so /work stays clean.
set -eu
export STAGING_DIR=/opt/sdk/staging_dir
CC=$(ls /opt/sdk/staging_dir/toolchain-*/bin/*-openwrt-linux-musl-gcc 2>/dev/null | head -n1)
[ -n "$CC" ] || { echo "ERROR: cross gcc not found"; exit 1; }
SYSROOT=$(cd "$(dirname "$CC")/.." && pwd)
W=/tmp/mabur-selftest
rm -rf "$W"; mkdir -p "$W"
cp -a /work/feed/net/mabur-relay/src /work/feed/net/mabur-relay/tests "$W/"
make -C "$W/src" clean >/dev/null
make -C "$W/src" unit_tests CC="$CC" CFLAGS="-O2"
file "$W/src/unit_tests" | grep -q 'ELF 32-bit MSB.*MIPS' || { echo "ERROR: unit_tests not BE MIPS"; exit 1; }
echo "Running unit_tests under qemu-mips-static (sysroot=$SYSROOT)..."
qemu-mips-static -L "$SYSROOT" "$W/src/unit_tests" "$W/tests/fixtures"
echo "OK: relay unit tests passed on big-endian MIPS"
