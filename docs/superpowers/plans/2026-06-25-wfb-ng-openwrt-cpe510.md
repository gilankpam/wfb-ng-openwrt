# wfb-ng OpenWrt CPE510 Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a reproducible Docker build that emits a minimal wfb-ng ground-station OpenWrt firmware image for the TP-Link CPE510.

**Architecture:** A two-stage Docker pipeline. Stage 1 (OpenWrt SDK) compiles a custom `wfb-ng` package (`wfb_rx` + `wfb_tx` only, from the user's `swfec` fork) for `ath79/generic` and verifies the software-FEC on big-endian MIPS under qemu. Stage 2 (OpenWrt ImageBuilder) assembles per-variant CPE510 images that bundle that package plus an on-demand shell launcher, a plain config file, and a baked test key. The device runs nothing at boot; the operator SSHes in, configures, and starts wfb-ng manually.

**Tech Stack:** OpenWrt 25.12 SDK + ImageBuilder (Docker, Debian base), OpenWrt package Makefile, POSIX `sh` (busybox ash) launcher, `wfb_rx`/`wfb_tx` C binaries, `qemu-mips-static`.

## Global Constraints

Every task implicitly inherits these (verbatim from the spec):

- **OpenWrt:** `25.12.4`, target `ath79`, subtarget `generic`, arch `mips_24kc` (big-endian).
- **Image profiles:** `tplink_cpe510-v1`, `tplink_cpe510-v2`, `tplink_cpe510-v3` (build all three).
- **Image budget:** rootfs+kernel ≤ `7680k` (7864320 bytes). Build asserts this on every `sysupgrade.bin`.
- **Source:** `https://github.com/gilankpam/wfb-ng.git` @ `e8033cf9cf5a2081447ae45bf441bc68c28a26da` (branch `swfec`). Fetched via git at SDK build time; **not** upstream, **not** local tree.
- **Device has no bash.** The launcher and all on-device scripts MUST be POSIX `sh` / busybox ash — no arrays, no `[[ ]]`, no `${var//}`, no `local` reliance beyond ash support.
- **No boot autostart, no procd/init script.** Operator drives `wfb-ng.sh start|stop|status` over SSH.
- **Networking:** LAN `192.168.1.1/24`, no DHCP/DNS server. Operator host static `192.168.1.10`. Downlink UDP → host `:5600`; uplink UDP listen `:5601`.
- **Keys:** one fixed shared test keypair (insecure, accepted). `gs.key` → `/etc/gs.key` in image; `drone.key` → `output/` for the air unit.
- **On-device package set:** `wfb_rx`, `wfb_tx`, `iw`, `kmod-ath9k`, `mac80211`, `dropbear`. Removed: `wpad*`, `dnsmasq`, `odhcpd`, `ppp*`, Python, `wfb_tun`, `wfb_keygen`, `kmod-tun`.

---

## APK Rework Addendum (2026-06-25, supersedes the ipk specifics in Tasks 4–6)

This plan was written assuming OpenWrt's legacy opkg/`.ipk` flow. OpenWrt **25.12
ships the `apk` package manager instead**, so Tasks 4–6 were reworked (user chose to
stay on 25.12). Tasks 1–3 are unchanged. The reworked build was implemented
controller-led (the apk path was exploratory and needed live iteration), and the
**committed files are the source of truth** — `feed/net/wfb-ng/Makefile`,
`docker/{Dockerfile.sdk,Dockerfile.imagebuilder,sdk-build.sh,sdk-fectest.sh,ib-build.sh}`,
`build.sh`, `files/`. Deltas from the `.ipk` code blocks below:

- **Package format:** SDK produces `wfb-ng-<ver>-r<rel>.apk` (e.g.
  `wfb-ng-2025.06.25-r1.apk`), not `wfb-ng_*.ipk`. Copy/glob patterns use `*.apk`.
- **libpcap source:** in the `base` feed (`src-git --root=package base … openwrt.git`),
  not `packages`. `Dockerfile.sdk` bakes `feeds update packages` and `feeds update base`
  as separate cached layers.
- **Package-name collision:** `wfb-ng` also exists in the official `packages` feed
  (v`25.01`). `sdk-build.sh` uses `feeds install -p wfbng -f wfb-ng` so our fork wins;
  our `2025.06.25` also outranks `25.01` so the ImageBuilder selects our local apk.
- **ImageBuilder:** drop `wfb-ng-*.apk` into `packages/`, build with
  `make image PROFILE=<v> PACKAGES=... FILES=... ADD_LOCAL_KEY=1` (auto `apk mkndx` +
  local signing-key trust). `Dockerfile.imagebuilder` also needs `python3-distutils`.
- **Networking:** all `docker run` builds use `--network host` (the host resolver is
  reliable; the default bridge hit transient DNS failures mid-build).
- **qemu FEC test:** built from the source tarball in `dl/` (OpenWrt cleans the source
  out of `build_dir` after packaging), linked **dynamically** and run under
  `qemu-mips-static -L <toolchain sysroot>` (static musl C++ mishandles `_Unwind_*`).
  Result: `fec_swfec_test: ALL OK` on big-endian MIPS.
- **No static `/etc/config/wireless`** is shipped (see the §5 deviation note at the end).
- **Validated:** `./build.sh all` from scratch produces all three CPE510 variants with
  our `wfb-ng 2025.06.25-r1` (manifest-confirmed), sysupgrade ≈7.80 MB (within the 7680k budget).

---

## File Structure

| File | Responsibility |
|---|---|
| `versions.env` | Single source of pinned versions/commit/profiles, sourced by `build.sh`. |
| `.gitignore` | Ignore build artifacts (`/build/`, `/output/`). |
| `build.sh` | Orchestrator: builds Docker images, runs package + image stages. |
| `docker/Dockerfile.sdk` | Debian image with the pinned OpenWrt SDK + `packages` feed + qemu. |
| `docker/Dockerfile.imagebuilder` | Debian image with the pinned OpenWrt ImageBuilder. |
| `docker/sdk-build.sh` | Runs inside SDK container: compile `wfb-ng` `.ipk`, arch-check. |
| `docker/sdk-fectest.sh` | Runs inside SDK container: cross-build + qemu-run the swfec self-test. |
| `docker/ib-build.sh` | Runs inside ImageBuilder container: assemble per-variant images + size assert. |
| `feed/net/wfb-ng/Makefile` | OpenWrt package: build `wfb_rx`/`wfb_tx` from the fork, install launcher/conf. |
| `feed/net/wfb-ng/files/wfb-ng.sh` | POSIX-sh on-demand launcher (`start`/`stop`/`status`). |
| `feed/net/wfb-ng/files/wfb-ng.conf` | Default config sourced by the launcher (conffile). |
| `feed/net/wfb-ng/tests/test_launcher.sh` | Host-side stub-based tests for the launcher. |
| `files/` | ImageBuilder rootfs overlay (static parts; `gs.key` staged in at build). |
| `keys/gs.key`, `keys/drone.key` | Fixed test keypair. |
| `README.md` | Build/flash/configure/smoke-test/key-regen docs. |

---

## Task 1: Repo scaffolding & pinned versions

**Files:**
- Create: `versions.env`
- Create: `.gitignore`

**Interfaces:**
- Produces: `versions.env` defining shell vars `OPENWRT_VERSION`, `OPENWRT_TARGET`, `OPENWRT_SUBTARGET`, `OPENWRT_ARCH`, `WFB_REPO`, `WFB_COMMIT`, `WFB_VERSION`, `CPE510_PROFILES`. Every later script sources this.

- [ ] **Step 1: Write `versions.env`**

```sh
# Pinned build inputs for the wfb-ng CPE510 firmware. Sourced by build.sh.

# OpenWrt release + target
OPENWRT_VERSION=25.12.4
OPENWRT_TARGET=ath79
OPENWRT_SUBTARGET=generic
OPENWRT_ARCH=mips_24kc

# wfb-ng source (user's fork; pushed before building)
WFB_REPO=https://github.com/gilankpam/wfb-ng.git
WFB_COMMIT=e8033cf9cf5a2081447ae45bf441bc68c28a26da
WFB_VERSION=2025.06.25

# CPE510 hardware revisions to build images for
CPE510_PROFILES="tplink_cpe510-v1 tplink_cpe510-v2 tplink_cpe510-v3"
```

- [ ] **Step 2: Write `.gitignore`**

```gitignore
/build/
/output/
```

- [ ] **Step 3: Verify `versions.env` sources cleanly**

Run: `sh -c '. ./versions.env && echo "$OPENWRT_VERSION $WFB_COMMIT $CPE510_PROFILES"'`
Expected: `25.12.4 e8033cf9cf5a2081447ae45bf441bc68c28a26da tplink_cpe510-v1 tplink_cpe510-v2 tplink_cpe510-v3`

- [ ] **Step 4: Commit**

```bash
git add versions.env .gitignore
git commit -m "Add pinned versions and gitignore"
```

---

## Task 2: Fixed test keypair

**Files:**
- Create: `keys/gs.key` (binary, 64 bytes)
- Create: `keys/drone.key` (binary, 64 bytes)

**Interfaces:**
- Produces: `keys/gs.key` (baked to `/etc/gs.key`), `keys/drone.key` (air-unit artifact). Both are `crypto_box` secret(32)+public(32) = 64 bytes.

- [ ] **Step 1: Generate the keypair with the host-built `wfb_keygen`**

`wfb_keygen` writes `gs.key` + `drone.key` into the current directory. Use the binary already built in the sibling source tree.

Run:
```bash
mkdir -p keys && cd keys && ../../wfb-ng/wfb_keygen && cd ..
```
Expected stderr: lines ending `... saved to drone.key` and `... saved to gs.key`.
(If it fails with a missing `libsodium` shared lib, install it on the host, e.g. `sudo apt-get install -y libsodium23`, then rerun.)

- [ ] **Step 2: Verify both keys are 64 bytes**

Run: `wc -c keys/gs.key keys/drone.key`
Expected: each file reports `64`.

- [ ] **Step 3: Commit**

```bash
git add -f keys/gs.key keys/drone.key
git commit -m "Add fixed test keypair (insecure, PoC pairing)"
```

---

## Task 3: On-demand launcher + config (TDD with stubs)

**Files:**
- Create: `feed/net/wfb-ng/files/wfb-ng.sh`
- Create: `feed/net/wfb-ng/files/wfb-ng.conf`
- Test: `feed/net/wfb-ng/tests/test_launcher.sh`

**Interfaces:**
- Produces: launcher honoring env overrides `WFB_CONF` (default `/etc/wfb-ng.conf`) and `WFB_RUN_DIR` (default `/var/run`); subcommands `start|stop|restart|status`. It invokes `iw`, `ip`, `wfb_rx`, `wfb_tx` **by name** (PATH-resolvable, so tests can stub them). Config keys consumed: `CHANNEL`, `BW`, `REG`, `TXPOWER`, `LINK_ID`, `KEY`, `RX_RADIO_PORT`, `HOST_ADDR`, `RX_UDP_PORT`, `RX_EXTRA_ARGS`, `TX_ENABLED`, `TX_RADIO_PORT`, `TX_UDP_PORT`, `TX_EXTRA_ARGS`, `PHY`, `MON`.

- [ ] **Step 1: Write the failing test `feed/net/wfb-ng/tests/test_launcher.sh`**

```sh
#!/bin/sh
# Stub-based tests for wfb-ng.sh. Stubs log "<name> <args>" so we can assert
# the launcher builds the right command lines. POSIX sh only.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LAUNCHER="$HERE/../files/wfb-ng.sh"
fail=0

setup() {
  TMP=$(mktemp -d)
  BIN="$TMP/bin"; mkdir -p "$BIN"
  LOG="$TMP/log"; : > "$LOG"
  for c in iw ip wfb_rx wfb_tx; do
    printf '#!/bin/sh\necho "%s $*" >> "%s"\nexit 0\n' "$c" "$LOG" > "$BIN/$c"
    chmod +x "$BIN/$c"
  done
  export PATH="$BIN:$PATH"
  export WFB_CONF="$TMP/wfb-ng.conf"
  export WFB_RUN_DIR="$TMP/run"; mkdir -p "$WFB_RUN_DIR"
}
teardown() { rm -rf "$TMP"; }
assert_log() { if grep -q -- "$1" "$LOG"; then echo "ok - $2"; else echo "NOT ok - $2 (missing: $1)"; fail=1; fi; }
refute_log() { if grep -q -- "$1" "$LOG"; then echo "NOT ok - $2 (present: $1)"; fail=1; else echo "ok - $2"; fi; }

# Test 1: RX-only start builds the expected commands
setup
cat > "$WFB_CONF" <<'EOF'
CHANNEL=149
BW=HT20
REG=US
LINK_ID=7
RX_RADIO_PORT=0
HOST_ADDR=192.168.1.10
RX_UDP_PORT=5600
TX_ENABLED=0
KEY=/etc/gs.key
EOF
sh "$LAUNCHER" start
assert_log "iw reg set US" "reg domain set"
assert_log "iw phy phy0 interface add mon0 type monitor" "monitor vif created"
assert_log "set channel 149 HT20" "channel/bandwidth set"
assert_log "wfb_rx .*-p 0 .*-i 7 .*-c 192.168.1.10 .*-u 5600 .*-K /etc/gs.key .*mon0" "wfb_rx command line"
refute_log "^wfb_tx " "wfb_tx not started when TX disabled"
teardown

# Test 2: TX enabled also starts wfb_tx
setup
cat > "$WFB_CONF" <<'EOF'
LINK_ID=7
KEY=/etc/gs.key
TX_ENABLED=1
TX_RADIO_PORT=1
TX_UDP_PORT=5601
EOF
sh "$LAUNCHER" start
assert_log "wfb_tx .*-p 1 .*-i 7 .*-u 5601 .*-K /etc/gs.key .*mon0" "wfb_tx command line"
teardown

# Test 3: stop kills tracked process and tears down the monitor vif
setup
: > "$WFB_CONF"
sleep 30 & FAKE=$!; echo "$FAKE" > "$WFB_RUN_DIR/wfb_rx.pid"
sh "$LAUNCHER" stop
if kill -0 "$FAKE" 2>/dev/null; then echo "NOT ok - stop did not kill rx"; fail=1; kill "$FAKE" 2>/dev/null; else echo "ok - stop killed rx"; fi
assert_log "iw dev mon0 del" "monitor vif removed on stop"
teardown

[ "$fail" -eq 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `sh feed/net/wfb-ng/tests/test_launcher.sh`
Expected: FAIL (launcher does not exist yet) — `NOT ok` lines / nonzero exit.

- [ ] **Step 3: Write `feed/net/wfb-ng/files/wfb-ng.sh`**

```sh
#!/bin/sh
# Minimal on-demand wfb-ng launcher for a single-card OpenWrt ground station.
# POSIX sh / busybox ash only (no bash on the device).

WFB_CONF="${WFB_CONF:-/etc/wfb-ng.conf}"
WFB_RUN_DIR="${WFB_RUN_DIR:-/var/run}"

# Defaults (overridable via $WFB_CONF)
PHY="phy0"
MON="mon0"
CHANNEL="149"
BW="HT20"
REG="US"
TXPOWER=""
LINK_ID="0"
KEY="/etc/gs.key"
RX_RADIO_PORT="0"
HOST_ADDR="192.168.1.10"
RX_UDP_PORT="5600"
RX_EXTRA_ARGS=""
TX_ENABLED="0"
TX_RADIO_PORT="1"
TX_UDP_PORT="5601"
TX_EXTRA_ARGS=""

[ -f "$WFB_CONF" ] && . "$WFB_CONF"

RX_PID="$WFB_RUN_DIR/wfb_rx.pid"
TX_PID="$WFB_RUN_DIR/wfb_tx.pid"

setup_mon() {
    iw dev "$MON" del 2>/dev/null
    iw phy "$PHY" interface add "$MON" type monitor || return 1
    ip link set "$MON" up || return 1
    iw reg set "$REG"
    iw dev "$MON" set channel "$CHANNEL" "$BW" || return 1
    [ -n "$TXPOWER" ] && iw dev "$MON" set txpower fixed "$TXPOWER"
    return 0
}

start() {
    mkdir -p "$WFB_RUN_DIR"
    setup_mon || { echo "wfb-ng: monitor setup failed" >&2; exit 1; }
    wfb_rx -p "$RX_RADIO_PORT" -i "$LINK_ID" -c "$HOST_ADDR" -u "$RX_UDP_PORT" -K "$KEY" $RX_EXTRA_ARGS "$MON" &
    echo $! > "$RX_PID"
    if [ "$TX_ENABLED" = "1" ]; then
        wfb_tx -p "$TX_RADIO_PORT" -i "$LINK_ID" -u "$TX_UDP_PORT" -K "$KEY" $TX_EXTRA_ARGS "$MON" &
        echo $! > "$TX_PID"
    fi
    echo "wfb-ng: started"
}

stop_one() {
    [ -f "$1" ] || return 0
    pid=$(cat "$1")
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
    rm -f "$1"
}

stop() {
    stop_one "$RX_PID"
    stop_one "$TX_PID"
    iw dev "$MON" del 2>/dev/null
    echo "wfb-ng: stopped"
}

status() {
    for f in "$RX_PID" "$TX_PID"; do
        name=$(basename "$f" .pid)
        if [ -f "$f" ] && kill -0 "$(cat "$f")" 2>/dev/null; then
            echo "$name: running (pid $(cat "$f"))"
        else
            echo "$name: stopped"
        fi
    done
    iw dev "$MON" info 2>/dev/null || echo "$MON: absent"
}

case "${1:-}" in
    start) start ;;
    stop) stop ;;
    restart) stop; start ;;
    status) status ;;
    *) echo "usage: $0 {start|stop|restart|status}" >&2; exit 1 ;;
