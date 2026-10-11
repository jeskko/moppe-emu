#!/bin/bash
# Run an emulated Talkman against osmocom-analog's NMT base station
# (python/nmtlink.py starts osmocom and links the two).
#
#   tools/osmocom-analog/run-nmt.sh NMT ROM SECONDS [nmtlink options] [-- nmt options]
#
# e.g. nmtlink options --keys 26:0123456,30:offhook,50:onhook (dial from
# the handset) or --bs-keys 26:d --keys 35:offhook (osmocom calls the
# phone); --realtime with --net-audio/--radio-out/--radio-in for speech.
#
# NMT is the binary from build.sh; the base station is channel 1, Denmark
# area 1 (traffic area 51) unless nmt options after -- say otherwise.
# The link's log goes to stdout, osmocom's terminal to osmo.log.
set -e
NMT=$(realpath "$1"); ROM=$(realpath "$2"); SECS=$3; shift 3
HERE=$(cd "$(dirname "$0")/../.." && pwd)
exec python3 -u "$HERE/python/nmtlink.py" --nmt "$NMT" "$ROM" --seconds "$SECS" \
	--osmo-log osmo.log "$@"
