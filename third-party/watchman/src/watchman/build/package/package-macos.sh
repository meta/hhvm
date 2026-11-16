#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Usage: package-macos.sh BUILD_DIR RELEASE OUT_DIR
#
# Packages the watchman and watchmanctl that a macOS build linked with
# prefer-static.cmake as watchman-RELEASE-macos.zip in OUT_DIR.
set -euo pipefail

build=$1
release=$2
mkdir -p "$3"
out=$(cd "$3" && pwd)

tmp=
rc=
for sig in 1 2 3 13 15; do eval "trap 'exit $((sig + 128))' $sig"; done
trap 'rc=$?; set +e; rm -rf "$tmp"; exit $rc' EXIT
# Some managed Macs won't run unsigned binaries from the temporary directory,
# which the checks below do.
tmp=$(mktemp -d "$out/tmp.XXXXXX") || exit 1

name="watchman-$release-macos"
tree="$tmp/$name"
# Installers written for older zips, which had libraries in lib/, may look for
# it, so the zip keeps it, empty.
mkdir -p "$tree/bin" "$tree/lib"
cp "$build/bin/watchman" "$build/watchman/cli/release/watchmanctl" "$tree/bin/"
strip "$tree"/bin/*

for bin in "$tree"/bin/*; do
  # Read otool's output whole, so that a failing otool stops the script.
  commands=$(otool -l "$bin")
  if grep -A2 LC_RPATH <<<"$commands"; then
    echo "$bin has an LC_RPATH" >&2
    exit 1
  fi
  libraries=$(otool -L "$bin")
  extra=$(sed 1d <<<"$libraries" | awk '{print $1}' | grep -vE '^(/usr/lib|/System/Library)/' || true)
  if [ -n "$extra" ]; then
    echo "$bin needs libraries that aren't part of macOS: ${extra//$'\n'/ }" >&2
    exit 1
  fi
done

"$tree/bin/watchmanctl" --help >/dev/null
version=$("$tree/bin/watchman" --version)
if [ "$version" = 0.0.0 ]; then
  echo "watchman has no version; build from a git checkout" >&2
  exit 1
fi

(cd "$tmp" && zip -qr "$out/$name.zip" "$name")
ls -l "$out/$name.zip"
