#!/bin/bash
# Run an emulated Talkman against osmocom-analog's NMT base station, in
# step through two FIFOs (no sound card, no real time).
#
#   tools/osmocom-analog/run-nmt.sh NMT ROM SECONDS [nmtlink options] [-- nmt options]
#
# e.g. nmtlink options --keys 26:0123456,30:offhook,50:onhook (dial from
# the handset) or --bs-keys 26:d --keys 35:offhook (osmocom calls the phone)
#
# NMT is the binary from build.sh; the base station is channel 1, Denmark
# area 1 (traffic area 51) unless nmt options after -- say otherwise.
# Logs: link.log (phone side), osmo.log (base station) in the current
# directory.
set -e
NMT=$(realpath "$1"); ROM=$(realpath "$2"); SECS=$3; shift 3
LINK=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do LINK+=("$1"); shift; done
[ "$1" = "--" ] && shift
OPTS=("$@")
[ ${#OPTS[@]} -gt 0 ] || OPTS=(-k 1 -Y DK,1)
HERE=$(cd "$(dirname "$0")/../.." && pwd)
rm -f bs_tx.wav bs_rx.wav console.fifo
mkfifo bs_tx.wav bs_rx.wav console.fifo
# osmocom's console needs a terminal: give it one with script(1), whose
# input nmtlink.py writes (--bs-keys)
script -qec "stty cols 160 rows 50; $NMT ${OPTS[*]} -a null --write-tx-wave bs_tx.wav --read-rx-wave bs_rx.wav" /dev/null < console.fifo > osmo.log 2>&1 &
python3 -u "$HERE/python/nmtlink.py" "$ROM" bs_tx.wav bs_rx.wav --seconds "$SECS" --console console.fifo "${LINK[@]}" > link.log 2>&1 || true
pkill -INT -x nmt || true
sleep 1
pkill -9 -x nmt || true
rm -f bs_tx.wav bs_rx.wav console.fifo
cat link.log
sed 's/\x1b\[[0-9;]*m//g; s/\x1b[78]//g' osmo.log | grep -a -o 'nmt.c:[0-9]* .*' | grep -av 'RX Level' || true
