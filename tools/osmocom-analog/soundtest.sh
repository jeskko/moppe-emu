#!/bin/bash
# Real-time NMT link test with sound, on private PipeWire null sinks only
# (nothing reaches the default output):
#
#   tools/osmocom-analog/soundtest.sh DIR [nmtlink options...] [-- nmt options]
#
# e.g.  soundtest.sh /tmp/st --nmt ~/src/osmo/osmocom-analog/src/nmt/nmt \
#           .../MD59SB.raw.orig --eeprom ... --seconds 62 \
#           --keys 26:0123456,30:offhook,55:onhook --hold-music music.flac
#
# Sinks t_ear, t_mic, t_net, each with an unmuted remapped source X_src.
# The phone's microphone hears a 700 Hz tone (pw-play into t_mic, no
# fallback); its earpiece goes to t_ear, osmocom's call audio plays to
# t_net (and records t_mic_src unless --hold-music/--net-audio say
# otherwise).  DIR gets ear.raw, net.raw (48 kHz s16 mono, from the start)
# and link.log.  Look at them with tones.py.  Everything started here is
# stopped before the sinks are unloaded (an ALSA stream whose sink goes
# away moves to the speakers).
set -e
DIR=$(realpath -m "${1:?usage: soundtest.sh DIR nmtlink-options}"); shift
HERE=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$DIR"
MODS=(); PIDS=()
cleanup() {
	for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
	wait 2>/dev/null || true
	for m in "${MODS[@]}"; do pactl unload-module "$m" || true; done
}
trap cleanup EXIT
mk() {
	MODS+=("$(pactl load-module module-null-sink sink_name=$1 channel_map=mono rate=48000)")
	MODS+=("$(pactl load-module module-remap-source master=$1.monitor source_name=$1_src channel_map=mono)")
	pactl set-source-mute "$1_src" 0
}
mk t_ear; mk t_mic; mk t_net
python3 -c "
import numpy as np
t = np.arange(48000 * 600) / 48000
open('$DIR/mic.raw', 'wb').write((np.sin(2 * np.pi * 700 * t) * 12000).astype('<i2').tobytes())"
pw-play --target t_mic -P '{ node.dont-fallback = true node.dont-reconnect = true }' \
	--raw --rate 48000 --channels 1 --format s16 "$DIR/mic.raw" & PIDS+=($!)
arecord -q -D pipewire:NODE=t_ear_src -t raw -f S16_LE -r 48000 -c 1 "$DIR/ear.raw" & PIDS+=($!)
arecord -q -D pipewire:NODE=t_net_src -t raw -f S16_LE -r 48000 -c 1 "$DIR/net.raw" & PIDS+=($!)
# split our options from osmocom's
LINK=(); NMT=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do LINK+=("$1"); shift; done
[ "$1" = "--" ] && shift && NMT=(-- "$@")
NET=(--net-audio "pipewire:NODE=t_net/pipewire:NODE=t_mic_src")
case " ${LINK[*]} " in *" --hold-music "*|*" --net-audio "*) NET=(--net-audio pipewire:NODE=t_net) ;; esac
timeout -k 5 600 python3 -u "$HERE/python/nmtlink.py" --realtime \
	--radio-out pipewire:NODE=t_ear --radio-in pipewire:NODE=t_mic_src "${NET[@]}" \
	"${LINK[@]}" "${NMT[@]}" > "$DIR/link.log" 2>&1 || true