esac
```

- [ ] **Step 4: Write the default config `feed/net/wfb-ng/files/wfb-ng.conf`**

```sh
# wfb-ng ground-station config. Sourced by /usr/sbin/wfb-ng.sh.
# Edit, then: /usr/sbin/wfb-ng.sh restart

# --- Radio (must match the air unit) ---
CHANNEL=149          # 5 GHz channel number
BW=HT20              # HT20 | HT40+ | HT40-
REG=US               # regulatory domain (your legal responsibility)
TXPOWER=             # fixed TX power in mBm (e.g. 2000 = 20 dBm); empty = driver default

# --- Link pairing (must match the air unit) ---
LINK_ID=0            # 24-bit wifibroadcast link id
KEY=/etc/gs.key      # shared key file

# --- Downlink RX -> operator host ---
RX_RADIO_PORT=0
HOST_ADDR=192.168.1.10
RX_UDP_PORT=5600
RX_EXTRA_ARGS=

# --- Optional uplink TX (host -> air unit) ---
TX_ENABLED=0
TX_RADIO_PORT=1
TX_UDP_PORT=5601
# Radio/FEC tuning passed verbatim to wfb_tx, e.g. "-M 1 -B 20 -S 1 -L 1 -k 8 -n 12"
TX_EXTRA_ARGS=
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `sh feed/net/wfb-ng/tests/test_launcher.sh`
Expected: all `ok -` lines and final `ALL PASS`.

