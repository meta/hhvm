(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(** The runtime name of a class, as computed by hackc's
    [hhbc::ClassName::mangle]: the leading namespace separator is stripped
    and XHP names are mangled, e.g. [\NS\:foo:bar-baz] becomes
    [NS\xhp_foo__bar_baz]. *)
external mangle_class_name : string -> string = "hh_mangle_class_name"
