// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
#![feature(box_patterns)]

#[macro_use]
extern crate rust_to_ocaml_attr;

pub use file_info;
pub use file_info::prim_defs;
pub use naming_types;
pub use pos::file_pos;
pub use pos::file_pos_large;
pub use pos::file_pos_small;
pub use pos::pos_span_raw;
pub use pos::pos_span_tiny;
pub use rc_pos as pos;

mod direct_decl_parser_types;
mod manual;

pub use manual::aast;
pub use manual::aast_defs_impl;
pub use manual::aast_impl;
pub use manual::ast_defs_impl;
pub use manual::custom_error_config_impl;
pub use manual::diagnostics_impl;
pub use manual::direct_decl_parser;
pub use manual::edenfs_watcher_types_impl;
pub use manual::error_hash_set;
pub use manual::global_options_impl;
pub use manual::i_map;
pub use manual::i_set;
pub use manual::ident;
pub use manual::lazy;
pub use manual::local_id;
pub use manual::method_flags;
pub use manual::namespace_env_impl;
pub use manual::package_info_impl;
pub use manual::parser_options_impl;
pub use manual::prop_flags;
pub use manual::s_map;
pub use manual::s_set;
pub use manual::saved_state_rollouts_impl;
pub use manual::scoured_comments_impl;
pub use manual::shallow_decl_defs_impl;
pub use manual::shape_map;
pub use manual::symbol_name;
pub use manual::t_shape_map;
pub use manual::tany_sentinel;
pub use manual::tast_collector_impl;
pub use manual::tast_hashes_impl;
pub use manual::tvid;
pub use manual::typing_defs_flags;
pub use manual::typing_env;
pub use manual::typing_reason_impl;
pub use manual::typing_set;
pub use manual::user_diagnostic_impl;
pub use manual::warnings_saved_state_impl;

mod stubs;

pub use stubs::opaque_digest;

mod impl_gen;
pub use impl_gen::*;

pub mod aast_visitor;

mod asts;
pub use asts::ast;
pub use asts::nast;

mod r#gen;

pub use r#gen::aast_defs;
pub use r#gen::ast_defs;
pub use r#gen::classish_positions_types;
pub use r#gen::custom_error;
pub use r#gen::custom_error_config;
pub use r#gen::decl_defs;
pub use r#gen::decl_fold_options;
pub use r#gen::decl_parser_options;
pub use r#gen::decl_reference;
pub use r#gen::diagnostics;
pub use r#gen::edenfs_watcher_types;
pub use r#gen::error_codes;
pub use r#gen::error_message;
pub use r#gen::experimental_features;
pub use r#gen::explanation;
pub use r#gen::files_to_ignore;
pub use r#gen::full_fidelity_parser_env;
pub use r#gen::global_options;
pub use r#gen::hh_config;
pub use r#gen::map_reduce_ffi;
pub use r#gen::message;
pub use r#gen::name_context;
pub use r#gen::namespace_env;
pub use r#gen::naming_error;
pub use r#gen::naming_phase_error;
pub use r#gen::nast_check_error;
pub use r#gen::package;
pub use r#gen::package_info;
pub use r#gen::parser_options;
pub use r#gen::parsing_error;
pub use r#gen::patt_binding_ty;
pub use r#gen::patt_error;
pub use r#gen::patt_file;
pub use r#gen::patt_locl_ty;
pub use r#gen::patt_member_name;
pub use r#gen::patt_name;
pub use r#gen::patt_naming_error;
pub use r#gen::patt_string;
pub use r#gen::patt_typing_error;
pub use r#gen::patt_var;
pub use r#gen::pos_or_decl;
pub use r#gen::quickfix;
pub use r#gen::reason_collector;
pub use r#gen::refinement_counter;
pub use r#gen::saved_state_lookup_eden_types;
pub use r#gen::saved_state_rollouts;
pub use r#gen::scoured_comments;
pub use r#gen::search_types;
pub use r#gen::shallow_decl_defs;
pub use r#gen::tast_collector;
pub use r#gen::tast_hashes;
pub use r#gen::type_counter;
pub use r#gen::type_parameter_env;
pub use r#gen::typechecker_options;
pub use r#gen::typing_defs;
pub use r#gen::typing_defs_core;
pub use r#gen::typing_deps_types;
pub use r#gen::typing_kinding_defs;
pub use r#gen::typing_reason;
pub use r#gen::typing_tyvar_occurrences;
pub use r#gen::user_diagnostic;
pub use r#gen::validation_err;
pub use r#gen::warnings_saved_state;
pub use r#gen::xhp_attribute;