- [ ] **Step 6: Commit**

```bash
git add feed/net/wfb-ng/files/wfb-ng.sh feed/net/wfb-ng/files/wfb-ng.conf feed/net/wfb-ng/tests/test_launcher.sh
git commit -m "Add POSIX-sh wfb-ng launcher, default config, and launcher tests"
```

---

## Task 4: OpenWrt package + SDK Docker stage → build the `.ipk`

**Files:**
- Create: `feed/net/wfb-ng/Makefile`
- Create: `docker/Dockerfile.sdk`
- Create: `docker/sdk-build.sh`
- Create: `build.sh` (first version: `package` stage only)

**Interfaces:**
- Consumes: `versions.env` (Task 1); `feed/net/wfb-ng/files/*` (Task 3).
- Produces: `build/packages/wfb-ng_*.ipk` (a `mips_24kc` package). `build.sh package` is the entry point. The package Makefile reads `WFB_REPO`/`WFB_COMMIT`/`WFB_VERSION` as make variables (defaulted via `?=`).

- [ ] **Step 1: Write the package Makefile `feed/net/wfb-ng/Makefile`**

```makefile
include $(TOPDIR)/rules.mk

PKG_NAME:=wfb-ng

WFB_VERSION?=2025.06.25
WFB_REPO?=https://github.com/gilankpam/wfb-ng.git
WFB_COMMIT?=e8033cf9cf5a2081447ae45bf441bc68c28a26da

PKG_VERSION:=$(WFB_VERSION)
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=$(WFB_REPO)
PKG_SOURCE_VERSION:=$(WFB_COMMIT)
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION)-$(PKG_SOURCE_VERSION).tar.gz
PKG_MIRROR_HASH:=skip

PKG_LICENSE:=GPL-3.0-only
PKG_LICENSE_FILES:=LICENSE.txt
PKG_MAINTAINER:=gilankpam <gilankpam@gmail.com>

PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

# Build only the two binaries we ship (skip wfb_tun/keygen/cmd and their deps).
MAKE_FLAGS += VERSION=$(PKG_VERSION) COMMIT=$(PKG_SOURCE_VERSION) wfb_rx wfb_tx

define Package/wfb-ng
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Wireless
  TITLE:=wfb-ng minimal RX/TX (CPE510 ground station)
  URL:=https://github.com/gilankpam/wfb-ng
  DEPENDS:=+libpcap +libsodium +libstdcpp
endef

define Package/wfb-ng/description
  Minimal wfb-ng build: wfb_rx and wfb_tx binaries plus an on-demand POSIX-sh
  launcher (wfb-ng.sh) for a single-card OpenWrt ground-station node.
endef

define Package/wfb-ng/conffiles
/etc/wfb-ng.conf
endef

define Package/wfb-ng/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/wfb_rx $(1)/usr/bin/wfb_rx
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/wfb_tx $(1)/usr/bin/wfb_tx
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/wfb-ng.sh $(1)/usr/sbin/wfb-ng.sh
	$(INSTALL_DIR) $(1)/etc
	$(INSTALL_CONF) ./files/wfb-ng.conf $(1)/etc/wfb-ng.conf
endef

$(eval $(call BuildPackage,wfb-ng))
```

