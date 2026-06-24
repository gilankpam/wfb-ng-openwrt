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
