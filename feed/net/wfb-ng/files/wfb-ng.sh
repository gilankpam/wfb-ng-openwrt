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