- [ ] **Step 2: Write `docker/Dockerfile.sdk`**

```dockerfile
FROM debian:bookworm-slim
ARG OPENWRT_VERSION
ARG OPENWRT_TARGET
ARG OPENWRT_SUBTARGET
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential clang flex bison g++ gawk gcc-multilib g++-multilib \
      gettext git libncurses-dev libssl-dev python3 python3-setuptools \
      rsync swig unzip zlib1g-dev file wget ca-certificates \
      xz-utils zstd qemu-user-static \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt
RUN set -eu; \
    BASE="https://downloads.openwrt.org/releases/${OPENWRT_VERSION}/targets/${OPENWRT_TARGET}/${OPENWRT_SUBTARGET}"; \
    wget -q "$BASE/sha256sums" -O sha256sums; \
    SDK=$(grep -oE 'openwrt-sdk-[^*[:space:]]+\.tar\.(xz|zst)' sha256sums | head -n1); \
    echo "Downloading $SDK"; \
    wget -q "$BASE/$SDK" -O "/opt/$SDK"; \
    mkdir -p /opt/sdk; \
    case "$SDK" in \
      *.zst) zstd -d -c "/opt/$SDK" | tar -x -C /opt/sdk --strip-components=1 ;; \
      *.xz)  tar -xJf "/opt/$SDK" -C /opt/sdk --strip-components=1 ;; \
    esac; \
    rm -f "/opt/$SDK"

WORKDIR /opt/sdk
# Bake the (slow) packages feed clone into the image; chmod so non-root host uid can build.
RUN cp feeds.conf.default feeds.conf && ./scripts/feeds update packages
RUN chmod -R a+rwX /opt/sdk
```

