#!/bin/sh
# audio-dsp.sh — OIR DSP presets (PulseAudio LADSPA sink: HPF + peaking EQ)
# Moduli se ucitavaju jednom (init) i ostaju; preset se menja preko default sink-a
# i restartuje se MPD (tako novi stream ide na izabrani izlaz).
# upotreba: audio-dsp.sh off|clean|clean_eq|restore|init|status
set -u
STATE=/opt/radio/dsp-last
PLUGIN=oir_dsp
SINK_CLEAN=oir_dsp_clean
SINK_EQ=oir_dsp_eq

dac_sink() { pactl list sinks short 2>/dev/null | awk '/soc_sound/{print $2; exit}'; }
bt_sink()  { pactl list sinks short 2>/dev/null | awk '/bluez/{print $2; exit}'; }
has_sink() { pactl list sinks short 2>/dev/null | awk '{print $2}' | grep -qx "$1"; }

unload_dsp_sinks() {
    for m in $(pactl list short modules 2>/dev/null | awk '/ladspa-sink/ && (/oir_dsp_clean/ || /oir_dsp_eq/) {print $1}'); do
        pactl unload-module "$m" 2>/dev/null || true
    done
}

load_sinks() {
    # ocisti stare module (preimenovanja)
    for m in $(pactl list short modules 2>/dev/null | awk '/ladspa-sink/ && /sink_name=oir_dsp / {print $1}'); do
        pactl unload-module "$m" 2>/dev/null || true
    done
    DAC=$(dac_sink)
    [ -z "$DAC" ] && return 1
    if ! has_sink "$SINK_CLEAN"; then
        pactl load-module module-ladspa-sink sink_name=$SINK_CLEAN sink_master="$DAC" \
              plugin=$PLUGIN label=$PLUGIN control=45,140,0,0,1 >/dev/null 2>&1 || true
    fi
    if ! has_sink "$SINK_EQ"; then
        pactl load-module module-ladspa-sink sink_name=$SINK_EQ sink_master="$DAC" \
              plugin=$PLUGIN label=$PLUGIN control=45,85,-10,0,1.4 >/dev/null 2>&1 || true
    fi
}

target_for() {
    case "$1" in
        clean)    echo "$SINK_CLEAN" ;;
        clean_eq) echo "$SINK_EQ" ;;
        *)        dac_sink ;;
    esac
}

apply() {
    mode="$1"
    load_sinks
    case "$mode" in
        off|clean|clean_eq) ;;
        *) exit 2 ;;
    esac
    T=$(target_for "$mode")
    [ -z "$T" ] && exit 1
    was_playing=$(mpc status 2>/dev/null | grep -c '\[playing\]' || true)
    pactl set-default-sink "$T" 2>/dev/null || exit 1
    echo "$mode" > "$STATE"
    systemctl restart mpd >/dev/null 2>&1 || true
    if [ "$was_playing" = "1" ]; then
        sleep 1
        mpc play >/dev/null 2>&1 || true
    fi
}

case "${1:-status}" in
    init)    unload_dsp_sinks; load_sinks; exit $? ;;
    off|clean|clean_eq) apply "$1" ;;
    restore)
        # ako je BT izlaz aktivan, ne diraj DSP (BT ide direktno)
        def=$(pactl get-default-sink 2>/dev/null)
        case "$def" in
            bluez*) exit 0 ;;
        esac
        apply "$(cat "$STATE" 2>/dev/null || echo off)"
        ;;
    status)  cat "$STATE" 2>/dev/null || echo off ;;
    *) echo "upotreba: $0 off|clean|clean_eq|restore|init|status" >&2; exit 2 ;;
esac
exit 0
