#!/bin/sh
# Stub-based tests for mabur-relay-start: stub iw/ip/daemon log their calls.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
START="$HERE/../files/mabur-relay-start"
fail=0
TMP=$(mktemp -d); LOG="$TMP/log"; BIN="$TMP/bin"; mkdir -p "$BIN"
for c in iw ip; do printf '#!/bin/sh\necho "%s $*" >> "%s"\nexit 0\n' "$c" "$LOG" > "$BIN/$c"; chmod +x "$BIN/$c"; done
printf '#!/bin/sh\necho "relay $*" >> "%s"\nexit 0\n' "$LOG" > "$BIN/mabur-relay"; chmod +x "$BIN/mabur-relay"

assert() { if grep -q -- "$1" "$LOG"; then echo "ok - $2"; else echo "NOT ok - $2 (missing: $1)"; fail=1; fi; }
refute() { if grep -q -- "$1" "$LOG"; then echo "NOT ok - $2 (present: $1)"; fail=1; else echo "ok - $2"; fi; }
run() { : > "$LOG"; PATH="$BIN:$PATH" RELAY_CONF="$TMP/conf" RELAY_STATE="$TMP/state" RELAY_BIN="$BIN/mabur-relay" sh "$START"; }

cat > "$TMP/conf" <<'EOF'
PHY=phy0
MON=mon0
REG=US
TXPOWER=2000
BOOT_CHANNEL=136
BOOT_SEC=HT40-
UDP_PORT=8310
WS_PORT=8311
EOF

# No state file -> boot channel.
rm -f "$TMP/state"; run
assert "iw dev mon0 del" "stale vif removed first"
assert "iw reg set US" "reg domain set"
assert "iw phy phy0 interface add mon0 type monitor flags fcsfail otherbss" "monitor vif with fcsfail+otherbss"
assert "ip link set mon0 up" "vif up"
assert "iw dev mon0 set channel 136 HT40-" "boot channel used without state"
assert "iw phy phy0 set txpower fixed 2000" "txpower on the phy"
refute "iw dev mon0 set txpower" "txpower NOT on the vif (EOPNOTSUPP on AR9344)"
assert "relay -i mon0 -u 8310 -w 8311 -c 136 -s 2 -S $TMP/state" "daemon exec'd with boot channel"

# State file wins over the boot channel (respawn after a crash).
echo "149 HT40+" > "$TMP/state"; run
assert "iw dev mon0 set channel 149 HT40+" "state file channel used"
assert "relay -i mon0 -u 8310 -w 8311 -c 149 -s 1" "daemon told the state channel"

# Garbage state file -> boot channel.
echo "banana" > "$TMP/state"; run
assert "iw dev mon0 set channel 136 HT40-" "garbage state ignored"

rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
