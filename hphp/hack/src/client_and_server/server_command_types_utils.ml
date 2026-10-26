open Hh_prelude
open Server_command_types

let debug_describe_t : type a. a t -> string = function
  | STATUS _ -> "STATUS"
  | STATUS_SINGLE _ -> "STATUS_SINGLE"
  | LOG_ERRORS _ -> "LOG_ERRORS"
  | INFER_TYPE _ -> "INFER_TYPE"
  | INFER_DYNAMIC _ -> "INFER_DYNAMIC"
  | ENFORCEMENT_AT_POS_BATCH _ -> "ENFORCEMENT_AT_POS_BATCH"
  | INFER_TYPE_BATCH _ -> "INFER_TYPE_BATCH"
  | INFER_TYPE_ERROR _ -> "INFER_TYPE_ERROR"
  | IS_SUBTYPE _ -> "IS_SUBTYPE"
  | TAST_HOLES _ -> "TAST_HOLES"
  | TAST_HOLES_BATCH _ -> "TAST_HOLES_BATCH"
  | IDENTIFY_FUNCTION _ -> "IDENTIFY_FUNCTION"
  | IDENTIFY_SYMBOL _ -> "IDENTIFY_SYMBOL"
  | METHOD_JUMP _ -> "METHOD_JUMP"
  | METHOD_JUMP_BATCH _ -> "METHOD_JUMP_BATCH"
  | FIND_REFS _ -> "FIND_REFS"
  | GO_TO_IMPL _ -> "GO_TO_IMPL"
  | IDE_FIND_REFS_BY_SYMBOL _ -> "IDE_FIND_REFS_BY_SYMBOL"
  | IDE_GO_TO_IMPL_BY_SYMBOL _ -> "IDE_GO_TO_IMPL_BY_SYMBOL"
  | RENAME _ -> "RENAME"
  | IDE_RENAME_BY_SYMBOL _ -> "IDE_RENAME_BY_SYMBOL"
  | DUMP_SYMBOL_INFO _ -> "DUMP_SYMBOL_INFO"
  | REMOVE_DEAD_FIXMES _ -> "REMOVE_DEAD_FIXMES"
  | REMOVE_DEAD_UNSAFE_CASTS -> "REMOVE_DEAD_UNSAFE_CASTS"
  | REWRITE_LAMBDA_PARAMETERS _ -> "REWRITE_LAMBDA_PARAMETERS"
  | CHECK_LIVENESS -> "CHECK_LIVENESS"
  | PAUSE -> "PAUSE"
  | LINT _ -> "LINT"
  | LINT_STDIN _ -> "LINT_STDIN"
  | LINT_ALL _ -> "LINT_ALL"
  | IN_MEMORY_DEP_TABLE_SIZE -> "IN_MEMORY_DEP_TABLE_SIZE"
  | SAVE_NAMING _ -> "SAVE_NAMING"
  | STATS -> "STATS"
  | DUMP_FULL_FIDELITY_PARSE _ -> "DUMP_FULL_FIDELITY_PARSE"
  | RAGE -> "RAGE"
  | CST_SEARCH _ -> "CST_SEARCH"
  | NO_PRECHECKED_FILES -> "NO_PRECHECKED_FILES"
  | FUN_DEPS_BATCH _ -> "FUN_DEPS_BATCH"
  | LIST_FILES_WITH_ERRORS -> "LIST_FILES_WITH_ERRORS"
  | FILE_DEPENDENTS _ -> "FILE_DEPENDENTS"
  | FIND_ISOLATABLE_CLUSTERS _ -> "FIND_ISOLATABLE_CLUSTERS"
  | VALIDATE_ISOLATION _ -> "VALIDATE_ISOLATION"
  | VERBOSE _ -> "VERBOSE"
  | DEPS_OUT_BATCH _ -> "DEPS_OUT_BATCH"
  | DEPS_IN_BATCH _ -> "DEPS_IN_BATCH"
  | FIND_MY_TESTS _ -> "FIND_MY_TESTS"
  | PACKAGE_LINT _ -> "PACKAGE_LINT"
  | PACKAGE_LINT_FULL _ -> "PACKAGE_LINT_FULL"

let debug_describe_cmd (type a) (command : a command) : string =
  let ({ Server_command_types.from; _ }, rpc) = command in
  debug_describe_t rpc
  ^
  if String.equal from "" then
    ""
  else
    " --from " ^ from

(** This returns a string that's shown "hh_server is busy [STATUS]".
The intent is that users understand what command hh_server is currently busy with.
For command-line commands, we show the "--" option that the user used, e.g. --type-at-pos.
For IDE commands like hover, we show a description like "hover". *)
let status_describe_cmd (type a) (command : a command) : string =
  let ({ Server_command_types.from; desc }, _rpc) = command in
  (if String.equal from "" then
    ""
  else
    from ^ ":")
  ^ desc

let debug_describe_message_type : type a. a message_type -> string = function
  | Hello -> "Hello"
  | Monitor_failed_to_handoff -> "Monitor_failed_to_handoff"
  | Ping -> "Ping"
  | Response _ -> "Response"
