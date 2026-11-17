#!/bin/sh
# Copyright (C) 2026 Grigory Tuboltsev
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Clicks through every synthesizer of the dialog and takes two screenshots of
# each: the page (voice, "Voices..." button, "keep loaded" check box, voice
# type) and the open voice list.  Runs on its own Xvfb with a copy of the
# user's speech-dispatcher configuration and its own speech-dispatcher, so
# the live one is never touched; nothing can play (no audio server).
#   tests/gui-modules.sh [binary] [count]
# Results: $OUT (default /tmp/gui-modules-$USER)/page.png and voices.png,
# NN.png / NNv.png per synthesizer.  Needs Xvfb, xdotool, ImageMagick.
set -eu
BIN=${1:-xfce4-speech-settings}
COUNT=${2:-8}
OUT=${OUT:-/tmp/gui-modules-$USER}
DPY=${DPY:-:79}
# a socket path must stay under 108 bytes: keep the runtime dir short
RUN=$(mktemp -d /tmp/gm-XXXXXX)
rm -rf "$OUT"; mkdir -p "$OUT/cfg"
cp -r "${XDG_CONFIG_HOME:-$HOME/.config}/speech-dispatcher" "$OUT/cfg/" 2>/dev/null || true

Xvfb "$DPY" -screen 0 1280x1000x24 >/dev/null 2>&1 &
XVFB=$!
APP=
cleanup() {
	[ -n "$APP" ] && kill "$APP" 2>/dev/null
	[ -f "$RUN/speech-dispatcher/pid" ] && kill "$(cat "$RUN/speech-dispatcher/pid")" 2>/dev/null
	kill "$XVFB" 2>/dev/null
	rm -rf "$RUN"
}
trap cleanup EXIT INT TERM
sleep 1

start() {
	env -i PATH=/usr/bin:/bin HOME="$HOME" DISPLAY="$DPY" XDG_CONFIG_HOME="$OUT/cfg" \
		XDG_RUNTIME_DIR="$RUN" PULSE_SERVER=unix:/nonexistent LANG="${LANG:-C.UTF-8}" \
		"$BIN" >>"$OUT/app.log" 2>&1 &
	APP=$!
}
# the first start may time out while the modules load: start once more
start; sleep 15
if ! DISPLAY=$DPY xdotool search --name . >/dev/null 2>&1 || grep -q "Autospawn failed" "$OUT/app.log"; then
	kill "$APP" 2>/dev/null; sleep 1; start; sleep 8
fi

export DISPLAY=$DPY
WIN=$(xdotool search --onlyvisible --name . | tail -1)
eval "$(xdotool getwindowgeometry --shell "$WIN")"
# combo positions from the layout: synthesizer row 2, voice row 3
SX=290; SY=96; VY=136
k=0
while [ $k -lt "$COUNT" ]; do
	n=$(printf %02d $k)
	xdotool mousemove $SX $SY click 1; sleep 0.7
	xdotool key Home
	i=0; while [ $i -lt $k ]; do xdotool key Down; i=$((i + 1)); done
	xdotool key Return; sleep 3
	xdotool mousemove 1200 950; sleep 0.5
	import -window root -crop "${WIDTH}x${HEIGHT}+0+0" "$OUT/$n.png"
	# open the voice list, then take the same voice again (never Escape:
	# with no list open it closes the dialog)
	xdotool mousemove $SX $VY click 1; sleep 0.8
	import -window root -crop "${WIDTH}x${HEIGHT}+0+0" "$OUT/${n}v.png"
	xdotool key Return; sleep 0.5
	kill -0 "$APP" || { echo "the dialog quit at synthesizer $k" >&2; exit 1; }
	k=$((k + 1))
done
cd "$OUT"
montage $(ls [0-9][0-9].png) -tile 3x -geometry +3+3 page.png
montage $(ls [0-9][0-9]v.png) -tile 3x -geometry +3+3 voices.png
echo "$OUT/page.png $OUT/voices.png"
