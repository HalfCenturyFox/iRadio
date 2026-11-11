#!/bin/bash
# Auto-connect to the last used BT speaker on boot (and after adapter reset).
[ -f /opt/radio/bt-last ] || exit 0
MAC=$(cat /opt/radio/bt-last 2>/dev/null)
[ -z "$MAC" ] && exit 0

rfkill unblock bluetooth 2>/dev/null

# sacekaj da adapter bude powered (do ~30 s)
for i in $(seq 1 30); do
    if bluetoothctl show 2>/dev/null | grep -q "Powered: yes"; then break; fi
    sleep 1
done

for i in $(seq 1 20); do
    if bluetoothctl info "$MAC" 2>/dev/null | grep -q "Connected: yes"; then
        CARD=$(pactl list cards short 2>/dev/null | awk '/bluez_card/ {print $2; exit}')
        [ -n "$CARD" ] && pactl set-card-profile "$CARD" a2dp_sink >/dev/null 2>&1
        exit 0
    fi
    bluetoothctl connect "$MAC" >/dev/null 2>&1
    sleep 2
    if bluetoothctl info "$MAC" 2>/dev/null | grep -q "Connected: yes"; then
        sleep 1
        CARD=$(pactl list cards short 2>/dev/null | awk '/bluez_card/ {print $2; exit}')
        [ -n "$CARD" ] && pactl set-card-profile "$CARD" a2dp_sink >/dev/null 2>&1
        exit 0
    fi
done
exit 0