- [ ] **Step 3: Write `docker/sdk-build.sh`**

```sh
#!/bin/sh
# Runs inside the SDK container (as the host uid). Compiles the wfb-ng package
# and copies the .ipk to /work/build/packages, then checks the binary arch.
set -eu
cd /opt/sdk

grep -q '^src-link wfbng ' feeds.conf 2>/dev/null || echo 'src-link wfbng /work/feed' >> feeds.conf
./scripts/feeds update wfbng
./scripts/feeds install -f wfb-ng libpcap libsodium

grep -q '^CONFIG_PACKAGE_wfb-ng=y' .config 2>/dev/null || echo 'CONFIG_PACKAGE_wfb-ng=y' >> .config
make defconfig
make package/wfb-ng/compile -j"$(nproc)" \
  WFB_REPO="${WFB_REPO}" WFB_COMMIT="${WFB_COMMIT}" WFB_VERSION="${WFB_VERSION}"

mkdir -p /work/build/packages
find bin/packages -name 'wfb-ng_*.ipk' -exec cp -v {} /work/build/packages/ \;

# Architecture sanity: the binaries must be big-endian (MSB) MIPS.
BIN=$(find build_dir -maxdepth 3 -type f -name wfb_rx | head -n1)
echo "Checking arch of $BIN"
file "$BIN" | grep -q 'ELF 32-bit MSB.*MIPS' || { echo "ERROR: wfb_rx not big-endian MIPS"; file "$BIN"; exit 1; }
echo "OK: wfb_rx is big-endian MIPS"
```

- [ ] **Step 4: Write `build.sh` (package stage)**

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck disable=SC1091
. ./versions.env

SDK_IMAGE="wfbng-sdk:${OPENWRT_VERSION}"
DOCKER_RUN=(docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD:/work")

build_sdk_image() {
  docker build -t "$SDK_IMAGE" \
    --build-arg OPENWRT_VERSION="$OPENWRT_VERSION" \
    --build-arg OPENWRT_TARGET="$OPENWRT_TARGET" \
    --build-arg OPENWRT_SUBTARGET="$OPENWRT_SUBTARGET" \
    -f docker/Dockerfile.sdk docker
}

cmd_package() {
  build_sdk_image
  mkdir -p build/packages
  "${DOCKER_RUN[@]}" \
    -e WFB_REPO="$WFB_REPO" -e WFB_COMMIT="$WFB_COMMIT" -e WFB_VERSION="$WFB_VERSION" \
    "$SDK_IMAGE" sh -ec '/work/docker/sdk-build.sh'
}

case "${1:-package}" in
  package) cmd_package ;;
  *) echo "usage: $0 {package}" >&2; exit 1 ;;
