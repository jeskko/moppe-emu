#!/bin/bash
# Build osmocom-analog's NMT base station for the emulator link
# (python/nmtlink.py) into a prefix, with the wave-FIFO lockstep patch.
#
#   tools/osmocom-analog/build.sh DIR      # sources and prefix under DIR
#
# Needs git, autotools, libtool, a C compiler, talloc and ALSA headers.
# libosmocore and libosmo-cc are built too (shallow clones carry no tags,
# so their pkg-config version is set by hand).  Tested with osmocom-analog
# 0c72677 (2025-12-06).
set -e
DIR=$(realpath -m "${1:?usage: build.sh DIR}")
HERE=$(cd "$(dirname "$0")" && pwd)
P=$DIR/prefix
mkdir -p "$DIR" "$P"
export PKG_CONFIG_PATH=$P/lib/pkgconfig
cd "$DIR"
[ -d libosmocore ] || git clone -q --depth 1 https://gitea.osmocom.org/osmocom/libosmocore.git
[ -d libosmo-cc ] || git clone -q --depth 1 https://gitea.osmocom.org/cc/libosmo-cc
[ -d osmocom-analog ] || git clone -q https://gitea.osmocom.org/cellular-infrastructure/osmocom-analog

(cd libosmocore && autoreconf -fi && ./configure -q --prefix="$P" \
	--disable-pcsc --disable-gnutls --disable-libsctp --disable-libmnl \
	--disable-systemd-logging --disable-uring --disable-doxygen --disable-werror \
	&& make -j"$(nproc)" -s && make -s install) >/dev/null
sed -i 's/^Version: UNKNOWN/Version: 1.11.0/' "$P"/lib/pkgconfig/*.pc
(cd libosmo-cc && autoreconf -fi && ./configure -q --prefix="$P" \
	&& make -j"$(nproc)" -s && make -s install) >/dev/null
sed -i 's/^Version: UNKNOWN/Version: 3.0.0/' "$P"/lib/pkgconfig/*.pc
cd osmocom-analog
git apply --check "$HERE/wave-fifo-lockstep.patch" 2>/dev/null && git apply "$HERE/wave-fifo-lockstep.patch"
(autoreconf -fi && ./configure -q --prefix="$P" --without-uhd --without-soapy \
	--without-imagemagick --without-fuse CPPFLAGS="-I$P/include" \
	LDFLAGS="-L$P/lib -Wl,-rpath,$P/lib" && make -j"$(nproc)" -s) >/dev/null
echo "$DIR/osmocom-analog/src/nmt/nmt"
