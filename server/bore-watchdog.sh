#!/bin/bash
# Decides whether to restart bore.service. Two independent failure signals:
#   1. Direct curl through bore.pub returns code 000 (tunnel definitely dead).
#   2. No real tracker traffic (HTTP 200 to hmac-proxy) in TRAFFIC_TIMEOUT min.
# Either triggers a restart, with a cooldown to avoid thrashing.
#
# The traffic check exists because bore.pub has been seen to "half-wedge":
# the in-tunnel control plane keeps answering Pi-side probes while external
# connections from the tracker are silently dropped. Signal 1 alone misses
# that case; signal 2 catches it on the next stationary cycle.
set -u

PORT=51452
TRAFFIC_TIMEOUT_MIN=25     # tracker posts every 5 min stationary, 30 min low-batt
RESTART_COOLDOWN_MIN=15    # don't restart again within this window
STATE_DIR=/var/lib/bore-watchdog
LAST_RESTART_FILE=$STATE_DIR/last-restart
mkdir -p "$STATE_DIR"

now=$(date +%s)
last_restart=$(cat "$LAST_RESTART_FILE" 2>/dev/null || echo 0)
since_restart_min=$(( (now - last_restart) / 60 ))

restart_bore() {
    local reason="$1"
    if [ "$since_restart_min" -lt "$RESTART_COOLDOWN_MIN" ]; then
        logger -t bore-watchdog "would restart ($reason) but cooldown ${since_restart_min}m < ${RESTART_COOLDOWN_MIN}m"
        return
    fi
    logger -t bore-watchdog "RESTARTING bore.service: $reason"
    systemctl restart bore.service
    echo "$now" > "$LAST_RESTART_FILE"
}

# Signal 1: direct tunnel probe
code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 8 "http://bore.pub:${PORT}/" 2>/dev/null || echo 000)
if [ "$code" = "000" ]; then
    restart_bore "direct probe failed (code=000)"
    exit 0
fi

# Signal 2: no recent tracker traffic. Look for the most recent 200 OK
# response in hmac-proxy journal (real tracker posts; our 403 probes excluded).
last_200_iso=$(journalctl -u hmac-proxy --since "6 hours ago" --no-pager 2>/dev/null \
    | awk '/" 200 /{ts=$1" "$2" "$3} END{print ts}')
if [ -n "$last_200_iso" ]; then
    last_200_epoch=$(date -d "$last_200_iso" +%s 2>/dev/null || echo 0)
    silence_min=$(( (now - last_200_epoch) / 60 ))
    if [ "$silence_min" -ge "$TRAFFIC_TIMEOUT_MIN" ]; then
        restart_bore "no tracker traffic for ${silence_min}m (last 200 at $last_200_iso)"
    fi
else
    logger -t bore-watchdog "no 200 found in last 6h of hmac-proxy log — not restarting (insufficient signal)"
fi
