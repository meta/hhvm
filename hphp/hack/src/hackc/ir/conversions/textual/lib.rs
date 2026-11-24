// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

//! Textual is the name of the text format for Infer's IR.  This crate converts
//! from HackC's IR to Textual for ingestion by Infer.
//!
//! End-to-end usage example:
//!   hackc compile-infer test/infer/basic.hack > basic.sil
//!   ./infer/bin/infer compile --capture-textual basic.sil

#![feature(deref_patterns)]

pub static KEEP_GOING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

pub fn keep_going_mode() -> bool {
    KEEP_GOING.load(std::sync::atomic::Ordering::Acquire)
}

// If KEEP_GOING is false then execute a 'todo!' otherwise call the function.
//
// Used like:
//   textual_todo! { w.comment("TODO: Try-Catch Block")? };
#[allow(unused)]
macro_rules! textual_todo {
    ( message = ($($msg:expr),+), $($rest:tt)+ ) => {
        (if $crate::keep_going_mode() {
            $($rest)+
        } else {
            todo!($($msg),+)
        })
    };
    ( $($rest:tt)+ ) => {
        (if $crate::keep_going_mode() {
            $($rest)+
        } else {
            todo!()
        })
    };
}

mod class;
mod decls;
mod func;
mod hack;
mod lower;
mod mangle;
mod member_op;
mod textual;
mod typed_value;
mod types;
mod util;
mod writer;

pub use writer::textual_writer;
