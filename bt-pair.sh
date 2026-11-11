#!/bin/bash
# bt-pair.sh <MAC> - pair/trust/connect; log u /tmp/bt-pair.log (UI cita "DONE")
MAC="$1"
LOG=/tmp/bt-pair.log
[ -z "$MAC" ] && exit 1
echo "$MAC" > /opt/radio/bt-last
: > "$LOG"
exec >>"$LOG" 2>&1
echo "pair $MAC"
bluetoothctl trust "$MAC" 2>&1 | tail -1
bluetoothctl pair "$MAC" 2>&1 | tail -2
for i in 1 2 3 4 5; do
    bluetoothctl connect "$MAC" 2>&1 | tail -2
    if bluetoothctl info "$MAC" 2>/dev/null | grep -q "Connected: yes"; then
        sleep 1
        CARD=$(pactl list cards short 2>/dev/null | awk '/bluez_card/ {print $2; exit}')
        [ -n "$CARD" ] && pactl set-card-profile "$CARD" a2dp_sink >/dev/null 2>&1
        echo "DONE ok"
        exit 0
    fi
    sleep 2
done
echo "DONE fail"
