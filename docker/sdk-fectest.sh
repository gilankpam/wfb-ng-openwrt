#!/bin/sh
# Cross-build the self-contained swfec self-test for the target and run it under
# qemu (big-endian MIPS). Built from the source tarball the package compile
# already downloaded into dl/ (OpenWrt cleans the source out of build_dir after
# packaging, so we cannot reuse that directory).
set -eu
DL=$(ls /opt/sdk/dl/wfb-ng-*.tar.gz 2>/dev/null | head -n1)
[ -n "$DL" ] || { echo "ERROR: wfb-ng source tarball not found in dl/ (run the package build first)"; exit 1; }

WORK=/tmp/wfb-fectest
rm -rf "$WORK"; mkdir -p "$WORK"
tar -C "$WORK" -xzf "$DL"
SRC=$(find "$WORK" -maxdepth 1 -type d -name 'wfb-ng-*' | head -n1)
[ -d "$SRC" ] || SRC="$WORK"

export STAGING_DIR=/opt/sdk/staging_dir
CC=$(ls /opt/sdk/staging_dir/toolchain-*/bin/*-openwrt-linux-musl-gcc 2>/dev/null | head -n1)
[ -n "$CC" ] || { echo "ERROR: cross gcc not found"; exit 1; }
CXX="${CC%-gcc}-g++"
# Toolchain sysroot holds the musl loader + libstdc++/libgcc_s for qemu -L.
SYSROOT=$(cd "$(dirname "$CC")/.." && pwd)
echo "Using CC=$CC"

cd "$SRC"
# fec_swfec_test has its own main() and links no external libs. Build it
# dynamically (static C++ on musl mishandles the _Unwind_* EH symbols) with the
# same ZFEX defines the package uses (added by the repo Makefile), then run it
# under qemu-user with the toolchain sysroot.
make fec_swfec_test CC="$CC" CXX="$CXX" CFLAGS="-O2" VERSION=test COMMIT=testtest

echo "Running fec_swfec_test under qemu-mips-static (sysroot=$SYSROOT)..."
qemu-mips-static -L "$SYSROOT" ./fec_swfec_test
echo "OK: swfec FEC self-test passed on big-endian MIPS"