esac
```

- [ ] **Step 5: Make scripts executable**

Run: `chmod +x build.sh docker/sdk-build.sh feed/net/wfb-ng/files/wfb-ng.sh feed/net/wfb-ng/tests/test_launcher.sh`

- [ ] **Step 6: Build the package and verify the `.ipk` + arch**

Run: `./build.sh package`
Expected (final lines): a copied `wfb-ng_*.ipk` path under `build/packages/`, then `OK: wfb_rx is big-endian MIPS`.
Then: `ls build/packages/` shows `wfb-ng_2025.06.25-1_mips_24kc.ipk` (name may vary by version).

(First run downloads the SDK and builds `libpcap`/`libsodium` — several minutes. Add `V=s` to the `make` line in `docker/sdk-build.sh` if you need to debug a compile failure.)

- [ ] **Step 7: Commit**

```bash
git add feed/net/wfb-ng/Makefile docker/Dockerfile.sdk docker/sdk-build.sh build.sh
git commit -m "Add wfb-ng OpenWrt package and SDK Docker build stage"
```

---

## Task 5: qemu-MIPS swfec FEC verification

**Files:**
- Create: `docker/sdk-fectest.sh`
- Modify: `build.sh` (run the FEC test in the same SDK container run as the package build)

**Interfaces:**
- Consumes: the wfb-ng build dir produced by Task 4's compile (present in the same container run); the SDK cross toolchain under `/opt/sdk/staging_dir/toolchain-*`.
- Produces: a pass/fail gate — `./build.sh package` now also cross-builds `fec_swfec_test` for `mips_24kc` and runs it under `qemu-mips-static`.

- [ ] **Step 1: Write `docker/sdk-fectest.sh`**

```sh
#!/bin/sh
# Cross-build the self-contained swfec self-test for the target and run it under
# qemu (big-endian MIPS), reusing the object files from the package compile.
set -eu
SRC=$(find /opt/sdk/build_dir -maxdepth 2 -type d -name 'wfb-ng-*' | head -n1)
[ -d "$SRC" ] || { echo "ERROR: wfb-ng build dir not found (run the package build first)"; exit 1; }

