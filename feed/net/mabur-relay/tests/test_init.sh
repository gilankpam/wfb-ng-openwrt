#!/bin/sh
# Stub procd helpers, source the init script, assert the registered instance.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
INIT="$HERE/../files/mabur-relay.init"
fail=0
TMP=$(mktemp -d); LOG="$TMP/log"; : > "$LOG"; BIN="$TMP/bin"; mkdir -p "$BIN"
printf '#!/bin/sh\necho "iw $*" >> "%s"\n' "$LOG" > "$BIN/iw"; chmod +x "$BIN/iw"
procd_open_instance() { echo "open_instance $*" >> "$LOG"; }
procd_set_param() { echo "set_param $*" >> "$LOG"; }
procd_close_instance() { echo "close_instance" >> "$LOG"; }
procd_add_reload_trigger() { :; }
assert() { if grep -q -- "$1" "$LOG"; then echo "ok - $2"; else echo "NOT ok - $2 (missing: $1)"; fail=1; fi; }

export RELAY_CONF="$TMP/none.conf"
PATH="$BIN:$PATH"
. "$INIT"
start_service
assert "open_instance relay" "one relay instance"
assert "set_param command /usr/libexec/mabur-relay-start" "procd runs the start wrapper"
assert "set_param respawn 3600 5 0" "respawn enabled"
: > "$LOG"
stop_service
assert "iw dev mon0 del" "stop removes mon0"
[ "$START" = 99 ] && echo "ok - START=99" || { echo "NOT ok - START=$START"; fail=1; }

rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
