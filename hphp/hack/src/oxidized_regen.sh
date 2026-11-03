#!/bin/bash
set -u # terminate upon read of uninitalized variable
set -o pipefail # in a pipe, the whole pipe runs, but its exit code is that of the first failure

CYAN=""
WHITE=""
BOLD=""
RESET=""

# If these fail that's just fine.
if [ -t 1 ]; then
    CYAN=$(tput setaf 6)
    WHITE=$(tput setaf 7)
    BOLD=$(tput bold)
    RESET=$(tput sgr0)
fi

set -e # terminate upon non-zero-exit-codes (in case of pipe, only checks at end of pipe)
trap 'echo "exit code $? at line $LINENO" >&2' ERR

function summary {
    echo -e "\n$BOLD$CYAN==>$WHITE ${1}$RESET"
}

function run_hh_codegen {
    local command="$1"
    shift

    if [ -n "$HH_CODEGEN_PATH" ]; then
        "$HH_CODEGEN_PATH" \
            --regen-cmd "$REGEN_COMMAND" \
            --rustfmt "$RUSTFMT_PATH" \
            "$command" \
            "$@"
    else
        "${BUILD_AND_RUN}" src/hh_codegen hh_codegen \
            --regen-cmd "$REGEN_COMMAND" \
            --rustfmt "$RUSTFMT_PATH" \
            "$command" \
            "$@"
    fi
}

function run_hh_oxidize {
    if [ -n "$HH_OXIDIZE_PATH" ]; then
        "$HH_OXIDIZE_PATH" \
            --regen-command "$REGEN_COMMAND" \
            --rustfmt-path "$RUSTFMT_PATH" \
            "$@"
    else
        "${BUILD_AND_RUN}" src/hh_oxidize hh_oxidize \
            --regen-command "$REGEN_COMMAND" \
            --rustfmt-path "$RUSTFMT_PATH" \
            "$@"
    fi
}

# Parse command line arguments
# When this script is run by Buck, Buck dependencies
# are passed in as command line arguments.
# This is because Buck doesn't like it when Bucking causes Bucking
HH_CODEGEN_PATH=""
HH_OXIDIZE_PATH=""
while [[ $# -gt 0 ]]; do
    case $1 in
        # optional
        --hh-codegen=*)
            HH_CODEGEN_PATH="${1#*=}"
            shift
            ;;
        # optional
        --hh-oxidize=*)
            HH_OXIDIZE_PATH="${1#*=}"
            shift
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

#set -x # echo every statement in the script

# Try to get the path of this script relative to fbcode
if [ -n "${BUCK_PROJECT_ROOT:-}" ]; then
    # Running under Buck - find fbsource root by searching up for .projectid file
    CURRENT_DIR="$BUCK_PROJECT_ROOT"
    FBSOURCE_ROOT=""

    while [ "$CURRENT_DIR" != "/" ]; do
        if [ -f "$CURRENT_DIR/.projectid" ] && [ "$(cat "$CURRENT_DIR/.projectid" 2>/dev/null)" = "fbsource" ]; then
            FBSOURCE_ROOT="$CURRENT_DIR"
            break
        fi
        CURRENT_DIR="$(dirname "$CURRENT_DIR")"
    done

    if [ -z "$FBSOURCE_ROOT" ]; then
        echo "Error: Could not find fbsource root directory" >&2
        exit 1
    fi

    FBCODE_ROOT="$FBSOURCE_ROOT/fbcode"
    RUSTFMT_PATH="$FBSOURCE_ROOT/tools/third-party/rustfmt/rustfmt"
else
    # Running directly (without Buck)
    FBCODE_ROOT="$(dirname "${BASH_SOURCE[0]}")/../../.."
    RUSTFMT_PATH="${RUSTFMT_PATH:-"$(realpath "$FBCODE_ROOT/../tools/third-party/rustfmt/rustfmt")"}"
fi