CC=$(ls /opt/sdk/staging_dir/toolchain-*/bin/*-openwrt-linux-musl-gcc 2>/dev/null | head -n1)
[ -n "$CC" ] || { echo "ERROR: cross gcc not found"; exit 1; }
CXX="${CC%-gcc}-g++"
echo "Using CC=$CC"

cd "$SRC"
# fec_swfec_test has its own main() and links no external libs; build it static.
# zfex.o/fec_swfec.o already exist from the package compile (same ZFEX defines).
make fec_swfec_test CC="$CC" CXX="$CXX" \
  CFLAGS="-O2 -static" LDFLAGS="-static -static-libstdc++ -static-libgcc" \
  VERSION=test COMMIT=testtest

echo "Running fec_swfec_test under qemu-mips-static..."
qemu-mips-static ./fec_swfec_test
echo "OK: swfec FEC self-test passed on big-endian MIPS"
```

- [ ] **Step 2: Wire the FEC test into the package stage of `build.sh`**

Change the `cmd_package` docker run command so both scripts run in one container (the build dir from `sdk-build.sh` must still exist for `sdk-fectest.sh`):

Replace:
```bash
    "$SDK_IMAGE" sh -ec '/work/docker/sdk-build.sh'
```
with:
```bash
    "$SDK_IMAGE" sh -ec '/work/docker/sdk-build.sh && /work/docker/sdk-fectest.sh'
```

- [ ] **Step 3: Make the new script executable**

Run: `chmod +x docker/sdk-fectest.sh`

- [ ] **Step 4: Re-run the package stage and verify the FEC gate passes**

Run: `./build.sh package`
Expected (final lines): `Running fec_swfec_test under qemu-mips-static...` followed by the test's own pass output and `OK: swfec FEC self-test passed on big-endian MIPS`.
(If the cross-toolchain glob in `sdk-fectest.sh` finds nothing, run `docker run --rm wfbng-sdk:${OPENWRT_VERSION} sh -c 'ls /opt/sdk/staging_dir/toolchain-*/bin/*-gcc'` and adjust the glob to the actual triplet.)

- [ ] **Step 5: Commit**

```bash
git add docker/sdk-fectest.sh build.sh
git commit -m "Verify swfec FEC on big-endian MIPS via qemu in the package stage"
```

---

## Task 6: ImageBuilder Docker stage → assemble CPE510 images

**Files:**
- Create: `docker/Dockerfile.imagebuilder`
- Create: `docker/ib-build.sh`
- Create: `files/.gitkeep` (so the empty overlay dir is tracked)
- Modify: `build.sh` (add `image` stage; stage the overlay with `gs.key`)

**Interfaces:**
- Consumes: `build/packages/wfb-ng_*.ipk` (Task 4/5); `keys/gs.key` (Task 2); `versions.env`.
- Produces: per-variant images + `output/drone.key`. `build.sh image` is the entry point.

- [ ] **Step 1: Write `docker/Dockerfile.imagebuilder`**

```dockerfile
FROM debian:bookworm-slim
ARG OPENWRT_VERSION
ARG OPENWRT_TARGET
ARG OPENWRT_SUBTARGET
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential libncurses-dev zlib1g-dev gawk gettext xz-utils zstd \
      wget file python3 unzip rsync ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt
RUN set -eu; \
    BASE="https://downloads.openwrt.org/releases/${OPENWRT_VERSION}/targets/${OPENWRT_TARGET}/${OPENWRT_SUBTARGET}"; \
    wget -q "$BASE/sha256sums" -O sha256sums; \
    IB=$(grep -oE 'openwrt-imagebuilder-[^*[:space:]]+\.tar\.(xz|zst)' sha256sums | head -n1); \
    echo "Downloading $IB"; \
    wget -q "$BASE/$IB" -O "/opt/$IB"; \
    mkdir -p /opt/ib; \
    case "$IB" in \
      *.zst) zstd -d -c "/opt/$IB" | tar -x -C /opt/ib --strip-components=1 ;; \
      *.xz)  tar -xJf "/opt/$IB" -C /opt/ib --strip-components=1 ;; \
    esac; \
    rm -f "/opt/$IB"; \
    chmod -R a+rwX /opt/ib

WORKDIR /opt/ib
```

- [ ] **Step 2: Write `docker/ib-build.sh`**

```sh
#!/bin/sh
# Runs inside the ImageBuilder container. Adds our package to the local repo,
# builds one image per CPE510 profile, copies results to /work/output, and
# asserts the size budget.
set -eu
cd /opt/ib

cp /work/build/packages/wfb-ng_*.ipk packages/

for p in $PROFILES; do
  echo "=== building image for $p ==="
  make image PROFILE="$p" PACKAGES="$PACKAGES" FILES=/work/build/overlay
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
```

- [ ] **Step 3: Track the overlay dir**

Run: `mkdir -p files && touch files/.gitkeep`

- [ ] **Step 4: Add the `image` stage to `build.sh`**

Add the `IB_IMAGE` and `IMG_PACKAGES` definitions near the top (after `SDK_IMAGE`):
```bash
IB_IMAGE="wfbng-ib:${OPENWRT_VERSION}"
IMG_PACKAGES="wfb-ng iw -wpad-basic-mbedtls -dnsmasq -odhcpd -ppp -ppp-mod-pppoe"
```

Add the image-builder image function (next to `build_sdk_image`):
```bash
build_ib_image() {
  docker build -t "$IB_IMAGE" \
    --build-arg OPENWRT_VERSION="$OPENWRT_VERSION" \
    --build-arg OPENWRT_TARGET="$OPENWRT_TARGET" \
    --build-arg OPENWRT_SUBTARGET="$OPENWRT_SUBTARGET" \
    -f docker/Dockerfile.imagebuilder docker
}
```

Add the `cmd_image` function:
```bash
cmd_image() {
  build_ib_image
  ls build/packages/wfb-ng_*.ipk >/dev/null 2>&1 || { echo "Run './build.sh package' first."; exit 1; }
  rm -rf build/overlay && mkdir -p build/overlay/etc
  [ -d files ] && cp -a files/. build/overlay/ 2>/dev/null || true
  rm -f build/overlay/.gitkeep
  cp keys/gs.key build/overlay/etc/gs.key
  mkdir -p output
  "${DOCKER_RUN[@]}" \
    -e PROFILES="$CPE510_PROFILES" -e PACKAGES="$IMG_PACKAGES" \
    "$IB_IMAGE" sh -ec '/work/docker/ib-build.sh'
  echo "Images in ./output:"; ls -lh output/
}
```

Replace the `case` block with:
```bash
case "${1:-all}" in
  package) cmd_package ;;
  image) cmd_image ;;
  all) cmd_package; cmd_image ;;
  *) echo "usage: $0 {package|image|all}" >&2; exit 1 ;;
esac
```

- [ ] **Step 5: Make `ib-build.sh` executable**

Run: `chmod +x docker/ib-build.sh`

- [ ] **Step 6: Build the images and verify outputs + size**

Run: `./build.sh image`
Expected (final lines): `OK: all images within size budget`, then a listing of `output/` containing, for each of v1/v2/v3, a `*tplink_cpe510-vN-squashfs-sysupgrade.bin` and `*-factory.bin`, plus `drone.key`.

Verify the wfb-ng files actually landed in an image's rootfs (optional sanity):
Run: `docker run --rm -v "$PWD:/work" "wfbng-ib:${OPENWRT_VERSION}" sh -ec 'cd /opt/ib && ls -l build_dir/*/root-*/usr/sbin/wfb-ng.sh build_dir/*/root-*/etc/gs.key build_dir/*/root-*/usr/bin/wfb_rx'`
Expected: all three paths exist.

- [ ] **Step 7: Commit**

```bash
git add docker/Dockerfile.imagebuilder docker/ib-build.sh build.sh files/.gitkeep
git commit -m "Add ImageBuilder stage to assemble CPE510 images with size assertion"
```

---

## Task 7: Orchestrator polish, README & on-device smoke test

**Files:**
- Create: `README.md`

**Interfaces:**
- Consumes: everything prior. `build.sh all` is the full pipeline (`package` incl. FEC test, then `image`).

- [ ] **Step 1: Verify the full pipeline runs end-to-end**

Run: `./build.sh all`
Expected: SDK package build → `OK: wfb_rx is big-endian MIPS` → `OK: swfec FEC self-test passed on big-endian MIPS` → ImageBuilder → `OK: all images within size budget` → `output/` listing.

- [ ] **Step 2: Write `README.md`**

````markdown
# wfb-ng firmware for TP-Link CPE510

Minimal OpenWrt firmware that turns a TP-Link CPE510 into a single-card
wfb-ng **ground-station receiver** (with an optional uplink). It runs nothing
at boot; you SSH in, configure, and start it on demand.

- OpenWrt **25.12** (`ath79/generic`), CPE510 **v1/v2/v3**.
- Ships `wfb_rx` + `wfb_tx` built from `gilankpam/wfb-ng@swfec`, an on-demand
  launcher, a baked **test key** (insecure — see *Keys*), and SSH for config.
- See `docs/superpowers/specs/2026-06-25-wfb-ng-openwrt-cpe510-design.md`.

## Build

Requires Docker. All inputs are pinned in `versions.env`.

```sh
./build.sh all       # package (+ FEC test) then images
# or individually:
./build.sh package   # compile the wfb-ng .ipk + qemu FEC self-test
./build.sh image     # assemble the CPE510 images
```

Outputs land in `output/`:
- `*tplink_cpe510-v{1,2,3}-squashfs-sysupgrade.bin` — flash from OpenWrt.
- `*tplink_cpe510-v{1,2,3}-squashfs-factory.bin` — flash from TP-Link firmware / TFTP.
- `drone.key` — copy this to your **air unit** (it pairs with the baked `gs.key`).

To build newer wfb-ng work: push to the fork, set `WFB_COMMIT` in `versions.env`, rerun.

## Flash

Pick the file matching your hardware revision. From stock TP-Link: use the
`-factory.bin` (Pharos web UI or TFTP recovery). From an existing OpenWrt:
`sysupgrade -n <...-sysupgrade.bin>` (use `-n`, do not keep settings).

## Configure & run

1. Set your computer's NIC to a static **192.168.1.10/24** and connect it to the
   CPE510 (PoE LAN port). The device is **192.168.1.1**, no DHCP.
2. `ssh root@192.168.1.1`
3. Edit `/etc/wfb-ng.conf` to match your air unit (`CHANNEL`, `LINK_ID`, and, if
   you want the uplink, `TX_ENABLED=1`). `HOST_ADDR` defaults to `192.168.1.10`.
4. Start it: `/usr/sbin/wfb-ng.sh start`  (also `stop`, `restart`, `status`).
5. On your computer: decoded downlink arrives as UDP on **192.168.1.10:5600**
   (point your video player / GCS there). For the uplink, send UDP to
   **192.168.1.1:5601**.

There is no autostart. To make it persist across reboots you must add your own
init hook — by design it stays off until you start it.

## Keys (important)

The image bakes a **fixed, shared, non-unique test key** (`keys/gs.key`) — fine
for bench/PoC, **not secure**. For a real link, regenerate a unique pair and
rebuild:

```sh
cd keys && rm -f gs.key drone.key && ../../wfb-ng/wfb_keygen && cd ..
./build.sh image
```

Install the new `output/drone.key` on the air unit.

## On-device smoke test

```sh
ssh root@192.168.1.1
iw dev                      # radio present (phy0)
/usr/sbin/wfb-ng.sh start
/usr/sbin/wfb-ng.sh status  # wfb_rx running; mon0 present on the channel
```
With the air unit transmitting (matching key / `LINK_ID` / radio port / channel),
on the host: `tcpdump -ni <nic> udp port 5600` should show decoded packets.
````

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "Add README with build, flash, configure, and smoke-test docs"
```

---

## Notes for the implementer

- **Network is required** during `./build.sh package` (git-fetches the fork, builds deps) and `./build.sh image` (ImageBuilder downloads base packages). The first SDK/ImageBuilder image build downloads ~tens of MB each and is cached by Docker thereafter.
- **Do not run the OpenWrt build as root.** `build.sh` already runs containers with `-u $(id -u):$(id -g)`; the Docker images make `/opt/sdk` and `/opt/ib` world-writable so this works.
- If `make image` reports an unknown package for a `-pkgname` removal, that default isn't present in 25.12 `ath79/generic` — drop it from `IMG_PACKAGES`. Confirm names with: `docker run --rm wfbng-ib:${OPENWRT_VERSION} sh -ec 'cd /opt/ib && make info | sed -n "1,40p"'`.
- The launcher assumes the single radio is `phy0` (true for CPE510). Override via `PHY=` in `/etc/wfb-ng.conf` if needed.
- **Wireless config (deviation from spec §5):** the plan intentionally does **not** ship a static `/etc/config/wireless` to disable the radio. OpenWrt 25.12 ships wifi disabled until configured, `wpad` is removed (so nothing can claim `phy0`), and the launcher's `setup_mon` deletes any pre-existing vif before adding `mon0`. This keeps netifd off `phy0` without a device-specific `path`/`phy` anchor that would differ across CPE510 v1/v2/v3. If a future variant comes up with an active managed vif at boot, add `iw dev <vif> del` handling or ship a per-variant wireless file then.
- **LAN address:** the static `192.168.1.1/24` LAN is OpenWrt's default for this target, so no `/etc/config/network` overlay is needed; removing `dnsmasq`/`odhcpd` is what makes it DHCP/DNS-free.

