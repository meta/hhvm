// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

#![feature(deref_patterns)]

pub mod r#async;
pub mod clean;
pub mod control;
pub mod critedge;
pub mod rpo_sort;
pub mod ssa;

pub use r#async::unasync;
pub use critedge::split_critical_edges;
pub use rpo_sort::rpo_sort;
