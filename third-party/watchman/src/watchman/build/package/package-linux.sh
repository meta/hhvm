#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Usage: package-linux.sh BUILD_DIR RELEASE OUT_DIR
#
# Packages the watchman and watchmanctl that a Linux build linked with
# prefer-static.cmake as watchman-RELEASE-linux.zip, a .deb and an .rpm in
# OUT_DIR. They install to /usr/local, where the binaries look for their state
# directory.
set -euo pipefail

build=$1
release=$2
mkdir -p "$3"
out=$(cd "$3" && pwd)
here=$(cd "$(dirname "$0")" && pwd)

tmp=
rc=
for sig in 1 2 3 13 15; do eval "trap 'exit $((sig + 128))' $sig"; done
trap 'rc=$?; set +e; rm -rf "$tmp"; exit $rc' EXIT
tmp=$(mktemp -d) || exit 1

name="watchman-$release-linux"
tree="$tmp/$name"
# install.md and other installers copy lib/, so the zip keeps it, empty.
mkdir -p "$tree/bin" "$tree/lib"
cp "$build/bin/watchman" "$build/watchman/cli/release/watchmanctl" "$tree/bin/"
strip "$tree"/bin/*

# What every supported distribution has.
allowed='libstdc\+\+\.so\.6|libm\.so\.6|libgcc_s\.so\.1|libc\.so\.6|ld-linux-x86-64\.so\.2'
for bin in "$tree"/bin/*; do
  if readelf -d "$bin" | grep -E '\((RPATH|RUNPATH)\)'; then
    echo "$bin has an RPATH or RUNPATH" >&2
    exit 1
  fi
  extra=$(readelf -d "$bin" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | grep -vxE "$allowed" || true)
  if [ -n "$extra" ]; then
    echo "$bin needs libraries outside the allowed set: ${extra//$'\n'/ }" >&2
    exit 1
  fi
done

version=$("$tree/bin/watchman" --version)
if [ "$version" = 0.0.0 ]; then
  echo "watchman has no version; build from a git checkout" >&2
  exit 1
fi

(cd "$tmp" && zip -qr "$out/$name.zip" "$name")

deb="$tmp/deb"
mkdir -p "$deb/usr/local/bin"
cp "$tree"/bin/* "$deb/usr/local/bin/"
cp -r "$here/watchman-deb/DEBIAN" "$deb/"
# dpkg-shlibdeps reads debian/control from its working directory.
mkdir -p "$tmp/shlibs/debian"
touch "$tmp/shlibs/debian/control"
depends=$(cd "$tmp/shlibs" && dpkg-shlibdeps -O "$deb"/usr/local/bin/* | sed -n 's/^shlibs:Depends=//p')
control=$(<"$deb/DEBIAN/control")
control=${control//%VERSION%/$version}
control=${control//%DEPENDS%/$depends}
printf '%s\n' "$control" > "$deb/DEBIAN/control"
dpkg-deb --root-owner-group -b "$deb" "$out/watchman_${release}_amd64.deb"

rpmbuild -bb \
  --define "_topdir $tmp/rpmbuild" \
  --define "_rpmdir $out" \
  --define "_build_name_fmt %%{NAME}-%%{VERSION}-%%{RELEASE}.%%{ARCH}.rpm" \
  --define "version $version" \
  --define "prefix /usr/local/" \
  --define "image $deb/usr/local" \
  "$here/watchman.spec"

ls -l "$out"
