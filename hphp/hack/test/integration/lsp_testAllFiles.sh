#!/bin/bash
# shellcheck disable=SC2000  # we need bytecount "wc -c", not charcount "${#CMD}" as it suggests

# This test does didOpen+hover+didClose for a selection of file in the repository
# Usage: lsp_testAllFiles.sh <HH_CLIENT_PATH> <EMPTY_NT_PATH> <ROOT> <EVERY> <OUT_OF>
# The use of EVERY/OUT_OF is to help shard the work. For instance EVERY=2 OUT_OF=6 will do
# every sixth file starting at the third (zero-based).
# Upon success, exits with exit code 0
# Upon failure, exits with non-zero exit code, often 143 (SIGTERM) if this script failed to
# get back an LSP response we were looking for, or other error codes from abnormal hh_client exit.

# e.g. to run over everything
#   buck2 test @//mode/opt //hphp/hack/test/integration:lsp_testAllFiles_{0,1,2,3,4,5,6,7,8,9}
# If one of the tests failed, it will look like this:
#   4503. Check didOpen+hover+didClose for stricter_consts/trait_typeconst_1.bad.php
#   failed

# e.g. to run over an individual file (given its index from the failure above)
#   ROOT=$(buck2 build @//mode/opt //hphp/hack/test/typecheck:repo --show-full-output | cut -f2 -d' ')
#   HH_CLIENT=$(buck2 build @//mode/opt //hphp/hack/src:hh_client_precopy --show-full-output | cut -f2 -d' ')
#   NT=$(buck2 build @//mode/opt //hphp/hack/test/integration/data/empty_repo:naming_table --show-full-output | cut -f2 -d' ')
#   ./lsp_testAllFiles.sh $HH_CLIENT $NT $ROOT 4503 999999


HH_CLIENT="$1"
NAMING_TABLE=$(realpath "$2")
ROOT=$(realpath "$3")
EVERY=$4
OUT_OF=$5
LOG=$(mktemp)  # will contain LSP responses, and verbose logging from hack
PROC=$$
echo "TESTING LSP. transcript: $LOG"
cd "$ROOT" || exit 1

{
  CMD='{"jsonrpc":"2.0", "id":"init", "method":"initialize", "params":{"rootPath":"'$ROOT'", "rootUri":"file://'$ROOT'", "initializationOptions":{"delayUntilDoneInit":true, "namingTableSavedStatePath":"'$NAMING_TABLE'"}}}'
  printf "Content-Length: %s\r\n\r\n%s" "$(printf "%s" "$CMD" | wc -c)" "$CMD"

  timeout 60 bash -c "tail -n +1 -f $LOG | grep -m 1 'Finished init: ok'" || { echo "failed" 1>&2; kill $PROC; exit 1; }

  ID=-1
  shopt -s globstar nullglob
  for FILE in "$ROOT"/**/*.php; do
    ID=$(( ID + 1 ))
    [ $(( ID % OUT_OF - EVERY )) == "0" ] || continue;
    # This baseline intentionally triggers an ICE before the recovery fix.
    if [[ ${FILE#"$ROOT/"} == "named_parameters/variadic_fn_types/named_params_variadic_position_bad.php" ]]; then
      continue
    fi
    # This perf baseline can take longer than the 60s hover timeout.
    if [[ ${FILE#"$ROOT/"} == "shape_splat/nonnull_intersection/conditional_writes.php" ]]; then
      continue
    fi
    echo "$ID. Check didOpen+hover+didClose for ${FILE#"$ROOT/"}" 1>&2
    TEXT=$(jq --slurp --raw-input . "$FILE")
    URI="file://$ROOT/$FILE"
    CMD='{"jsonrpc":"2.0", "method":"textDocument/didOpen", "params":{"textDocument":{"uri":"'$URI'","version":1,"text":'$TEXT'}}}'
    printf "Content-Length: %s\r\n\r\n%s" "$(printf "%s" "$CMD" | wc -c)" "$CMD"

    CMD='{"jsonrpc":"2.0", "id":"id'$ID'", "method":"textDocument/hover", "params":{"textDocument":{"uri":"'$URI'"}, "position":{"line":0, "character":0}}}'
    printf "Content-Length: %s\r\n\r\n%s" "$(printf "%s" "$CMD" | wc -c)" "$CMD"

    timeout 60 bash -c "tail -n +1 -f $LOG | grep -m 1 '\"id$ID\",\"result\":'" || { echo "failed" 1>&2; kill $PROC; exit 1; }

    CMD='{"jsonrpc":"2.0", "method":"textDocument/didClose", "params":{"textDocument":{"uri":"'$URI'"}}}'
    printf "Content-Length: %s\r\n\r\n%s" "$(printf "%s" "$CMD" | wc -c)" "$CMD"
  done

  CMD='{"jsonrpc":"2.0", "id":"shutdown", "method":"shutdown"}'
  printf "Content-Length: %s\r\n\r\n%s" "$(printf "%s" "$CMD" | wc -c)" "$CMD"

  CMD='{"jsonrpc":"2.0", "method":"exit"}'
  printf "Content-Length: %s\r\n\r\n%s" "$(printf "%s" "$CMD" | wc -c)" "$CMD"
} | "$HH_CLIENT" lsp --verbose 2>&1 | tee >(cat) > "$LOG"