REGEN_COMMAND="buck run @fbcode//mode/dev-nosan-lg fbcode//hphp/hack/src:oxidized_regen"
cd "$FBCODE_ROOT"

BUILD_AND_RUN="hphp/hack/scripts/build_and_run.sh"

summary "Write oxidized/gen/"
run_hh_oxidize \
  --out-dir hphp/hack/src/oxidized/gen                                        \
  --copy-types-file hphp/hack/src/oxidized/copy_types.txt                      \
  --safe-ints-types-file hphp/hack/src/oxidized/safe_ints_types.txt            \
  hphp/hack/src/annotated_ast/aast_defs.ml                                    \
  hphp/hack/src/annotated_ast/namespace_env.ml                                \
  hphp/hack/src/ast/ast_defs.ml                                               \
  hphp/hack/src/custom_error/custom_error.ml                                  \
  hphp/hack/src/custom_error/custom_error_config.ml                            \
  hphp/hack/src/custom_error/error_message.ml                                 \
  hphp/hack/src/custom_error/patt_binding_ty.ml                               \
  hphp/hack/src/custom_error/patt_naming_error.ml                             \
  hphp/hack/src/custom_error/patt_typing_error.ml                             \
  hphp/hack/src/custom_error/patt_error.ml                                    \
  hphp/hack/src/custom_error/patt_file.ml                                      \
  hphp/hack/src/custom_error/patt_locl_ty.ml                                  \
  hphp/hack/src/custom_error/patt_name.ml                                     \
  hphp/hack/src/custom_error/patt_member_name.ml                              \
  hphp/hack/src/custom_error/patt_string.ml                                   \
  hphp/hack/src/custom_error/patt_var.ml                                      \
  hphp/hack/src/custom_error/validation_err.ml                                \
  hphp/hack/src/decl/decl_defs.ml                                             \
  hphp/hack/src/decl/pos/pos_or_decl.ml                                       \
  hphp/hack/src/decl/shallow_decl_defs.ml                                     \
  hphp/hack/src/deps/typing_deps_types.ml                                     \
  hphp/hack/src/diagnostics/explanation.ml                                         \
  hphp/hack/src/diagnostics/user_diagnostic.ml                                          \
  hphp/hack/src/diagnostics/diagnostics.ml                                              \
  hphp/hack/src/diagnostics/warnings_saved_state.ml                                \
  hphp/hack/src/diagnostics/error_codes.ml                                         \
  hphp/hack/src/diagnostics/message.ml                                             \
  hphp/hack/src/facebook/edenfs_watcher/edenfs_watcher_types.ml               \
  hphp/hack/src/facebook/state_lookup/saved_state_lookup_eden_types.ml         \
  hphp/hack/src/naming/name_context.ml                                        \
  hphp/hack/src/naming/naming_error.ml                                        \
  hphp/hack/src/typing/nast_check/nast_check_error.ml                         \
  hphp/hack/src/parser/parsing_error.ml                                       \
  hphp/hack/src/diagnostics/classish_positions_types.ml                            \
  hphp/hack/src/diagnostics/quickfix.ml                                             \
  hphp/hack/src/naming/naming_phase_error.ml                                  \
  hphp/hack/src/options/decl_fold_options.ml                                    \
  hphp/hack/src/options/decl_parser_options.ml                                  \
  hphp/hack/src/options/experimental_features.ml                              \
  hphp/hack/src/options/global_options.ml                                      \
  hphp/hack/src/options/hh_config.ml                                           \
  hphp/hack/src/options/parser_options.ml                                      \
  hphp/hack/src/options/saved_state_rollouts.ml                               \
  hphp/hack/src/options/typechecker_options.ml                                 \
  hphp/hack/src/packages/package.ml                                            \
  hphp/hack/src/packages/package_info.ml                                      \
  hphp/hack/src/parser/full_fidelity_parser_env.ml                             \
  hphp/hack/src/search/utils/search_types.ml                                   \
  hphp/hack/src/typing/service/reason_collector.ml                            \
  hphp/hack/src/typing/service/refinement_counter.ml                           \
  hphp/hack/src/typing/service/tast_collector.ml                              \
  hphp/hack/src/typing/service/tast_hashes.ml                                 \
  hphp/hack/src/typing/service/type_counter.ml                                \
  hphp/hack/src/typing/service/map_reduce_ffi.ml                               \
  hphp/hack/src/typing/type_parameter_env.ml                                  \
  hphp/hack/src/typing/typing_defs_core.ml                                    \
  hphp/hack/src/typing/typing_defs.ml                                         \
  hphp/hack/src/typing/typing_kinding_defs.ml                                 \
  hphp/hack/src/typing/typing_reason.ml                                       \
  hphp/hack/src/typing/typing_tyvar_occurrences.ml                            \
  hphp/hack/src/typing/xhp_attribute.ml                                       \
  hphp/hack/src/utils/decl_reference.ml                                       \
  hphp/hack/src/utils/ignore/facebook/files_to_ignore.ml                        \
  hphp/hack/src/parser/scoured_comments.ml                                    \

