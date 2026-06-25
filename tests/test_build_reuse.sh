#!/bin/sh
# Verify WFB_REUSE_IMAGES controls whether build.sh runs `docker build` for the
# SDK/IB images. Uses a stub `docker` on PATH that logs only its subcommand.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
fail=0

run_with_stub() {  # $1 = value for WFB_REUSE_IMAGES ; echoes the docker-call log
  TMP=$(mktemp -d); BIN="$TMP/bin"; mkdir -p "$BIN"
  printf '#!/bin/sh\necho "$1" >> "%s/log"\nexit 0\n' "$TMP" > "$BIN/docker"
  chmod +x "$BIN/docker"
  ( cd "$ROOT" && PATH="$BIN:$PATH" WFB_REUSE_IMAGES="$1" ./build.sh package >/dev/null 2>&1 )
  cat "$TMP/log" 2>/dev/null
  rm -rf "$TMP"
}

reuse=$(run_with_stub 1)
if printf '%s\n' "$reuse" | grep -qx build; then echo "NOT ok - docker build ran with WFB_REUSE_IMAGES=1"; fail=1; else echo "ok - image build skipped when WFB_REUSE_IMAGES=1"; fi
if printf '%s\n' "$reuse" | grep -qx run;   then echo "ok - stage container still runs"; else echo "NOT ok - stage 'docker run' missing"; fail=1; fi

default=$(run_with_stub "")
if printf '%s\n' "$default" | grep -qx build; then echo "ok - image build runs by default"; else echo "NOT ok - docker build missing by default"; fail=1; fi

[ "$fail" -eq 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
