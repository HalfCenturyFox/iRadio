#!/bin/bash
# pulse-switch.sh dac|bt|restore - prebaci MPD zvuk na DAC ili BT zvucnik
TARGET="$1"
STATE=/opt/radio/out-last

sink_for() {
    case "$1" in
        bt)  pactl list sinks short 2>/dev/null | awk '/bluez/ {print $2; exit}' ;;
        dac) pactl list sinks short 2>/dev/null | awk '/soc_sound/ {print $2; exit}' ;;
        *)   echo "" ;;
    esac
}

if [ "$TARGET" = "restore" ]; then
    TARGET=$(cat "$STATE" 2>/dev/null)
    [ -z "$TARGET" ] && TARGET=dac
fi

if [ "$TARGET" = "bt" ]; then
    # ako zvucnik nije spojen, probaj da ga vratis (poslednji koriscen)
    for i in 1 2 3 4 5; do
        S=$(sink_for bt)
        [ -n "$S" ] && break
        MAC=$(cat /opt/radio/bt-last 2>/dev/null)
        [ -n "$MAC" ] && bluetoothctl connect "$MAC" >/dev/null 2>&1
        sleep 2
    done
fi

S=$(sink_for "$TARGET")
# fallback: ako trazeni izlaz ne postoji (npr. BT zvucnik je iskljucen), idi na DAC
if [ -z "$S" ] && [ "$TARGET" != "dac" ]; then
    TARGET=dac
    S=$(sink_for dac)
fi
[ -z "$S" ] && exit 1

pactl set-default-sink "$S" >/dev/null 2>&1
for i in $(pactl list sink-inputs short 2>/dev/null | awk '{print $1}'); do
    pactl move-sink-input "$i" "$S" >/dev/null 2>&1
done
echo "$TARGET" > "$STATE"
# ako je DAC izabran, vrati sacuvani DSP preset (BT ide direktno)
if [ "$TARGET" = "dac" ] && [ -x /opt/radio/audio-dsp.sh ]; then
    /opt/radio/audio-dsp.sh restore >/dev/null 2>&1 || true
fi
exit 0