# Add exports in oxidized/lib.rs from oxidized/gen/mod.rs.
# BSD sed doesn't have -i
sed "/^pub use r#gen::/d" hphp/hack/src/oxidized/lib.rs > hphp/hack/src/oxidized/lib.rs.tmp
mv hphp/hack/src/oxidized/lib.rs.tmp hphp/hack/src/oxidized/lib.rs
grep "^pub mod " hphp/hack/src/oxidized/gen/mod.rs | sed 's/^pub mod /pub use r#gen::/' >> hphp/hack/src/oxidized/lib.rs

summary "Write individually-converted oxidized files"
run_hh_oxidize hphp/hack/src/deps/file_info.ml > hphp/hack/src/deps/rust/file_info.rs
run_hh_oxidize hphp/hack/src/deps/typing_deps_kind.ml > hphp/hack/src/deps/typing_deps_kind.rs
run_hh_oxidize hphp/hack/src/utils/core/prim_defs.ml > hphp/hack/src/deps/rust/prim_defs.rs
run_hh_oxidize hphp/hack/src/naming/naming_types.ml > hphp/hack/src/naming/rust/naming_types.rs
run_hh_oxidize hphp/hack/src/lints/lints_core.ml > hphp/hack/src/utils/lint/lint.rs
run_hh_oxidize hphp/hack/src/typing/hh_distc_types.ml > hphp/hack/src/typing/hh_distc_types/hh_distc_types.rs

summary "Write oxidized/impl_gen/"
run_hh_codegen enum-helpers \
  --input "hphp/hack/src/oxidized/gen/aast_defs.rs|crate::aast_defs::*|crate::ast_defs" \
  --input "hphp/hack/src/oxidized/gen/ast_defs.rs|crate::ast_defs::*" \
  --output "hphp/hack/src/oxidized/impl_gen/"

summary "Write oxidized/aast_visitor/"
run_hh_codegen visitor \
  --input "hphp/hack/src/oxidized/gen/aast_defs.rs" \
  --input "hphp/hack/src/oxidized/gen/ast_defs.rs" \
  --output "hphp/hack/src/oxidized/aast_visitor/" \
  --root "Program"

summary "Write oxidized/asts"
run_hh_codegen asts \
  --input "hphp/hack/src/oxidized/gen/aast_defs.rs" \
  --input "hphp/hack/src/oxidized/gen/ast_defs.rs" \
  --output "hphp/hack/src/oxidized/asts/" \
  --root "Program"

summary "Write elab/transform/"
run_hh_codegen elab-transform \
  --input "hphp/hack/src/oxidized/gen/aast_defs.rs" \
  --input "hphp/hack/src/oxidized/gen/ast_defs.rs" \
  --output "hphp/hack/src/elab/" \
  --root "Program"
