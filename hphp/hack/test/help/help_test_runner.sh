#!/bin/bash
set -euo pipefail
# Wrapper for verify.py to test hh_client help output.
# verify.py calls: help_test_runner [binary_path] [flags] < test_file
# We run the binary with the flags and normalize the Usage: line
# to replace the full binary path with "hh_client".
HH_CLIENT="$1"
shift
"$HH_CLIENT" "$@" 2>&1 | sed -e 's|Usage: [^ ]* |Usage: hh_client |' -e 's|^[^ ]*: unknown option |hh_client: unknown option |'
