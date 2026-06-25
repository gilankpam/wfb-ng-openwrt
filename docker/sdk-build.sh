#!/bin/sh
# Runs inside the SDK container (as the host uid). Compiles the wfb-ng package
# and copies the built package to /work/build/packages, then checks the binary
# arch. OpenWrt 25.12 builds the apk format (.apk), not legacy .ipk.
set -eu
cd /opt/sdk

grep -q '^src-link wfbng ' feeds.conf 2>/dev/null || echo 'src-link wfbng /work/feed' >> feeds.conf
./scripts/feeds update wfbng
# wfb-ng also exists in the official 'packages' feed (the full upstream version);
# -p wfbng forces OUR fork's package to win the name collision.
./scripts/feeds install -p wfbng -f wfb-ng
./scripts/feeds install libpcap libsodium

grep -q '^CONFIG_PACKAGE_wfb-ng=y' .config 2>/dev/null || echo 'CONFIG_PACKAGE_wfb-ng=y' >> .config
make defconfig
make package/wfb-ng/compile -j1 V=s \
  WFB_REPO="${WFB_REPO}" WFB_COMMIT="${WFB_COMMIT}" WFB_VERSION="${WFB_VERSION}"

mkdir -p /work/build/packages
PKG=$(find bin/packages -name 'wfb-ng-*.apk' | head -n1)
[ -n "$PKG" ] || { echo "ERROR: no wfb-ng .apk produced"; find bin/packages -name 'wfb-ng*' -o -name '*.apk' | head; exit 1; }
cp -v "$PKG" /work/build/packages/

# Architecture sanity: the binaries must be big-endian (MSB) MIPS.
BIN=$(find build_dir -type f -name wfb_rx | head -n1)
echo "Checking arch of $BIN"
file "$BIN" | grep -q 'ELF 32-bit MSB.*MIPS' || { echo "ERROR: wfb_rx not big-endian MIPS"; file "$BIN"; exit 1; }
echo "OK: wfb_rx is big-endian MIPS"
