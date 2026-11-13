#!/bin/bash
# rusqlite is built with its "bundled" feature, which statically links a
# private copy of SQLite into the Rust FFI staticlib. Linked into hhvm as-is,
# that copy would shadow the system libsqlite3 used by ext_sqlite3 and
# ext_pdo_sqlite. Prefix every SQLite symbol defined in the staticlib (and,
# consistently, every reference to them) so the two copies cannot collide.
set -e

if (( "$#" != 3 )); then
  echo >&2 "Usage: $0 NM OBJCOPY STATICLIB"
  exit 2
fi
nm="$1"
objcopy="$2"
lib="$3"
prefix="hackc_"
map="${lib}.sqlite-syms"

# Global symbols defined by SQLite all start with "sqlite3"; its internals are
# static. Symbols already carrying the prefix are skipped, so reruns on an
# already-processed archive are no-ops.
"$nm" -g --defined-only "$lib" 2>/dev/null \
  | awk -v p="$prefix" 'NF == 3 && $3 ~ /^sqlite3/ { print $3, p $3 }' \
  | sort -u > "$map"

if [ -s "$map" ]; then
  "$objcopy" --redefine-syms="$map" "$lib"
fi
