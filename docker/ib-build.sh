#!/bin/sh
# Runs inside the ImageBuilder container. Adds our locally-built apk to the
# ImageBuilder's local package repo, builds one image per CPE510 profile,
# copies results to /work/output, and asserts the size budget.
#
# OpenWrt 25.12 uses apk: dropping wfb-ng-*.apk into packages/ makes the IB
# regenerate packages.adb (apk mkndx) automatically; ADD_LOCAL_KEY=1 installs
# the local signing pubkey into the image so the local package is trusted.
set -eu
cd /opt/ib

mkdir -p packages
cp /work/build/packages/wfb-ng-*.apk packages/

for p in $PROFILES; do
  echo "=== building image for $p ==="
  make image PROFILE="$p" PACKAGES="$PACKAGES" FILES=/work/build/overlay ADD_LOCAL_KEY=1
done

mkdir -p /work/output
find bin -type f \( -name '*cpe510*sysupgrade.bin' -o -name '*cpe510*factory.bin' \) -exec cp -v {} /work/output/ \;
cp /work/keys/drone.key /work/output/drone.key

# Image budget: every sysupgrade image must fit the 7680k partition.
max=$((7680 * 1024))
for f in /work/output/*sysupgrade.bin; do
  sz=$(wc -c < "$f")
  echo "$f: $sz bytes (max $max)"
  [ "$sz" -le "$max" ] || { echo "ERROR: $f exceeds $max bytes"; exit 1; }
done
echo "OK: all images within size budget"
