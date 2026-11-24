// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

ocamlrep_ocamlpool::ocaml_ffi! {
    fn hh_mangle_class_name(name: String) -> String {
        // Same as `hhbc::ClassName::mangle`, without interning.
        hhbc_string_utils::strip_global_ns(&hhbc_string_utils::mangle(name)).to_owned()
    }
}
